// infcore gateway — MIT licence (see LICENSE).
// Loads the gateway configuration from JSON (also valid as a YAML subset).
#pragma once

#include <string>
#include <vector>

#include "registry/model_registry.h"
#include "security/authn/authn.h"
#include "security/rbac/rbac.h"

namespace infcore {

// An API key plus who it belongs to (subject/role). The identity source, offline.
struct ApiKeyPrincipal {
    std::string api_key;
    Principal   principal;
};

struct GatewayConfig {
    std::string host = "127.0.0.1";
    int         port = 8080;
    int         max_concurrent_requests = 64;   // worker pool size = ceiling on concurrent
                                                 // requests (SSE holds a worker for the whole stream)
    int         request_timeout_ms = 120000;     // timeout of the request to the backend (upstream)
    // Front-side limits (protection from slowloris/OOM on a public perimeter):
    int         read_timeout_ms  = 30000;        // client sending the request slowly -> disconnect
    int         write_timeout_ms = 120000;       // dead SSE consumer -> free the worker
    long        max_body_bytes   = 8 * 1024 * 1024;  // request body ceiling (otherwise OOM in json::parse)
    bool        rbac_enabled = true;
    bool        enforce_no_egress = true;
    bool        require_model_integrity = false;

    // Trusted reverse proxies (IPv4 or CIDR). ONLY for requests arriving from these
    // addresses is the real client taken from X-Real-IP / X-Forwarded-For; for anyone
    // else the header is not trusted (otherwise any client could forge its own client_ip
    // in the audit journal). Empty (the default) means trust nobody: client_ip is always
    // the connection peer.
    //
    // Without this, behind a TLS proxy EVERY audit record gets client_ip=127.0.0.1 and
    // the journal loses its "where from" dimension.
    std::vector<std::string> trusted_proxies;

    // runtime: lazy start-up of managed backends (models with an empty backend_url)
    std::string llama_server_bin;            // path to our llama-server (from the build)
    int         port_range_start   = 8100;
    int         idle_timeout_ms    = 300000;
    int         startup_timeout_ms = 120000;
    int         max_loaded_models  = 0;
    int         max_parallel_starts = 1;
    int         rate_limit_per_minute = 0;     // 0 = disabled; per subject/key after auth

    std::vector<std::string> api_keys;   // legacy: a flat list of keys (admin role)
    std::vector<ModelEntry>  models;

    // RBAC: principals (key -> subject/role) and roles (model/endpoint allowlists).
    std::vector<ApiKeyPrincipal> principals;
    std::vector<Role>            roles;

    // audit: a local append-only journal.
    std::string audit_sink = "file";                    // "file" | "none"
    std::string audit_path = "/var/log/infcore/audit.log";
    bool        audit_require = true;                    // sink=file and the journal failed to open -> fail fast

    bool        metrics_enabled = true;
    std::string metrics_path = "/metrics";
};

// Loads the config from a file. Throws std::runtime_error on error or invalid content.
GatewayConfig load_config(const std::string& path);

// "10.0.0.0/8" | "192.168.1.5" (no mask = /32) -> network + prefix length.
bool parse_cidr_v4(const std::string& s, unsigned net[4], int& bits);
// Whether addr falls inside the cidr network. false if either string fails to parse.
bool cidr_contains_v4(const std::string& cidr, const std::string& addr);

}  // namespace infcore
