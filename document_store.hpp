#pragma once
/*
 * CollabDocs C++ — Document Store (v2, with broadcast batching)
 *
 * Changes from v1:
 *   - Added pending_broadcast queue + last_flush timestamp per Document
 *   - flush_broadcast_locked() collapses pending ops into one "batch" JSON
 *   - Callers decide when to flush (size >= 20 OR age >= 8 ms)
 */

#include "ot_engine.hpp"
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <chrono>
#include <optional>
#include <memory>

namespace collab {

static constexpr int SNAPSHOT_EVERY_N = 100;
static constexpr int MAX_OP_LOG       = 2000;
static constexpr int BATCH_MAX_OPS    = 20;           // flush when queue hits this
static constexpr int BATCH_MAX_MS     = 8;            // or when this many ms old

// ─── Snapshot ────────────────────────────────────────────────────────────────

struct Snapshot {
    int         version;
    std::string content;
    double      timestamp;
};

// ─── Document ────────────────────────────────────────────────────────────────

struct Document {
    std::string doc_id;
    std::string title   = "Untitled Document";
    std::string content;
    int         version = 0;

    std::vector<std::pair<int, Operation>> op_log;
    std::vector<Snapshot>                 snapshots;

    double created_at;
    double updated_at;

    // ── Broadcast batch state (guarded by mu) ────────────────────────────────
    std::vector<nlohmann::json>                          pending_broadcast;
    std::chrono::steady_clock::time_point                last_flush{};
    // broadcast_fn is set by the server layer so Document doesn't depend on
    // Boost.Beast directly.  Signature: void(std::string json_batch)
    std::function<void(const std::string& doc_id,
                       const std::string& json_str,
                       const std::string& exclude)>      broadcast_fn;

    mutable std::mutex mu; // per-doc lock

    explicit Document(std::string id)
        : doc_id(std::move(id))
    {
        auto now = std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        created_at = updated_at = now;
        last_flush = std::chrono::steady_clock::now();
    }

    // Non-copyable (has mutex)
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // ── Must be called with mu held ──────────────────────────────────────────
    Operation apply_op_locked(Operation op) {
        if (op.base_version < version)
            op = transform_against_log(op, op_log, op.base_version, version);

        content = apply_operation(content, op);
        version++;
        op.base_version = version;

        op_log.emplace_back(version, op);
        if (static_cast<int>(op_log.size()) > MAX_OP_LOG)
            op_log.erase(op_log.begin(),
                         op_log.begin() + (static_cast<int>(op_log.size()) - MAX_OP_LOG));

        updated_at = std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (version % SNAPSHOT_EVERY_N == 0)
            snapshots.push_back({version, content, updated_at});

        return op;
    }

    // Queue one outgoing op for broadcast and flush if thresholds are met.
    // Must be called with mu held.
    // Returns true if a flush was triggered (caller can log/metric if wanted).
    bool enqueue_broadcast_locked(nlohmann::json op_json,
                                  const std::string& exclude_user)
    {
        pending_broadcast.push_back(std::move(op_json));

        auto now = std::chrono::steady_clock::now();
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - last_flush).count();

        bool full   = static_cast<int>(pending_broadcast.size()) >= BATCH_MAX_OPS;
        bool stale  = age_ms >= BATCH_MAX_MS;

        if (full || stale) {
            flush_broadcast_locked(exclude_user);
            return true;
        }
        return false;
    }

    // Flush pending ops as a single "batch" message.
    // Must be called with mu held; clears pending_broadcast.
    void flush_broadcast_locked(const std::string& exclude_user = "") {
        if (pending_broadcast.empty()) return;
        if (!broadcast_fn) { pending_broadcast.clear(); return; }

        nlohmann::json batch;
        if (pending_broadcast.size() == 1) {
            // Single op — send as-is (no extra wrapper; clients don't need
            // to handle "batch" for the common single-user case).
            batch = pending_broadcast[0];
        } else {
            batch = {{"type", "batch"}, {"ops", pending_broadcast}};
        }

        pending_broadcast.clear();
        last_flush = std::chrono::steady_clock::now();

        // Release mu before calling broadcast_fn to avoid potential deadlock.
        // We capture what we need by value.
        auto fn   = broadcast_fn;
        auto did  = doc_id;
        auto dump = batch.dump();
        auto excl = exclude_user;

        // Unlock-and-call pattern: we're still inside mu here, so we post
        // the actual call outside.  The simplest safe approach is to store
        // the closure and call it AFTER the lock is released by the caller.
        // We do that by assigning to a thread_local trampoline.
        pending_after_unlock_.emplace_back([fn, did, dump, excl]() {
            fn(did, dump, excl);
        });
    }

    // Call this AFTER releasing mu to fire any deferred broadcast callbacks.
    void fire_pending_broadcasts() {
        for (auto& cb : pending_after_unlock_) cb();
        pending_after_unlock_.clear();
    }

    nlohmann::json get_state_json() const;

private:
    // Deferred callbacks to be called once mu is released.
    std::vector<std::function<void()>> pending_after_unlock_;
};

// ─── Document Store ───────────────────────────────────────────────────────────

class DocumentStore {
public:
    std::shared_ptr<Document> get_or_create(const std::string& doc_id) {
        std::lock_guard<std::mutex> lk(store_mu_);
        auto it = docs_.find(doc_id);
        if (it != docs_.end()) return it->second;
        auto doc = std::make_shared<Document>(doc_id);
        docs_[doc_id] = doc;
        return doc;
    }

    std::shared_ptr<Document> get(const std::string& doc_id) {
        std::lock_guard<std::mutex> lk(store_mu_);
        auto it = docs_.find(doc_id);
        return (it != docs_.end()) ? it->second : nullptr;
    }

    void update_title(const std::string& doc_id, const std::string& title) {
        auto doc = get(doc_id);
        if (doc) {
            std::lock_guard<std::mutex> lk(doc->mu);
            doc->title = title;
        }
    }

    std::vector<nlohmann::json> list_docs();

    size_t size() const {
        std::lock_guard<std::mutex> lk(store_mu_);
        return docs_.size();
    }

private:
    mutable std::mutex store_mu_;
    std::unordered_map<std::string, std::shared_ptr<Document>> docs_;
};

// Global singleton
inline DocumentStore g_store;

} // namespace collab
