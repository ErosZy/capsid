#include "capsid/runtime.h"
#include "worker_exit_status.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Clock=std::chrono::steady_clock;
unsigned remaining=0,injected=0,blocking_calls=0;
bool active=false,killed=false;
Clock::time_point killed_at;
unsigned delay_ms=0;
}
extern "C" pid_t __real_waitpid(pid_t,int*,int);
extern "C" int __real_kill(pid_t,int);
extern "C" int __wrap_kill(pid_t pid,int number) {
    const int result=__real_kill(pid,number);
    if(active && number==SIGKILL && result==0){killed=true;killed_at=Clock::now();}
    return result;
}
extern "C" pid_t __wrap_waitpid(pid_t pid,int* status,int options) {
    if(active && killed) {
        if(options&WNOHANG) {
            if(Clock::now()-killed_at<std::chrono::milliseconds(delay_ms))return 0;
        } else {
            ++blocking_calls;
            if(remaining){--remaining;++injected;errno=EINTR;return -1;}
        }
    }
    return __real_waitpid(pid,status,options);
}
struct Case{unsigned delay,interruptions;};
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    bool passed=true;
    for(const Case sample: {Case{0,0},Case{350,0},Case{350,1},Case{350,3}}) {
        capsid_worker_config config;capsid_worker_config_init(&config);config.worker_path=argv[1];
        capsid_worker* worker=nullptr;
        if(capsid_worker_spawn(&config,&worker)!=CAPSID_OK || !worker)return 2;
        const int64_t raw=capsid_worker_pid(worker);
        if(raw<=0 || raw==getpid() || static_cast<int64_t>(static_cast<pid_t>(raw))!=raw){capsid_worker_destroy(worker);return 2;}
        const pid_t pid=static_cast<pid_t>(raw);
        if(__real_kill(pid,SIGSTOP))return 2;
        siginfo_t stopped{};int result;
        do{result=waitid(P_PID,static_cast<id_t>(pid),&stopped,WSTOPPED|WNOWAIT);}while(result<0&&errno==EINTR);
        if(result || stopped.si_pid!=pid || stopped.si_code!=CLD_STOPPED)return 2;
        delay_ms=sample.delay;remaining=sample.interruptions;injected=0;blocking_calls=0;killed=false;active=true;
        const auto status=capsid::destroy_worker_with_status(worker);active=false;
        siginfo_t info{};
        do{errno=0;result=waitid(P_PID,static_cast<id_t>(pid),&info,WEXITED|WNOHANG|WNOWAIT);}while(result<0&&errno==EINTR);
        const bool reaped=result<0&&errno==ECHILD;
        const bool exact=killed && remaining==0 && injected==sample.interruptions &&
            (!sample.delay || blocking_calls==sample.interruptions+1);
        const bool terminal=status.observed && status.destroy_forced && status.signal==SIGKILL && status.wait_error==0 && !status.clean();
        if(!reaped){int code=0;pid_t waited;do{waited=__real_waitpid(pid,&code,0);}while(waited<0&&errno==EINTR);if(waited!=pid)return 2;}
        std::printf("REAP_CONTROL delay_ms=%u injected=%u observed=%u blocking_calls=%u reaped_before_fixture_cleanup=%d exact=%d terminal=%d\n",
            sample.delay,sample.interruptions,injected,blocking_calls,reaped?1:0,exact?1:0,terminal?1:0);
        if(!reaped||!exact||!terminal)passed=false;
    }
    if(!passed)std::fprintf(stderr,"FAIL: actual destroy must observe and reap forced exit before returning, including interrupted waits\n");
    return passed?0:1;
}
