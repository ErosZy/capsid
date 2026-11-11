#pragma once

#include <boost/asio/compose.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_prefix.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/system/system_error.hpp>
#include <cstddef>
#include <utility>

namespace capsid::host {

// Count the serialized request head, including its final CRLFCRLF. The
// parser still owns all HTTP syntax and framing decisions. This counter
// makes the read budget independent of network and parser buffer splits.
class RequestHeaderBudget {
public:
    explicit RequestHeaderBudget(std::size_t limit) : limit_(limit) {}

    template<class ConstBuffers>
    bool observe(const ConstBuffers& buffers) {
        if (complete_ || exceeded_) return !exceeded_;
        static constexpr char end[] = "\r\n\r\n";
        auto at = boost::asio::buffer_sequence_begin(buffers);
        const auto last = boost::asio::buffer_sequence_end(buffers);
        for (; at != last; ++at) {
            const auto buffer = *at;
            const auto* data = static_cast<const unsigned char*>(buffer.data());
            for (std::size_t i = 0; i < buffer.size(); ++i) {
                if (bytes_ == limit_) {
                    exceeded_ = true;
                    return false;
                }
                ++bytes_;
                const unsigned char byte = data[i];
                matched_ = byte == static_cast<unsigned char>(end[matched_]) ?
                    matched_ + 1 : (byte == '\r' ? 1 : 0);
                if (matched_ == 4) {
                    complete_ = true;
                    return true;
                }
            }
        }
        return true;
    }

    bool complete() const { return complete_; }
    bool exceeded() const { return exceeded_; }
    std::size_t bytes() const { return bytes_; }

private:
    std::size_t limit_;
    std::size_t bytes_ = 0;
    unsigned matched_ = 0;
    bool complete_ = false;
    bool exceeded_ = false;
};

template<class Stream>
class HeaderLimitedReadStream {
public:
    using executor_type = typename Stream::executor_type;

    HeaderLimitedReadStream(Stream& stream, std::size_t limit)
        : stream_(stream), budget_(limit) {}

    executor_type get_executor() noexcept { return stream_.get_executor(); }

    // The caller checks bytes already read ahead for this request before
    // starting Beast. Otherwise a complete buffered request could bypass
    // read_some entirely. Body and later pipelined bytes are not counted.
    template<class ConstBuffers>
    bool observe_initial_buffer(const ConstBuffers& buffers) {
        return budget_.observe(buffers);
    }

    template<class MutableBuffers>
    std::size_t read_some(const MutableBuffers& buffers,
                          boost::system::error_code& error) {
        if (budget_.exceeded()) {
            error = boost::beast::http::error::header_limit;
            return 0;
        }
        const auto bytes = stream_.read_some(buffers, error);
        observe_read(buffers, bytes, error);
        return bytes;
    }

    template<class MutableBuffers>
    std::size_t read_some(const MutableBuffers& buffers) {
        boost::system::error_code error;
        const auto bytes = read_some(buffers, error);
        if (error) throw boost::system::system_error(error);
        return bytes;
    }

    template<class MutableBuffers, class CompletionToken>
    auto async_read_some(const MutableBuffers& buffers, CompletionToken&& token) {
        return boost::asio::async_compose<CompletionToken,
            void(boost::system::error_code, std::size_t)>(
                ReadOperation<MutableBuffers>{*this, buffers}, token, stream_);
    }

private:
    template<class MutableBuffers>
    void observe_read(const MutableBuffers& buffers, std::size_t bytes,
                      boost::system::error_code& error) {
        if (!budget_.observe(boost::beast::buffers_prefix(bytes, buffers)))
            error = boost::beast::http::error::header_limit;
    }

    template<class MutableBuffers>
    struct ReadOperation {
        HeaderLimitedReadStream& owner;
        MutableBuffers buffers;
        bool started = false;

        template<class Self>
        void operator()(Self& self, boost::system::error_code error = {},
                        std::size_t bytes = 0) {
            if (!started) {
                started = true;
                if (owner.budget_.exceeded()) boost::asio::post(std::move(self));
                else owner.stream_.async_read_some(buffers, std::move(self));
                return;
            }
            if (owner.budget_.exceeded()) error = boost::beast::http::error::header_limit;
            else owner.observe_read(buffers, bytes, error);
            self.complete(error, bytes);
        }
    };

    Stream& stream_;
    RequestHeaderBudget budget_;
};

}  // namespace capsid::host
