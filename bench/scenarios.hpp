// bench/scenarios.hpp
#pragma once

// The six benchmark workloads, plus the counters that prove each one generated
// what its name claims.
//
// WHY THE COUNTERS EXIST: a scenario that silently degenerates into "everything
// rests, nothing crosses" produces excellent and completely meaningless numbers,
// and nothing else in the project would notice. measure_workload() runs the stream
// through an engine and reports what actually happened, and each scenario has a
// test asserting its counts land in the band it advertises.
//
// PRICES ARE GENERATED WITH INTEGER OPERATIONS ONLY. A floating-point power law
// would produce different streams on different libm implementations, and a
// benchmark input that is not reproducible is not a benchmark input. Counting
// trailing zero bits gives P(offset = k) = 2^-(k+1) exactly, everywhere.

#include <algorithm>
#include <cstdint>
#include <ob/command.hpp>
#include <ob/fast_engine.hpp>
#include <unordered_map>
#include <vector>

#include "../tests/model/scenario_gen.hpp"

namespace ob::bench {

enum class Scenario : std::uint8_t {
    RestOnly = 0,
    CrossShallow,
    CrossDeep,
    CancelHeavy,
    MixedRealistic,
    WorstCaseSweep,
};

inline constexpr Scenario kAllScenarios[] = {
    Scenario::RestOnly,    Scenario::CrossShallow,   Scenario::CrossDeep,
    Scenario::CancelHeavy, Scenario::MixedRealistic, Scenario::WorstCaseSweep,
};

inline const char* name(Scenario s) {
    switch (s) {
        case Scenario::RestOnly:
            return "rest_only";
        case Scenario::CrossShallow:
            return "cross_shallow";
        case Scenario::CrossDeep:
            return "cross_deep";
        case Scenario::CancelHeavy:
            return "cancel_heavy";
        case Scenario::MixedRealistic:
            return "mixed_realistic";
        case Scenario::WorstCaseSweep:
            return "worst_case_sweep";
    }
    return "?";
}

inline constexpr Ticks kMid = 10000;

// Geometric offset from the touch: P(k) = 2^-(k+1), integer-only, capped.
inline Ticks geometric_offset(obtest::Xoshiro256ss& rng, Ticks cap) {
    // The OR guards __builtin_ctzll(0), which is undefined.
    const std::uint64_t r = rng.next() | (std::uint64_t{1} << 48);
    const Ticks         k = static_cast<Ticks>(__builtin_ctzll(r));
    return k > cap ? cap : k;
}

// Tracks which orders are ACTUALLY resting, by running a shadow engine while the
// stream is generated and reading its events.
//
// The naive alternative - "assume every Limit rests" - is wrong whenever a Limit
// fully fills on arrival, and it made 11% of mixed_realistic's commands cancels
// against orders that never existed. That measures the rejection path, not the
// cancel path, which is exactly the self-deception the workload counters exist to
// catch.
class RestingSet {
public:
    explicit RestingSet(std::size_t capacity) : engine_(FastEngine::Config{capacity}) {
        storage_.resize(1 << 16);
    }

    // Submits `c` to the shadow engine and updates the resting set from its events.
    void apply(const Command& c) {
        EventBuffer buf(storage_.data(), storage_.size());
        engine_.submit(c, buf);

        bool rested = (c.type == CommandType::New) &&
                      (c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly);
        for (const Event& ev : buf) {
            switch (ev.type) {
                case EventType::Filled:
                case EventType::Cancelled:
                    if (ev.order_id == c.id) {
                        rested = false;
                    }
                    erase(ev.order_id);
                    break;
                case EventType::Rejected:
                    if (ev.order_id == c.id) {
                        rested = false;
                    }
                    break;
                default:
                    break;
            }
        }
        if (rested) {
            insert(c.id);
        }
    }

    [[nodiscard]] bool        empty() const noexcept { return ids_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }
    [[nodiscard]] OrderId     pick(std::uint64_t r) const noexcept {
        return ids_[static_cast<std::size_t>(r % ids_.size())];
    }

private:
    void insert(OrderId id) {
        if (where_.find(id) != where_.end()) {
            return;
        }
        where_[id] = ids_.size();
        ids_.push_back(id);
    }
    // Swap-with-last so removal stays O(1) and pick() stays uniform.
    void erase(OrderId id) {
        const auto it = where_.find(id);
        if (it == where_.end()) {
            return;
        }
        const std::size_t pos  = it->second;
        const OrderId     last = ids_.back();
        ids_[pos]              = last;
        where_[last]           = pos;
        ids_.pop_back();
        where_.erase(id);
    }

    FastEngine                               engine_;
    std::vector<Event>                       storage_;
    std::vector<OrderId>                     ids_;
    std::unordered_map<OrderId, std::size_t> where_;
};

inline std::vector<Command> build(Scenario s, std::uint64_t seed, std::size_t n) {
    obtest::Xoshiro256ss rng(seed ^ (static_cast<std::uint64_t>(s) << 56));
    std::vector<Command> out;
    out.reserve(n);
    OrderId id = 1;

    switch (s) {
        // Every order rests: buys strictly below the mid, sells strictly above.
        // Isolates the insert path, the ladder write and the bitmap set.
        case Scenario::RestOnly: {
            while (out.size() < n) {
                const bool  buy = (rng.bounded(2) == 0);
                const Ticks off = geometric_offset(rng, 200) + 1;
                const Ticks px  = buy ? (kMid - off) : (kMid + off);
                out.push_back(make_new(id++, buy ? Side::Buy : Side::Sell, OrderType::Limit, px,
                                       1 + static_cast<Qty>(rng.bounded(100))));
            }
            break;
        }

        // Alternates: rest one order, then cross it with exactly enough quantity.
        // This is the common real case, and it keeps the book one level deep.
        case Scenario::CrossShallow: {
            while (out.size() + 1 < n) {
                const bool    sell_first = (rng.bounded(2) == 0);
                const Qty     qty        = 1 + static_cast<Qty>(rng.bounded(100));
                const OrderId maker      = id++;
                const OrderId taker      = id++;
                out.push_back(make_new(maker, sell_first ? Side::Sell : Side::Buy, OrderType::Limit,
                                       kMid, qty));
                out.push_back(make_new(taker, sell_first ? Side::Buy : Side::Sell, OrderType::Limit,
                                       kMid, qty));
            }
            break;
        }

        // Builds depth across many levels, then sweeps 10 to 50 of them.
        // Isolates level traversal and bitmap advance.
        case Scenario::CrossDeep: {
            constexpr Ticks kLevels = 50;
            while (out.size() < n) {
                for (Ticks d = 1; d <= kLevels && out.size() < n; ++d) {
                    out.push_back(make_new(id++, Side::Sell, OrderType::Limit, kMid + d, 10));
                }
                if (out.size() < n) {
                    const Ticks depth = 10 + static_cast<Ticks>(rng.bounded(40));
                    out.push_back(make_new(id++, Side::Buy, OrderType::Limit, kMid + depth,
                                           static_cast<Qty>(10 * depth)));
                }
            }
            break;
        }

        // ~90% cancels, the realistic ratio. Cancels always target a live order,
        // because a flood of UnknownOrderId rejections would measure validation.
        // Nearly every order is cancelled rather than filled: the realistic
        // property. Note the CEILING: every cancel consumes one resting order and
        // every new order creates at most one, so cancels <= rests <= creates and
        // the cancel share of COMMANDS cannot exceed 50%. A "90% cancel" workload
        // is arithmetically impossible when each cancel must hit a distinct live
        // order. What is ~90:10 in real markets is cancels per TRADE, not per
        // message. This scenario therefore targets the 50% ceiling with a
        // near-zero unknown-cancel rate.
        case Scenario::CancelHeavy: {
            RestingSet resting(1 << 20);
            while (out.size() < n) {
                Command c{};
                if (!resting.empty()) {
                    c = make_cancel(resting.pick(rng.next()));
                } else {
                    const bool  buy = (rng.bounded(2) == 0);
                    const Ticks off = geometric_offset(rng, 200) + 1;
                    c               = make_new(id++, buy ? Side::Buy : Side::Sell, OrderType::Limit,
                                 buy ? kMid - off : kMid + off, 10);
                }
                resting.apply(c);
                out.push_back(c);
            }
            break;
        }

        // The headline workload: geometric depth, all five order types, roughly
        // a 1:3 cancel ratio, and enough crossing to exercise matching.
        case Scenario::MixedRealistic: {
            RestingSet resting(1 << 20);
            while (out.size() < n) {
                if (!resting.empty() && rng.bounded(100) < 30) {
                    const Command cx = make_cancel(resting.pick(rng.next()));
                    resting.apply(cx);
                    out.push_back(cx);
                    continue;
                }
                const bool          buy  = (rng.bounded(2) == 0);
                const std::uint64_t roll = rng.bounded(100);
                OrderType           t    = OrderType::Limit;
                if (roll < 3) {
                    t = OrderType::Market;
                } else if (roll < 8) {
                    t = OrderType::Ioc;
                } else if (roll < 11) {
                    t = OrderType::Fok;
                } else if (roll < 16) {
                    t = OrderType::PostOnly;
                }
                // Signed offset so roughly half the limits cross.
                const Ticks   off = geometric_offset(rng, 100);
                const Ticks   px  = buy ? (kMid + off - 2) : (kMid - off + 2);
                const Qty     qty = 1 + static_cast<Qty>(rng.bounded(100));
                const Command c   = make_new(id++, buy ? Side::Buy : Side::Sell, t, px, qty);
                resting.apply(c);
                out.push_back(c);
            }
            break;
        }

        // The true worst case: fill hundreds of levels, then one order that eats
        // the entire book. This is where the tail of the distribution comes from.
        case Scenario::WorstCaseSweep: {
            constexpr Ticks kLevels = 400;
            while (out.size() < n) {
                for (Ticks d = 1; d <= kLevels && out.size() < n; ++d) {
                    out.push_back(make_new(id++, Side::Sell, OrderType::Limit, kMid + d, 10));
                }
                if (out.size() < n) {
                    out.push_back(make_new(id++, Side::Buy, OrderType::Limit, kMid + kLevels,
                                           static_cast<Qty>(10 * kLevels)));
                }
            }
            break;
        }
    }
    out.resize(n);
    return out;
}

struct WorkloadStats {
    std::size_t commands = 0;
    std::size_t news     = 0;
    std::size_t cancels  = 0;
    std::size_t by_type[5]{};
    std::size_t trades                 = 0;
    std::size_t rejects                = 0;
    std::size_t rests                  = 0;
    std::size_t unknown_cancels        = 0;
    std::size_t max_sweep_levels       = 0;
    std::size_t max_events_per_command = 0;
};

// Runs the stream through a real engine and reports what actually happened. This
// is the anti-self-deception check: it is how a scenario that quietly stopped
// generating trades gets caught before its numbers are published.
inline WorkloadStats measure_workload(const std::vector<Command>& cmds) {
    WorkloadStats      st;
    FastEngine         e(FastEngine::Config{1 << 20});
    std::vector<Event> storage(1 << 16);
    EventBuffer        buf(storage.data(), storage.size());

    for (const Command& c : cmds) {
        ++st.commands;
        if (c.type == CommandType::Cancel) {
            ++st.cancels;
        } else {
            ++st.news;
            ++st.by_type[static_cast<std::size_t>(c.order_type)];
        }

        buf.clear();
        e.submit(c, buf);
        st.max_events_per_command = std::max(st.max_events_per_command, buf.size());

        std::size_t distinct_prices = 0;
        Ticks       last_px         = kNoPrice;
        bool        rested          = (c.type == CommandType::New);
        for (const Event& ev : buf) {
            switch (ev.type) {
                case EventType::Trade:
                    ++st.trades;
                    if (ev.price != last_px) {
                        ++distinct_prices;
                        last_px = ev.price;
                    }
                    break;
                case EventType::Rejected:
                    ++st.rejects;
                    rested = false;
                    if (ev.reject == RejectReason::UnknownOrderId) {
                        ++st.unknown_cancels;
                    }
                    break;
                case EventType::Cancelled:
                case EventType::Filled:
                    if (ev.order_id == c.id) {
                        rested = false;
                    }
                    break;
                case EventType::Accepted:
                    break;
            }
        }
        st.max_sweep_levels = std::max(st.max_sweep_levels, distinct_prices);
        if (rested) {
            ++st.rests;
        }
    }
    return st;
}

}  // namespace ob::bench
