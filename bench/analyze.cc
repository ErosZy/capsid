// Static ceiling analyzer for the exec-throughput benchmark (Step 8).
// Wraps the optimizer's analyze_only mode: parses a .qjsb bundle and
// reports per-function foldability statistics to stderr (never stdout).
// This is the reproducible re-run of the Step 0 ceiling measurement on
// the committed fixture set.
//
// Usage: analyze [--regions] <in.qjsb>
#include "bytecode_optimizer/bytecode_optimizer.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const bool regions = argc == 3 && std::string(argv[1]) == "--regions";
    if ((!regions && argc != 2) || (regions && argc != 3)) {
        std::fprintf(stderr, "usage: %s [--regions] <in.qjsb>\n", argv[0]);
        return 2;
    }
    const char* input = argv[regions ? 2 : 1];
    FILE* f = std::fopen(input, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open: %s\n", input);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> buf(static_cast<size_t>(size));
    if (size > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fprintf(stderr, "read failed: %s\n", argv[1]);
        return 2;
    }
    std::fclose(f);
    std::string error;
    const bool ok = regions ? capsid::bytecode::region_census(buf, &error)
                            : capsid::bytecode::analyze_only(buf, &error);
    if (!ok) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
