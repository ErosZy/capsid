#include <windows.h>

#include <cstdio>
#include <string>

namespace {
std::wstring directory(HMODULE module) {
    wchar_t path[32768];
    const DWORD size = GetModuleFileNameW(module, path, 32768);
    if (size == 0 || size >= 32768) {
        return {};
    }
    std::wstring value(path, size);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : value.substr(0, slash);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    const auto expected = directory(nullptr);
    if (expected.empty()) {
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        HMODULE module = GetModuleHandleA(argv[i]);
        const auto actual = module == nullptr ? std::wstring() : directory(module);
        if (actual.empty() || CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1,
                                                  TRUE) != CSTR_EQUAL) {
            std::fprintf(stderr, "FAIL: %s was not loaded from the consumer directory\n", argv[i]);
            return 1;
        }
        std::printf("PASS: %s loaded beside the relocated consumer\n", argv[i]);
    }
    return 0;
}
