// infcore runtime — MIT licence (see LICENSE).
// A lazy backend supervisor: it starts child llama-server processes on demand for managed
// models (those with an empty backend_url) and shuts them down when idle. Models with an
// explicit backend_url are treated as external and are not managed here.
#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "registry/model_registry.h"

namespace infcore {

class BackendSupervisor {
public:
    struct Options {
        std::string llama_server_bin;          // path to our llama-server (from the build)
        int port_range_start  = 8100;          // where local ports are handed out from
        int idle_timeout_ms   = 300000;        // idle time before unloading
        int startup_timeout_ms = 120000;       // how long to wait for /health at start-up
        std::string backend_token_path = "/dev/urandom";
        int max_loaded_models = 0;             // 0 = unlimited
        int max_parallel_starts = 1;           // admission control for expensive loads
    };

    explicit BackendSupervisor(Options opt);
    ~BackendSupervisor();

    BackendSupervisor(const BackendSupervisor&) = delete;
    BackendSupervisor& operator=(const BackendSupervisor&) = delete;

    // Guarantees that the managed backend for this model is up and healthy.
    // Returns the backend's base URL, or an empty string (with err filled in).
    // Blocks for the duration of the start-up; concurrent calls for the same model wait on
    // a single start.
    std::string ensure_ready(const ModelEntry& e, std::string& err);

    // The per-boot key the child llama-server processes are started with (--api-key).
    // The proxy adds it to the Authorization header of requests to managed backends.
    const std::string& api_key() const { return api_key_; }

    // In-flight request accounting: the reaper never stops a backend with active > 0
    // (doing so would cut a stream off mid-flight).
    void acquire(const std::string& logical_name);
    void release(const std::string& logical_name);

    // Immediately stop a model's managed backend (e.g. on disable via /admin).
    // If there are in-flight requests it is stopped as soon as they finish (streams are
    // never cut). A no-op for external or not-yet-started models.
    void stop(const std::string& logical_name);

    // How many backends are currently up (Ready/Starting) - for the /metrics gauge.
    int loaded_count() const;

private:
    // Stopping: the process is sent SIGTERM and reaped WITHOUT holding mu_ (otherwise the
    // five-second wait would block every other model). Concurrent ensure_ready calls for
    // the same model wait on b.cv until the state becomes Stopped.
    enum class State { Stopped, Starting, Ready, Failed, Stopping };

    struct Backend {
        State        state = State::Stopped;
        pid_t        pid   = -1;
        int          port  = 0;          // assigned before start-up; reset on failure
        std::string  url;
        std::string  last_error;
        long long    last_used_ms = 0;
        long long    retry_after_ms = 0; // backoff: do not respawn before this timestamp
        int          fail_count = 0;
        int          active = 0;
        bool         stop_requested = false; // /admin disable: stop as soon as active==0
        std::condition_variable cv;
    };

    Backend& get_or_create(const std::string& logical_name);  // mu_ is held by the caller
    int loaded_count_locked() const;           // Ready/Starting models, mu_ is held by the caller
    bool spawn(const ModelEntry& e, int port, pid_t& out_pid, std::string& err);  // fork+exec, mu_ not held
    bool wait_health(int port);          // polls /health for up to startup_timeout_ms
    // SIGTERM -> SIGKILL, waitpid. Releases the lock passed in for the duration of the wait
    // (state=Stopping) so mu_ is not held for up to 5 s. The lock is returned re-acquired.
    void stop_backend(Backend& b, std::unique_lock<std::mutex>& lock);
    void reaper_loop();
    long long now_ms();

    Options     opt_;
    std::string api_key_;                // generated in the constructor (per boot)
    mutable std::mutex  mu_;             // mutable: loaded_count() const reads state under it
    std::map<std::string, std::unique_ptr<Backend>> backends_;
    int         next_port_;
    int         starts_in_flight_ = 0;
    std::thread reaper_;
    bool        stop_ = false;
    std::condition_variable reaper_cv_;
};

}  // namespace infcore
