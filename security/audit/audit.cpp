// infcore — корп. лицензия.
#include "audit/audit.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace infcore {

namespace {
std::string utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}
}  // namespace

AuditLog::~AuditLog() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_work_.notify_all();
    cv_commit_.notify_all();
    if (writer_.joinable()) writer_.join();   // дописывает остаток очереди перед выходом
    if (fd_ >= 0) ::close(fd_);
}

bool AuditLog::open(const std::string& path) {
    // O_CLOEXEC: дочерние llama-server не должны наследовать fd журнала.
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd_ < 0) return false;
    path_ = path;   // до старта писателя: дальше path_ читает только он
    writer_ = std::thread([this] { writer_loop(); });
    return true;
}

// Переоткрытие журнала после ротации. Вызывается ТОЛЬКО из потока-писателя: fd_
// пишется/читается без мьютекса, и писатель — его единственный владелец после open().
void AuditLog::reopen_if_requested() {
    if (!reopen_requested_.exchange(false, std::memory_order_acq_rel)) return;

    const int nfd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (nfd < 0) {
        // Осознанно НЕ уходим в fail-closed: прежний fd жив, события продолжают
        // фиксироваться с fsync (пусть и в уже переименованный файл), т.е. инвариант
        // «ни один запрос не остаётся без записи» цел. Ронять трафик из-за неудачной
        // ротации значило бы лечить хуже болезни. Ops видит счётчик и stderr.
        reopen_failures_.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
            "infcore: audit: не удалось переоткрыть журнал '%s' (%s); продолжаем "
            "писать в прежний файл — ротация де-факто не состоялась.\n",
            path_.c_str(), std::strerror(errno));
        return;
    }
    ::close(fd_);
    fd_ = nfd;
    reopens_.fetch_add(1, std::memory_order_relaxed);
}

void AuditLog::log(const AuditEvent& e) {
    if (fd_ < 0) return;
    json j = {
        {"ts", utc_now()},
        {"subject", e.subject},
        {"role", e.role},
        {"request_id", e.request_id},
        {"endpoint", e.endpoint},
        {"model", e.model},
        {"model_sha256", e.model_sha256},
        {"backend_id", e.backend_id},
        {"client_ip", e.client_ip},
        {"decision", e.decision},
        {"reason", e.reason},
        {"status", e.status},
    };
    if (e.latency_ms >= 0) j["latency_ms"] = e.latency_ms;
    if (e.request_bytes >= 0) j["request_bytes"] = e.request_bytes;
    if (e.response_bytes >= 0) j["response_bytes"] = e.response_bytes;
    if (e.prompt_tokens >= 0) j["prompt_tokens"] = e.prompt_tokens;
    if (e.completion_tokens >= 0) j["completion_tokens"] = e.completion_tokens;
    if (e.total_tokens >= 0) j["total_tokens"] = e.total_tokens;
    std::string line = j.dump();
    line.push_back('\n');

    std::unique_lock<std::mutex> lock(mu_);
    if (stop_ || writer_failed_) return;   // писатель мёртв -> не залипаем навсегда
    const unsigned long long seq = ++enqueued_seq_;
    queue_.push_back(std::move(line));
    cv_work_.notify_one();
    // Ждём, пока наша запись зафиксирована на диске (делим fsync с соседями по батчу).
    // Очередь при этом ограничена числом одновременных запросов (каждый продьюсер
    // блокируется до коммита), так что расти неограниченно не может.
    cv_commit_.wait(lock, [&] { return committed_seq_ >= seq || writer_failed_ || stop_; });
}

// Поток-писатель: спит до появления работы, затем ЗАБИРАЕТ ВСЮ очередь одним
// батчем, пишет её и делает один fsync -> group-commit. После fsync поднимает
// committed_seq_ и будит всех ждущих продьюсеров этого батча.
void AuditLog::writer_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_work_.wait(lock, [&] { return !queue_.empty() || stop_; });
        if (queue_.empty()) {
            if (stop_) return;
            continue;
        }
        std::deque<std::string> batch;
        batch.swap(queue_);                       // забрали всё -> один fsync на всех
        const unsigned long long upto = enqueued_seq_;
        lock.unlock();

        // Ротация: переоткрываем ДО записи батча, чтобы события ушли уже в новый
        // файл. Здесь fd_ гарантированно никем не используется.
        reopen_if_requested();

        bool ok = true;
        int  werr = 0;
        for (const auto& line : batch) {
            ssize_t off = 0, n = (ssize_t)line.size();
            while (off < n) {
                ssize_t w = ::write(fd_, line.data() + off, n - off);
                if (w < 0) {
                    if (errno == EINTR) continue;  // повтор, а не потеря записи
                    werr = errno; ok = false; break;  // фатальная I/O-ошибка
                }
                off += w;
            }
            if (!ok) break;
        }
        if (ok && ::fsync(fd_) < 0) { werr = errno; ok = false; }

        lock.lock();
        if (ok) {
            committed_seq_ = upto;
        } else {
            writer_failed_ = true;   // разблокируем всех ждущих и больше не блокируем log()
            failed_.store(true, std::memory_order_release);
            // Громко: инвариант «нет трафика без аудита» иначе деградировал бы молча.
            // Шлюз, увидев failed(), начнёт fail-closed (503) при audit.require=true.
            std::fprintf(stderr,
                "infcore: КРИТИЧНО: сбой записи audit-журнала (%s); дальнейшие события "
                "НЕ фиксируются. При audit.require=true шлюз перестаёт отдавать трафик.\n",
                std::strerror(werr));
        }
        cv_commit_.notify_all();
        if (writer_failed_) return;  // журнал сломан; продьюсеры увидят writer_failed_
    }
}

}  // namespace infcore
