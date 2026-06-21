#pragma once

#include <chronos/market_data/adapter_concept.hpp>
#include <chronos/core/types.hpp>
#include <chronos/core/config.hpp>
#include <chronos/utils/mpmc_queue.hpp>
#include <memory>
#include <string_view>
#include <type_traits>

namespace chronos {
namespace market_data {

/// Type-erased wrapper for any type satisfying ExchangeAdapter.
///
/// Provides runtime polymorphism without requiring inheritance from a
/// common base class.  Internally uses a classic virtual Concept + Model<T>
/// pattern (one vtable dispatch per call — equivalent cost to the current
/// MarketDataGateway virtual functions).
///
/// Usage:
///   AnyGateway gw(ThinMuxAdapter{});
///   gw.start();
///   gw.subscribe("BTCUSDT");
class AnyGateway {
public:
    AnyGateway() = default;

    /// Construct from any type satisfying ExchangeAdapter.
    ///
    /// Takes adapter by value (not forwarding reference).  This guarantees:
    ///  - lvalues T& → compile error (can't bind to value parameter whose
    ///    type is deduced from an lvalue — adapter must be std::move()'d)
    ///  - const/volatile qualification is stripped by value semantics
    ///  - T is always a plain type, removing the need for remove_cvref_t
    ///
    /// The extra move (vs forwarding ref) costs one unique_ptr pointer swap
    /// and is trivially cheap.
    template <ExchangeAdapter T>
        requires (!std::same_as<T, AnyGateway>)
    explicit AnyGateway(T adapter)
        : impl_(std::make_unique<Model<T>>(std::move(adapter))) {}

    // Movable
    AnyGateway(AnyGateway&&) noexcept = default;
    AnyGateway& operator=(AnyGateway&&) noexcept = default;

    // --- Public interface (matches ExchangeAdapter concept) ---

    bool initialize(const ExchangeConfig& cfg,
                    utils::MPMCQueue<Tick, 65536>& queue) {
        return impl_ && impl_->initialize(cfg, queue);
    }

    bool start()              { return impl_ && impl_->start(); }
    void stop()               { if (impl_) impl_->stop(); }
    bool subscribe(std::string_view sym) { return impl_ && impl_->subscribe(sym); }
    bool unsubscribe(std::string_view sym) { return impl_ && impl_->unsubscribe(sym); }
    bool isRunning() const    { return impl_ && impl_->isRunning(); }
    ConnectionStatus getStatus() const { return impl_ ? impl_->getStatus()
                                                       : ConnectionStatus::DISCONNECTED; }
    GatewayStatistics getStatistics() const {
        return impl_ ? impl_->getStatistics() : GatewayStatistics{};
    }

    explicit operator bool() const { return impl_ != nullptr; }

private:
    struct Concept {
        virtual ~Concept() = default;
        virtual bool initialize(const ExchangeConfig&,
                                utils::MPMCQueue<Tick, 65536>&) = 0;
        virtual bool              start() = 0;
        virtual void              stop() = 0;
        virtual bool              subscribe(std::string_view) = 0;
        virtual bool              unsubscribe(std::string_view) = 0;
        virtual bool              isRunning() const = 0;
        virtual ConnectionStatus  getStatus() const = 0;
        virtual GatewayStatistics getStatistics() const = 0;
    };

    template <ExchangeAdapter T>
    struct Model final : Concept {
        T adapter;
        explicit Model(T&& a) : adapter(std::move(a)) {}

        bool initialize(const ExchangeConfig& cfg,
                        utils::MPMCQueue<Tick, 65536>& q) override {
            return adapter.initialize(cfg, q);
        }
        bool              start() override               { return adapter.start(); }
        void              stop() override                { adapter.stop(); }
        bool              subscribe(std::string_view s) override  { return adapter.subscribe(s); }
        bool              unsubscribe(std::string_view s) override { return adapter.unsubscribe(s); }
        bool              isRunning() const override     { return adapter.isRunning(); }
        ConnectionStatus  getStatus() const override     { return adapter.getStatus(); }
        GatewayStatistics getStatistics() const override { return adapter.getStatistics(); }
    };

    std::unique_ptr<Concept> impl_;
};

} // namespace market_data
} // namespace chronos
