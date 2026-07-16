// infcore gateway — корпоративная лицензия.
// Загрузка конфигурации gateway из JSON (валиден и как YAML-подмножество).
#pragma once

#include <string>
#include <vector>

#include "registry/model_registry.h"
#include "security/authn/authn.h"
#include "security/rbac/rbac.h"

namespace infcore {

// API-ключ + кому он принадлежит (subject/role). Источник identity, offline.
struct ApiKeyPrincipal {
    std::string api_key;
    Principal   principal;
};

struct GatewayConfig {
    std::string host = "127.0.0.1";
    int         port = 8080;
    int         max_concurrent_requests = 64;   // размер пула воркеров = потолок одновременных
                                                 // запросов (SSE держит воркер на весь стрим)
    int         request_timeout_ms = 120000;     // таймаут запроса к бэкенду (upstream)
    // Фронтовые лимиты (защита от slowloris/OOM на публичном периметре):
    int         read_timeout_ms  = 30000;        // медленная отправка запроса клиентом -> разрыв
    int         write_timeout_ms = 120000;       // мёртвый потребитель SSE -> освобождаем воркер
    long        max_body_bytes   = 8 * 1024 * 1024;  // потолок тела запроса (иначе OOM на json::parse)
    bool        rbac_enabled = true;
    bool        enforce_no_egress = true;
    bool        require_model_integrity = false;

    // Доверенные reverse-proxy (IPv4 или CIDR). ТОЛЬКО для запросов, пришедших с
    // этих адресов, реальный клиент берётся из X-Real-IP / X-Forwarded-For;
    // остальным заголовок не верим (иначе любой клиент подделает себе client_ip
    // в audit-журнале). Пусто (по умолчанию) = не доверять никому: client_ip
    // всегда равен peer'у соединения.
    //
    // Без этого за TLS-прокси КАЖДАЯ запись аудита получает client_ip=127.0.0.1
    // и журнал теряет измерение «откуда».
    std::vector<std::string> trusted_proxies;

    // runtime: lazy-подъём управляемых бэкендов (модели с пустым backend_url)
    std::string llama_server_bin;            // путь к нашему llama-server (из сборки)
    int         port_range_start   = 8100;
    int         idle_timeout_ms    = 300000;
    int         startup_timeout_ms = 120000;
    int         max_loaded_models  = 0;
    int         max_parallel_starts = 1;
    int         rate_limit_per_minute = 0;     // 0 = disabled; per subject/key after auth

    std::vector<std::string> api_keys;   // legacy: плоский список ключей (роль admin)
    std::vector<ModelEntry>  models;

    // RBAC: principals (ключ -> subject/role) и роли (allowlists моделей/эндпоинтов).
    std::vector<ApiKeyPrincipal> principals;
    std::vector<Role>            roles;

    // audit: локальный append-only журнал.
    std::string audit_sink = "file";                    // "file" | "none"
    std::string audit_path = "/var/log/infcore/audit.log";
    bool        audit_require = true;                    // sink=file и журнал не открылся -> fail-fast

    bool        metrics_enabled = true;
    std::string metrics_path = "/metrics";
};

// Загружает конфиг из файла. Бросает std::runtime_error при ошибке/невалидности.
GatewayConfig load_config(const std::string& path);

// "10.0.0.0/8" | "192.168.1.5" (без маски = /32) -> сеть + длина префикса.
bool parse_cidr_v4(const std::string& s, unsigned net[4], int& bits);
// Входит ли addr в сеть cidr. false, если любая из строк не разбирается.
bool cidr_contains_v4(const std::string& cidr, const std::string& addr);

}  // namespace infcore
