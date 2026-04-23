#pragma once
/*
 * CollabDocs C++ — Document Store
 *
 * Manages in-memory documents with:
 *   - Snapshot every N ops (fast recovery)
 *   - OT application with per-doc mutex
 *   - Bounded op_log (last 2000 ops)
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

    mutable std::mutex mu; // per-doc lock

    explicit Document(std::string id)
        : doc_id(std::move(id))
    {
        auto now = std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        created_at = updated_at = now;
    }

    // Non-copyable (has mutex)
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Must be called with mu held
    Operation apply_op_locked(Operation op) {
        // Transform if client is behind
        if (op.base_version < version)
            op = transform_against_log(op, op_log, op.base_version, version);

        // Apply to content
        content = apply_operation(content, op);
        version++;
        op.base_version = version; // stamp

        // Store in log
        op_log.emplace_back(version, op);

        // Bound log
        if (static_cast<int>(op_log.size()) > MAX_OP_LOG)
            op_log.erase(op_log.begin(),
                         op_log.begin() + (static_cast<int>(op_log.size()) - MAX_OP_LOG));

        // Update timestamp
        updated_at = std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Snapshot
        if (version % SNAPSHOT_EVERY_N == 0)
            snapshots.push_back({version, content, updated_at});

        return op;
    }

    nlohmann::json get_state_json() const;
};

// ─── Document Store ───────────────────────────────────────────────────────────

class DocumentStore {
public:
    // Returns existing or newly created document (thread-safe)
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
