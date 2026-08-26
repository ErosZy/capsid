// Static ceiling analyzer for the exec-throughput benchmark (Step 8).
// Wraps the rewriter's analyze_only mode: parses a .qjsb bundle and
// reports per-function foldability statistics to stderr (never stdout).
// This is the reproducible re-run of the Step 0 ceiling measurement on
// the committed fixture set.
//
// Usage: analyze [--regions | --regions-profile sites.tsv |
//                 --regions-profile-classic sites.tsv] <in.qjsb>
#include "bytecode_rewriter/bytecode_rewriter.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const bool regions = argc == 3 && std::string(argv[1]) == "--regions";
    const bool profiled =
        argc == 4 && std::string(argv[1]) == "--regions-profile";
    const bool profiled_classic =
        argc == 4 && std::string(argv[1]) == "--regions-profile-classic";
    if ((!regions && !profiled && !profiled_classic && argc != 2) ||
        (regions && argc != 3) ||
        ((profiled || profiled_classic) && argc != 4)) {
        std::fprintf(stderr,
                     "usage: %s [--regions | --regions-profile sites.tsv | "
                     "--regions-profile-classic sites.tsv] "
                     "<in.qjsb>\n", argv[0]);
        return 2;
    }
    const char* input = argv[(profiled || profiled_classic) ? 3
                                                            : (regions ? 2 : 1)];
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
        std::fprintf(stderr, "read failed: %s\n", input);
        return 2;
    }
    std::fclose(f);
    std::string error;
    bool ok = false;
    if (profiled || profiled_classic) {
        std::ifstream stream(argv[2]);
        if (!stream) {
            std::fprintf(stderr, "cannot open profile TSV: %s\n", argv[2]);
            return 2;
        }
        std::vector<capsid::bytecode::RegionProfileSite> sites;
        std::string line;
        unsigned line_number = 0;
        while (std::getline(stream, line)) {
            line_number++;
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string hash;
            capsid::bytecode::RegionProfileSite site = {};
            if (!(fields >> hash >> site.code_len >> site.line >> site.column >>
                  site.pc >> site.executions)) {
                std::fprintf(stderr, "malformed profile TSV line %u\n",
                             line_number);
                return 2;
            }
            try {
                size_t used = 0;
                site.code_hash = std::stoull(hash, &used, 16);
                if (used != hash.size()) throw std::invalid_argument("hash");
            } catch (const std::exception&) {
                std::fprintf(stderr, "invalid profile hash on line %u\n",
                             line_number);
                return 2;
            }
            sites.push_back(site);
        }
        ok = profiled_classic
                 ? capsid::bytecode::region_census_profiled_classic(
                       buf, sites, &error)
                 : capsid::bytecode::region_census_profiled(
                       buf, sites, &error);
    } else {
        ok = regions ? capsid::bytecode::region_census(buf, &error)
                     : capsid::bytecode::analyze_only(buf, &error);
    }
    if (!ok) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    return 0;
}
