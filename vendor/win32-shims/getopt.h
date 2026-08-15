/*
 * getopt.h — POSIX getopt declarations for MSVC builds of tjsc
 * (vendor/txiki.js/src/qjsc.c uses optarg/optind without a portability
 * layer; upstream only ever compiles it on POSIX hosts).
 *
 * Implemented by getopt.c in this directory; the tjsc target force-
 * includes this header on WIN32 (cmake/build_worker.cmake). Public
 * domain, written for Capsid — no upstream.
 */
#ifndef CAPSID_WIN32_SHIMS_GETOPT_H
#define CAPSID_WIN32_SHIMS_GETOPT_H

#ifdef __cplusplus
extern "C" {
#endif

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

#ifdef __cplusplus
}
#endif

#endif  /* CAPSID_WIN32_SHIMS_GETOPT_H */
