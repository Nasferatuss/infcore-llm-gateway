// infcore gateway — MIT licence (see LICENSE).
// A minimal JSON-Schema validator (the subset of draft 2020-12 actually used by
// gateway.schema.json): type, required, properties,
// additionalProperties:false, enum, items, minimum/maximum, minItems, pattern,
// $ref ("#/$defs/..."). No external dependencies (offline deployment).
#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace infcore {

// Returns a list of human-readable errors (empty = the instance is valid against the schema).
std::vector<std::string> json_schema_validate(const nlohmann::json& instance,
                                              const nlohmann::json& schema);

}  // namespace infcore
