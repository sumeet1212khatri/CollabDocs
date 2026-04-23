/*
 * CollabDocs C++ — Main Server (UPDATED)
 *
 * Stack:
 * Boost.Asio  — async I/O
 * Boost.Beast — HTTP + WebSocket
 * nlohmann/json — JSON
 * C++17
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
} 

// ─── HTTP handler ─────────────────────────────────────────────────────────────

template<class Body, class Allocator>
http::response<http::string_body>
handle_http(http::request<Body, http::basic_fields<Allocator>> const& req)
{
    auto target = std::string(req.target());
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

    if (req.method() == http::verb::get && (path == "/" || path == "/index.html")) {
        return make_response(http::status::ok, "text/html; charset=utf-8", g_html_content);
    }

    if (req.method() == http::verb::get && path == "/health") {
        json j = {{"status", "ok"}, {"docs", collab::g_store.size()}};
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
        , strand_(asio::make_strand(ioc))
        , write_strand_(asio::make_strand(ioc))
    {}

    // FIX 1: Accepts the request object to complete the handshake correctly
    void run(http::request<http::string_body> req, 
             std::string doc_id, 
             std::string user_id, 
             asio::yield_context yield) {
        
        beast::error_code ec;
        doc_id_ = doc_id;
        user_id_ = user_id;

        ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
        ws_.set_option(ws::stream_base::decorator([](ws::response_type& res) {
            res.set(http::field::server, "CollabDocs/1.0");
        }));

        // FIX 2: Pass the already-read request into async_accept
        ws_.async_accept(req, yield[ec]);
        if (ec) return;

        auto self = shared_from_this();
        collab::g_presence.join(doc_id, user_id, [self](const std::string& msg) {
            self->enqueue_send(msg);
        });

        auto doc = collab::g_store.get_or_create(doc_id);

        // Send initialization state
        {
            auto user_opt = collab::g_presence.get_user(doc_id, user_id);
            json init;
            {
                std::lock_guard<std::mutex> lk(doc->mu);
                init = {
                    {"type", "init"},
                    {"user_id", user_id},
                    {"name", user_opt ? user_opt->name : "Unknown"},
                    {"color", user_opt ? user_opt->color : "#999"},
                    {"doc_state", doc->get_state_json()},
                    {"users", collab::g_presence.get_users_json(doc_id)},
                };
            }
            enqueue_send(init.dump());
        }

        // Broadast join to other users in the same doc
        {
            auto user_opt = collab::g_presence.get_user(doc_id, user_id);
            json joined = {
                {"type", "user_joined"},
                {"user_id", user_id},
                {"name", user_opt ? user_opt->name : "Unknown"},
                {"color", user_opt ? user_opt->color : "#999"},
                {"users", collab::g_presence.get_users_json(doc_id)},
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
        collab::g_presence.leave(doc_id, user_id);

        json left = {
            {"type", "user_left"},
            {"user_id", user_id},
            {"name", "Someone"},
            {"users", collab::g_presence.get_users_json(doc_id)},
        };
        collab::g_presence.broadcast(doc_id, left.dump());
    }

private:
    ws::stream<tcp::socket> ws_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::strand<asio::io_context::executor_type> write_strand_;

    std::string doc_id_;
    std::string user_id_;

    std::mutex wq_mu_;
    std::vector<std::string> write_queue_;
    std::atomic<bool> writing_{false};

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
            if (!ec) self->do_write();
        });
    }

    void handle_message(const std::string& raw, std::shared_ptr<collab::Document> doc) {
        json msg;
        try { msg = json::parse(raw); } catch (...) { return; }

        std::string type = msg.value("type", "");
        if (type == "operation") {
            handle_operation(msg, doc);
        } else if (type == "cursor") {
            handle_cursor(msg);
        } else if (type == "title_change") {
            handle_title_change(msg);
        } else if (type == "ping") {
            auto u = collab::g_presence.get_user(doc_id_, user_id_);
            (void)u;
        }
    }

    void handle_operation(const json& msg, std::shared_ptr<collab::Document> doc) {
        std::string op_type_s = msg.value("op_type", "");
        if (op_type_s != "insert" && op_type_s != "delete") return;

        collab::Operation op;
        op.type = (op_type_s == "insert") ? collab::OpType::INSERT : collab::OpType::DELETE;
        op.position = msg.value("position", 0);
        op.value = msg.value("value", "");
        op.length = (op.type == collab::OpType::INSERT) ? static_cast<int>(op.value.size()) : msg.value("length", 0);
        op.base_version = msg.value("base_version", 0);
        op.user_id = user_id_;
        op.op_id = msg.value("op_id", make_op_id());

        collab::Operation transformed;
        int new_version;
        {
            std::lock_guard<std::mutex> lk(doc->mu);
            transformed = doc->apply_op_locked(op);
            new_version = doc->version;
        }

        enqueue_send(json({{"type", "ack"}, {"op_id", op.op_id}, {"server_version", new_version}}).dump());

        json bcast = {
            {"type", "operation"}, {"op_type", op_type_str(transformed.type)},
            {"position", transformed.position}, {"value", transformed.value},
            {"length", transformed.length}, {"server_version", new_version},
            {"user_id", user_id_}, {"op_id", transformed.op_id},
        };
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }

    void handle_cursor(const json& msg) {
        int pos = msg.value("cursor_pos", 0);
        collab::g_presence.update_cursor(doc_id_, user_id_, pos);
        auto u = collab::g_presence.get_user(doc_id_, user_id_);
        json bcast = {
            {"type", "cursor"}, {"user_id", user_id_}, {"name", u ? u->name : ""},
            {"color", u ? u->color : "#999"}, {"cursor_pos", pos}
        };
        collab::g_presence.broadcast(doc_id_, bcast.dump(), user_id_);
    }

    void handle_title_change(const json& msg) {
        std::string title = msg.value("title", "Untitled Document");
        collab::g_store.update_title(doc_id_, title);
        collab::g_presence.broadcast(doc_id_, json({{"type", "title_change"}, {"title", title}, {"user_id", user_id_}}).dump(), user_id_);
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
                std::string doc_path = (qpos != std::string::npos) ? target.substr(0, qpos) : target;
                
                // Extract doc_id and user_id
                std::string doc_id = (doc_path.size() > 4) ? doc_path.substr(4) : "welcome";
                
                // Simple user_id extraction
                std::string user_id = make_op_id();
                if (qpos != std::string::npos) {
                    std::string query = target.substr(qpos + 1);
                    auto upos = query.find("user_id=");
                    if (upos != std::string::npos) user_id = query.substr(upos + 8);
                }

                auto session = std::make_shared<WsSession>(stream.release_socket(), ioc);
                asio::spawn(ioc, [session, req = std::move(req), doc_id, user_id](asio::yield_context y) mutable {
                    session->run(std::move(req), doc_id, user_id, y);
                });
            } else {
                auto res = handle_http(req);
                http::async_write(stream, res, yield[ec]);
                if (!req.keep_alive()) stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            }
        });
    }
}

int main(int argc, char* argv[]) {
    uint16_t port = 7860;
    if (auto* p = std::getenv("PORT")) port = static_cast<uint16_t>(std::stoi(p));
    
    std::ifstream f("index.html");
    if (f) { std::ostringstream ss; ss << f.rdbuf(); g_html_content = ss.str(); }

    auto doc = collab::g_store.get_or_create("welcome");
    { 
        std::lock_guard<std::mutex> lk(doc->mu); 
        if (doc->content.empty()) { doc->title = "Welcome"; doc->content = "Start typing..."; doc->version = 1; }
    }

    unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    asio::io_context ioc(static_cast<int>(threads));
    auto ep = tcp::endpoint(asio::ip::make_address("0.0.0.0"), port);
    
    std::cout << "[CollabDocs] Listening on 0.0.0.0:" << port << " (" << threads << " threads)" << std::endl;
    auto work_guard = asio::make_work_guard(ioc);
    std::thread listener([&]{ do_listen(ioc, ep); });

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads - 1; ++i) pool.emplace_back([&]{ ioc.run(); });
    ioc.run();
    return 0;
}
