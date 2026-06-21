#include <chronos/io/transports/websocket_tls_transport.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <memory>
#include <string>

namespace chronos {
namespace market_data {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;

struct WebSocketTlsTransport::Impl {
    using tcp = asio::ip::tcp;
    using SSLSocket = ssl::stream<tcp::socket>;
    using WSStream = websocket::stream<SSLSocket&>;

    asio::io_context ioc;
    ssl::context ssl_ctx{ssl::context::tls_client};
    std::unique_ptr<SSLSocket> ssl_socket;
    std::unique_ptr<WSStream> ws_stream;

    // Parsed from cfg.websocket_url during connect()
    std::string host, port, path;
    bool use_proxy = false;
    std::string proxy_host;
    int proxy_port = 0;

    void parseUrl(const std::string& ws_url) {
        std::string url = ws_url;
        if (url.starts_with("wss://")) url = url.substr(6);
        else if (url.starts_with("ws://")) url = url.substr(5);

        auto colon = url.find(':');
        auto slash = url.find('/');

        host = url.substr(0, std::min(colon, slash));

        if (colon != std::string::npos && colon < slash) {
            port = url.substr(colon + 1, slash - colon - 1);
        } else {
            port = ws_url.starts_with("wss") ? "443" : "80";
        }

        path = (slash != std::string::npos) ? url.substr(slash) : "/";
    }

    void doDisconnect() {
        try {
            if (ws_stream && ws_stream->is_open()) {
                ws_stream->close(websocket::close_code::normal);
            }
        } catch (...) {}
        ws_stream.reset();
        ssl_socket.reset();
    }

    bool doConnect(const ExchangeConfig& cfg) {
        ioc.restart();  // allow reuse after stop()
        use_proxy = (cfg.proxy_port > 0 && !cfg.proxy_host.empty());
        proxy_host = cfg.proxy_host;
        proxy_port = cfg.proxy_port;

        parseUrl(cfg.websocket_url);

        spdlog::debug("WS connecting: host={} port={} path={} proxy={}",
                      host, port, path, use_proxy ? "yes" : "no");

        if (use_proxy) {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(proxy_host, std::to_string(proxy_port));

            tcp::socket proxy_sock(ioc);
            asio::connect(proxy_sock, results);

            std::string req =
                "CONNECT " + host + ":" + port + " HTTP/1.1\r\n"
                "Host: " + host + ":" + port + "\r\n\r\n";
            asio::write(proxy_sock, asio::buffer(req));

            asio::streambuf response_buf;
            asio::read_until(proxy_sock, response_buf, "\r\n\r\n");
            std::string response_str(
                asio::buffers_begin(response_buf.data()),
                asio::buffers_begin(response_buf.data()) + response_buf.size());

            if (response_str.find("200") == std::string::npos) {
                spdlog::error("Proxy CONNECT failed: {}", response_str);
                return false;
            }
            spdlog::info("WS proxy tunnel via {}:{}", proxy_host, proxy_port);

            ssl_socket = std::make_unique<SSLSocket>(std::move(proxy_sock), ssl_ctx);
        } else {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(host, port);
            ssl_socket = std::make_unique<SSLSocket>(ioc, ssl_ctx);
            asio::connect(ssl_socket->lowest_layer(), results);
        }

        if (!SSL_set_tlsext_host_name(ssl_socket->native_handle(), host.c_str())) {
            spdlog::error("SSL_set_tlsext_host_name failed");
            return false;
        }

        ssl_socket->handshake(ssl::stream_base::client);

        ws_stream = std::make_unique<WSStream>(*ssl_socket);

        // Set User-Agent — required by some exchanges (Binance, OKX)
        ws_stream->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent,
                        "Chronos/1.0 Boost.Beast");
            }));

        ws_stream->handshake(host, path);

        spdlog::info("WS connected to {}:{}{}", host, port, path);
        return true;
    }
};

WebSocketTlsTransport::WebSocketTlsTransport()
    : pimpl_(std::make_unique<Impl>()) {}

WebSocketTlsTransport::~WebSocketTlsTransport() {
    disconnect();
}

bool WebSocketTlsTransport::connect(const ExchangeConfig& cfg) {
    try {
        return pimpl_->doConnect(cfg);
    } catch (const boost::system::system_error& e) {
        auto code = e.code().value();
        if (code == boost::asio::error::host_not_found ||
            code == boost::asio::error::host_not_found_try_again) {
            spdlog::error("WS DNS resolution failed for {}:{} — "
                          "check network or configure a proxy (proxy_host/proxy_port)",
                          pimpl_->host, pimpl_->port);
        } else if (code == boost::asio::error::connection_refused) {
            spdlog::error("WS connection refused by {}:{} — "
                          "exchange may be unreachable, try a proxy",
                          pimpl_->host, pimpl_->port);
        } else if (code == boost::asio::error::timed_out) {
            spdlog::error("WS connection timed out to {}:{} — "
                          "firewall may be blocking, configure a proxy",
                          pimpl_->host, pimpl_->port);
        } else {
            spdlog::error("WS connection failed: {} (code={})",
                          e.what(), code);
        }
        pimpl_->doDisconnect();
        return false;
    } catch (const std::exception& e) {
        spdlog::error("WS connection failed: {}", e.what());
        pimpl_->doDisconnect();
        return false;
    }
}

void WebSocketTlsTransport::disconnect() {
    pimpl_->doDisconnect();
}

bool WebSocketTlsTransport::send(const std::string& msg) {
    if (!pimpl_->ws_stream || !pimpl_->ws_stream->is_open()) return false;
    boost::system::error_code ec;
    pimpl_->ws_stream->write(asio::buffer(msg), ec);
    if (ec) {
        spdlog::error("WS send error: {}", ec.message());
        return false;
    }
    return true;
}

std::string WebSocketTlsTransport::receive() {
    if (!pimpl_->ws_stream || !pimpl_->ws_stream->is_open()) {
        return {};
    }
    beast::flat_buffer buffer;
    boost::system::error_code ec;
    pimpl_->ws_stream->read(buffer, ec);

    if (ec == asio::error::operation_aborted) return {};
    if (ec == websocket::error::closed) return {};
    if (ec == asio::ssl::error::stream_truncated) return {};
    if (ec) {
        throw std::runtime_error("WS read error: " + ec.message());
    }
    return beast::buffers_to_string(buffer.data());
}

bool WebSocketTlsTransport::sendPing() {
    if (!pimpl_->ws_stream || !pimpl_->ws_stream->is_open()) return false;
    boost::system::error_code ec;
    pimpl_->ws_stream->ping(websocket::ping_data{}, ec);
    if (ec) {
        spdlog::warn("WS ping failed: {}", ec.message());
        return false;
    }
    return true;
}

bool WebSocketTlsTransport::isConnected() const {
    return pimpl_->ws_stream && pimpl_->ws_stream->is_open();
}

void WebSocketTlsTransport::stop() {
    // Unblock pending receive() by stopping the io_context first.
    // doDisconnect() (ws_stream->close()) must NOT be called while
    // another thread is inside ws_stream->read() — Beast streams
    // are not thread-safe.  Stopping the ioc causes read() to return
    // with operation_aborted, the read thread exits, and the caller
    // then calls disconnect() for the clean WebSocket close handshake.
    pimpl_->ioc.stop();
}

} // namespace market_data
} // namespace chronos
