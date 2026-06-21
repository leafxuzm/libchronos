#include <chronos/execution/binance_http_client.hpp>
#include <chronos/io/security/hmac_sha256.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>

namespace chronos {
namespace execution {

namespace beast = boost::beast;
namespace http  = beast::http;
namespace asio  = boost::asio;
namespace ssl   = asio::ssl;
using tcp = asio::ip::tcp;

static std::string extractHost(const std::string& url) {
    std::string s = url;
    if (s.starts_with("https://")) s = s.substr(8);
    else if (s.starts_with("http://")) s = s.substr(7);
    auto pos = s.find(':');
    if (pos != std::string::npos) s = s.substr(0, pos);
    pos = s.find('/');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

// ============================================================================
// Pimpl
// ============================================================================

struct BinanceHttpClient::Impl {
    // --- Binance API helpers ---

    static std::string timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
        return std::to_string(ms);
    }

    static std::string buildQueryString(const std::string& baseParams,
                                        const std::string& apiSecret) {
        std::string ts = timestamp();
        std::string query = baseParams + "&timestamp=" + ts + "&recvWindow=5000";
        std::string sig = security::hmacSha256(apiSecret, query);
        return query + "&signature=" + sig;
    }

    static std::string encodeURI(const std::string& s) {
        std::ostringstream oss;
        oss << std::hex << std::uppercase;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                oss << c;
            else
                oss << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
        return oss.str();
    }

    // --- One-shot HTTPS request (with proxy support, retry on transient errors) ---

    std::string doRequest(const ExchangeConfig& cfg,
                          const std::string& method,
                          const std::string& target,
                          const std::string& body,
                          bool /*needSign*/) {
        std::string host = extractHost(cfg.rest_url);
        std::string port = "443";

        static constexpr int kMaxRetries = 2;

        std::string lastError;
        for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
            if (attempt > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                spdlog::debug("Binance HTTP {} {} retry {}/{}",
                              method, target, attempt, kMaxRetries - 1);
            }

            asio::io_context ioc;
            ssl::context ssl_ctx(ssl::context::tls_client);
            std::unique_ptr<ssl::stream<tcp::socket>> ssl_sock;

            try {
                bool useProxy = (cfg.proxy_port > 0 && !cfg.proxy_host.empty());

                if (useProxy) {
                    tcp::resolver resolver(ioc);
                    auto results = resolver.resolve(cfg.proxy_host,
                                                    std::to_string(cfg.proxy_port));
                    tcp::socket proxySock(ioc);
                    asio::connect(proxySock, results);

                    std::string connReq =
                        "CONNECT " + host + ":" + port +
                        " HTTP/1.1\r\nHost: " + host + ":" + port + "\r\n\r\n";
                    asio::write(proxySock, asio::buffer(connReq));

                    asio::streambuf respBuf;
                    asio::read_until(proxySock, respBuf, "\r\n\r\n");
                    std::string respStr(asio::buffers_begin(respBuf.data()),
                                        asio::buffers_begin(respBuf.data()) + respBuf.size());
                    if (respStr.find("200") == std::string::npos) {
                        spdlog::error("HTTP proxy CONNECT failed: {}", respStr);
                        return {};
                    }

                    ssl_sock = std::make_unique<ssl::stream<tcp::socket>>(
                        std::move(proxySock), ssl_ctx);
                } else {
                    tcp::resolver resolver(ioc);
                    auto results = resolver.resolve(host, port);
                    ssl_sock = std::make_unique<ssl::stream<tcp::socket>>(ioc, ssl_ctx);
                    asio::connect(ssl_sock->lowest_layer(), results);
                }

                if (!SSL_set_tlsext_host_name(ssl_sock->native_handle(), host.c_str())) {
                    spdlog::error("SSL SNI failed for {}", host);
                    return {};
                }
                ssl_sock->handshake(ssl::stream_base::client);

                http::verb v = (method == "POST")   ? http::verb::post
                               : (method == "DELETE") ? http::verb::delete_
                                                      : http::verb::put;

                http::request<http::string_body> req{v, target, 11};
                req.set(http::field::host, host);
                req.set(http::field::user_agent, "Chronos/1.0");
                req.set(http::field::content_type, "application/x-www-form-urlencoded");

                if (!cfg.api_key.empty())
                    req.set("X-MBX-APIKEY", cfg.api_key);

                if (!body.empty()) {
                    req.body() = body;
                    req.prepare_payload();
                }

                http::write(*ssl_sock, req);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(*ssl_sock, buffer, res);

                boost::system::error_code ec;
                ssl_sock->shutdown(ec);

                if (res.result() != http::status::ok) {
                    spdlog::error("Binance HTTP {} {} → {} {}",
                                  method, target,
                                  static_cast<int>(res.result()),
                                  res.body());
                    return {};
                }

                return res.body();
            } catch (const std::exception& e) {
                lastError = e.what();
                // Connection-level errors: retry. Application errors: fail fast.
                if (lastError.find("truncated") != std::string::npos ||
                    lastError.find("handshake") != std::string::npos ||
                    lastError.find("connection reset") != std::string::npos ||
                    lastError.find("broken pipe") != std::string::npos ||
                    lastError.find("eof") != std::string::npos) {
                    // Will retry
                } else {
                    break;  // Non-transient error, don't retry
                }
            }
        }

        spdlog::error("Binance HTTP {} {} failed after {} retries: {}",
                      method, target, kMaxRetries, lastError);
        return {};
    }
};

// ============================================================================
// Public interface
// ============================================================================

BinanceHttpClient::BinanceHttpClient() : pimpl_(std::make_unique<Impl>()) {}
BinanceHttpClient::~BinanceHttpClient() = default;

std::string BinanceHttpClient::placeOrder(const ExchangeConfig& cfg,
                                          const std::string& symbol,
                                          const std::string& side,
                                          const std::string& quantity,
                                          const std::string& price,
                                          const std::string& newClientOrderId) {
    std::string base = "symbol=" + symbol +
                       "&side=" + side +
                       "&type=LIMIT" +
                       "&timeInForce=GTC" +
                       "&quantity=" + quantity +
                       "&price=" + price;
    if (!newClientOrderId.empty())
        base += "&newClientOrderId=" + newClientOrderId;

    std::string query = Impl::buildQueryString(base, cfg.api_secret);
    return pimpl_->doRequest(cfg, "POST", "/fapi/v1/order?" + query, "", false);
}

bool BinanceHttpClient::cancelOrder(const ExchangeConfig& cfg,
                                    const std::string& symbol,
                                    uint64_t orderId) {
    std::string base = "symbol=" + symbol + "&orderId=" + std::to_string(orderId);
    std::string query = Impl::buildQueryString(base, cfg.api_secret);
    std::string resp = pimpl_->doRequest(cfg, "DELETE", "/fapi/v1/order?" + query, "", false);
    return !resp.empty();
}

std::string BinanceHttpClient::createListenKey(const ExchangeConfig& cfg) {
    // createListenKey is a POST with apikey header but NO signature
    std::string resp = pimpl_->doRequest(cfg, "POST", "/fapi/v1/listenKey", "", false);
    if (resp.empty()) return {};

    try {
        auto j = nlohmann::json::parse(resp);
        return j.value("listenKey", "");
    } catch (...) {
        spdlog::error("Failed to parse listenKey response: {}", resp);
        return {};
    }
}

bool BinanceHttpClient::keepAliveListenKey(const ExchangeConfig& cfg,
                                           const std::string& listenKey) {
    std::string resp = pimpl_->doRequest(
        cfg, "PUT", "/fapi/v1/listenKey?listenKey=" + listenKey, "", false);
    return !resp.empty();
}

bool BinanceHttpClient::closeListenKey(const ExchangeConfig& cfg,
                                       const std::string& listenKey) {
    std::string resp = pimpl_->doRequest(
        cfg, "DELETE", "/fapi/v1/listenKey?listenKey=" + listenKey, "", false);
    return !resp.empty();
}

}  // namespace execution
}  // namespace chronos
