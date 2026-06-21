#pragma once

#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <memory>
#include <functional>
#include <string>

namespace chronos {
namespace market_data {

// fwd
struct Transport;
struct Protocol;

/// Single concrete adapter — composes Transport + Protocol.
///
/// Satisfies ExchangeAdapter (verified by static_assert).  No inheritance.
/// All implementation details (threads, atomics, mutex, subscription state,
/// statistics, heartbeats, reconnect logic) are hidden behind Pimpl.
///
/// A new exchange requires only a new Protocol implementation (~100-150 lines);
/// the Transport and adapter lifecycle are fully reused.
class ThinMuxAdapter {
public:
    using StatusCallback = std::function<void(ConnectionStatus)>;
    using ErrorCallback  = std::function<void(const std::string&)>;

    ThinMuxAdapter();
    ~ThinMuxAdapter();

    // Movable (unique_ptr members + user-declared dtor suppress implicit move)
    ThinMuxAdapter(ThinMuxAdapter&&) noexcept;
    ThinMuxAdapter& operator=(ThinMuxAdapter&&) noexcept;

    /// Take ownership of transport and protocol (typically created by factory).
    ThinMuxAdapter(std::unique_ptr<Transport> transport,
                   std::unique_ptr<Protocol> protocol);

    // --- ExchangeAdapter contract ---
    bool initialize(const ExchangeConfig& cfg,
                    utils::MPMCQueue<Tick, 65536>& queue);
    bool start();
    void stop();
    bool subscribe(std::string_view symbol);
    bool unsubscribe(std::string_view symbol);
    bool isRunning() const;
    ConnectionStatus getStatus() const;
    GatewayStatistics getStatistics() const;

    // --- Callbacks ---
    void setStatusCallback(StatusCallback cb);
    void setErrorCallback(ErrorCallback cb);

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace market_data
} // namespace chronos
