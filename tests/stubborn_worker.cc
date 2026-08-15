#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <signal.h>
#include <unistd.h>
#endif

// A worker that never answers and ignores termination: lifecycle tests
// use it to force the Host's bounded terminate/backstop paths.
int main() {
#if defined(_WIN32)
    for (;;) {
        Sleep(60000);
    }
#else
    signal(SIGTERM, SIG_IGN);
    for (;;) {
        pause();
    }
#endif
}
