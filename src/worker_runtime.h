#ifndef CAPSID_WORKER_RUNTIME_H
#define CAPSID_WORKER_RUNTIME_H

int capsid_run_worker(int ipc_fd, int network_namespace_fd);
struct CapsidWorkerIo;
int capsid_run_worker(int ipc_fd, int network_namespace_fd,
                      const CapsidWorkerIo* io);
// Shared launch path; only a test embedding supplies a custom I/O strategy.
int capsid_worker_main(int argc, char** argv, const CapsidWorkerIo* io);

#endif
