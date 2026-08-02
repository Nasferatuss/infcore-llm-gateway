// infcore runtime — MIT licence (see LICENSE).
#include "backend_supervisor.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "httplib.h"

namespace infcore {

namespace {
// Per-boot secret for the child llama-server --api-key (offline, from /dev/urandom).
std::string gen_token(const std::string& token_path) {
    std::ifstream ur(token_path, std::ios::binary);
    unsigned char buf[24];
    std::string tok;
    static const char* hx = "0123456789abcdef";
    if (ur.read(reinterpret_cast<char*>(buf), sizeof(buf))) {
        for (unsigned char c : buf) { tok.push_back(hx[c >> 4]); tok.push_back(hx[c & 0xF]); }
    }
    return tok;
}

void add_env(std::vector<std::string>& env, const char* name, const char* fallback = nullptr) {
    if (const char* v = std::getenv(name); v && *v) {
        env.emplace_back(std::string(name) + "=" + v);
    } else if (fallback) {
        env.emplace_back(std::string(name) + "=" + fallback);
    }
}

std::vector<std::string> backend_environment() {
    std::vector<std::string> env;
    add_env(env, "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    add_env(env, "LANG", "C.UTF-8");
    add_env(env, "LC_ALL");
    add_env(env, "LD_LIBRARY_PATH");
    add_env(env, "CUDA_VISIBLE_DEVICES");
    add_env(env, "NVIDIA_VISIBLE_DEVICES");
    add_env(env, "NVIDIA_DRIVER_CAPABILITIES");
    add_env(env, "VK_ICD_FILENAMES");
    add_env(env, "XDG_CACHE_HOME");
    add_env(env, "TMPDIR", "/tmp");
    return env;
}

std::string resolve_executable(const std::string& bin) {
    if (bin.find('/') != std::string::npos) return bin;
    const char* path = "/opt/infcore/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::string p = path;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + bin;
            if (access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::string();
}
}  // namespace

BackendSupervisor::BackendSupervisor(Options opt)
    : opt_(std::move(opt)), api_key_(gen_token(opt_.backend_token_path)), next_port_(opt_.port_range_start) {
    if (api_key_.empty())
        throw std::runtime_error("infcore: could not generate the internal backend API key");
    reaper_ = std::thread([this] { reaper_loop(); });
}

BackendSupervisor::~BackendSupervisor() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    reaper_cv_.notify_all();
    if (reaper_.joinable()) reaper_.join();

    std::unique_lock<std::mutex> lock(mu_);
    for (auto& kv : backends_)
        if (kv.second->state == State::Ready || kv.second->state == State::Starting)
            stop_backend(*kv.second, lock);
}

long long BackendSupervisor::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

BackendSupervisor::Backend& BackendSupervisor::get_or_create(const std::string& name) {
    auto it = backends_.find(name);
    if (it == backends_.end())
        it = backends_.emplace(name, std::make_unique<Backend>()).first;
    return *it->second;
}

int BackendSupervisor::loaded_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return loaded_count_locked();
}

int BackendSupervisor::loaded_count_locked() const {
    int n = 0;
    for (const auto& kv : backends_) {
        const State s = kv.second->state;
        if (s == State::Ready || s == State::Starting) ++n;
    }
    return n;
}

bool BackendSupervisor::wait_health(int port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(1, 0);
    cli.set_read_timeout(2, 0);
    httplib::Headers h;
    if (!api_key_.empty()) h.emplace("Authorization", "Bearer " + api_key_);
    const long long deadline = now_ms() + opt_.startup_timeout_ms;
    while (now_ms() < deadline) {
        auto r = cli.Get("/health", h);
        if (r && r->status == 200) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

// Managed backends always listen on loopback and require the per-boot --api-key: they
// cannot be reached directly, bypassing the gateway's RBAC and audit.
bool BackendSupervisor::spawn(const ModelEntry& e, int port, pid_t& out_pid, std::string& err) {
    if (opt_.llama_server_bin.empty()) {
        err = "runtime.llama_server_bin is not set";
        return false;
    }
    const std::string exec_path = resolve_executable(opt_.llama_server_bin);
    if (exec_path.empty()) {
        err = "runtime.llama_server_bin not found or not executable: " + opt_.llama_server_bin;
        return false;
    }

    std::vector<std::string> args = {
        exec_path,
        "--host", "127.0.0.1",
        "--port", std::to_string(port),
        "--model", e.gguf_path,
        "--ctx-size", std::to_string(e.n_ctx),
    };
    // n_gpu_layers < 0 means the flag is NOT passed: llama.cpp fits the offload to the
    // free VRAM itself. Passing it unconditionally is not an option - any explicit value
    // disables the auto-fit ("n_gpu_layers already set by user ... abort"), and for models
    // that do not fit into VRAM entirely a hand-picked offload loses noticeably to the
    // automatic one.
    if (e.n_gpu_layers >= 0) {
        args.push_back("--n-gpu-layers");
        args.push_back(std::to_string(e.n_gpu_layers));
    }
    if (!api_key_.empty()) { args.push_back("--api-key"); args.push_back(api_key_); }
    if (e.modality == Modality::Embedding) args.push_back("--embedding");
    if (e.modality == Modality::Rerank) args.push_back("--reranking");
    if (!e.mmproj_path.empty()) { args.push_back("--mmproj"); args.push_back(e.mmproj_path); }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_strings = backend_environment();
    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& evar : env_strings) envp.push_back(evar.data());
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { err = "fork() failed"; return false; }
    if (pid == 0) {
        // child: a new process group, so signals do not hit the gateway
        setpgid(0, 0);
        // do not inherit the gateway's descriptors (the listening socket, client connections,
        // the audit journal fd) - otherwise the child could hold the port open or undermine
        // the append-only guarantee of the log
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 3 || maxfd > 4096) maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; ++fd) ::close(fd);
        execve(argv[0], argv.data(), envp.data());
        std::fprintf(stderr, "infcore: execve llama-server failed: %s\n", std::strerror(errno));
        _exit(127);
    }

    out_pid = pid;
    return true;
}

void BackendSupervisor::stop_backend(Backend& b, std::unique_lock<std::mutex>& lock) {
    const pid_t pid = b.pid;
    b.url.clear();
    if (pid <= 0) {                 // no process — nothing to wait for
        b.pid = -1;
        b.port = 0;
        b.state = State::Stopped;
        return;
    }
    // Mark it Stopping and release mu_: SIGTERM->SIGKILL can take up to 5 s, and the shared
    // lock must not be held for that long (every other model would stall). The Backend&
    // stays valid — entries in backends_ are never erased. The port is reset so that a new
    // start of this model takes a fresh port and does not collide with the still-dying one.
    b.state = State::Stopping;
    lock.unlock();

    kill(pid, SIGTERM);
    const long long deadline = now_ms() + 5000;
    bool reaped = false;
    while (now_ms() < deadline) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || r < 0) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }

    lock.lock();
    b.pid = -1;
    b.port = 0;
    b.state = State::Stopped;
    b.cv.notify_all();              // wake the ensure_ready callers waiting out the Stopping state
}

std::string BackendSupervisor::ensure_ready(const ModelEntry& e, std::string& err) {
    std::unique_lock<std::mutex> lock(mu_);
    Backend& b = get_or_create(e.logical_name);

    for (;;) {
        switch (b.state) {
            case State::Ready:
                b.last_used_ms = now_ms();
                return b.url;
            case State::Failed:
                // stay Failed until the backoff expires, then allow another attempt
                if (now_ms() < b.retry_after_ms) { err = b.last_error; return std::string(); }
                b.state = State::Stopped;
                continue;
            case State::Starting:
            case State::Stopping:
                b.cv.wait(lock);   // wait for this model's start-up/shutdown to finish
                continue;
            case State::Stopped: {
                if (now_ms() < b.retry_after_ms) { err = b.last_error; return std::string(); }
                if (opt_.max_loaded_models > 0 && loaded_count_locked() >= opt_.max_loaded_models) {
                    err = "capacity exceeded: max_loaded_models";
                    return std::string();
                }
                if (opt_.max_parallel_starts > 0 && starts_in_flight_ >= opt_.max_parallel_starts) {
                    err = "capacity exceeded: max_parallel_starts";
                    return std::string();
                }
                if (b.port == 0) {                       // the port is assigned under mu_ (no race)
                    b.port = next_port_++;
                    if (next_port_ > opt_.port_range_start + 1000) next_port_ = opt_.port_range_start;
                }
                b.stop_requested = false;                // a new start cancels a pending stop request
                b.state = State::Starting;
                ++starts_in_flight_;
                const int port = b.port;
                std::string serr;
                lock.unlock();  // fork + /health polling without blocking the other models
                pid_t pid = -1;
                bool ok = spawn(e, port, pid, serr);
                if (ok) ok = wait_health(port);
                lock.lock();
                if (starts_in_flight_ > 0) --starts_in_flight_;
                b.pid = pid;  // assigned under mu_ (the reaper reads pid under mu_)
                if (ok && b.stop_requested) {
                    // The disable arrived while the backend was starting (pid was -1 then, so
                    // there was nothing to stop). Now that pid is known, stop it cleanly and
                    // fail the caller (the model is disabled). Deterministic, no race with the
                    // reaper.
                    stop_backend(b, lock);        // -> Stopped, pid=-1, port=0
                    b.stop_requested = false;
                    b.cv.notify_all();
                    err = "model was disabled while starting";
                    return std::string();
                }
                if (ok) {
                    b.url = "http://127.0.0.1:" + std::to_string(port);
                    b.state = State::Ready;
                    b.last_used_ms = now_ms();
                    b.fail_count = 0;
                    b.retry_after_ms = 0;
                } else {
                    if (serr.empty()) serr = "backend did not pass health-check within startup_timeout_ms";
                    b.last_error = serr;
                    stop_backend(b, lock);   // resets pid/port, sets Stopped (mu_ is released during the kill)
                    b.fail_count++;
                    int backoff = 5000 * b.fail_count;
                    if (backoff > 60000) backoff = 60000;
                    b.retry_after_ms = now_ms() + backoff;
                    b.state = State::Failed;
                }
                b.cv.notify_all();
                continue;
            }
        }
    }
}

void BackendSupervisor::acquire(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    Backend& b = get_or_create(name);
    b.active++;
    b.last_used_ms = now_ms();
}

void BackendSupervisor::release(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = backends_.find(name);
    if (it == backends_.end()) return;
    Backend& b = *it->second;
    if (b.active > 0) b.active--;
    b.last_used_ms = now_ms();
    if (b.active == 0 && b.stop_requested) reaper_cv_.notify_all();  // deferred stop — wake the reaper
}

void BackendSupervisor::stop(const std::string& logical_name) {
    std::unique_lock<std::mutex> lock(mu_);
    auto it = backends_.find(logical_name);
    if (it == backends_.end()) return;
    Backend& b = *it->second;
    // Stopping: a shutdown is already running on another thread — no need to wait.
    if (b.state == State::Stopped || b.state == State::Failed || b.state == State::Stopping) {
        b.stop_requested = false; return;
    }
    // Starting: the process is still coming up and b.pid is still -1 (it is assigned only
    // after the health check, under mu_). There is nothing to kill yet, so the stop is
    // deferred: ensure_ready will see stop_requested and will not let the backend linger
    // (the reaper/release path stops it as soon as the request that triggered the start
    // finishes). Without this, a disable issued during start-up was lost and the
    // llama-server lived on until the idle timeout.
    if (b.state == State::Starting) { b.stop_requested = true; return; }
    if (b.active == 0) {
        stop_backend(b, lock);    // nothing to interrupt — stop it right away
        b.stop_requested = false;
    } else {
        b.stop_requested = true;  // the reaper stops it as soon as the in-flight requests finish
    }
}

void BackendSupervisor::reaper_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stop_) {
        reaper_cv_.wait_for(lock, std::chrono::seconds(5));
        if (stop_) break;
        const long long t = now_ms();
        for (auto& kv : backends_) {
            Backend& b = *kv.second;
            if (b.state != State::Ready) continue;
            // crashed process: waitpid reaps the zombie and reliably tells a live process from a
            // dead one (kill(pid,0) succeeds for a zombie and would not detect the crash)
            if (b.pid > 0) {
                int st = 0;
                pid_t r = waitpid(b.pid, &st, WNOHANG);
                if (r != 0) {  // r==pid: exited and reaped; r<0: the process is already gone
                    b.pid = -1;
                    b.url.clear();
                    b.state = State::Stopped;
                    continue;
                }
            }
            if (b.active == 0 &&
                (b.stop_requested || t - b.last_used_ms > opt_.idle_timeout_ms)) {
                stop_backend(b, lock);   // releases mu_ during the kill (other models do not wait)
                b.stop_requested = false;
            }
        }
    }
}

}  // namespace infcore
