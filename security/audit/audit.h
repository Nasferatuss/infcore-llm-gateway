// infcore — MIT licence (see LICENSE). An immutable audit journal recording who did what,
// when, against which model and with what outcome. Local writes only (append-only JSONL,
// fsync); nothing is exported anywhere. True immutability is an OS-level concern
// (chattr +a / auditd); what this file provides is O_APPEND + fsync.
//
// Durability plus throughput: event writing is moved to a dedicated writer thread with
// group commit. log() puts a line on the queue and BLOCKS until that line is physically on
// disk (fsync). The writer drains the whole accumulated queue as one batch and issues ONE
// fsync for all of it, so N concurrent requests share a single fsync instead of doing N
// sequential ones. No event is ever lost (the fsync happens before log() returns), and a
// burst of traffic is not serialised on the disk.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace infcore {

struct AuditEvent {
    std::string subject;
    std::string role;
    std::string endpoint;
    std::string model;
    std::string model_sha256;
    std::string request_id;
    std::string backend_id;
    std::string client_ip;
    std::string decision;   // "allow" | "deny" | "error"
    std::string reason;
    int         status = 0;
    long long   latency_ms = -1;
    long long   request_bytes = -1;
    long long   response_bytes = -1;
    long long   prompt_tokens = -1;
    long long   completion_tokens = -1;
    long long   total_tokens = -1;
};

class AuditLog {
public:
    ~AuditLog();
    bool open(const std::string& path);   // false if the file could not be opened
    void log(const AuditEvent& e);
    bool enabled() const { return fd_ >= 0; }
    // true if the writer thread died fatally at runtime (ENOSPC/EIO), meaning further
    // events are NOT recorded. Read without locking from the request threads so that the
    // gateway can fail closed (serve no traffic without audit when audit.require=true).
    bool failed() const { return failed_.load(std::memory_order_acquire); }

    // Requests that the journal be reopened at the same path (logrotate: postrotate
    // kill -HUP). Async-signal-safe: it only raises a lock-free flag, so it can be called
    // directly from a signal handler. The reopen itself is performed by the writer thread —
    // the sole owner of fd_ — before it writes the next batch.
    //
    // The actual reopen is deferred until the next event. Nothing is lost by this: with no
    // events there is nothing to write to the renamed file, and the very first new event
    // lands in the new one. The writer cannot be woken from the handler —
    // condition_variable::notify is not async-signal-safe.
    void request_reopen() { reopen_requested_.store(true, std::memory_order_release); }

    // Counters for /metrics: how many times the journal was reopened and how many reopens
    // failed (the latter is worth alerting on — rotation is effectively not working).
    unsigned long long reopens() const { return reopens_.load(std::memory_order_relaxed); }
    unsigned long long reopen_failures() const { return reopen_failures_.load(std::memory_order_relaxed); }

private:
    void writer_loop();                   // group commit: one batch, one fsync
    void reopen_if_requested();           // writer thread only (the owner of fd_)

    std::string              path_;       // journal path for reopen; written in open() before the writer starts
    std::atomic<bool>        reopen_requested_{false};
    std::atomic<unsigned long long> reopens_{0};
    std::atomic<unsigned long long> reopen_failures_{0};

    std::mutex               mu_;
    std::condition_variable  cv_work_;     // wakes the writer: work arrived, or stop
    std::condition_variable  cv_commit_;   // wakes waiters: their line is on disk
    std::deque<std::string>  queue_;       // lines waiting to be written
    unsigned long long       enqueued_seq_  = 0;   // last sequence number handed out
    unsigned long long       committed_seq_ = 0;   // last sequence number committed (fsynced)
    bool                     stop_          = false;
    bool                     writer_failed_ = false; // fatal I/O error -> stop blocking forever
    std::atomic<bool>        failed_{false};         // the same, but for lock-free reads from outside
    std::thread              writer_;
    int                      fd_ = -1;
};

}  // namespace infcore
