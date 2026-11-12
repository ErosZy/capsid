// Test-only launch wrapper. The protocol and responses still come from the
// actual capsid-worker after exec; the child models a helper orphaned at exit.
#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {
struct Child { pid_t parent; int ready; };
int child_main(void* opaque) {
    const auto* child = static_cast<const Child*>(opaque);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) != 0 || getppid() != child->parent) _exit(91);
    if (write(child->ready, "R", 1) != 1) _exit(92);
    close(child->ready);
    for (;;) pause();
}
}
int main(int argc, char** argv) {
    // Spawn deliberately supplies an empty environment. Fixture settings are
    // local to this copied wrapper executable; no production env forwarding.
    std::ifstream config{std::string(argv[0]) + ".config"};
    std::string worker, record, mode;
    if (argc < 2 || !std::getline(config, worker) || !std::getline(config, record) ||
        !std::getline(config, mode) || worker.empty() || record.empty()) return 90;
    int ready[2];
    if (pipe(ready) != 0) return 93;
    Child child{getpid(), ready[1]};
    alignas(16) char stack[65536];
    pid_t pid;
    if (mode == "clone") {
        // No SIGCHLD termination signal: cleanup must handle Linux clone
        // children as well as ordinary fork children (__WALL).
        pid = clone(child_main, stack + sizeof(stack), 0, &child);
    } else if (mode == "fork") {
        pid = fork();
        if (pid == 0) _exit(child_main(&child));
    } else return 94;
    if (pid < 0) return 95;
    close(ready[1]);
    char value = 0;
    if (read(ready[0], &value, 1) != 1 || value != 'R') return 96;
    close(ready[0]);
    FILE* output = std::fopen(record.c_str(), "w");
    if (!output) return 97;
    std::fprintf(output, "%ld %ld\n", static_cast<long>(getpid()), static_cast<long>(pid));
    if (std::fclose(output) != 0) return 98;
    argv[0] = worker.data();
    execv(worker.c_str(), argv);
    return 99;
}
