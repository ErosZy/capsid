#ifndef CAPSID_TESTS_WORKER_EXIT_ASSERTIONS_H
#define CAPSID_TESTS_WORKER_EXIT_ASSERTIONS_H

#include "worker_exit_status.h"

#include <iostream>

namespace capsid_test {

inline void print_worker_exit(const capsid::WorkerExitStatus& status) {
    std::cout << "{\"observed\":" << (status.observed ? "true" : "false")
              << ",\"clean\":" << (status.clean() ? "true" : "false")
              << ",\"exitCode\":" << status.exit_code
              << ",\"signal\":" << status.signal
              << ",\"waitError\":" << status.wait_error
              << ",\"destroyForced\":" << (status.destroy_forced ? "true" : "false")
              << '}';
}

inline bool natural_exit(const capsid::WorkerExitStatus& status,
                         std::uint32_t code) {
    return status.observed && !status.destroy_forced && status.wait_error == 0 &&
           status.signal == 0 && status.exit_code == code;
}

}  // namespace capsid_test

#endif
