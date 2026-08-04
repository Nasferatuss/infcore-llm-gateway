// infcore — MIT licence (see LICENSE).
// A registry of local models: logical_name -> {GGUF path, modality, provider, parameters}.
// The source of truth is the config (offline; weights are never downloaded).
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace infcore {

enum class Modality { Text, Embedding, Vision, Rerank };

struct ModelEntry {
    std::string logical_name;   // e.g. "qwen3-moe-a3b"
    std::string gguf_path;      // local path to the weights (for reference and future in-process use)
    std::string arch;           // e.g. "qwen3moe" (from the GGUF metadata)
    std::string backend_url;    // base URL of the llama-server backend, e.g. http://127.0.0.1:8081
    std::string upstream_model; // model name on the backend, if it differs; defaults to logical_name
    std::string mmproj_path;    // mtmd projector for vision (--mmproj); required for that modality
    std::string sha256;         // expected SHA-256 of the GGUF (release integrity gate)
    std::string mmproj_sha256;  // expected SHA-256 of the mmproj
    std::string license;        // provenance/licence metadata for the release manifest
    std::string source;         // provenance/source metadata for the release manifest
    int64_t     size_bytes = 0;
    int64_t     mmproj_size_bytes = 0;
    Modality    modality = Modality::Text;
    bool        enabled  = true;
    int32_t     n_ctx        = 8192;
    // -1 means --n-gpu-layers is not passed to the backend: llama.cpp fits the offload to
    // the free VRAM itself. Any explicit value DISABLES that auto-fit (llama.cpp does not
    // override a user-supplied setting) — see backend_supervisor.
    int32_t     n_gpu_layers = 0;
};

const char* modality_to_string(Modality m);

// A thread-safe registry of model metadata. Loading and unloading contexts is the
// gateway's job; this is only the catalogue and its validation.
class ModelRegistry {
public:
    void add(const ModelEntry& e);
    bool get(const std::string& logical_name, ModelEntry& out) const;
    bool set_enabled(const std::string& logical_name, bool enabled);  // false if there is no such model
    std::vector<ModelEntry> list() const;

private:
    mutable std::mutex      mu_;
    std::map<std::string, ModelEntry> models_;
};

}  // namespace infcore
