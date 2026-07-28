// infcore gateway — лицензия MIT (см. LICENSE).
#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>

#include <openssl/evp.h>
#include "nlohmann/json.hpp"

#include "json_schema.hpp"
#include "schema_embedded.h"   // сгенерировано CMake: kGatewaySchemaJson

using json = nlohmann::json;

namespace infcore {

static Modality parse_modality(const std::string& s) {
    if (s == "embedding") return Modality::Embedding;
    if (s == "vision")    return Modality::Vision;
    if (s == "rerank")    return Modality::Rerank;
    return Modality::Text;
}

// Разрешает секрет, не зашивая его в конфиг/образ:
//   "env:VAR"   -> значение переменной окружения VAR
//   "file:/p"   -> содержимое файла /p (обрезаются хвостовые переводы строк)
//   иначе       -> строка как есть (literal)
static std::string resolve_secret(const std::string& v) {
    if (v.rfind("env:", 0) == 0) {
        const char* e = std::getenv(v.c_str() + 4);
        if (!e || !*e)
            throw std::runtime_error("infcore: переменная окружения не задана: " + v.substr(4));
        return e;
    }
    if (v.rfind("file:", 0) == 0) {
        std::ifstream f(v.substr(5));
        if (!f) throw std::runtime_error("infcore: не удалось прочитать файл секрета: " + v.substr(5));
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        return s;
    }
    return v;
}

static bool weak_secret(const std::string& key) {
    if (key.size() < 24) return true;
    std::string lower = key;
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    static const char* bad[] = {
        "change-me", "changeme", "replace_me", "replace-me", "example",
        "dummy", "secret", "password", "test-key", "generate-with"
    };
    for (const char* b : bad)
        if (lower.find(b) != std::string::npos) return true;
    return false;
}

static std::string sha256_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("infcore: не удалось открыть artifact для sha256: " + path);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("infcore: EVP_MD_CTX_new failed");
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("infcore: EVP_DigestInit_ex sha256 failed");
    }
    char buf[64 * 1024];
    while (f.good()) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx, buf, static_cast<size_t>(n)) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("infcore: EVP_DigestUpdate sha256 failed: " + path);
        }
    }
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(ctx, out, &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("infcore: EVP_DigestFinal_ex sha256 failed: " + path);
    }
    EVP_MD_CTX_free(ctx);
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(out_len * 2);
    for (unsigned int i = 0; i < out_len; ++i) {
        unsigned char c = out[i];
        s.push_back(hx[c >> 4]);
        s.push_back(hx[c & 0x0f]);
    }
    return s;
}

static void validate_artifact_integrity(const std::string& path, const std::string& sha256,
                                        int64_t size_bytes, bool required,
                                        const std::string& label) {
    const bool check = required || !sha256.empty() || size_bytes > 0;
    if (!check) return;
    if (path.empty())
        throw std::runtime_error("infcore: " + label + " требует path для integrity validation");
    if (required && sha256.empty())
        throw std::runtime_error("infcore: " + label + " требует sha256 при require_model_integrity=true");
    if (required && size_bytes <= 0)
        throw std::runtime_error("infcore: " + label + " требует size_bytes при require_model_integrity=true");
    struct stat st {};
    if (stat(path.c_str(), &st) != 0)
        throw std::runtime_error("infcore: не удалось stat artifact " + label + ": " + path);
    if (!S_ISREG(st.st_mode))
        throw std::runtime_error("infcore: artifact не является regular file " + label + ": " + path);
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        throw std::runtime_error("infcore: artifact writable для group/other " + label + ": " + path);
    if (size_bytes > 0 && static_cast<int64_t>(st.st_size) != size_bytes)
        throw std::runtime_error("infcore: size mismatch для " + label + ": " + path);
    if (!sha256.empty()) {
        const std::string got = sha256_file(path);
        if (got != sha256)
            throw std::runtime_error("infcore: sha256 mismatch для " + label + ": " + path);
    }
}

// Строгий разбор dotted-quad IPv4: ровно 4 числовых октета 0..255, без лишних
// символов. Хостнейм вида "127.0.0.1.evil.com" или "10.example.org" НЕ является
// IPv4 -> не пройдёт как локальный (закрывает обход по префиксу).
static bool parse_ipv4(const std::string& h, unsigned o[4]) {
    unsigned vals[4];
    size_t i = 0;
    const size_t len = h.size();
    for (int part = 0; part < 4; ++part) {
        if (i >= len || !std::isdigit((unsigned char)h[i])) return false;
        unsigned v = 0;
        int digits = 0;
        while (i < len && std::isdigit((unsigned char)h[i])) {
            v = v * 10 + (unsigned)(h[i] - '0');
            if (++digits > 3) return false;
            ++i;
        }
        if (v > 255) return false;
        vals[part] = v;
        if (part < 3) { if (i >= len || h[i] != '.') return false; ++i; }
    }
    if (i != len) return false;   // хвост после 4-го октета -> это не IPv4
    for (int k = 0; k < 4; ++k) o[k] = vals[k];
    return true;
}

// Разбор "10.0.0.0/8" или "192.168.1.5" (без маски = /32). Опирается на строгий
// parse_ipv4, поэтому мусор вида "10.0.0.0/33" или "1.2.3.4.5/8" не проходит.
bool parse_cidr_v4(const std::string& s, unsigned net[4], int& bits) {
    const auto slash = s.find('/');
    std::string ip = (slash == std::string::npos) ? s : s.substr(0, slash);
    bits = 32;
    if (slash != std::string::npos) {
        const std::string b = s.substr(slash + 1);
        if (b.empty() || b.size() > 2) return false;
        for (char c : b) if (!std::isdigit((unsigned char)c)) return false;
        bits = std::stoi(b);
        if (bits < 0 || bits > 32) return false;
    }
    return parse_ipv4(ip, net);
}

// Адрес входит в сеть? Сравниваем как 32-битные числа под маской.
bool cidr_contains_v4(const std::string& cidr, const std::string& addr) {
    unsigned net[4], a[4];
    int bits = 0;
    if (!parse_cidr_v4(cidr, net, bits)) return false;
    if (!parse_ipv4(addr, a)) return false;
    const uint32_t nv = (net[0] << 24) | (net[1] << 16) | (net[2] << 8) | net[3];
    const uint32_t av = (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
    if (bits == 0) return true;                       // 0.0.0.0/0 - вся сеть
    const uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ~((1u << (32 - bits)) - 1);
    return (nv & mask) == (av & mask);
}

// Хост URL локальный (loopback/RFC1918/localhost)? Для offline-инварианта:
// внешние backend_url обязаны указывать внутрь контура, не в интернет.
// Строго отсекаем userinfo ("http://127.0.0.1@evil.com" -> host = evil.com) и
// проверяем именно IP/точное localhost, а не совпадение префикса строки.
static bool is_local_host(const std::string& url) {
    std::string h = url;
    auto p = h.find("://");
    if (p != std::string::npos) h = h.substr(p + 3);
    h = h.substr(0, h.find_first_of("/?#"));           // только authority
    auto at = h.rfind('@');                            // userinfo -> берём хост после '@'
    if (at != std::string::npos) h = h.substr(at + 1);
    if (h.empty()) return false;
    if (h.front() == '[') {                            // IPv6 в скобках [..]:port
        auto e = h.find(']');
        if (e == std::string::npos) return false;
        std::string v6 = h.substr(1, e - 1);
        for (auto& c : v6) c = (char)std::tolower((unsigned char)c);
        return v6 == "::1" || v6.rfind("fd", 0) == 0 || v6.rfind("fc", 0) == 0 ||
               v6.rfind("fe80", 0) == 0;
    }
    auto colon = h.rfind(':');                         // отбрасываем порт
    if (colon != std::string::npos) h = h.substr(0, colon);
    if (h.empty()) return false;
    {
        std::string lower = h;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower == "localhost") return true;
    }
    unsigned o[4];
    if (!parse_ipv4(h, o)) return false;               // не IPv4 и не localhost -> внешний
    if (o[0] == 127) return true;                              // 127.0.0.0/8 loopback
    if (o[0] == 10)  return true;                              // 10.0.0.0/8
    if (o[0] == 192 && o[1] == 168) return true;               // 192.168.0.0/16
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return true;  // 172.16.0.0/12
    if (o[0] == 169 && o[1] == 254) return true;               // 169.254.0.0/16 link-local (не в интернет)
    return false;
}

GatewayConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("infcore: не удалось открыть конфиг: " + path);

    std::stringstream ss;
    ss << f.rdbuf();

    json j;
    try {
        j = json::parse(ss.str(), /*cb*/ nullptr, /*allow_exceptions*/ true,
                        /*ignore_comments*/ true);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("infcore: ошибка разбора конфига: ") + e.what());
    }

    // Формальная валидация по встроенной JSON-Schema (fail-fast при старте).
    {
        json schema = json::parse(kGatewaySchemaJson, nullptr, true, true);
        auto errs = json_schema_validate(j, schema);
        if (!errs.empty()) {
            std::string msg = "infcore: конфиг не соответствует JSON-Schema:";
            for (const auto& e : errs) msg += "\n  - " + e;
            throw std::runtime_error(msg);
        }
    }

    GatewayConfig cfg;

    if (j.contains("server")) {
        const auto& s = j.at("server");
        cfg.host = s.value("host", cfg.host);
        cfg.port = s.value("port", cfg.port);
        cfg.max_concurrent_requests = s.value("max_concurrent_requests", cfg.max_concurrent_requests);
        cfg.request_timeout_ms = s.value("request_timeout_ms", cfg.request_timeout_ms);
        cfg.read_timeout_ms  = s.value("read_timeout_ms", cfg.read_timeout_ms);
        cfg.write_timeout_ms = s.value("write_timeout_ms", cfg.write_timeout_ms);
        cfg.max_body_bytes   = s.value("max_body_bytes", cfg.max_body_bytes);
        if (s.contains("trusted_proxies")) {
            for (const auto& p : s.at("trusted_proxies")) {
                const std::string cidr = p.get<std::string>();
                // Валидируем на старте, а не при каждом запросе: опечатка в CIDR
                // означала бы, что прокси молча не доверяют и весь аудит уезжает
                // в client_ip=127.0.0.1 - такое надо ловить fail-fast.
                unsigned net[4]; int bits = 0;
                if (!parse_cidr_v4(cidr, net, bits))
                    throw std::runtime_error("infcore: server.trusted_proxies: не IPv4/CIDR: " + cidr);
                cfg.trusted_proxies.push_back(cidr);
            }
        }
    }
    if (j.contains("security")) {
        const auto& s = j.at("security");
        cfg.rbac_enabled = s.value("rbac_enabled", cfg.rbac_enabled);
        if (s.contains("api_keys"))
            for (const auto& k : s.at("api_keys")) {
                std::string key = resolve_secret(k.get<std::string>());
                if (weak_secret(key))
                    throw std::runtime_error("infcore: слабый или заглушечный ключ в security.api_keys - задайте случайный ключ длиной не менее 24 символов (env:/file:)");
                cfg.api_keys.push_back(std::move(key));
            }
        if (s.contains("principals")) {
            for (const auto& p : s.at("principals")) {
                ApiKeyPrincipal ap;
                ap.api_key           = resolve_secret(p.value("api_key", std::string()));
                ap.principal.subject = p.value("subject", std::string());
                ap.principal.role    = p.value("role", std::string());
                if (ap.api_key.empty())
                    throw std::runtime_error("infcore: principal без api_key");
                if (weak_secret(ap.api_key))
                    throw std::runtime_error("infcore: слабый или заглушечный ключ у principal '" +
                        ap.principal.subject + "' - задайте случайный ключ длиной не менее 24 символов (env:/file:)");
                cfg.principals.push_back(std::move(ap));
            }
        }
        if (s.contains("roles")) {
            for (const auto& r : s.at("roles")) {
                Role role;
                role.name = r.value("name", std::string());
                if (role.name.empty())
                    throw std::runtime_error("infcore: role без name");
                if (r.contains("allow_models"))
                    for (const auto& m : r.at("allow_models")) role.allow_models.push_back(m.get<std::string>());
                if (r.contains("allow_endpoints"))
                    for (const auto& ep : r.at("allow_endpoints")) role.allow_endpoints.push_back(ep.get<std::string>());
                cfg.roles.push_back(std::move(role));
            }
        }
        if (s.contains("audit")) {
            const auto& a = s.at("audit");
            cfg.audit_sink    = a.value("sink", cfg.audit_sink);
            cfg.audit_path    = a.value("path", cfg.audit_path);
            cfg.audit_require = a.value("require", cfg.audit_require);
        }
    }
    if (j.contains("offline"))
        cfg.enforce_no_egress = j.at("offline").value("enforce_no_egress", true);
    if (j.contains("offline"))
        cfg.require_model_integrity = j.at("offline").value("require_model_integrity", cfg.require_model_integrity);

    if (j.contains("observability")) {
        const auto& o = j.at("observability");
        cfg.metrics_enabled = o.value("metrics_enabled", cfg.metrics_enabled);
        cfg.metrics_path = o.value("metrics_path", cfg.metrics_path);
        if (cfg.metrics_path.empty() || cfg.metrics_path.front() != '/')
            throw std::runtime_error("infcore: observability.metrics_path должен начинаться с '/'");
    }

    if (j.contains("runtime")) {
        const auto& r = j.at("runtime");
        cfg.llama_server_bin   = r.value("llama_server_bin", cfg.llama_server_bin);
        cfg.port_range_start   = r.value("port_range_start", cfg.port_range_start);
        cfg.idle_timeout_ms    = r.value("idle_timeout_ms", cfg.idle_timeout_ms);
        cfg.startup_timeout_ms = r.value("startup_timeout_ms", cfg.startup_timeout_ms);
        cfg.max_loaded_models  = r.value("max_loaded_models", cfg.max_loaded_models);
        cfg.max_parallel_starts = r.value("max_parallel_starts", cfg.max_parallel_starts);
        cfg.rate_limit_per_minute = r.value("rate_limit_per_minute", cfg.rate_limit_per_minute);
    }

    if (j.contains("models")) {
        for (const auto& m : j.at("models")) {
            ModelEntry e;
            e.logical_name   = m.value("logical_name", std::string());
            e.gguf_path      = m.value("gguf_path", std::string());
            e.arch           = m.value("arch", std::string());
            e.backend_url    = m.value("backend_url", std::string());
            e.upstream_model = m.value("upstream_model", e.logical_name);
            e.mmproj_path    = m.value("mmproj_path", std::string());
            e.sha256         = m.value("sha256", std::string());
            e.mmproj_sha256  = m.value("mmproj_sha256", std::string());
            e.license        = m.value("license", std::string());
            e.source         = m.value("source", std::string());
            e.size_bytes     = m.value("size_bytes", int64_t{0});
            e.mmproj_size_bytes = m.value("mmproj_size_bytes", int64_t{0});
            e.modality       = parse_modality(m.value("modality", std::string("text")));
            e.enabled        = m.value("enabled", true);
            e.n_ctx          = m.value("n_ctx", 8192);
            e.n_gpu_layers   = m.value("n_gpu_layers", 0);
            if (e.logical_name.empty())
                throw std::runtime_error("infcore: model без logical_name");
            cfg.models.push_back(std::move(e));
        }
    }

    if (cfg.api_keys.empty() && cfg.principals.empty())
        throw std::runtime_error("infcore: нет ни security.api_keys, ни security.principals — нужен хотя бы один ключ");
    if (cfg.models.empty())
        throw std::runtime_error("infcore: models пуст");
    if (cfg.audit_require && cfg.audit_sink == "none")
        throw std::runtime_error("infcore: security.audit.require=true несовместим с sink=none");

    std::set<std::string> model_names;
    for (const auto& m : cfg.models) {
        if (!model_names.insert(m.logical_name).second)
            throw std::runtime_error("infcore: duplicate model logical_name: " + m.logical_name);
    }
    std::set<std::string> role_names;
    for (const auto& r : cfg.roles) {
        if (!role_names.insert(r.name).second)
            throw std::runtime_error("infcore: duplicate role name: " + r.name);
        for (const auto& model : r.allow_models) {
            if (model != "*" && !model_names.count(model))
                throw std::runtime_error("infcore: role '" + r.name +
                    "' references unknown model '" + model + "'");
        }
    }
    std::set<std::string> api_keys_seen;
    for (const auto& k : cfg.api_keys) {
        if (!api_keys_seen.insert(k).second)
            throw std::runtime_error("infcore: duplicate API key in security.api_keys");
    }
    for (const auto& ap : cfg.principals) {
        if (!api_keys_seen.insert(ap.api_key).second)
            throw std::runtime_error("infcore: duplicate API key for principal '" + ap.principal.subject + "'");
    }

    // При включённом RBAC роль каждого principal должна быть объявлена в security.roles.
    if (cfg.rbac_enabled) {
        for (const auto& ap : cfg.principals) {
            bool found = false;
            for (const auto& r : cfg.roles) if (r.name == ap.principal.role) { found = true; break; }
            if (!found)
                throw std::runtime_error("infcore: роль '" + ap.principal.role +
                    "' principal'а '" + ap.principal.subject + "' не объявлена в security.roles");
        }
    }

    // Оркестрация/Docker: host и port можно переопределить окружением, НЕ редактируя
    // смонтированный read-only конфиг. В контейнере INFCORE_HOST=0.0.0.0 (наружу
    // публикуется только loopback хоста), на bare-metal обычно не задаётся.
    if (const char* h = std::getenv("INFCORE_HOST"); h && *h) cfg.host = h;
    if (const char* p = std::getenv("INFCORE_PORT"); p && *p) {
        int v = std::atoi(p);
        if (v < 1 || v > 65535)
            throw std::runtime_error("infcore: INFCORE_PORT вне диапазона 1..65535: " + std::string(p));
        cfg.port = v;
    }

    for (const auto& m : cfg.models) {
        const bool vlm = (m.modality == Modality::Vision);
        const bool rerank = (m.modality == Modality::Rerank);
        if (!m.backend_url.empty()) {
            // Внешний бэкенд: при жёстком offline обязан быть локальным (не в интернет).
            if (cfg.enforce_no_egress && !is_local_host(m.backend_url))
                throw std::runtime_error("infcore: enforce_no_egress: backend_url модели '" +
                    m.logical_name + "' не локальный: " + m.backend_url);
            continue;
        }
        // Управляемая модель (без backend_url) требует llama_server_bin и gguf_path.
        if (cfg.llama_server_bin.empty())
            throw std::runtime_error("infcore: модель '" + m.logical_name +
                "' без backend_url требует runtime.llama_server_bin");
        if (m.gguf_path.empty())
            throw std::runtime_error("infcore: управляемая модель '" + m.logical_name +
                "' требует gguf_path");
        // Vision без проектора запустилась бы битой - ловим на старте.
        if (vlm && m.mmproj_path.empty())
            throw std::runtime_error("infcore: модель '" + m.logical_name +
                "' модальности vision требует mmproj_path");
        if (rerank && m.n_ctx < 512)
            throw std::runtime_error("infcore: модель '" + m.logical_name +
                "' модальности rerank требует n_ctx >= 512");
        validate_artifact_integrity(m.gguf_path, m.sha256, m.size_bytes,
                                    cfg.require_model_integrity,
                                    "model '" + m.logical_name + "'");
        if (!m.mmproj_path.empty())
            validate_artifact_integrity(m.mmproj_path, m.mmproj_sha256, m.mmproj_size_bytes,
                                        cfg.require_model_integrity,
                                        "mmproj '" + m.logical_name + "'");
    }

    return cfg;
}

}  // namespace infcore
