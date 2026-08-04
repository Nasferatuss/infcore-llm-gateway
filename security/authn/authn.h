// infcore — MIT licence (see LICENSE). Authentication: static API keys for an internal
// deployment (mTLS/OIDC are future work). No calls to external providers.
#pragma once

#include <string>
#include <vector>

namespace infcore {

struct Principal {
    std::string subject;   // who (for the audit journal)
    std::string role;      // role (for RBAC)
};

// Maps a bearer token to a principal. The source of truth is the config (offline).
// Key comparison is constant-time and does not exit the list early, so no timing side
// channel leaks either the length of the match or the position of the key.
class Authenticator {
public:
    void add_key(const std::string& api_key, const Principal& p);
    bool verify(const std::string& token, Principal& out) const;  // token without the "Bearer " prefix
    bool empty() const { return keys_.empty(); }

private:
    struct Entry { std::string key; Principal principal; };
    std::vector<Entry> keys_;
};

// Extracts the token from the Authorization header ("Bearer <token>"); empty on failure.
std::string parse_bearer(const std::string& header);

}  // namespace infcore
