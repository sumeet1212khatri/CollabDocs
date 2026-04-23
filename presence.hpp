#pragma once
/*
 * CollabDocs C++ — Presence Manager
 *
 * Tracks active users per doc:
 *   color, name, cursor_pos, websocket connection pointer.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <functional>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

namespace collab {

static const std::vector<std::string> USER_COLORS = {
    "#E63946","#2196F3","#FF9800","#9C27B0","#00BCD4",
    "#4CAF50","#FF5722","#3F51B5","#009688","#F44336",
    "#8BC34A","#FF4081","#00ACC1","#7E57C2","#FFA726",
};

static const std::vector<std::string> USER_NAMES = {
    "Alice","Bob","Carol","Dave","Eve","Frank","Grace",
    "Hank","Iris","Jack","Kate","Leo","Mia","Nick","Olivia",
    "Pete","Quinn","Rosa","Sam","Tara",
};

static constexpr double STALE_TIMEOUT_SEC = 60.0;

inline double now_sec() {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ─── WebSocket abstraction ────────────────────────────────────────────────────
// We store a send-callback so the presence manager is decoupled from
// Boost.Beast specifics (easier to test too).

using SendFn = std::function<void(const std::string&)>;

// ─── UserPresence ─────────────────────────────────────────────────────────────

struct UserPresence {
    std::string user_id;
    std::string name;
    std::string color;
    int         cursor_pos     = 0;
    int         selection_start = -1;
    int         selection_end   = -1;
    double      last_seen      = 0.0;
    SendFn      send_fn;        // call this to push JSON to this client

    void ping() { last_seen = now_sec(); }

    nlohmann::json to_json() const {
        return {
            {"user_id",          user_id},
            {"name",             name},
            {"color",            color},
            {"cursor_pos",       cursor_pos},
            {"selection_start",  selection_start},
            {"selection_end",    selection_end},
        };
    }
};

// ─── PresenceManager ─────────────────────────────────────────────────────────

class PresenceManager {
public:
    // Called when a user opens a document
    UserPresence& join(const std::string& doc_id,
                       const std::string& user_id,
                       SendFn             send_fn)
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& users = doc_users_[doc_id];

        auto it = users.find(user_id);
        if (it == users.end()) {
            UserPresence p;
            p.user_id  = user_id;
            p.color    = pick_color(doc_id);
            p.name     = USER_NAMES[
                std::hash<std::string>{}(user_id) % USER_NAMES.size()];
            p.last_seen = now_sec();
            p.send_fn  = std::move(send_fn);
            users[user_id] = std::move(p);
        } else {
            it->second.send_fn = std::move(send_fn);
            it->second.ping();
        }
        return users[user_id];
    }

    void leave(const std::string& doc_id, const std::string& user_id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto dit = doc_users_.find(doc_id);
        if (dit == doc_users_.end()) return;
        auto uit = dit->second.find(user_id);
        if (uit == dit->second.end()) return;
        release_color(doc_id, uit->second.color);
        dit->second.erase(uit);
    }

    void update_cursor(const std::string& doc_id, const std::string& user_id,
                       int pos, int sel_start = -1, int sel_end = -1) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& users = doc_users_[doc_id];
        auto it = users.find(user_id);
        if (it == users.end()) return;
        it->second.cursor_pos      = pos;
        it->second.selection_start = sel_start;
        it->second.selection_end   = sel_end;
        it->second.ping();
    }

    // Get snapshot of a user's presence data (thread-safe)
    std::optional<UserPresence> get_user(const std::string& doc_id,
                                          const std::string& user_id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto dit = doc_users_.find(doc_id);
        if (dit == doc_users_.end()) return std::nullopt;
        auto uit = dit->second.find(user_id);
        if (uit == dit->second.end()) return std::nullopt;
        return uit->second;
    }

    // Get list of active user JSON objects
    std::vector<nlohmann::json> get_users_json(const std::string& doc_id) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<nlohmann::json> out;
        auto dit = doc_users_.find(doc_id);
        if (dit == doc_users_.end()) return out;
        double cutoff = now_sec() - STALE_TIMEOUT_SEC;
        for (auto& [uid, p] : dit->second)
            if (p.last_seen >= cutoff)
                out.push_back(p.to_json());
        return out;
    }

    // Broadcast to all users in doc except one; calls their send_fn.
    // Returns list of user_ids whose send_fn threw (dead connections).
    std::vector<std::string> broadcast(const std::string& doc_id,
                                        const std::string& json_str,
                                        const std::string& exclude = "") {
        // Snapshot so we don't hold lock while calling send_fn
        std::vector<std::pair<std::string, SendFn>> targets;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto dit = doc_users_.find(doc_id);
            if (dit == doc_users_.end()) return {};
            for (auto& [uid, p] : dit->second)
                if (uid != exclude && p.send_fn)
                    targets.emplace_back(uid, p.send_fn);
        }

        std::vector<std::string> dead;
        for (auto& [uid, fn] : targets) {
            try { fn(json_str); }
            catch (...) { dead.push_back(uid); }
        }
        return dead;
    }

    void send_to_user(const std::string& doc_id,
                      const std::string& user_id,
                      const std::string& json_str) {
        std::lock_guard<std::mutex> lk(mu_);
        auto dit = doc_users_.find(doc_id);
        if (dit == doc_users_.end()) return;
        auto uit = dit->second.find(user_id);
        if (uit == dit->second.end() || !uit->second.send_fn) return;
        try { uit->second.send_fn(json_str); } catch (...) {}
    }

    size_t user_count(const std::string& doc_id) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = doc_users_.find(doc_id);
        return (it != doc_users_.end()) ? it->second.size() : 0;
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string,
        std::unordered_map<std::string, UserPresence>> doc_users_;
    std::unordered_map<std::string,
        std::unordered_set<std::string>> used_colors_;

    std::string pick_color(const std::string& doc_id) {
        auto& used = used_colors_[doc_id];
        for (auto& c : USER_COLORS)
            if (!used.count(c)) { used.insert(c); return c; }
        // All used — recycle
        auto& c = USER_COLORS[used.size() % USER_COLORS.size()];
        return c;
    }

    void release_color(const std::string& doc_id, const std::string& color) {
        auto it = used_colors_.find(doc_id);
        if (it != used_colors_.end()) it->second.erase(color);
    }
};

inline PresenceManager g_presence;

} // namespace collab
