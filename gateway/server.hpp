// infcore gateway — MIT licence (see LICENSE).
// OpenAI-compatible gateway: a control plane (auth/registry/routing/metrics) in front of
// llama-server backends. Proxies with passthrough SSE for streaming responses.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "config.hpp"
#include "registry/model_registry.h"
#include "runtime/backend_supervisor.h"
#include "security/audit/audit.h"
#include "security/authn/authn.h"
#include "security/rbac/rbac.h"

// A forward declaration instead of #include "httplib.h": the gateway header must not drag
// the HTTP library in with it (only server.cpp includes it, and the unit tests build
// config.cpp/json_schema.cpp without it).
namespace httplib { struct Request; }

namespace infcore {

class GatewayServer {
public:
    explicit GatewayServer(GatewayConfig cfg);
    int run();   // blocking; returns the exit code

private:
    GatewayConfig cfg_;
    ModelRegistry registry_;
    std::unique_ptr<BackendSupervisor> supervisor_;

    // security
    Authenticator authn_;
    Authorizer    rbac_;
    AuditLog      audit_;

    // primitive metrics (pull, /metrics)
    std::mutex                                metrics_mu_;
    std::map<std::string, std::atomic<long>>  counters_;

    // Request latency histogram. The bounds (seconds) are chosen for LLM inference: the
    // interesting range is not microseconds but "fractions of a second -> minutes",
    // including backend cold start. Accumulated under metrics_mu_ together with the
    // counters: one write per request, and against inference the lock cost is negligible.
    struct LatencyHist {
        static constexpr size_t NBOUNDS = 12;
        static const double bounds[NBOUNDS];        // upper bounds, seconds
        unsigned long long buckets[NBOUNDS + 1]{};  // +1 = +Inf
        unsigned long long count = 0;
        double             sum_seconds = 0.0;
        void observe(double seconds);
    };
    LatencyHist hist_;
    struct RateState {
        long long window_start_ms = 0;
        int count = 0;
    };
    std::mutex                        rate_mu_;
    std::map<std::string, RateState>  rate_;

    void   inc(const std::string& key);
    long   get_counter(const std::string& key);

    // The client's real IP for the audit journal. Behind a trusted reverse proxy this is
    // X-Real-IP / X-Forwarded-For, otherwise the connection peer. See cfg_.trusted_proxies:
    // headers are trusted ONLY from trusted proxies, otherwise client_ip can be forged with
    // one extra request header.
    std::string client_ip_of(const httplib::Request& req) const;
    std::string render_metrics();
    bool   allow_rate(const Principal& pr, std::string& reason);

    void   audit_event(const Principal& pr, const std::string& client_ip,
                       const std::string& endpoint, const std::string& model,
                       const char* decision, const std::string& reason, int status,
                       const std::string& request_id = "",
                       const std::string& backend_id = "",
                       const std::string& model_sha256 = "",
                       long long latency_ms = -1,
                       long long request_bytes = -1,
                       long long response_bytes = -1,
                       long long prompt_tokens = -1,
                       long long completion_tokens = -1,
                       long long total_tokens = -1);
};

}  // namespace infcore
