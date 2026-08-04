// infcore — MIT licence (see LICENSE).
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
    if (writer_.joinable()) writer_.join();   // flushes the rest of the queue before exiting
    if (fd_ >= 0) ::close(fd_);
}

bool AuditLog::open(const std::string& path) {
    // O_CLOEXEC: the child llama-server processes must not inherit the journal fd.
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (fd_ < 0) return false;
    path_ = path;   // before the writer starts: from here on only the writer reads path_
    writer_ = std::thread([this] { writer_loop(); });
    return true;
}

// Reopens the journal after rotation. Called ONLY from the writer thread: fd_ is read and
// written without a mutex, and the writer is its sole owner once open() has returned.
void AuditLog::reopen_if_requested() {
    if (!reopen_requested_.exchange(false, std::memory_order_acq_rel)) return;

    const int nfd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0640);
    if (nfd < 0) {
        // Deliberately NOT failing closed here: the previous fd is still alive and events
        // keep being committed with fsync (into the already-renamed file), so the invariant
        // "no request goes unrecorded" holds. Dropping traffic because a rotation failed
        // would be a cure worse than the disease. Operators see the counter and stderr.
        reopen_failures_.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr,
            "infcore: audit: could not reopen journal '%s' (%s); still writing to "
            "the previous file — rotation did not actually happen.\n",
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
    if (stop_ || writer_failed_) return;   // the writer is dead -> do not block forever
    const unsigned long long seq = ++enqueued_seq_;
    queue_.push_back(std::move(line));
    cv_work_.notify_one();
    // Wait until our line is committed to disk (sharing the fsync with the rest of the
    // batch). The queue is bounded by the number of concurrent requests — every producer
    // blocks until commit — so it cannot grow without limit.
    cv_commit_.wait(lock, [&] { return committed_seq_ >= seq || writer_failed_ || stop_; });
}

// The writer thread: sleeps until there is work, then TAKES THE WHOLE queue as one batch,
// writes it and issues a single fsync — group commit. After the fsync it advances
// committed_seq_ and wakes every producer waiting on that batch.
void AuditLog::writer_loop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_work_.wait(lock, [&] { return !queue_.empty() || stop_; });
        if (queue_.empty()) {
            if (stop_) return;
            continue;
        }
        std::deque<std::string> batch;
        batch.swap(queue_);                       // took everything -> one fsync for all of it
        const unsigned long long upto = enqueued_seq_;
        lock.unlock();

        // Rotation: reopen BEFORE writing the batch so the events land in the new file.
        // At this point fd_ is guaranteed to be unused by anyone else.
        reopen_if_requested();

        bool ok = true;
        int  werr = 0;
        for (const auto& line : batch) {
            ssize_t off = 0, n = (ssize_t)line.size();
            while (off < n) {
                ssize_t w = ::write(fd_, line.data() + off, n - off);
                if (w < 0) {
                    if (errno == EINTR) continue;  // retry, rather than lose the record
                    werr = errno; ok = false; break;  // fatal I/O error
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
            writer_failed_ = true;   // release every waiter and stop blocking log() from now on
            failed_.store(true, std::memory_order_release);
            // Loudly: otherwise the "no traffic without audit" invariant would degrade in
            // silence. Once the gateway sees failed() it fails closed (503) whenever
            // audit.require=true.
            std::fprintf(stderr,
                "infcore: CRITICAL: audit journal write failed (%s); further events are "
                "NOT recorded. With audit.require=true the gateway stops serving traffic.\n",
                std::strerror(werr));
        }
        cv_commit_.notify_all();
        if (writer_failed_) return;  // the journal is broken; producers will see writer_failed_
    }
}

}  // namespace infcore
