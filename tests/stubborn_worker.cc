#include <signal.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

int main() {
    signal(SIGTERM, SIG_IGN);
    for (;;) {
        pause();
    }
}
