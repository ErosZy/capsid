// capsid-bytecode-compile: the first-party QuickJS bytecode compiler front
// end.
//
// M0.2 exposes the identity surface only: --print-compatibility-id emits the
// exact 71-byte compatibility ID from the single generated identity source
// (build_identity.h). The real source -> bytecode compile round-trip is
// implemented in M1; until then every other invocation fails closed.

#include <cstdio>
#include <cstring>

#include "build_identity.h"

int main(int argc, char **argv) {
    if (argc == 2 &&
        std::strcmp(argv[1], "--print-compatibility-id") == 0) {
        std::fputs(CAPSID_BUILD_COMPATIBILITY_ID, stdout);
        std::fputc('\n', stdout);
        return 0;
    }
    std::fputs("capsid-bytecode-compile: M0.2 supports only "
               "--print-compatibility-id; the compile round-trip lands in "
               "M1\n",
               stderr);
    return 2;
}
