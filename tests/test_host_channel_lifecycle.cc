#include "host/generation_pool.h"
#include "host/protocol_launch.h"
#include "host/channel_ledger.h"
#include "host/metrics.h"
#include "helpers/host_ws_peer.h"
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#ifdef __linux__
#include <filesystem>
#include <signal.h>
#include <unistd.h>
#endif

using namespace capsid_test;
using namespace capsid::host;
namespace {
struct Responses {
    std::mutex mutex;
    std::map<uint64_t, std::string> bodies;
    std::set<uint64_t> completed;
    std::atomic<bool> expect_exit{false};
    void event(WorkerEvent event) {
        std::lock_guard<std::mutex> lock(mutex);
        require((event.type != WorkerEvent::Type::kError || expect_exit) && event.type != WorkerEvent::Type::kRequestFailure && event.type != WorkerEvent::Type::kRequestTimeout,
                "pool worker remains healthy");
        if (event.type == WorkerEvent::Type::kResponseHead) require(event.status == 200, "HTTP response succeeds");
        if (event.type == WorkerEvent::Type::kResponseBody) bodies[event.request_id].append(event.body.begin(), event.body.end());
        if (event.type == WorkerEvent::Type::kResponseEnd) completed.insert(event.request_id);
    }
    bool done(uint64_t id, const std::string &body) {
        std::lock_guard<std::mutex> lock(mutex); return completed.count(id) && bodies[id] == body;
    }
};
GenerationPoolOptions options(const char *worker_path, uint16_t port, uint16_t pending_port, char digest, Responses &responses) {
    GenerationPoolOptions result; result.application_id = "ws-lifecycle"; result.version = "v1";
    result.generation_digest = "sha256:" + std::string(64, digest);
    result.drain_deadline = std::chrono::seconds(3);
    result.recovery.backoff_initial_ms = 5; result.recovery.backoff_maximum_ms = 20;
    result.recovery.max_events = 3; result.recovery.window_ms = 60000;
    result.recovery.stable_reset_ms = 60000; result.recovery.replacements_concurrent_per_app = 1;
    result.event_sink = [&](const WorkerExecutor *, WorkerEvent event) { responses.event(std::move(event)); };
    const std::string source =
        "let ws; const pendingSockets=[]; export default {async fetch(request) {"
        "if(new URL(request.url).pathname !== '/start') return new Response('healthy');"
        "ws = new WebSocket('ws://127.0.0.1:" + std::to_string(port) + "/');"
        "ws.onmessage = event => {"
        + (pending_port ? "if(event.data==='pending') {pendingSockets.push(new WebSocket('ws://127.0.0.1:" + std::to_string(pending_port) + "/'));return;}" : "") +
        "ws.send('reply:' + event.data);};"
        "if(!await new Promise(resolve=>{ws.onopen=()=>resolve(true);ws.onerror=()=>resolve(false);})) return new Response('denied');"
        "return new Response('started');}}";
    result.factory = [worker_path, source, port, pending_port](capsid_worker **out, std::string *error) {
        capsid_egress_rule rules[2];
        for (auto &rule : rules) {
            capsid_egress_rule_init(&rule); rule.action = CAPSID_EGRESS_ALLOW; rule.target = "127.0.0.1";
        }
        rules[0].port_start = rules[0].port_end = port;
        rules[1].port_start = rules[1].port_end = pending_port;
        capsid_egress_policy policy; capsid_egress_policy_init(&policy); policy.rules = rules; policy.rule_count = pending_port ? 2 : 1;
        capsid_worker_config config; capsid_worker_config_init(&config); config.worker_path = worker_path;
        config.egress_policy = &policy; config.request_timeout_ms = 5000;
        ProtocolLaunch launch; launch.options.close_timeout_ms = 300;
        capsid_worker *worker = nullptr;
        if (spawn_with_protocol(&config, launch, &worker) != CAPSID_OK) { *error = "spawn failed"; return false; }
        if (capsid_worker_load_bundle(worker, reinterpret_cast<const uint8_t *>(source.data()), source.size()) != CAPSID_OK ||
            capsid_worker_flush(worker) != CAPSID_OK) {
            capsid_worker_destroy(worker); *error = "load failed"; return false;
        }
        *out = worker; return true;
    };
    return result;
}
void begin(WorkerExecutor *worker, uint64_t id, const char *path) {
    require(worker && !worker->exited(), "request admits to READY executor");
    Command command; command.type = CommandType::kBeginRequest; command.request_id = id;
    command.method = "GET"; command.url = std::string("https://app.test") + path; command.end_request = true;
    worker->submit(std::move(command));
    Command grant; grant.type = CommandType::kGrantResponseCredit; grant.request_id = id; grant.credit = 4096;
    worker->submit(std::move(grant));
}
void send_frame(Peer &peer, uint8_t opcode, const std::string &text) {
    require(text.size() < 126, "small independent test frame");
    std::vector<uint8_t> bytes{uint8_t(0x80 | opcode), uint8_t(text.size())}; bytes.insert(bytes.end(), text.begin(), text.end());
    boost::system::error_code error; asio::write(*peer.socket, asio::buffer(bytes), error); require(!error, "server frame written");
}
void echo(Peer &peer) {
    send_frame(peer, 1, "probe"); std::vector<uint8_t> bytes;
    require(wait_until([&] {
        uint8_t chunk[1024]; boost::system::error_code error; const auto size = peer.socket->read_some(asio::buffer(chunk), error);
        if (size) bytes.insert(bytes.end(), chunk, chunk+size);
        require(!error || error == asio::error::would_block || error == asio::error::try_again, "live channel readable during retire");
        return bytes.size() >= 6 && bytes.size() >= 6 + size_t(bytes[1] & 127);
    }), "retiring generation still handles connection event");
    require(bytes[0] == 0x81 && (bytes[1] & 0x80), "outbound client frame remains masked");
    std::string text; for (size_t i = 6; i < bytes.size(); ++i) text.push_back(char(bytes[i] ^ bytes[2+(i-6)%4]));
    require(text == "reply:probe", "live channel retains correct generation and listener");
}
std::shared_ptr<GenerationPool> start(const char *worker, Peer &peer, Responses &responses, bool pending = false,
                                    std::chrono::milliseconds deadline = std::chrono::seconds(3), Peer *trigger = nullptr) {
    require(!pending || trigger, "a connection event owns a pending lifecycle dial");
    auto settings = options(worker, pending ? trigger->port() : peer.port(), pending ? peer.port() : 0, 'a', responses);
    settings.drain_deadline = deadline;
    std::string error; auto pool = GenerationPool::create(std::move(settings), &error); require(bool(pool), error);
    auto executor = pool->pick_worker(); begin(executor, 1, "/start");
    (pending ? *trigger : peer).upgrade();
    require(wait_until([&] { return responses.done(1, "started") && executor->inflight() == 0 && executor->channels() == 1; }),
            "standalone connection pins executor after HTTP owner completes");
    if (pending) {
        // The HTTP owner has ended. Start this held dial from a live
        // connection callback so the fixture observes a valid pending owner.
        send_frame(*trigger, 1, "pending");
        require(wait_until([&] { return peer.read_head(); }), "connection-origin pending handshake remains unacknowledged");
        require(wait_until([&] { return executor->inflight() == 0 && executor->channels() == 2 && default_channel_budget()->dials() == 1; }),
                "HTTP-idle live connection and pending dial both pin the generation");
    }
    return pool;
}
void budget_returned(size_t channels, size_t dials, size_t bytes) {
    auto budget = default_channel_budget();
    require(wait_until([&] { return budget->channels() == channels && budget->dials() == dials && budget->queued_bytes() == bytes; }),
            "generation sockets/dials/buffers return before next generation");
}
void retire_live(const char *worker) {
    Peer peer, new_peer; Responses responses, new_responses;
    const auto budget = default_channel_budget(); const auto channels = budget->channels(), dials = budget->dials(), bytes = budget->queued_bytes();
    auto old = start(worker, peer, responses);
    auto next_options = options(worker, new_peer.port(), false, 'b', new_responses);
    std::string error; auto next = GenerationPool::create(std::move(next_options), &error); require(bool(next), error);
    const auto before = Clock::now(); old->request_drain();
    require(old->pick_worker() == nullptr, "retired generation rejects new admissions");
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    require(old->state() == GenerationPool::State::kDraining, "HTTP-idle live channel prevents premature retirement");
    begin(next->pick_worker(), 2, "/hello"); require(wait_until([&] { return new_responses.done(2, "healthy"); }), "replacement generation serves new HTTP requests");
    echo(peer); send_frame(peer, 8, std::string("\x03\xe8", 2));
    require(wait_until([&] { return old->state() == GenerationPool::State::kDead; }), "ordinary WS close releases draining generation");
    old->wait(&error); require(Clock::now() - before < std::chrono::seconds(2), "graceful channel drain beats forced deadline");
    require(wait_until([&] { return peer.closed(); }), "retired socket closed");
    next->stop_and_join(); budget_returned(channels, dials, bytes);
    std::puts("live channel survives generation replacement; ordinary close completes retirement PASS");
}
void retire_deadline(const char *worker, bool pending) {
    Peer peer, trigger; Responses responses;
    const auto budget = default_channel_budget(); const auto channels = budget->channels(), dials = budget->dials(), bytes = budget->queued_bytes();
    auto pool = start(worker, peer, responses, pending, std::chrono::milliseconds(350), pending ? &trigger : nullptr);
    const auto before = Clock::now(); pool->request_drain();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(pool->state() == GenerationPool::State::kDraining, "pending/live channel pins until drain deadline");
    std::string error; pool->wait(&error);
    require(Clock::now() - before < std::chrono::seconds(3), "forced channel drain and network join bounded");
    require(wait_until([&] { return peer.closed(); }), "deadline terminates pending/live socket"); budget_returned(channels, dials, bytes);
    if (pending) require(wait_until([&] { return trigger.closed(); }), "deadline also terminates the triggering connection");
    std::printf("generation drain deadline, pending=%d, bounded teardown PASS\n", int(pending));
}
void shared_service_budget(const char *worker) {
    Peer a,b; Responses ra,rb; MetricsRegistry metrics;
    auto budget=std::make_shared<ServiceChannelBudget>(ChannelBudgetLimits{1,1,65536});
    metrics.observe_channel_budget(budget);
    auto oa=options(worker,a.port(),false,'a',ra),ob=options(worker,b.port(),false,'b',rb);
    oa.channel_budget=ob.channel_budget=budget;
    std::string error;auto pa=GenerationPool::create(std::move(oa),&error);require(bool(pa),error);
    auto pb=GenerationPool::create(std::move(ob),&error);require(bool(pb),error);
    begin(pa->pick_worker(),1,"/start");a.upgrade();
    require(wait_until([&] { return ra.done(1,"started") && budget->snapshot().active==1; }),"first pool's accepted standalone channel charged to service");
    begin(pb->pick_worker(),2,"/start");
    require(wait_until([&] { return rb.done(2,"denied"); }),"other generation cannot escape common channel ceiling");
    require(!b.accept(),"channel capacity rejection happens before TCP socket connection");
    require(budget->channels()==1 && budget->dials()==0 && budget->snapshot().channels.rejected==1,"failed dial rolls back temporary dial quota but keeps first actor");
    echo(a);send_frame(a,8,std::string("\x03\xe8",2));
    require(wait_until([&] { return budget->channels()==0 && budget->snapshot().active==0; }),"ordinary close returns shared channel capacity");
    begin(pb->pick_worker(),3,"/start");b.upgrade();
    require(wait_until([&] { return rb.done(3,"started") && budget->snapshot().active==1; }),"other generation reuses returned service capacity");
    echo(b);
    auto previous=pb->current_worker();rb.expect_exit=true;require(pb->recycle_worker(previous),"replacement exercises shared budget inheritance");
    require(wait_until([&] { return previous->exited() && pb->ready_workers()==1 && pb->current_worker()!=previous && budget->channels()==0; }),"recycle releases old service slot before replacement READY");
    rb.expect_exit=false;a.socket.reset();a.head.clear();
    begin(pa->pick_worker(),4,"/start");a.upgrade();require(wait_until([&] { return ra.done(4,"started"); }),"first generation holds service slot again");
    b.socket.reset();b.head.clear();begin(pb->pick_worker(),5,"/start");
    require(wait_until([&] { return rb.done(5,"denied"); }),"replacement executor retains common service parent");
    require(!b.accept() && budget->snapshot().channels.rejected>=2,"replacement never connects around configured quota");
    send_frame(a,8,std::string("\x03\xe8",2));pa->stop_and_join();pb->stop_and_join();
    require(budget->channels()==0 && budget->dials()==0 && budget->queued_bytes()==0 && budget->snapshot().active==0,"both generations return all shared service resources");
    const auto text=metrics.render_prometheus_text();
    require(text.find("capsid_channel_reservations{scope=\"service\",listener=\"\"} 0\n")!=std::string::npos,"actual network cleanup visible in Prometheus registry");
    std::puts("shared service channel quota across generations and replacement, pre-connect refusal and metrics PASS");
}
#ifdef __linux__
pid_t child_worker() {
    for (const auto &entry : std::filesystem::directory_iterator("/proc")) {
        const auto name = entry.path().filename().string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
        std::ifstream file(entry.path()/"stat"); std::string line; std::getline(file, line);
        auto end = line.rfind(')'); if (end == std::string::npos) continue;
        std::istringstream stat(line.substr(end+2)); char state; pid_t parent; stat >> state >> parent;
        if (stat && parent == getpid()) return static_cast<pid_t>(std::stoi(name));
    }
    return -1;
}
void replace_live(const char *worker, bool crash) {
    Peer peer; Responses responses;
    const auto budget = default_channel_budget(); const auto channels = budget->channels(), dials = budget->dials(), bytes = budget->queued_bytes();
    auto pool = start(worker, peer, responses); auto previous = pool->current_worker();
    responses.expect_exit = true;
    if (crash) { const auto pid = child_worker(); require(pid > 0 && kill(pid, SIGKILL) == 0, "kill worker with live socket"); }
    else require(pool->recycle_worker(previous), "health recycle with live socket accepted");
    require(wait_until([&] { return peer.closed(); }), "old executor closes actual socket on crash/recycle");
    require(wait_until([&] { return previous->exited() && pool->ready_workers() == 1 && pool->current_worker() != previous; }, std::chrono::seconds(10)),
            "same generation replacement reaches READY");
    budget_returned(channels, dials, bytes);
    responses.expect_exit = false;
    begin(pool->pick_worker(), 2, "/hello"); require(wait_until([&] { return responses.done(2, "healthy"); }), "replacement serves healthy HTTP");
    peer.socket.reset(); peer.head.clear();
    auto replacement = pool->pick_worker(); begin(replacement, 3, "/start"); peer.upgrade();
    require(wait_until([&] { return responses.done(3, "started") && replacement->channels() == 1; }), "replacement creates fresh standalone connection");
    echo(peer); pool->stop_and_join(); budget_returned(channels, dials, bytes);
    std::printf("worker replacement, crash=%d, old socket reclaimed and new connection healthy PASS\n", int(crash));
}
#endif
}
int main(int argc, char **argv) {
    require(argc == 2, "worker path required");
    shared_service_budget(argv[1]);
    retire_live(argv[1]); retire_deadline(argv[1], false); retire_deadline(argv[1], true);
#ifdef __linux__
    replace_live(argv[1], true); replace_live(argv[1], false);
#endif
}
