#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace {

using ob::OrderType;
using ob::Side;

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(Invariants, HoldOnAnEmptyBook) {
    const ob::ReferenceEngine e;
    const auto r = ob::check_invariants(e);
    EXPECT_TRUE(r.ok) << r.failure;
}

TEST(Invariants, HoldAfterASingleResting) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    const auto r = ob::check_invariants(e);
    EXPECT_TRUE(r.ok) << r.failure;
}

TEST(Invariants, HoldAfterEveryOperationInARandomStream) {
    ob::ReferenceEngine e;
    std::mt19937_64 rng(20260922);
    std::uniform_int_distribution<int> which(0, 9);
    std::uniform_int_distribution<ob::Ticks> px(9990, 10010);
    std::uniform_int_distribution<std::uint32_t> qty(1, 50);

    std::vector<ob::OrderId> live;
    ob::OrderId next_id = 1;

    for (int i = 0; i < 20000; ++i) {
        ob::Command c{};
        if (which(rng) < 3 && !live.empty()) {
            const std::size_t k = rng() % live.size();
            c = ob::make_cancel(live[k]);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(k));
        } else {
            const Side s = (which(rng) % 2 == 0) ? Side::Buy : Side::Sell;
            const OrderType t = static_cast<OrderType>(which(rng) % 5);
            c = ob::make_new(next_id, s, t, px(rng), qty(rng));
            if (t == OrderType::Limit || t == OrderType::PostOnly) {
                live.push_back(next_id);
            }
            ++next_id;
        }
        feed(e, c);

        const auto r = ob::check_invariants(e);
        ASSERT_TRUE(r.ok) << "broke at i=" << i << ": " << r.failure;
    }
}

// Test doubles that deliberately violate an invariant.
//
// These MUST live at namespace scope: C++ forbids both static data members and
// member templates inside a local class, and these need `kTracksArrival` and a
// templated `for_each_resting`.

// arrival sequence 5 before 2 at the same level: FIFO is violated.
struct BrokenFifoEngine {
    static constexpr bool kTracksArrival = true;
    void submit(const ob::Command&, ob::EventBuffer&) {}
    [[nodiscard]] ob::Ticks best_bid() const { return 10000; }
    [[nodiscard]] ob::Ticks best_ask() const { return ob::kNoPrice; }
    void reset() {}
    [[nodiscard]] std::size_t live_order_count() const { return 2; }

    template <class Fn>
    void for_each_resting(Fn&& fn) const {
        fn(ob::RestingOrder{Side::Buy, 10000, 1, 10, 5});
        fn(ob::RestingOrder{Side::Buy, 10000, 2, 10, 2});
    }
};

// best_bid above best_ask: the book is crossed.
struct CrossedBookEngine {
    static constexpr bool kTracksArrival = true;
    void submit(const ob::Command&, ob::EventBuffer&) {}
    [[nodiscard]] ob::Ticks best_bid() const { return 10010; }
    [[nodiscard]] ob::Ticks best_ask() const { return 10000; }
    void reset() {}
    [[nodiscard]] std::size_t live_order_count() const { return 2; }

    template <class Fn>
    void for_each_resting(Fn&& fn) const {
        fn(ob::RestingOrder{Side::Buy, 10010, 1, 10, 0});
        fn(ob::RestingOrder{Side::Sell, 10000, 2, 10, 1});
    }
};

// The checker must be able to fail. A checker that can only return ok is not a
// checker, and these two tests are what prove it works.
TEST(Invariants, DetectAnInjectedFifoViolation) {
    const BrokenFifoEngine b;
    const auto r = ob::check_invariants(b);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string(r.failure).find("FIFO"), std::string::npos) << r.failure;
}

TEST(Invariants, DetectAnInjectedCrossedBook) {
    const CrossedBookEngine c;
    const auto r = ob::check_invariants(c);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string(r.failure).find("crossed"), std::string::npos) << r.failure;
}

}  // namespace
