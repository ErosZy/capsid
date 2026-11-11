#ifndef CAPSID_RESTRICTED_WORKER_TU_MARKER_H
#define CAPSID_RESTRICTED_WORKER_TU_MARKER_H

/* Clang LTO does not preserve GCC's per-file symbol markers. Keep one
 * compiler-generated record for each extracted restricted-runtime object.
 * SHF_GNU_RETAIN keeps the record through linker section collection too. */
#if !defined(__ELF__) || !__has_attribute(retain)
#error "Restricted Clang LTO audit requires ELF retain attribute support"
#endif
#ifndef CAPSID_RESTRICTED_TU_NAME
#error "Restricted Clang LTO audit requires the compiled source path"
#endif
static const char capsid_restricted_compilation_unit[]
    __attribute__((used, retain, section(".capsid.restricted_tu"))) =
        "capsid-restricted-tu:" CAPSID_RESTRICTED_TU_NAME;

#endif
