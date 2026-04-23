/*
 * CollabDocs C++ — Main Server
 *
 * Stack:
 * Boost.Asio  — async I/O
 * Boost.Beast — HTTP + WebSocket
 * nlohmann/json — JSON
 * C++17
 *
 * Each WebSocket connection gets its own session coroutine (stackful via
 * Boost.Asio's spawn/coroutine).  All document mutations are protected by
 * per-document mutexes.  Broadcasts use the PresenceManager's snapshot
 * approach to avoid holding the lock during I/O.
 */

#include "ot_engine.hpp"
#include "document_store.hpp"
#include "presence.hpp"

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <random>
#include <filesystem>
#include <functional>

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ws    = beast::websocket;
using tcp       = asio::ip::tcp;
using json      = nlohmann::json;

// ─── Globals ─────────────────────────────────────────────────────────────────

static std::string g_html_content;     // cached index.html
static std::atomic<int> g_conn_count{0};

// ─── Helpers ─────────────────────────────────────────────────────────────────

inline std::string make_op_id() {
    static std::mt19937_64 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(dist(rng)));
    return std::string(buf, 8); // first 8 hex chars
}

inline std::string op_type_str(collab::OpType t) {
    return t == collab::OpType::INSERT ? "insert" : "delete";
}

// collab::Document::get_state_json() — defined here (forward decl in header)
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
    // Strip query string
    auto qpos = target.find('?');
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

    // GET /  → index.html
    if (req.method() == http::verb::get && (path == "/" || path == "/index.html")) {
        return make_response(http::status::ok, "text/html; charset=utf-8", g_html_content);
    }

    // GET /health
    if (req.method() == http::verb::get && path == "/health") {
        json j = {
            {"status", "ok"},
            {"docs",   collab::g_store.size()},
        };
        return make_response(http::status::ok, "application/json", j.dump());
    }

    // GET /api/docs
    if (req.method() == http::verb::get && path == "/api/docs") {
        json j = collab::g_store.list_docs();
        return make_response(http::status::ok, "application/json", j.dump());
    }

    // GET /api/docs/:id
    if (req.method() == http::verb::get && path.rfind("/api/docs/", 0) == 0) {
        std::string doc_id = path.substr(10);
        auto doc = collab::g_store.get(doc_id);
        if (!doc)
            return make_response(http::status::not_found, "application/json",
                                 R"({"error":"not found"})");
        std::lock_guard<std::mutex> lk(doc->mu);
        return make_response(http::status::ok, "application/json",
                             doc->get_state_json().dump());
    }

    // POST /api/docs  → create doc
    if (req.method() == http::verb::post && path == "/api/docs") {
        // Generate 8-char hex id
        static std::mt19937 rng(std::random_device{}());
        static std::uniform_int_distribution<uint32_t> dist;
        char buf[9];
        std::snprintf(buf, sizeof(buf), "%08x", dist(rng));
        std::string doc_id(buf, 8);
        collab::g_store.get_or_create(doc_id);
        json j = {{"doc_id", doc_id}, {"url", "/?doc=" + doc_id}};
        auto res = make_response(http::status::created, "application/json", j.dump());
        return res;
    }

    return make_response(http::status::not_found, "text/plain", "Not Found");
}

// ─── WebSocket session ────────────────────────────────────────────────────────

// A thread-safe write queue for a single WebSocket connection.
// Beast requires serialized writes; we queue them and drain with a strand.
class WsSession : public std::enable_shared_from_this<WsSession> {
public:
    explicit WsSession(tcp::socket socket, asio::io_context& ioc)
        : ws_(std::move(socket))
        , strand_(asio::make_strand(ioc))
        , write_strand_(asio::make_strand(ioc))
    {}

    void run(asio::yield_context yield) {
        beast::error_code ec;

        // WebSocket handshake
        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator([](ws::response_type& res) {
            res.set(http::field::server, "CollabDocs/1.0");
        }));
        ws_.async_accept(yield[ec]);
        if (ec) return;

        // Parse doc_id and user_id from target URL that was stored before upgrade
        // (We stored it in target_ before the upgrade)
        // Parse query string
        std::string doc_id  = "welcome";
        std::string user_id = make_op_id();

        auto parse_query = [](const std::string& target,
                               const std::string& key) -> std::string {
            auto qpos = target.find('?');
            if (qpos == std::string::npos) return "";
            std::string qs = target.substr(qpos + 1);
            // Simple key=value parser
            size_t pos = 0;
            while (pos < qs.size()) {
                auto amp = qs.find('&', pos);
                std::string kv = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                auto eq = kv.find('=');
                if (eq != std::string::npos) {
                    if (kv.substr(0, eq) == key)
                        return kv.substr(eq + 1);
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
            return "";
        };

        if (!target_.empty()) {
            auto d = parse_query(target_, "doc_id");
            if (!d.empty()) doc_id = d;
            auto u = parse_query(target_, "user_id");
            if (!u.empty()) user_id = u;
        }

        doc_id_ = doc_id;
        user_id_ = user_id;

        auto self = shared_from_this();

        // Register send callback with presence manager
        collab::g_presence.join(doc_id, user_id,
            [self](const std::string& msg) {
                self->enqueue_send(msg);
            });

        auto doc = collab::g_store.get_or_create(doc_id);

        // Send init message
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

        // Announce join
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

        // Read loop
        beast::flat_buffer buf;
        while (true) {
            ws_.async_read(buf, yield[ec]);
            if (ec == ws::error::closed || ec == asio::error::eof) break;
            if (ec) break;

            if (ws_.got_text()) {
                std::string raw = beast::buffers_to_string(buf.data());
                buf.consume(buf.size());
                handle_message(raw, doc, yield);
            } else {
                buf.consume(buf.size());
            }
        }

        // Cleanup
        g_conn_count--;
        collab::g_presence.leave(doc_id, user_id);

        // Notify departure
        json left = {
            {"type",    "user_left"},
            {"user_id", user_id},
            {"name",    "Someone"},
            {"users",   collab::g_presence.get_users_json(doc_id)},
        };
        collab::g_presence.broadcast(doc_id, left.dump());
    }

    // Store the upgrade target before coroutine starts
    std::string target_;

private:
    ws::stream<tcp::socket> ws_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::strand<asio::io_context::executor_type> write_strand_;

    std::string doc_id_;
    std::string user_id_;

    // Write queue
    std::mutex                   wq_mu_;
    std::vector<std::string>     write_queue_;
    std::atomic<bool>            writing_{false};

    void enqueue_send(const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(wq_mu_);
            write_queue_.push_back(msg);
        }
        do_write();
    }

    void do_write() {
        bool expected = false;
        if (!writing_.compare_exchange_strong(expected, true)) return;

        std::string msg;
        {
            std::lock_guard<std::mutex> lk(wq_mu_);
            if (write_queue_.empty()) { writing_ = false; return; }
            msg = std::move(write_queue_.front());
            write_queue_.erase(write_queue_.begin());
        }

        auto self = shared_from_this();
        asio::post(write_strand_, [self, m = std::move(msg)]() mutable {
            beast::error_code ec;
            self->ws_.text(true);
            self->ws_.write(asio::buffer(m), ec);
            self->writing_ = false;
            if (!ec) self->do_write(); // drain next
        });
    }

    void handle_message(const std::string& raw,
                        std::shared_ptr<collab::Document> doc,
                        asio::yield_context& /*yield*/)
    {
        json msg;
        try { msg = json::parse(raw); }
        catch (...) { return; }

        std::string type = msg.value("type", "");

        if (type == "operation") {
            handle_operation(msg, doc);
        } else if (type == "cursor") {
            handle_cursor(msg);
        } else if (type == "title_change") {
            handle_title_change(msg);
        } else if (type == "ping") {
            // Update last_seen
            auto u = collab::g_presence.get_user(doc_id_, user_id_);
            (void)u; // ping handled inside get_user via presence.ping()
        }
    }

    void handle_operation(const json& msg,
                          std::shared_ptr<collab::Document> doc)
    {
        std::string op_type_s = msg.value("op_type", "");
        if (op_type_s != "insert" && op_type_s != "delete") return;

        int position     = msg.value("position",     0);
        int base_version = msg.value("base_version", 0);
        int length       = msg.value("length",       0);
        std::string value   = msg.value("value", "");
        std::string op_id_s = msg.value("op_id", make_op_id());

        collab::Operation op;
        op.type         = (op_type_s == "insert") ? collab::OpType::INSERT
                                                   : collab::OpType::DELETE;
        op.position     = position;
        op.value        = value;
        op.length       = (op.type == collab::OpType::INSERT)
                              ? static_cast<int>(value.size()) : length;
        op.base_version = base_version;
        op.user_id      = user_id_;
        op.op_id        = op_id_s;

        collab::Operation transformed;
        int new_version;
        {
            std::lock_guard<std::mutex> lk(doc->mu);
            transformed = doc->apply_op_locked(op);
            new_version = doc->version;
        }

        // ACK to sender
        json ack = {
            {"type",           "ack"},
            {"op_id",          op_id_s},
            {"server_version", new_version},
        };
        enqueue_send(ack.dump());

        // Broadcast to others
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
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }

    void handle_cursor(const json& msg) {
        int pos       = msg.value("cursor_pos",      0);
        int sel_start = msg.value("selection_start", -1);
        int sel_end   = msg.value("selection_end",   -1);
        collab::g_presence.update_cursor(doc_id_, user_id_, pos, sel_start, sel_end);

        auto u = collab::g_presence.get_user(doc_id_, user_id_);
        json bcast = {
            {"type",             "cursor"},
            {"user_id",          user_id_},
            {"name",             u ? u->name  : ""},
            {"color",            u ? u->color : "#999"},
            {"cursor_pos",       pos},
            {"selection_start",  sel_start},
            {"selection_end",    sel_end},
        };
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }

    void handle_title_change(const json& msg) {
        std::string title = msg.value("title", "Untitled Document");
        if (title.size() > 200) title = title.substr(0, 200);
        collab::g_store.update_title(doc_id_, title);
        json bcast = {
            {"type",    "title_change"},
            {"title",   title},
            {"user_id", user_id_},
        };
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }
};

// ─── Listener ─────────────────────────────────────────────────────────────────

void do_listen(asio::io_context& ioc, tcp::endpoint ep) {
    beast::error_code ec;
    tcp::acceptor acceptor(ioc, ep);
    acceptor.set_option(asio::socket_base::reuse_address(true));

    while (true) {
        tcp::socket socket(ioc);
        acceptor.accept(socket, ec);
        if (ec) {
            std::cerr << "[accept] " << ec.message() << "\n";
            continue;
        }

        // Peek at first bytes to decide HTTP vs WS
        asio::spawn(ioc, [sock = std::move(socket), &ioc](asio::yield_context yield) mutable {
            beast::error_code ec;
            beast::flat_buffer buf;
            beast::tcp_stream stream(std::move(sock));

            // Read an HTTP request
            http::request<http::string_body> req;
            http::async_read(stream, buf, req, yield[ec]);
            if (ec) return;

            std::string target = std::string(req.target());

            // Check WebSocket upgrade
            if (ws::is_upgrade(req)) {
                // Parse /ws/{doc_id}?user_id=...
                std::string path = target;
                auto qpos = path.find('?');
                std::string doc_path = (qpos != std::string::npos)
                                           ? path.substr(0, qpos) : path;

                // Extract doc_id from /ws/{doc_id}
                std::string doc_id = "welcome";
                if (doc_path.rfind("/ws/", 0) == 0)
                    doc_id = doc_path.substr(4);

                // Build query string for session
                std::string query = (qpos != std::string::npos)
                                        ? target.substr(qpos + 1) : "";
                std::string user_id_param;
                // Simple parse
                auto find_param = [&](const std::string& key) {
                    size_t pos = 0;
                    while (pos < query.size()) {
                        auto amp = query.find('&', pos);
                        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
                        auto eq = kv.find('=');
                        if (eq != std::string::npos && kv.substr(0, eq) == key)
                            return kv.substr(eq + 1);
                        if (amp == std::string::npos) break;
                        pos = amp + 1;
                    }
                    return std::string{};
                };
                user_id_param = find_param("user_id");
                if (user_id_param.empty()) user_id_param = make_op_id();

                auto session = std::make_shared<WsSession>(
                    stream.release_socket(), ioc);
                session->target_ = "?doc_id=" + doc_id + "&user_id=" + user_id_param;

                asio::spawn(ioc, [session](asio::yield_context y) {
                    session->run(y);
                });
            } else {
                // Plain HTTP
                auto res = handle_http(req);
                http::async_write(stream, res, yield[ec]);
                if (ec) return;
                if (!req.keep_alive()) {
                    beast::error_code ec2;
                    stream.socket().shutdown(tcp::socket::shutdown_send, ec2);
                }
            }
        });
    }
}

// ─── main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Port from env (HF Spaces uses 7860)
    uint16_t port = 7860;
    if (auto* p = std::getenv("PORT")) port = static_cast<uint16_t>(std::stoi(p));
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    // Load index.html
    {
        std::ifstream f("index.html");
        if (!f.is_open()) {
            std::cerr << "[FATAL] index.html not found\n";
            return 1;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        g_html_content = ss.str();
    }

    // Seed welcome document
    {
        auto doc = collab::g_store.get_or_create("welcome");
        std::lock_guard<std::mutex> lk(doc->mu);
        if (doc->content.empty()) {
            doc->title   = "Welcome to CollabDocs";
            doc->content =
                "Welcome to CollabDocs C++!\n\n"
                "This is a real-time collaborative document editor.\n"
                "Built entirely in C++ with Boost.Beast WebSockets\n"
                "and a from-scratch Operational Transformation engine.\n\n"
                "Share the URL — multiple people can edit simultaneously.\n"
                "Conflicts are resolved via OT (insert-insert, delete-delete,\n"
                "insert-delete, delete-insert transformations).\n\n"
                "Start typing to collaborate...";
            doc->version = 1;
        }
    }

    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    asio::io_context ioc(static_cast<int>(threads));

    auto ep = tcp::endpoint(asio::ip::make_address("0.0.0.0"), port);
    
    // FIX 1: std::endl forces the log to print immediately
    std::cout << "[CollabDocs] Listening on 0.0.0.0:" << port
              << " (" << threads << " threads)" << std::endl;

    // FIX 2: THIS KEEPS THE SERVER ALIVE
    auto work_guard = asio::make_work_guard(ioc);

    // Run listener in background thread
    std::thread listener_thread([&]{ do_listen(ioc, ep); });

    // Thread pool
    std::vector<std::thread> pool;
    if (threads > 1) {
        pool.reserve(threads - 1);
        for (unsigned i = 0; i < threads - 1; ++i)
            pool.emplace_back([&]{ ioc.run(); });
    }

    // Now this will stay running and process your HTTP/WebSocket requests
    ioc.run();

    for (auto& t : pool) t.join();
    listener_thread.join();
    return 0;
}
