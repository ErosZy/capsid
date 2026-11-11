#include "capsid/runtime.h"
#include "win32_compat.h"
#include "graceful_worker_exit.h"
#include "worker_exit_status.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

namespace {
void check(bool value, const char* message) {if (!value) throw std::runtime_error(message);}
void ok(capsid_result value, const char* message) {check(value == CAPSID_OK, message);}
struct Reply {
    unsigned heads = 0;
    unsigned ends = 0;
    unsigned bodies = 0;
    uint64_t credits = 0;
    std::string body;
};
struct Peer {
    capsid_worker* worker = nullptr;
    std::map<uint64_t, Reply> replies;
    ~Peer() {capsid_worker_destroy(worker);}
    capsid_event next() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto flushed = capsid_worker_flush(worker);
            check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush");
            capsid_event event{}; event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                if (event.type == CAPSID_EVENT_ERROR)
                    std::fprintf(stderr, "worker error: %.*s\n", int(event.payload.size), event.payload.data);
                check(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT, "healthy worker");
                if (event.type == CAPSID_EVENT_AUDIT || event.type == CAPSID_EVENT_LOG || event.type == CAPSID_EVENT_MEMORY_METRICS) {
                    std::printf("CONTEXT_AUX type=%d request=%llu status=%u\n", int(event.type),
                        static_cast<unsigned long long>(event.request_id), event.status);
                    continue;
                }
                return event;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline, "original5s event deadline");
            capsid_pollfd fd{}; fd.fd = capsid_worker_fd(worker);
            fd.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&fd, 1, 10);
        }
    }
    void begin(uint64_t id, const std::string& url) {
        check(replies.count(id) == 0, "unique submitted HTTP request ID");
        replies.emplace(id, Reply{});
        ok(capsid_worker_begin_bodyless_request(worker, id, "GET", url.c_str(), nullptr, 0), "begin HTTP context request");
    }
    void consume() {
        const auto event = next();
        const auto found = replies.find(event.request_id);
        check(found != replies.end(), "event belongs to a submitted HTTP request");
        Reply& reply = found->second;
        if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
            check(reply.heads == 0 && reply.ends == 0 && event.status == 200, "one valid response head");
            ++reply.heads;
        } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            check(reply.heads == 1 && reply.ends == 0, "body belongs to an open response");
            ++reply.bodies;
            reply.body.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
            if (event.payload.size) {
                ok(capsid_worker_grant_response_credit(worker, event.request_id, static_cast<uint32_t>(event.payload.size)), "return actual consumed bytes");
                reply.credits += event.payload.size;
            }
        } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
            check(reply.heads == 1 && reply.ends == 0, "one response terminal"); ++reply.ends;
        } else if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
            check(event.credit > 0, "positive request credit");
        } else throw std::runtime_error("ordinary native fetch emitted a routed channel or unexpected event");
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 5, "worker, fixture, target, operation");
        std::ifstream input(argv[2], std::ios::binary); check(input.is_open(), "fixture opened");
        const std::string program((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        capsid_worker_config config; capsid_worker_config_init(&config); config.worker_path = argv[1];
        capsid_egress_rule rule; capsid_egress_rule_init(&rule); rule.action = CAPSID_EGRESS_ALLOW; rule.target = "localhost";
        capsid_egress_policy policy; capsid_egress_policy_init(&policy); policy.rules = &rule; policy.rule_count = 1; config.egress_policy = &policy;
        capsid_protocol_options options; capsid_protocol_options_init(&options, sizeof(options));
        options.wire_version = 4; options.capabilities = 123; options.max_channels = 1;
        Peer peer;
        ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "configured native context worker spawn");
        ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t*>(program.data()), program.size()), "bundle load");
        check(peer.next().type == CAPSID_EVENT_READY, "real READY");
        peer.begin(1, std::string("http://app.test/owner?target=") + argv[3]);
        const std::string ready = "OWNER_READY\n";
        const std::string gate = "OWNER_GUEST_DONE\n";
        while (peer.replies[1].body.size() < ready.size()) peer.consume();
        check(peer.replies[1].body == ready && peer.replies[1].ends == 0, "owner remains open before guest");
        peer.begin(2, std::string("http://app.test/guest?operation=") + argv[4]);
        while (peer.replies[2].ends == 0 || peer.replies[1].body.size() < ready.size() + gate.size()) peer.consume();
        check(peer.replies[1].body.compare(ready.size(), gate.size(), gate) == 0, "owner read-entry barrier before source release");
        std::printf("CONTEXT_GUEST %s\nCONTEXT_OWNER_READ_READY true\n", peer.replies[2].body.c_str()); std::fflush(stdout);
        std::string acknowledgment;
        check(static_cast<bool>(std::getline(std::cin, acknowledgment)) && acknowledgment == "continue", "held source released after both request barriers");
        while (peer.replies[1].ends == 0) peer.consume();
        const auto owner = peer.replies[1].body.substr(ready.size() + gate.size());
        std::printf("CONTEXT_OWNER %s\n", owner.c_str()); std::fflush(stdout);
        check(static_cast<bool>(std::getline(std::cin, acknowledgment)) && acknowledgment == "finish", "source terminal observed before shutdown");
        peer.begin(3, "http://app.test/healthy");
        while (peer.replies[3].ends == 0) peer.consume();
        check(peer.replies[3].body == "healthy", "same-worker healthy reuse after overlapping requests");
        std::printf("CONTEXT_PROTOCOL {\"requests\":3,\"replies\":[");
        bool first = true;
        for (const auto& item : peer.replies) {
            check(item.second.heads == 1 && item.second.ends == 1 && item.second.credits == item.second.body.size(), "complete ownership and exact credits");
            std::printf("%s{\"id\":%llu,\"heads\":%u,\"ends\":%u,\"bodyFrames\":%u,\"bodyBytes\":%zu,\"returnedCredit\":%llu}",
                first ? "" : ",", static_cast<unsigned long long>(item.first), item.second.heads, item.second.ends,
                item.second.bodies, item.second.body.size(), static_cast<unsigned long long>(item.second.credits));
            first = false;
        }
        std::printf("]}\n"); std::fflush(stdout);
        const auto exited = capsid_test::shutdown_and_wait(peer.worker, 2500);
        const auto cleanup = capsid::destroy_worker_with_status(peer.worker); peer.worker = nullptr;
        std::printf("CONTEXT_EXIT {\"clean\":%s,\"processObserved\":%s,\"exitStatus\":%d,\"forced\":%s,\"errorEvents\":%u}\n",
            exited.clean() ? "true" : "false", exited.reaped ? "true" : "false", exited.status,
            cleanup.destroy_forced ? "true" : "false", exited.error_events);
        check(exited.clean() && !cleanup.destroy_forced, "original2500ms natural OS exit");
        return 0;
    } catch (const std::exception& error) {std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;}
}
