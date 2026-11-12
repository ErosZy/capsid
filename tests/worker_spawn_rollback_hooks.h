#ifndef CAPSID_TEST_WORKER_SPAWN_ROLLBACK_HOOKS_H
#define CAPSID_TEST_WORKER_SPAWN_ROLLBACK_HOOKS_H

#include "win32_compat.h"
#if defined(_WIN32)
extern "C" BOOL WINAPI capsid_test_rollback_create(
    LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
extern "C" BOOL WINAPI capsid_test_rollback_terminate(HANDLE, UINT);
#if defined(CAPSID_TEST_SPAWN_ROLLBACK_RENAME)
#define CreateProcessA capsid_test_rollback_create
#define TerminateProcess capsid_test_rollback_terminate
#endif
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern "C" int capsid_test_rollback_spawn(
    pid_t*, const char*, const posix_spawn_file_actions_t*,
    const posix_spawnattr_t*, char* const[], char* const[]);
extern "C" int capsid_test_rollback_kill(pid_t, int);
extern "C" pid_t capsid_test_rollback_wait(pid_t, int*, int);
#if defined(CAPSID_TEST_SPAWN_ROLLBACK_RENAME)
#define posix_spawn capsid_test_rollback_spawn
#define kill capsid_test_rollback_kill
#define waitpid capsid_test_rollback_wait
#endif
#endif
#endif
