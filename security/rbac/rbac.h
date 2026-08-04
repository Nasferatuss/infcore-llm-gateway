// infcore — MIT licence (see LICENSE). RBAC: role -> allowed models and endpoints.
// Default-deny: with RBAC enabled, access exists only where a rule grants it explicitly.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace infcore {

struct Role {
    std::string name;
    std::vector<std::string> allow_models;     // "*" = any
    std::vector<std::string> allow_endpoints;  // "*" = any
};

class Authorizer {
public:
    void add_role(const Role& r);
    void set_enabled(bool e) { enabled_ = e; }

    // Checks that the role permits the endpoint and (when non-empty) the model.
    // reason is filled in with the denial cause, for the audit journal. Always true when
    // enabled_=false.
    bool allow(const std::string& role, const std::string& endpoint,
               const std::string& model, std::string& reason) const;

    // Whether the role may access this model (used to filter /v1/models).
    bool model_allowed(const std::string& role, const std::string& model) const;

private:
    bool enabled_ = true;
    std::map<std::string, Role> roles_;
};

}  // namespace infcore
