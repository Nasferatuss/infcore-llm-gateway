// infcore gateway — MIT licence (see LICENSE).
#include "server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace infcore {

GatewayServer::GatewayServer(GatewayConfig cfg) : cfg_(std::move(cfg)) {
    for (const auto& m : cfg_.models) registry_.add(m);

    BackendSupervisor::Options opt;
    opt.llama_server_bin   = cfg_.llama_server_bin;
    opt.port_range_start   = cfg_.port_range_start;
    opt.idle_timeout_ms    = cfg_.idle_timeout_ms;
    opt.startup_timeout_ms = cfg_.startup_timeout_ms;
    opt.max_loaded_models  = cfg_.max_loaded_models;
    opt.max_parallel_starts = cfg_.max_parallel_starts;
    supervisor_ = std::make_unique<BackendSupervisor>(opt);

    // RBAC: roles come from the config; principals map to keys.
    rbac_.set_enabled(cfg_.rbac_enabled);
    bool has_admin_role = false;
    for (const auto& r : cfg_.roles) { rbac_.add_role(r); if (r.name == "admin") has_admin_role = true; }
    for (const auto& ap : cfg_.principals) authn_.add_key(ap.api_key, ap.principal);

    // Backwards compatibility: flat api_keys become principals with the admin role.
    if (!cfg_.api_keys.empty()) {
        std::fprintf(stderr,
            "infcore: WARNING: security.api_keys (legacy) grant the admin role with subject "
            "'legacy' and audit poorly; move to security.principals + roles.\n");
        if (!has_admin_role) {
            Role admin; admin.name = "admin";
            admin.allow_models = {"*"}; admin.allow_endpoints = {"*"};
            rbac_.add_role(admin);
        }
        for (const auto& k : cfg_.api_keys) authn_.add_key(k, Principal{"legacy", "admin"});
    }

    // Audit is mandatory for this deployment: if the journal (sink=file) did not open,
    // do NOT serve traffic with audit silently off — fail fast (unless audit.require is
    // explicitly cleared).
    if (cfg_.audit_sink == "file" && !audit_.open(cfg_.audit_path)) {
        std::string msg = "infcore: could not open the audit journal: " + cfg_.audit_path +
            " (check that the directory exists and is writable by the service user)";
        if (cfg_.audit_require)
            throw std::runtime_error(msg + "; audit.require=true -> startup aborted");
        std::fprintf(stderr, "%s; audit.require=false -> continuing WITHOUT audit\n", msg.c_str());
    }
}

void GatewayServer::audit_event(const Principal& pr, const std::string& client_ip,
                                const std::string& endpoint, const std::string& model,
                                const char* decision, const std::string& reason, int status,
                                const std::string& request_id,
                                const std::string& backend_id,
                                const std::string& model_sha256,
                                long long latency_ms,
                                long long request_bytes,
                                long long response_bytes,
                                long long prompt_tokens,
                                long long completion_tokens,
                                long long total_tokens) {
    AuditEvent ev;
    ev.subject = pr.subject;
    ev.role = pr.role;
    if (request_id.empty()) {
        static std::atomic<unsigned long long> audit_seq{0};
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        ev.request_id = "req-" + std::to_string(ms) + "-" + std::to_string(++audit_seq);
    } else {
        ev.request_id = request_id;
    }
    ev.endpoint = endpoint;
    ev.model = model;
    ev.model_sha256 = model_sha256;
    ev.backend_id = backend_id;
    ev.client_ip = client_ip;
    ev.decision = decision;
    ev.reason = reason;
    ev.status = status;
    ev.latency_ms = latency_ms;
    ev.request_bytes = request_bytes;
    ev.response_bytes = response_bytes;
    ev.prompt_tokens = prompt_tokens;
    ev.completion_tokens = completion_tokens;
    ev.total_tokens = total_tokens;

    // Metrics are taken here: EVERY request outcome (allow/deny/error) goes through
    // audit_event, so a single hook covers all paths and no new branch can forget its
    // counter. The endpoint is used as-is — it is a fixed set of routes, not user input,
    // so there is no cardinality blow-up.
    inc("requests_total{endpoint=\"" + endpoint + "\",decision=\"" + std::string(decision) + "\"}");
    if (latency_ms >= 0) {
        std::lock_guard<std::mutex> lock(metrics_mu_);
        hist_.observe((double)latency_ms / 1000.0);
    }

    audit_.log(ev);
}

namespace {
// The rightmost element of X-Forwarded-For. Rightmost, not leftmost: nginx/Angie's
// proxy_add_x_forwarded_for APPENDS its own peer on the right, whereas the left-hand
// elements arrived from the client and are trivially forged ("X-Forwarded-For: 8.8.8.8").
std::string rightmost_xff(const std::string& v) {
    const auto end = v.find_last_not_of(" \t");
    if (end == std::string::npos) return {};
    const auto comma = v.find_last_of(',', end);
    const auto start = (comma == std::string::npos) ? 0 : comma + 1;
    const auto b = v.find_first_not_of(" \t", start);
    if (b == std::string::npos || b > end) return {};
    return v.substr(b, end - b + 1);
}
}  // namespace

// The client's real IP for the audit journal. Without this, behind a TLS proxy EVERY
// journal record would carry client_ip=127.0.0.1 (the peer is always the proxy itself).
//
// Headers are trusted ONLY when the request came from an address in trusted_proxies:
// otherwise a client forges its own audited IP with one extra header. The list is empty
// by default -> behaviour is unchanged (always the connection peer).
std::string GatewayServer::client_ip_of(const httplib::Request& req) const {
    const std::string peer = req.remote_addr;   // the only place the peer is read directly
    if (cfg_.trusted_proxies.empty()) return peer;

    bool trusted = false;
    for (const auto& cidr : cfg_.trusted_proxies)
        if (cidr_contains_v4(cidr, peer)) { trusted = true; break; }
    if (!trusted) return peer;

    // The proxy SETS X-Real-IP (proxy_set_header), overwriting whatever the client sent,
    // so it cannot be forged — prefer it.
    const std::string real = req.get_header_value("X-Real-IP");
    if (!real.empty()) return real;

    const std::string xff = rightmost_xff(req.get_header_value("X-Forwarded-For"));
    return xff.empty() ? peer : xff;
}

void GatewayServer::inc(const std::string& key) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    counters_[key].fetch_add(1, std::memory_order_relaxed);
}
long GatewayServer::get_counter(const std::string& key) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    return counters_[key].load(std::memory_order_relaxed);
}
// Latency histogram bounds (seconds).
const double GatewayServer::LatencyHist::bounds[GatewayServer::LatencyHist::NBOUNDS] = {
    0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0
};

void GatewayServer::LatencyHist::observe(double seconds) {
    size_t i = 0;
    while (i < NBOUNDS && seconds > bounds[i]) ++i;
    ++buckets[i];          // i == NBOUNDS -> the +Inf bucket
    ++count;
    sum_seconds += seconds;
}

std::string GatewayServer::render_metrics() {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    std::ostringstream os;
    os.setf(std::ios::fixed);

    // Prometheus/OpenMetrics format: HELP/TYPE are required for the series to render
    // meaningfully; without them everything arrives as untyped and rate() /
    // histogram_quantile() cannot be applied.
    os << "# HELP infcore_gateway_errors_total Gateway errors by type.\n"
       << "# TYPE infcore_gateway_errors_total counter\n";
    for (auto& kv : counters_) {
        if (kv.first.rfind("errors_total", 0) != 0) continue;
        os << "infcore_gateway_" << kv.first << " "
           << kv.second.load(std::memory_order_relaxed) << "\n";
    }

    os << "# HELP infcore_gateway_requests_total Requests by endpoint and policy decision.\n"
       << "# TYPE infcore_gateway_requests_total counter\n";
    for (auto& kv : counters_) {
        if (kv.first.rfind("requests_total", 0) != 0) continue;
        os << "infcore_gateway_" << kv.first << " "
           << kv.second.load(std::memory_order_relaxed) << "\n";
    }

    // Latency: without it, degradation under load is only visible through complaints.
    os << "# HELP infcore_gateway_request_duration_seconds Request latency (including backend cold start).\n"
       << "# TYPE infcore_gateway_request_duration_seconds histogram\n";
    unsigned long long cum = 0;
    for (size_t i = 0; i < LatencyHist::NBOUNDS; ++i) {
        cum += hist_.buckets[i];
        os.precision(2);
        os << "infcore_gateway_request_duration_seconds_bucket{le=\"" << LatencyHist::bounds[i]
           << "\"} " << cum << "\n";
    }
    cum += hist_.buckets[LatencyHist::NBOUNDS];
    os << "infcore_gateway_request_duration_seconds_bucket{le=\"+Inf\"} " << cum << "\n";
    os.precision(3);
    os << "infcore_gateway_request_duration_seconds_sum " << hist_.sum_seconds << "\n"
       << "infcore_gateway_request_duration_seconds_count " << hist_.count << "\n";

    // Audit liveness. audit_failed=1 means that, with audit.require=true, the gateway is
    // already returning 503 for all traffic: this is a "service is down" alert, not a
    // "something looks suspicious" one.
    os << "# HELP infcore_gateway_audit_failed The audit journal writer failed fatally (1) - traffic is cut fail-closed.\n"
       << "# TYPE infcore_gateway_audit_failed gauge\n"
       << "infcore_gateway_audit_failed " << (audit_.failed() ? 1 : 0) << "\n";
    os << "# HELP infcore_gateway_audit_reopens_total Journal reopens on SIGHUP (rotation).\n"
       << "# TYPE infcore_gateway_audit_reopens_total counter\n"
       << "infcore_gateway_audit_reopens_total " << audit_.reopens() << "\n";
    // >0 means rotation did not take effect and the journal is being written to the
    // renamed file.
    os << "# HELP infcore_gateway_audit_reopen_failures_total Failed journal reopens (rotation is not actually working).\n"
       << "# TYPE infcore_gateway_audit_reopen_failures_total counter\n"
       << "infcore_gateway_audit_reopen_failures_total " << audit_.reopen_failures() << "\n";

    os << "# HELP infcore_gateway_models_configured Models in the registry.\n"
       << "# TYPE infcore_gateway_models_configured gauge\n"
       << "infcore_gateway_models_configured " << registry_.list().size() << "\n";
    if (supervisor_) {
        os << "# HELP infcore_gateway_backends_loaded llama-server backends up (Ready/Starting).\n"
           << "# TYPE infcore_gateway_backends_loaded gauge\n"
           << "infcore_gateway_backends_loaded " << supervisor_->loaded_count() << "\n";
    }
    return os.str();
}

bool GatewayServer::allow_rate(const Principal& pr, std::string& reason) {
    if (cfg_.rate_limit_per_minute <= 0) return true;
    const std::string key = !pr.subject.empty() ? pr.subject : pr.role;
    using namespace std::chrono;
    const long long now = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(rate_mu_);
    RateState& st = rate_[key];
    if (st.window_start_ms == 0 || now - st.window_start_ms >= 60000) {
        st.window_start_ms = now;
        st.count = 0;
    }
    if (st.count >= cfg_.rate_limit_per_minute) {
        reason = "rate limit exceeded";
        return false;
    }
    ++st.count;
    return true;
}

// --- helpers -----------------------------------------------------------------
namespace {

// OpenAI-compatible error: {"error":{message,type,param,code}}.
// `code` is a machine-readable string (or null), as with OpenAI — not the HTTP status.
void error_json(httplib::Response& res, int status, const std::string& type,
                const std::string& msg, const std::string& code = "") {
    json err = {{"type", type}, {"message", msg}, {"param", nullptr}};
    if (code.empty()) err["code"] = nullptr; else err["code"] = code;
    res.status = status;
    res.set_content(json{{"error", err}}.dump(), "application/json");
}

std::string auth_token(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return std::string();
    return parse_bearer(it->second);
}

bool endpoint_accepts_modality(const std::string& path, Modality m) {
    if (path == "/v1/embeddings")
        return m == Modality::Embedding;
    if (path == "/v1/rerank" || path == "/v1/reranking" || path == "/rerank" || path == "/reranking")
        return m == Modality::Rerank;
    return m == Modality::Text || m == Modality::Vision;
}

long long steady_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string make_request_id() {
    static std::atomic<unsigned long long> seq{0};
    return "req-" + std::to_string(steady_ms()) + "-" + std::to_string(++seq);
}

void extract_usage(const std::string& body, long long& prompt, long long& completion, long long& total) {
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.contains("usage") || !j["usage"].is_object()) return;
    const auto& u = j["usage"];
    if (u.contains("prompt_tokens") && u["prompt_tokens"].is_number_integer())
        prompt = u["prompt_tokens"].get<long long>();
    if (u.contains("completion_tokens") && u["completion_tokens"].is_number_integer())
        completion = u["completion_tokens"].get<long long>();
    if (u.contains("total_tokens") && u["total_tokens"].is_number_integer())
        total = u["total_tokens"].get<long long>();
}

// Shared state of the SSE proxy: a background thread pulls the backend response while
// the downstream content provider hands bytes to the client. The backend status becomes
// known (headers_ready) BEFORE the streaming response is committed, so a non-2xx can
// still be returned as a plain JSON error without opening a text/event-stream.
struct StreamPump {
    std::mutex m;
    std::condition_variable cv;
    std::string buf;
    bool headers_ready = false;
    int  status = 0;
    bool done = false;
    bool ok = false;             // send() completed without a transport error
    bool consumer_gone = false;  // client went away -> stop pulling from the backend
    size_t bytes_total = 0;
};

// Orderly shutdown on a signal: C++ destructors do not run when a process dies from a
// signal, so SIGINT/SIGTERM are caught and listen() is asked to return -> ~BackendSupervisor
// runs and shuts the child llama-server processes down (no orphans left behind).
httplib::Server* g_active_srv = nullptr;
void on_term(int) { if (g_active_srv) g_active_srv->stop(); }

// SIGHUP means "the journal was rotated, reopen it" (logrotate: postrotate kill -HUP).
// The handler only raises a lock-free flag (async-signal-safe); the reopen itself is done
// by the audit writer thread before its next batch. The default action for SIGHUP is to
// kill the process, so not catching it would be more dangerous than catching it.
AuditLog* g_active_audit = nullptr;
void on_hup(int) { if (g_active_audit) g_active_audit->request_reopen(); }

}  // namespace

// --- server ------------------------------------------------------------------
int GatewayServer::run() {
    httplib::Server svr;

    // Concurrency cap: an SSE stream holds a worker for the whole duration of the stream,
    // so the pool size is the ceiling on concurrent requests.
    if (cfg_.max_concurrent_requests > 0) {
        const int n = cfg_.max_concurrent_requests;
        svr.new_task_queue = [n] { return new httplib::ThreadPool(n); };
    }

    // Perimeter limits. Without them: (1) slowloris — a client sends headers/body one byte
    // at a time and holds a worker forever; (2) a dead SSE consumer pins a worker on
    // sink.write indefinitely; (3) a huge body -> OOM in json::parse. The timeouts free the
    // worker; the body limit rejects the request before it is buffered.
    svr.set_read_timeout(cfg_.read_timeout_ms / 1000, (cfg_.read_timeout_ms % 1000) * 1000);
    svr.set_write_timeout(cfg_.write_timeout_ms / 1000, (cfg_.write_timeout_ms % 1000) * 1000);
    if (cfg_.max_body_bytes > 0)
        svr.set_payload_max_length((size_t)cfg_.max_body_bytes);

    g_active_srv = &svr;
    g_active_audit = &audit_;
    std::signal(SIGINT, on_term);
    std::signal(SIGTERM, on_term);
    std::signal(SIGHUP, on_hup);     // audit journal rotation: reopen the same path
    std::signal(SIGPIPE, SIG_IGN);   // a client dropping mid-SSE must not take the gateway down
    std::signal(SIGXFSZ, SIG_IGN);   // audit file size limit -> EFBIG (fail-closed), not a kill

    // Fail closed on a runtime audit failure: if the journal is mandatory (audit.require=true)
    // and the writer thread died fatally (disk full / IO error), do NOT serve traffic that
    // cannot be recorded. /health and /metrics stay up so operators can see the degradation.
    svr.set_pre_routing_handler(
        [this](const httplib::Request& req, httplib::Response& res) {
            if (cfg_.audit_require && audit_.failed() &&
                req.path != "/health" && (!cfg_.metrics_enabled || req.path != cfg_.metrics_path)) {
                error_json(res, 503, "api_error",
                           "audit journal unavailable; refusing traffic (fail-closed)");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

    // health (unauthenticated). Also reflects audit state so degradation is visible.
    svr.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        const bool audit_ok = !(cfg_.audit_require && audit_.failed());
        json h = {{"status", audit_ok ? "ok" : "degraded"},
                  {"models", (int)cfg_.models.size()},
                  {"audit", audit_.failed() ? "failed" : "ok"}};
        if (!audit_ok) res.status = 503;
        res.set_content(h.dump(), "application/json");
    });

    // pull metrics (unauthenticated; the deployment is a closed network), if enabled in config
    if (cfg_.metrics_enabled) {
        svr.Get(cfg_.metrics_path, [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(render_metrics(), "text/plain; version=0.0.4");
        });
    }

    // /v1/models — OpenAI-compatible listing (only models the role is allowed to see)
    svr.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        Principal pr;
        if (!authn_.verify(auth_token(req), pr)) { inc("errors_total{type=\"unauthorized\"}");
            audit_event({}, client_ip_of(req), "/v1/models", "", "deny", "unauthorized", 401);
            return error_json(res, 401, "invalid_request_error", "unauthorized"); }
        std::string reason;
        if (!allow_rate(pr, reason)) { inc("errors_total{type=\"rate_limited\"}");
            audit_event(pr, client_ip_of(req), "/v1/models", "", "deny", reason, 429);
            return error_json(res, 429, "rate_limit_error", reason, "rate_limit_exceeded"); }
        if (!rbac_.allow(pr.role, "/v1/models", "", reason)) { inc("errors_total{type=\"forbidden\"}");
            audit_event(pr, client_ip_of(req), "/v1/models", "", "deny", reason, 403);
            return error_json(res, 403, "invalid_request_error", "forbidden: " + reason); }
        json data = json::array();
        for (const auto& m : registry_.list()) {
            if (!m.enabled) continue;
            if (!rbac_.model_allowed(pr.role, m.logical_name)) continue;
            data.push_back({{"id", m.logical_name}, {"object", "model"},
                            {"created", 0}, {"owned_by", "infcore"},
                            {"modality", modality_to_string(m.modality)}});
        }
        audit_event(pr, client_ip_of(req), "/v1/models", "", "allow", "", 200);
        res.set_content(json{{"object", "list"}, {"data", data}}.dump(), "application/json");
    });

    // --- admin: manage models without a restart (RBAC-gated on the /admin/models endpoint) ---

    // GET /admin/models — the full list with status (including disabled models).
    svr.Get("/admin/models", [this](const httplib::Request& req, httplib::Response& res) {
        Principal pr;
        if (!authn_.verify(auth_token(req), pr)) { inc("errors_total{type=\"unauthorized\"}");
            audit_event({}, client_ip_of(req), "/admin/models", "", "deny", "unauthorized", 401);
            return error_json(res, 401, "invalid_request_error", "unauthorized"); }
        std::string reason;
        if (!allow_rate(pr, reason)) { inc("errors_total{type=\"rate_limited\"}");
            audit_event(pr, client_ip_of(req), "/admin/models", "", "deny", reason, 429);
            return error_json(res, 429, "rate_limit_error", reason, "rate_limit_exceeded"); }
        if (!rbac_.allow(pr.role, "/admin/models", "", reason)) { inc("errors_total{type=\"forbidden\"}");
            audit_event(pr, client_ip_of(req), "/admin/models", "", "deny", reason, 403);
            return error_json(res, 403, "invalid_request_error", "forbidden: " + reason); }
        json data = json::array();
        for (const auto& m : registry_.list())
            data.push_back({{"id", m.logical_name}, {"modality", modality_to_string(m.modality)},
                            {"enabled", m.enabled}, {"managed", m.backend_url.empty()},
                            {"backend_url", m.backend_url}, {"arch", m.arch}});
        audit_event(pr, client_ip_of(req), "/admin/models", "", "allow", "", 200);
        res.set_content(json{{"object", "list"}, {"data", data}}.dump(), "application/json");
    });

    // POST /admin/models/<name>/<enable|disable> — flip a model at runtime.
    svr.Post(R"(/admin/models/([^/]+)/(enable|disable))",
             [this](const httplib::Request& req, httplib::Response& res) {
        Principal pr;
        if (!authn_.verify(auth_token(req), pr)) { inc("errors_total{type=\"unauthorized\"}");
            audit_event({}, client_ip_of(req), "/admin/models", "", "deny", "unauthorized", 401);
            return error_json(res, 401, "invalid_request_error", "unauthorized"); }
        std::string reason;
        if (!allow_rate(pr, reason)) { inc("errors_total{type=\"rate_limited\"}");
            audit_event(pr, client_ip_of(req), "/admin/models", "", "deny", reason, 429);
            return error_json(res, 429, "rate_limit_error", reason, "rate_limit_exceeded"); }
        if (!rbac_.allow(pr.role, "/admin/models", "", reason)) { inc("errors_total{type=\"forbidden\"}");
            audit_event(pr, client_ip_of(req), "/admin/models", "", "deny", reason, 403);
            return error_json(res, 403, "invalid_request_error", "forbidden: " + reason); }
        const std::string name   = req.matches[1];
        const bool        enable = (req.matches[2] == "enable");
        if (!registry_.set_enabled(name, enable)) { inc("errors_total{type=\"model_not_found\"}");
            audit_event(pr, client_ip_of(req), "/admin/models", name, "deny", "model not found", 404);
            return error_json(res, 404, "invalid_request_error", "model not found: " + name); }
        // Disabling a managed model shuts its llama-server down (otherwise it would linger
        // until the idle timeout).
        if (!enable) {
            ModelEntry me;
            if (registry_.get(name, me) && me.backend_url.empty()) supervisor_->stop(name);
        }
        audit_event(pr, client_ip_of(req), "/admin/models", name, "allow",
                    enable ? "enabled" : "disabled", 200);
        res.set_content(json{{"id", name}, {"enabled", enable}}.dump(), "application/json");
    });

    // shared proxy to the llama-server backend (chat/completions, completions, embeddings, rerank)
    auto proxy = [this](const char* upstream_path, bool allow_stream) {
        return [this, upstream_path, allow_stream](const httplib::Request& req,
                                                   httplib::Response& res) {
            const std::string request_id = make_request_id();
            const long long started_ms = steady_ms();
            // The request counter is taken in audit_event(), where it covers ALL routes
            // and carries the decision. The previous inc(requests_total{path=...}) here
            // duplicated it for the proxy endpoints only, and did so with a different
            // label set under the same metric name — which Prometheus does not allow.
            Principal pr;
            if (!authn_.verify(auth_token(req), pr)) { inc("errors_total{type=\"unauthorized\"}");
                audit_event({}, client_ip_of(req), upstream_path, "", "deny", "unauthorized", 401, request_id);
                return error_json(res, 401, "invalid_request_error", "unauthorized"); }
            std::string rate_reason;
            if (!allow_rate(pr, rate_reason)) { inc("errors_total{type=\"rate_limited\"}");
                audit_event(pr, client_ip_of(req), upstream_path, "", "deny", rate_reason, 429, request_id);
                return error_json(res, 429, "rate_limit_error", rate_reason, "rate_limit_exceeded"); }

            const std::string client_ip = client_ip_of(req);

            json body;
            try { body = json::parse(req.body); }
            catch (...) { inc("errors_total{type=\"bad_request\"}");
                audit_event(pr, client_ip, upstream_path, "", "deny", "invalid JSON", 400, request_id);
                return error_json(res, 400, "invalid_request_error", "invalid JSON"); }

            if (!body.contains("model") || !body.at("model").is_string()) {
                inc("errors_total{type=\"bad_request\"}");
                audit_event(pr, client_ip, upstream_path, "", "deny", "model must be a string", 400, request_id);
                return error_json(res, 400, "invalid_request_error", "field 'model' must be a string", "invalid_type");
            }
            const std::string model = body.at("model").get<std::string>();
            ModelEntry e;
            if (model.empty() || !registry_.get(model, e)) { inc("errors_total{type=\"model_not_found\"}");
                audit_event(pr, client_ip, upstream_path, model, "deny", "model not found", 404, request_id);
                return error_json(res, 404, "invalid_request_error", "model not found: " + model, "model_not_found"); }
            if (!e.enabled) { inc("errors_total{type=\"model_disabled\"}");
                audit_event(pr, client_ip, upstream_path, model, "deny", "model disabled", 409, request_id);
                return error_json(res, 409, "invalid_request_error", "model disabled: " + model, "model_disabled"); }
            if (!endpoint_accepts_modality(upstream_path, e.modality)) {
                inc("errors_total{type=\"wrong_modality\"}");
                audit_event(pr, client_ip, upstream_path, model, "deny", "wrong model modality", 400, request_id);
                return error_json(res, 400, "invalid_request_error",
                                  "model '" + model + "' does not support endpoint " + upstream_path,
                                  "wrong_modality");
            }

            // RBAC: the role must permit both the endpoint and the model (default-deny).
            std::string reason;
            if (!rbac_.allow(pr.role, upstream_path, model, reason)) {
                inc("errors_total{type=\"forbidden\"}");
                audit_event(pr, client_ip, upstream_path, model, "deny", reason, 403, request_id);
                return error_json(res, 403, "invalid_request_error", "forbidden: " + reason);
            }

            // An empty backend_url means the model is managed: start the process on demand.
            std::string backend = e.backend_url;
            const bool managed = backend.empty();
            const std::string logical = e.logical_name;

            // RAII token for the in-flight request: release is guaranteed even if the client
            // disconnects (otherwise the active counter would leak and the reaper would never
            // unload the backend).
            std::shared_ptr<void> active_token;
            if (managed) {
                std::string serr;
                backend = supervisor_->ensure_ready(e, serr);
                if (backend.empty()) { inc("errors_total{type=\"backend_start_failed\"}");
                    audit_event(pr, client_ip, upstream_path, model, "error", "backend start failed", 502,
                                request_id, logical, e.sha256, steady_ms() - started_ms,
                                static_cast<long long>(req.body.size()), 0);
                    return error_json(res, 502, "api_error", "could not start backend: " + serr); }
                supervisor_->acquire(logical);
                BackendSupervisor* sup = supervisor_.get();
                active_token = std::shared_ptr<void>(nullptr, [sup, logical](void*) { sup->release(logical); });
            }

            // substitute the backend-side model name, if one is configured
            if (!e.upstream_model.empty()) body["model"] = e.upstream_model;
            bool stream = false;
            if (body.contains("stream")) {
                if (!body.at("stream").is_boolean()) {
                    inc("errors_total{type=\"bad_request\"}");
                    audit_event(pr, client_ip, upstream_path, model, "deny", "stream must be boolean", 400, request_id);
                    return error_json(res, 400, "invalid_request_error", "field 'stream' must be boolean", "invalid_type");
                }
                stream = allow_stream && body.at("stream").get<bool>();
            }
            if (!allow_stream) body.erase("stream");  // embeddings: do not forward stream upstream
            const std::string out_body = body.dump();

            const int t_sec  = cfg_.request_timeout_ms / 1000;
            const int t_usec = (cfg_.request_timeout_ms % 1000) * 1000;

            httplib::Headers fwd = {{"Content-Type", "application/json"}};
            if (managed) fwd.emplace("Authorization", "Bearer " + supervisor_->api_key());

            if (!stream) {
                httplib::Client cli(backend);
                cli.set_read_timeout(t_sec, t_usec);
                cli.set_write_timeout(t_sec, t_usec);
                auto up = cli.Post(upstream_path, fwd, out_body, "application/json");
                if (!up) { inc("errors_total{type=\"backend_unreachable\"}");
                    audit_event(pr, client_ip, upstream_path, model, "error", "backend unreachable", 502,
                                request_id, logical, e.sha256, steady_ms() - started_ms,
                                static_cast<long long>(out_body.size()), 0);
                    return error_json(res, 502, "api_error", "backend unavailable: " + backend); }
                res.status = up->status;
                res.set_content(up->body, up->has_header("Content-Type")
                                              ? up->get_header_value("Content-Type")
                                              : "application/json");
                long long prompt_tokens = -1, completion_tokens = -1, total_tokens = -1;
                extract_usage(up->body, prompt_tokens, completion_tokens, total_tokens);
                audit_event(pr, client_ip, upstream_path, model,
                            up->status < 400 ? "allow" : "error", "", up->status,
                            request_id, logical, e.sha256, steady_ms() - started_ms,
                            static_cast<long long>(out_body.size()),
                            static_cast<long long>(up->body.size()),
                            prompt_tokens, completion_tokens, total_tokens);
                return;
            }

            // Streaming: a background thread pulls the backend response; the status is known
            // before the streaming response is committed.
            auto pump = std::make_shared<StreamPump>();
            std::thread([backend, upstream_path, fwd, out_body, t_sec, t_usec, pump] {
                httplib::Client cli(backend);
                cli.set_read_timeout(t_sec, t_usec);
                cli.set_write_timeout(t_sec, t_usec);
                httplib::Request rq;
                rq.method = "POST";
                rq.path = upstream_path;
                rq.headers = fwd;
                rq.body = out_body;
                rq.response_handler = [pump](const httplib::Response& rr) {
                    std::lock_guard<std::mutex> lk(pump->m);
                    pump->status = rr.status;
                    pump->headers_ready = true;
                    pump->cv.notify_all();
                    return true;
                };
                rq.content_receiver = [pump](const char* d, size_t n, size_t, size_t) {
                    std::lock_guard<std::mutex> lk(pump->m);
                    pump->buf.append(d, n);
                    pump->bytes_total += n;
                    pump->cv.notify_all();
                    return !pump->consumer_gone;
                };
                httplib::Response rr;
                httplib::Error er = httplib::Error::Success;
                bool sent = cli.send(rq, rr, er);
                std::lock_guard<std::mutex> lk(pump->m);
                pump->ok = sent && er == httplib::Error::Success;
                if (!pump->headers_ready) { pump->status = pump->ok ? rr.status : 0; pump->headers_ready = true; }
                pump->done = true;
                pump->cv.notify_all();
            }).detach();

            int ust;
            {
                std::unique_lock<std::mutex> lk(pump->m);
                pump->cv.wait(lk, [&] { return pump->headers_ready; });
                ust = pump->status;
            }

            // An error BEFORE the stream (invalid parameters / backend unreachable) is returned
            // as a plain JSON error, not as SSE inside a 200.
            if (ust < 200 || ust >= 300) {
                std::string ebody; int st;
                {
                    std::unique_lock<std::mutex> lk(pump->m);
                    pump->cv.wait(lk, [&] { return pump->done; });
                    ebody = pump->buf;
                    st = pump->status ? pump->status : 502;
                }
                inc("errors_total{type=\"backend_error\"}");
                audit_event(pr, client_ip, upstream_path, model, "error",
                            "backend status " + std::to_string(st), st,
                            request_id, logical, e.sha256, steady_ms() - started_ms,
                            static_cast<long long>(out_body.size()),
                            static_cast<long long>(ebody.size()));
                res.status = st;
                if (!ebody.empty()) res.set_content(ebody, "application/json");
                else error_json(res, st, "api_error", "backend returned an error");
                return;  // active_token is released here
            }

            // 2xx: passthrough SSE. active_token lives inside the provider, so it is released
            // when the stream finishes (including when the client disconnects).
            res.set_chunked_content_provider(
                "text/event-stream",
                [pump, active_token](size_t /*offset*/, httplib::DataSink& sink) {
                    std::unique_lock<std::mutex> lk(pump->m);
                    pump->cv.wait(lk, [&] { return !pump->buf.empty() || pump->done; });
                    if (!pump->buf.empty()) {
                        std::string chunk;
                        chunk.swap(pump->buf);
                        lk.unlock();
                        if (!sink.write(chunk.data(), chunk.size())) {
                            std::lock_guard<std::mutex> lk2(pump->m);
                            pump->consumer_gone = true;
                            return false;
                        }
                        return true;
                    }
                    const bool ok = pump->ok;
                    lk.unlock();
                    if (!ok) {  // the backend stream broke: synthetic error + clean termination
                        const char* ev =
                            "data: {\"error\":{\"message\":\"backend stream interrupted\","
                            "\"type\":\"api_error\",\"code\":null}}\n\ndata: [DONE]\n\n";
                        sink.write(ev, std::char_traits<char>::length(ev));
                    }
                    sink.done();
                    return false;
                },
                [this, pump, pr, client_ip, upstream_path, model, request_id, logical, model_sha256 = e.sha256, started_ms, out_body](bool) {
                    audit_event(pr, client_ip, upstream_path, model, "allow",
                                pump->ok ? "" : "stream interrupted", 200,
                                request_id, logical, model_sha256, steady_ms() - started_ms,
                                static_cast<long long>(out_body.size()),
                                static_cast<long long>(pump->bytes_total));
                });
        };
    };

    svr.Post("/v1/chat/completions", proxy("/v1/chat/completions", true));
    svr.Post("/v1/completions",      proxy("/v1/completions", true));
    svr.Post("/v1/embeddings",       proxy("/v1/embeddings", false));
    svr.Post("/v1/rerank",           proxy("/v1/rerank", false));
    svr.Post("/v1/reranking",        proxy("/v1/reranking", false));
    svr.Post("/rerank",              proxy("/rerank", false));
    svr.Post("/reranking",           proxy("/reranking", false));

    std::printf("infcore gateway listening on http://%s:%d (models: %zu)\n",
                cfg_.host.c_str(), cfg_.port, cfg_.models.size());
    bool ok = svr.listen(cfg_.host, cfg_.port);
    g_active_srv = nullptr;
    if (!ok) {
        std::fprintf(stderr, "infcore: could not listen on %s:%d\n",
                     cfg_.host.c_str(), cfg_.port);
        return 1;
    }
    return 0;
}

}  // namespace infcore
