#ifndef CAPSID_HOST_ADMIN_SERVICE_H
#define CAPSID_HOST_ADMIN_SERVICE_H

#include "host/admin_api.h"
#include "host/admin_http.h"

#include <sys/types.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace capsid::host {

struct AdminServiceOptions {
    AdminSocketOptions socket;
    AdminHttpOptions http;
};

// Long-lived Admin service owning the Unix listener and the accept loop.
// start() returns only after the socket is bound and listening; the
// service accepts and serves connections until request_stop(), which
// wakes BOTH the idle accept and any accepted fd mid-header (via
// shutdown) so stop never waits out an HTTP deadline; wait() joins the
// loop and removes only the socket inode the service itself created
// (device/inode re-checked before the unlink).
class AdminService {
public:
    AdminService(AdminServiceOptions options, AdminBackend* backend);
    ~AdminService();

    AdminService(const AdminService&) = delete;
    AdminService& operator=(const AdminService&) = delete;

    bool start(std::string* error);
    void request_stop();
    bool wait(std::string* error);

private:
    void accept_loop();

    AdminServiceOptions options_;
    AdminBackend* backend_;
    int listener_ = -1;
    int stop_pipe_[2] = {-1, -1};
    // The accepted connection's fd lifecycle is mutex-protected: request_stop
    // shuts the fd down under the same lock that clears it, so shutdown can
    // never race a close+reuse of the descriptor number.
    std::mutex active_mutex_;
    int active_fd_ = -1;
    std::atomic<bool> stopping_{false};
    // One-shot gate: the stop pipe is written at most once.
    std::atomic<bool> stop_written_{false};
    std::thread thread_;
    // The socket inode this service created at start (for cleanup
    // ownership); zero when start never bound.
    dev_t socket_dev_ = 0;
    ino_t socket_ino_ = 0;
};

}  // namespace capsid::host

#endif
