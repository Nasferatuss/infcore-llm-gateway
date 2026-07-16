// infcore gateway — корпоративная лицензия.
// OpenAI-совместимый gateway: control-plane (auth/registry/routing/metrics) перед
// бэкендами llama-server. Прокси с passthrough SSE для stream-ответов.
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

// Forward-декларация вместо #include "httplib.h": заголовок шлюза не должен тащить
// за собой HTTP-библиотеку (её включает только server.cpp, а юнит-тесты собирают
// config.cpp/json_schema.cpp без неё).
namespace httplib { struct Request; }

namespace infcore {

class GatewayServer {
public:
    explicit GatewayServer(GatewayConfig cfg);
    int run();   // блокирующий; возвращает код выхода

private:
    GatewayConfig cfg_;
    ModelRegistry registry_;
    std::unique_ptr<BackendSupervisor> supervisor_;

    // security
    Authenticator authn_;
    Authorizer    rbac_;
    AuditLog      audit_;

    // примитивные метрики (pull, /metrics)
    std::mutex                                metrics_mu_;
    std::map<std::string, std::atomic<long>>  counters_;

    // Гистограмма латентности запросов. Границы (сек) подобраны под инференс LLM:
    // интерес не в микросекундах, а в диапазоне «доли секунды -> минуты», включая
    // холодный старт бэкенда. Копится под metrics_mu_ вместе со счётчиками: одна
    // запись на запрос, на фоне инференса стоимость блокировки неразличима.
    struct LatencyHist {
        static constexpr size_t NBOUNDS = 12;
        static const double bounds[NBOUNDS];        // верхние границы, сек
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

    // Реальный IP клиента для audit-журнала. За доверенным reverse-proxy это
    // X-Real-IP / X-Forwarded-For, иначе - peer соединения. См. cfg_.trusted_proxies:
    // заголовкам верим ТОЛЬКО от доверенных прокси, иначе client_ip подделывается
    // одним лишним заголовком в запросе.
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
