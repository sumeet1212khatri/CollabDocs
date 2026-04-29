/*
 * CollabDocs C++ — Main Server (v2)
 *
 * Improvements over v1:
 *   1. Strand-driven write queue (no spin-lock atomic)
 *   2. Op broadcast batching (8 ms / 20 ops window) via Document::enqueue_broadcast_locked
 *   3. File-descriptor limit raised at startup
 *   4. "batch" message type forwarded to clients
 *   5. user_left carries the name correctly (looked up before erase)
 */

#include "ot_engine.hpp"
#include "document_store.hpp"
#include "presence.hpp"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <sys/resource.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <random>
#include <filesystem>
#include <functional>
#include <deque>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ws    = beast::websocket;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;

// ─── Globals ─────────────────────────────────────────────────────────────────

static std::string g_html_content;
static std::atomic<int> g_conn_count{0};

// ─── Helpers ─────────────────────────────────────────────────────────────────

inline std::string make_op_id() {
    static std::mt19937_64 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(dist(rng)));
    return std::string(buf, 8);
}

inline std::string op_type_str(collab::OpType t) {
    return t == collab::OpType::INSERT ? "insert" : "delete";
}

namespace collab {

nlohmann::json Document::get_state_json() const {
    return {
        {"doc_id",  doc_id},
        {"title",   title},
        {"content", content},
        {"version", version},
    };
}

std::vector<nlohmann::json> DocumentStore::list_docs() {
    std::lock_guard<std::mutex> lk(store_mu_);
    std::vector<nlohmann::json> out;
    for (auto& [id, doc] : docs_) {
        std::lock_guard<std::mutex> dlk(doc->mu);
        out.push_back({
            {"doc_id",     doc->doc_id},
            {"title",      doc->title},
            {"version",    doc->version},
            {"updated_at", doc->updated_at},
        });
    }
    return out;
}

} // namespace collab

// ─── HTTP handler ─────────────────────────────────────────────────────────────

template<class Body, class Allocator>
http::response<http::string_body>
handle_http(http::request<Body, http::basic_fields<Allocator>> const& req)
{
    auto target = std::string(req.target());
    auto qpos   = target.find('?');
    std::string path = (qpos != std::string::npos) ? target.substr(0, qpos) : target;

    auto make_response = [&](http::status status,
                              const std::string& content_type,
                              const std::string& body) {
        http::response<http::string_body> res{status, req.version()};
        res.set(http::field::server, "CollabDocs/1.0");
        res.set(http::field::content_type, content_type);
        res.set(http::field::access_control_allow_origin, "*");
        res.keep_alive(req.keep_alive());
        res.body() = body;
        res.prepare_payload();
        return res;
    };

    if (req.method() == http::verb::get && (path == "/" || path == "/index.html"))
        return make_response(http::status::ok, "text/html; charset=utf-8", g_html_content);

    if (req.method() == http::verb::get && path == "/health") {
        json j = {{"status", "ok"}, {"docs", collab::g_store.size()}, {"conns", g_conn_count.load()}};
        return make_response(http::status::ok, "application/json", j.dump());
    }

    if (req.method() == http::verb::get && path == "/api/docs") {
        json j = collab::g_store.list_docs();
        return make_response(http::status::ok, "application/json", j.dump());
    }

    if (req.method() == http::verb::get && path.rfind("/api/docs/", 0) == 0) {
        std::string doc_id = path.substr(10);
        auto doc = collab::g_store.get(doc_id);
        if (!doc) return make_response(http::status::not_found, "application/json", R"({"error":"not found"})");
        std::lock_guard<std::mutex> lk(doc->mu);
        return make_response(http::status::ok, "application/json", doc->get_state_json().dump());
    }

    if (req.method() == http::verb::post && path == "/api/docs") {
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<uint32_t> dist;
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", dist(rng));
        std::string doc_id(buf, 8);
        collab::g_store.get_or_create(doc_id);
        json j = {{"doc_id", doc_id}, {"url", "/?doc=" + doc_id}};
        return make_response(http::status::created, "application/json", j.dump());
    }

    return make_response(http::status::not_found, "text/plain", "Not Found");
}

// ─── WebSocket session ────────────────────────────────────────────────────────

class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    explicit WsSession(tcp::socket socket, asio::io_context& ioc)
        : ws_(std::move(socket))
        , write_strand_(asio::make_strand(ioc))
    {}

    void run(http::request<http::string_body> req,
             std::string doc_id,
             std::string user_id,
             asio::yield_context yield)
    {
        beast::error_code ec;
        doc_id_  = doc_id;
        user_id_ = user_id;

        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator([](ws::response_type& res) {
            res.set(http::field::server, "CollabDocs/1.0");
        }));

        ws_.async_accept(req, yield[ec]);
        if (ec) return;

        auto self = shared_from_this();

        // Register with presence; give it a send callback
        collab::g_presence.join(doc_id, user_id, [self](const std::string& msg) {
            self->enqueue_send(msg);
        });

        auto doc = collab::g_store.get_or_create(doc_id);

        // Wire up the document's broadcast function once (idempotent)
        {
            std::lock_guard<std::mutex> lk(doc->mu);
            if (!doc->broadcast_fn) {
                doc->broadcast_fn = [](const std::string& did,
                                       const std::string& json_str,
                                       const std::string& exclude) {
                    collab::g_presence.broadcast(did, json_str, exclude);
                };
            }
        }

        // Send init state
        {
            auto user_opt = collab::g_presence.get_user(doc_id, user_id);
            json init;
            {
                std::lock_guard<std::mutex> lk(doc->mu);
                init = {
                    {"type",      "init"},
                    {"user_id",   user_id},
                    {"name",      user_opt ? user_opt->name  : "Unknown"},
                    {"color",     user_opt ? user_opt->color : "#999"},
                    {"doc_state", doc->get_state_json()},
                    {"users",     collab::g_presence.get_users_json(doc_id)},
                };
            }
            enqueue_send(init.dump());
        }

        // Broadcast join to peers
        {
            auto user_opt = collab::g_presence.get_user(doc_id, user_id);
            json joined = {
                {"type",    "user_joined"},
                {"user_id", user_id},
                {"name",    user_opt ? user_opt->name  : "Unknown"},
                {"color",   user_opt ? user_opt->color : "#999"},
                {"users",   collab::g_presence.get_users_json(doc_id)},
            };
            collab::g_presence.broadcast(doc_id, joined.dump(), user_id);
        }

        g_conn_count++;

        beast::flat_buffer buf;
        while (true) {
            ws_.async_read(buf, yield[ec]);
            if (ec == ws::error::closed || ec == asio::error::eof) break;
            if (ec) break;

            if (ws_.got_text()) {
                std::string raw = beast::buffers_to_string(buf.data());
                buf.consume(buf.size());
                handle_message(raw, doc);
            } else {
                buf.consume(buf.size());
            }
        }

        g_conn_count--;

        // Look up name before erasing from presence
        std::string leaving_name = "Someone";
        std::string leaving_color = "#999";
        {
            auto u = collab::g_presence.get_user(doc_id, user_id);
            if (u) { leaving_name = u->name; leaving_color = u->color; }
        }

        collab::g_presence.leave(doc_id, user_id);

        json left = {
            {"type",    "user_left"},
            {"user_id", user_id},
            {"name",    leaving_name},
            {"color",   leaving_color},
            {"users",   collab::g_presence.get_users_json(doc_id)},
        };
        collab::g_presence.broadcast(doc_id, left.dump());
    }

private:
    ws::stream<tcp::socket>                              ws_;
    asio::strand<asio::io_context::executor_type>        write_strand_;
    std::deque<std::string>                              write_queue_;

    std::string doc_id_;
    std::string user_id_;

    // ── Strand-serialised write queue ────────────────────────────────────────
    // All access to write_queue_ happens only on write_strand_, so no mutex needed.

    void enqueue_send(const std::string& msg) {
        asio::post(write_strand_, [self = shared_from_this(), msg]() {
            bool was_empty = self->write_queue_.empty();
            self->write_queue_.push_back(msg);
            if (was_empty) self->do_flush();
        });
    }

    void do_flush() {
        // Must be called on write_strand_
        if (write_queue_.empty()) return;

        const std::string& front = write_queue_.front();
        beast::error_code ec;
        ws_.text(true);
        ws_.write(asio::buffer(front), ec);
        write_queue_.pop_front();

        if (ec) {
            write_queue_.clear();
            return;
        }
        if (!write_queue_.empty()) do_flush();
    }

    // ── Message dispatch ──────────────────────────────────────────────────────

    void handle_message(const std::string& raw,
                        std::shared_ptr<collab::Document> doc)
    {
        try {
            json msg = json::parse(raw);
            std::string type = msg.value("type", "");
            if      (type == "operation")    handle_operation(msg, doc);
            else if (type == "cursor")       handle_cursor(msg);
            else if (type == "title_change") handle_title_change(msg);
            else if (type == "ping") {
                json pong = msg;
                pong["type"] = "pong";
                enqueue_send(pong.dump());
            }
        } catch (...) {
            // Ignore malformed messages or type errors to prevent crashing
            return;
        }
    }

    // ── Operation handler (with broadcast batching) ───────────────────────────

    void handle_operation(const json& msg,
                          std::shared_ptr<collab::Document> doc)
    {
        std::string op_type_s = msg.value("op_type", "");
        if (op_type_s != "insert" && op_type_s != "delete") return;

        collab::Operation op;
        op.type = (op_type_s == "insert") ? collab::OpType::INSERT : collab::OpType::DELETE;
        op.position     = msg.value("position", 0);
        op.value        = msg.value("value", "");
        op.length       = (op.type == collab::OpType::INSERT)
                              ? static_cast<int>(op.value.size())
                              : msg.value("length", 0);
        op.base_version = msg.value("base_version", 0);
        op.user_id      = user_id_;
        op.op_id        = msg.value("op_id", make_op_id());

        collab::Operation transformed;
        int new_version;

        {
            std::lock_guard<std::mutex> lk(doc->mu);
            transformed = doc->apply_op_locked(op);
            new_version = doc->version;

            // Build the broadcast payload
            json bcast = {
                {"type",           "operation"},
                {"op_type",        op_type_str(transformed.type)},
                {"position",       transformed.position},
                {"value",          transformed.value},
                {"length",         transformed.length},
                {"server_version", new_version},
                {"user_id",        user_id_},
                {"op_id",          transformed.op_id},
            };

            // Enqueue for batched broadcast (flushes automatically when thresholds hit)
            doc->enqueue_broadcast_locked(std::move(bcast), user_id_);
        }

        // Fire any deferred broadcasts (called AFTER lock release)
        doc->fire_pending_broadcasts();

        // ACK directly back to the sender (not batched — sender needs this immediately)
        enqueue_send(json({
            {"type",           "ack"},
            {"op_id",          op.op_id},
            {"server_version", new_version},
        }).dump());
    }

    void handle_cursor(const json& msg) {
        int pos = msg.value("cursor_pos", 0);
        collab::g_presence.update_cursor(doc_id_, user_id_, pos);
        auto u = collab::g_presence.get_user(doc_id_, user_id_);
        json bcast = {
            {"type",       "cursor"},
            {"user_id",    user_id_},
            {"name",       u ? u->name  : ""},
            {"color",      u ? u->color : "#999"},
            {"cursor_pos", pos},
        };
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }

    void handle_title_change(const json& msg) {
        std::string title = msg.value("title", "Untitled Document");
        collab::g_store.update_title(doc_id_, title);
        collab::g_presence.broadcast(
            doc_id_,
            json({{"type","title_change"},{"title",title},{"user_id",user_id_}}).dump(),
            user_id_);
    }
};

// ─── Listener ─────────────────────────────────────────────────────────────────

void do_listen(asio::io_context& ioc, tcp::endpoint ep) {
    tcp::acceptor acceptor(ioc, ep);
    acceptor.set_option(asio::socket_base::reuse_address(true));

    while (true) {
        beast::error_code ec;
        tcp::socket socket(ioc);
        acceptor.accept(socket, ec);
        if (ec) continue;

        asio::spawn(ioc, [sock = std::move(socket), &ioc](asio::yield_context yield) mutable {
            beast::error_code ec;
            beast::flat_buffer buf;
            beast::tcp_stream stream(std::move(sock));
            http::request<http::string_body> req;
            http::async_read(stream, buf, req, yield[ec]);
            if (ec) return;

            if (ws::is_upgrade(req)) {
                std::string target = std::string(req.target());
                auto qpos = target.find('?');
                std::string doc_path = (qpos != std::string::npos)
                                         ? target.substr(0, qpos)
                                         : target;

                // /ws/<doc_id>
                std::string doc_id = (doc_path.size() > 4)
                                       ? doc_path.substr(4)
                                       : "welcome";

                // Extract user_id from query string
                std::string user_id = make_op_id();
                if (qpos != std::string::npos) {
                    std::string query = target.substr(qpos + 1);
                    auto upos = query.find("user_id=");
                    if (upos != std::string::npos)
                        user_id = query.substr(upos + 8);
                    // Trim any trailing '&' params
                    auto amp = user_id.find('&');
                    if (amp != std::string::npos) user_id = user_id.substr(0, amp);
                }

                auto session = std::make_shared<WsSession>(stream.release_socket(), ioc);
                asio::spawn(ioc, [session, req = std::move(req), doc_id, user_id]
                                 (asio::yield_context y) mutable {
                    session->run(std::move(req), doc_id, user_id, y);
                });
            } else {
                auto res = handle_http(req);
                http::async_write(stream, res, yield[ec]);
                if (!req.keep_alive())
                    stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            }
        });
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── 1. Raise file-descriptor limit so 500+ WebSocket connections work ────
    {
        struct rlimit rl{};
        getrlimit(RLIMIT_NOFILE, &rl);
        rl.rlim_cur = std::min(static_cast<rlim_t>(65535), rl.rlim_max);
        if (setrlimit(RLIMIT_NOFILE, &rl) == 0)
            std::cout << "[CollabDocs] fd limit raised to " << rl.rlim_cur << "\n";
        else
            std::cerr << "[CollabDocs] Warning: could not raise fd limit\n";
    }

    // ── 2. Port ───────────────────────────────────────────────────────────────
    uint16_t port = 7860;
    if (auto* p = std::getenv("PORT")) port = static_cast<uint16_t>(std::stoi(p));

    // ── 3. Load frontend ──────────────────────────────────────────────────────
    {
        std::ifstream f("index.html");
        if (f) { std::ostringstream ss; ss << f.rdbuf(); g_html_content = ss.str(); }
        else   { std::cerr << "[CollabDocs] Warning: index.html not found\n"; }
    }

    // ── 4. Seed welcome document ──────────────────────────────────────────────
    {
        auto doc = collab::g_store.get_or_create("welcome");
        std::lock_guard<std::mutex> lk(doc->mu);
        if (doc->content.empty()) {
            doc->title   = "Welcome";
            doc->content = "Start typing…";
            doc->version = 1;
        }
    }

    // ── 5. IO threads ─────────────────────────────────────────────────────────
    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    asio::io_context ioc(static_cast<int>(threads));
    auto ep = tcp::endpoint(asio::ip::make_address("0.0.0.0"), port);

    std::cout << "[CollabDocs] Listening on 0.0.0.0:" << port
              << "  threads=" << threads << "\n";

    auto work_guard = asio::make_work_guard(ioc);
    std::thread listener([&] { do_listen(ioc, ep); });

    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned i = 0; i < threads - 1; ++i)
        pool.emplace_back([&] { ioc.run(); });

    ioc.run();
    return 0;
}
