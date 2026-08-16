/*
 * getopt.c — minimal POSIX getopt for MSVC builds of tjsc.
 *
 * Supports the option surface qjsc.c actually uses (a string with ':'
 * value options and bare flags; "--" and clustered flags). It is NOT a
 * full glibc-compatible getopt: unknown options and missing values
 * return '?' with optopt set, and the first non-option argument stops
 * the scan, which is exactly the qjsc contract.
 */
#include "getopt.h"

#include <string.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

int getopt(int argc, char *const argv[], const char *optstring) {
    static int position = 0;  /* offset into the current argument */

    if (optind >= argc) {
        return -1;
    }
    const char *current = argv[optind];
    if (position == 0) {
        /* "--" or a non-option argument ends the scan. */
        if (current[0] != '-' || current[1] == '\0' ||
            current[1] == '-') {
            return -1;
        }
        position = 1;
    }
    const char option = current[position];
    if (option == '\0') {
        position = 0;
        ++optind;
        return getopt(argc, argv, optstring);
    }
    const char *match = strchr(optstring, option);
    if (match == NULL) {
        optopt = option;
        ++position;
        if (current[position] == '\0') {
            position = 0;
            ++optind;
        }
        return '?';
    }
    if (match[1] == ':') {
        /* Value option: rest of the current token, or the next token. */
        if (current[position + 1] != '\0') {
            optarg = (char *)&current[position + 1];
            position = 0;
            ++optind;
        } else {
            ++optind;
            if (optind >= argc) {
                optarg = NULL;
                optopt = option;
                position = 0;
                return '?';
            }
            optarg = (char *)argv[optind];
            position = 0;
            ++optind;
        }
    } else {
        ++position;
        if (current[position] == '\0') {
            position = 0;
            ++optind;
        }
    }
    return option;
}
