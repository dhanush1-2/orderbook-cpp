# Phase 6: Multi-Symbol Book Reconstruction — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild the full limit order book for a chosen set of symbols from the decoded ITCH feed, validated against the feed's own executions, with the parser and the book on separate threads, and measure it.

**Architecture:** A parser thread decodes and filters by `stock_locate`, pushing accepted messages through a single-producer single-consumer ring to a book thread. The book thread owns one `SymbolBook` per tracked symbol: a per-symbol price grid over the existing flat ladder, plus an order table keyed by ITCH order reference. Reconstruction is driven entirely by the feed, so the feed's own execution messages are a free correctness oracle.

**Tech Stack:** C++20. Reuses `ob::PriceLadder`, `ob::IdIndex` and the arena from Phases 1–4. No new dependencies.

**Spec:** [`../specs/2026-09-25-itch-replay-and-research-agent-design.md`](../specs/2026-09-25-itch-replay-and-research-agent-design.md)

**Depends on:** Phase 5. The decoder, `ItchMessage`, and the committed 10 MB slice must exist.

## This phase does NOT match orders

The engine from Phases 1–4 is a **matching engine**: it decides which orders trade.
This phase is a **reconstruction engine**: the exchange already decided, and the feed
reports the outcome. Reconstruction never crosses a book, never picks a counterparty,
and never invents a fill. It applies what it is told.

Getting this backwards is the single most damaging mistake available here, because a
reconstruction that "helpfully" matches a crossed book produces plausible output that
disagrees with reality in ways no local test would catch. **The rule: only `E`, `C`,
`X`, `D` and `U` ever remove liquidity, and only by the amount the message states.**

## Global Constraints

Everything in [`README.md`](README.md) and Phase 5's constraints still apply. New here:

- **Order references are NOT monotonic and NOT dense.** Measured on real data (see the
  verification section): 26.5% of consecutive references are non-increasing. Any scheme
  that assumes `ref > previous_ref`, or indexes an array directly by reference, is wrong.
- **`E`, `C`, `X`, `D`, `U` may arrive for a reference that was never added.** This is
  normal, not corruption: it happens on every start that is not the file's first byte,
  and for orders added before the tracked window. Count them, do not fail on them.
- **An execution that empties an order implicitly deletes it.** There is no `D` to follow.
- **`U` (replace) is a delete plus an add with a NEW reference.** It loses time priority.
  The old reference must be removed and must not be reusable.
- **`X` (cancel) is PARTIAL.** It reduces the resting quantity; it does not remove the
  order. Only `D` removes it outright.
- **Never trust a quantity to be within bounds.** Subtracting more than rests underflows
  an unsigned counter into a colossal number that then corrupts every depth query.
- **`P` (non-cross trade) does NOT touch the book.** It reports a trade against a hidden
  or non-displayed order. Applying it double-counts. It is a signal input, not a mutation.
- **One writer.** The book thread is the only mutator, so snapshots use the existing
  seqlock rather than a mutex.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `include/ob/itch/symbol_router.hpp` | `stock_locate` → tracked book slot, or dropped | 1 |
| `include/ob/itch/price_grid.hpp` | Per-symbol raw price ↔ ladder index, and rebasing | 2 |
| `include/ob/itch/order_table.hpp` | ITCH order reference → live order record | 3 |
| `include/ob/itch/symbol_book.hpp` | One symbol's book; applies A/F/E/C/X/D/U | 4 |
| `include/ob/itch/replay.hpp` | Drives decoder → router → books; counters | 5 |
| `include/ob/spsc_ring.hpp` | Single-producer single-consumer ring | 6 |
| `tools/ob_replay.cpp` | Two-thread replay, snapshots, throughput | 7 |
| `bench/bench_replay.cpp` | Per-message latency percentiles | 7 |

---

## Task 1: Symbol routing

**Files:** Create `include/ob/itch/symbol_router.hpp`, `tests/test_symbol_router.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ob::itch::Symbol`, `ob::itch::StockLocate`, `ob::itch::StockDirectory`.
- Produces: `ob::itch::SymbolRouter` with `void watch(Symbol)`, `void on_directory(const StockDirectory&)`, `[[nodiscard]] BookSlot slot_of(StockLocate) const`, `kUntracked`, `[[nodiscard]] std::size_t tracked_count() const`, `[[nodiscard]] Symbol symbol_at(BookSlot) const`, `[[nodiscard]] bool all_resolved() const`, `[[nodiscard]] std::size_t unresolved_count() const`.

Every message carries a `stock_locate` in its common header, and `stock_locate` is a
`uint16`. So routing is a **65,536-byte lookup table**, one load per message, no hashing
and no branching on symbol text. This matters: the filter runs on all ~300M messages,
while the book runs on a few million.

The mapping from symbol name to locate exists **only** in the `R` StockDirectory
messages at the head of the file. A router asked to watch `AAPL` therefore knows nothing
until it sees `AAPL`'s directory entry. Any design that needs the mapping up front
is wrong, and any run that skips the directory block silently tracks nothing.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_symbol_router.cpp
#include <ob/itch/symbol_router.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <vector>

namespace {

using namespace ob::itch;

Symbol sym(const char* s) {
    char pad[8];
    std::memset(pad, ' ', 8);
    std::memcpy(pad, s, std::strlen(s));
    return Symbol::from_bytes(reinterpret_cast<const std::byte*>(pad));
}

StockDirectory dir(StockLocate loc, const char* name) {
    StockDirectory d{};
    d.h.type = MsgType::StockDirectory;
    d.h.stock_locate = loc;
    d.stock = sym(name);
    return d;
}

TEST(SymbolRouter, EverythingIsUntrackedBeforeAnyDirectoryMessage) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    EXPECT_EQ(r.tracked_count(), 0u) << "watching a name resolves nothing on its own";
    EXPECT_EQ(r.unresolved_count(), 1u);
    EXPECT_FALSE(r.all_resolved());
    // Every locate, including the ones we will eventually want.
    for (std::uint32_t l = 0; l <= 0xFFFF; ++l) {
        ASSERT_EQ(r.slot_of(static_cast<StockLocate>(l)), SymbolRouter::kUntracked)
            << "locate " << l;
    }
}

TEST(SymbolRouter, ResolvesOnlyWatchedSymbolsAndAssignsSlotsInWatchOrder) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    ASSERT_TRUE(r.watch(sym("MSFT")));

    r.on_directory(dir(1, "A"));        // not watched
    r.on_directory(dir(4242, "MSFT"));  // watched, but the SECOND one watched
    r.on_directory(dir(7, "AAPL"));

    EXPECT_EQ(r.tracked_count(), 2u);
    EXPECT_TRUE(r.all_resolved());
    // Slots follow WATCH order, not directory order, so a caller that passed a symbol
    // list can index its own arrays by slot without another lookup.
    EXPECT_EQ(r.slot_of(7), 0u);
    EXPECT_EQ(r.slot_of(4242), 1u);
    EXPECT_EQ(r.slot_of(1), SymbolRouter::kUntracked);
    EXPECT_EQ(r.symbol_at(0).str(), "AAPL");
    EXPECT_EQ(r.symbol_at(1).str(), "MSFT");
}

TEST(SymbolRouter, DuplicateWatchIsIdempotent) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    ASSERT_TRUE(r.watch(sym("AAPL")));
    r.on_directory(dir(9, "AAPL"));
    EXPECT_EQ(r.tracked_count(), 1u);
    EXPECT_EQ(r.slot_of(9), 0u);
}

// The same symbol can legitimately appear in the directory more than once in a day.
// Re-resolving must not allocate a second slot, or every later message for it routes
// to a book that holds half its orders.
TEST(SymbolRouter, RepeatedDirectoryEntryKeepsTheSameSlot) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    r.on_directory(dir(9, "AAPL"));
    r.on_directory(dir(9, "AAPL"));
    EXPECT_EQ(r.tracked_count(), 1u);
    EXPECT_EQ(r.slot_of(9), 0u);
}

// A symbol that changes locate mid-day must route from BOTH, otherwise orders silently
// vanish from the book at the moment of the change.
TEST(SymbolRouter, SymbolAppearingUnderASecondLocateRoutesFromBoth) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    r.on_directory(dir(9, "AAPL"));
    r.on_directory(dir(10, "AAPL"));
    EXPECT_EQ(r.tracked_count(), 1u) << "still one book";
    EXPECT_EQ(r.slot_of(9), 0u);
    EXPECT_EQ(r.slot_of(10), 0u) << "both locates must reach the same book";
}

TEST(SymbolRouter, LocateZeroIsNeverTracked) {
    SymbolRouter r;
    ASSERT_TRUE(r.watch(sym("AAPL")));
    r.on_directory(dir(0, "AAPL"));  // locate 0 is the administrative channel
    EXPECT_EQ(r.slot_of(0), SymbolRouter::kUntracked);
    EXPECT_EQ(r.tracked_count(), 0u);
}

TEST(SymbolRouter, RejectsMoreSymbolsThanSlotsRatherThanOverflowing) {
    SymbolRouter r;
    char name[8];
    for (unsigned i = 0; i < SymbolRouter::kMaxTracked; ++i) {
        std::snprintf(name, sizeof(name), "S%05u", i);
        EXPECT_TRUE(r.watch(sym(name))) << "symbol " << i;
    }
    EXPECT_FALSE(r.watch(sym("ONEMORE"))) << "must refuse, not wrap to slot 0";
}

// The real directory block, against real names.
TEST(SymbolRouter, ResolvesRealSymbolsFromTheRealSlice) {
    std::ifstream f("testdata/itch_slice_10mb.bin", std::ios::binary);
    ASSERT_TRUE(f) << "run scripts/fetch_itch.sh first";
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                                  raw.size());
    SymbolRouter r;
    for (const char* s : {"AAPL", "MSFT", "SPY", "AMZN", "INTC"}) {
        ASSERT_TRUE(r.watch(sym(s)));
    }
    std::size_t off = 0;
    while (off < in.size()) {
        ItchMessage m{};
        const auto res = decode(in.subspan(off), m);
        if (res.consumed == 0) break;
        if (res.ok() && m.header.type == MsgType::StockDirectory) {
            r.on_directory(m.stock_directory);
        }
        off += res.consumed;
    }
    EXPECT_EQ(r.tracked_count(), 5u) << "all five are listed on Nasdaq";
    EXPECT_TRUE(r.all_resolved()) << "unresolved: " << r.unresolved_count();
    for (unsigned s = 0; s < 5; ++s) {
        EXPECT_NE(r.slot_of(1), s) << "locate 1 is 'A', which is not watched";
    }
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails** (`'ob/itch/symbol_router.hpp' file not found`).

- [ ] **Step 3: Write `include/ob/itch/symbol_router.hpp`**

```cpp
// include/ob/itch/symbol_router.hpp
#pragma once

// Routes an ITCH message to a book by its stock_locate.
//
// Every message's common header carries a uint16 stock_locate, so routing is a single
// 64 KB table lookup rather than a hash of eight symbol bytes. The filter runs on all
// ~300M messages in a day while the books run on a few million, so this is the hottest
// decision in the replay.
//
// The symbol -> locate mapping exists ONLY in the R StockDirectory messages at the head
// of the file. A router therefore resolves nothing until it has seen them, and a run
// that starts after the directory block tracks nothing at all. `all_resolved()` is how
// a caller notices that rather than silently replaying an empty book.

#include <ob/itch/decoder.hpp>
#include <ob/itch/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob::itch {

using BookSlot = std::uint8_t;

class SymbolRouter {
public:
    static constexpr BookSlot   kUntracked  = 0xFF;
    static constexpr unsigned   kMaxTracked = 255;  // 0..254; 0xFF is the sentinel

    SymbolRouter() { slot_.fill(kUntracked); }

    // Register interest in a symbol. Returns false only when full. Idempotent.
    [[nodiscard]] bool watch(Symbol s) {
        for (const auto& w : watched_) {
            if (w.name == s) {
                return true;
            }
        }
        if (watched_.size() >= kMaxTracked) {
            return false;
        }
        watched_.push_back({s, false});
        return true;
    }

    // Feed every R message here. Assigns the slot on first resolution.
    void on_directory(const StockDirectory& d) {
        // Locate 0 is the administrative channel (system events carry it), never a
        // security. Tracking it would route unrelated messages into a book.
        if (d.h.stock_locate == 0) {
            return;
        }
        for (std::size_t i = 0; i < watched_.size(); ++i) {
            if (watched_[i].name != d.stock) {
                continue;
            }
            // A symbol can appear under more than one locate in a day. Point every
            // one of them at the SAME slot; allocating a second book would split the
            // symbol's orders across two half-populated books.
            slot_[d.h.stock_locate] = static_cast<BookSlot>(i);
            watched_[i].resolved = true;
            return;
        }
    }

    [[nodiscard]] BookSlot slot_of(StockLocate loc) const noexcept { return slot_[loc]; }

    [[nodiscard]] std::size_t tracked_count() const noexcept {
        std::size_t n = 0;
        for (const auto& w : watched_) {
            n += w.resolved ? 1 : 0;
        }
        return n;
    }
    [[nodiscard]] std::size_t unresolved_count() const noexcept {
        return watched_.size() - tracked_count();
    }
    [[nodiscard]] bool all_resolved() const noexcept { return unresolved_count() == 0; }
    [[nodiscard]] std::size_t watched_count() const noexcept { return watched_.size(); }
    [[nodiscard]] Symbol symbol_at(BookSlot s) const noexcept { return watched_[s].name; }

private:
    struct Watched {
        Symbol name;
        bool   resolved;
    };
    std::array<BookSlot, 65536> slot_{};  // 64 KB, indexed by stock_locate
    std::vector<Watched>        watched_;
};

}  // namespace ob::itch
```

Two notes for the implementer:

- `operator!=` is used on `Symbol`; C++20 synthesises it from the `operator==` defined
  in Phase 5, so nothing new is needed there.
- **`watch()` is `[[nodiscard]]` and every call site must consume the result.** It
  returns false when the table is full, which is a silent tracking failure if ignored.
  The tests above wrap each call in `ASSERT_TRUE` for exactly this reason; a bare call
  is a `-Wunused-result` warning and this project builds CI with `-Werror`.

- [ ] **Step 4: Run the tests**

```bash
cmake --build build && ./build/tests/ob_tests '--gtest_filter=SymbolRouter.*'
```

Expected: PASS, 8 tests.

- [ ] **Step 5: Commit**

```bash
git add include/ob/itch/symbol_router.hpp tests/test_symbol_router.cpp tests/CMakeLists.txt
git commit -m "feat(itch): route messages to books by stock_locate"
```

---

## Task 2: Per-symbol price grid

**Files:** Create `include/ob/itch/price_grid.hpp`, `tests/test_price_grid.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ob::Ticks`, `ob::kMinTick`, `ob::kMaxTick`, `ob::kLadderSize`, `ob::itch::Price4`.
- Produces: `ob::itch::PriceGrid` with `void init_from(Price4)`, `[[nodiscard]] bool initialized() const`, `[[nodiscard]] Ticks index_of(Price4) const` (returns `kOutOfWindow` when not representable), `[[nodiscard]] Price4 price_at(Ticks) const`, `[[nodiscard]] std::int32_t tick() const`, `[[nodiscard]] std::int64_t base() const`, and `kOutOfWindow`.

The existing ladder indexes `Ticks` in `[1, 65536]`. This maps a raw ITCH price onto
that range and reports honestly when it cannot.

**Read spec section 6.3 before starting.** The short version: the median symbol's price
span is 19,999,998 cents, so **the window cannot cover the range and is not supposed
to**. `index_of` returning `kOutOfWindow` is a normal, frequently-taken path — 6.9% of
AMZN's adds — not an error. Task 4 sends those to an overflow map.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_price_grid.cpp
#include <ob/itch/price_grid.hpp>

#include <gtest/gtest.h>

namespace {

using namespace ob::itch;
using ob::Ticks;

TEST(PriceGrid, IsUnusableUntilInitialised) {
    PriceGrid g;
    EXPECT_FALSE(g.initialized());
    EXPECT_EQ(g.index_of(553600), PriceGrid::kOutOfWindow)
        << "an uninitialised grid must refuse, never return index 0";
}

// At or above $1 the SEC forbids sub-penny quoting, so the tick is one cent.
TEST(PriceGrid, UsesACentTickAtOrAboveOneDollar) {
    PriceGrid g;
    g.init_from(553600);  // $55.36
    EXPECT_TRUE(g.initialized());
    EXPECT_EQ(g.tick(), 100);
}

// Below $1 sub-penny prices are legal and real: the measured minimum was $0.0004.
// A cent tick there would collapse distinct prices onto one level.
TEST(PriceGrid, UsesARawTickBelowOneDollar) {
    PriceGrid g;
    g.init_from(4);  // $0.0004
    EXPECT_EQ(g.tick(), 1);
}

TEST(PriceGrid, CentresTheWindowOnTheFirstPriceSoItHasRoomBothWays) {
    PriceGrid g;
    g.init_from(5'000'000);  // $500: high enough that a centred base stays positive
    const Ticks mid = g.index_of(5'000'000);
    EXPECT_NE(mid, PriceGrid::kOutOfWindow);
    // Roughly centred, so the market can move either way before overflowing.
    EXPECT_GT(mid, static_cast<Ticks>(ob::kLadderSize / 4));
    EXPECT_LT(mid, static_cast<Ticks>(3 * ob::kLadderSize / 4));
    EXPECT_GE(mid, ob::kMinTick);
    EXPECT_LE(mid, ob::kMaxTick);
}

// Below about $327 a centred base would be negative, so it clamps to zero and the
// window runs from $0 upward instead. That is the right answer, not a degradation: a
// cheap stock cannot fall below zero, so all the headroom belongs above.
TEST(PriceGrid, LowPricedSymbolGetsAZeroBasedWindowRatherThanANegativeBase) {
    PriceGrid g;
    g.init_from(553600);  // $55.36
    EXPECT_EQ(g.base(), 0);
    EXPECT_EQ(g.index_of(553600), 5537);
    EXPECT_EQ(g.price_at(ob::kMinTick), 0u);
    EXPECT_EQ(g.price_at(ob::kMaxTick), 6'553'500u) << "$655.35, the top of the window";
    // A $0.01 stub bid IS inside this window, and must be reported as such.
    EXPECT_NE(g.index_of(100), PriceGrid::kOutOfWindow);
}

TEST(PriceGrid, RoundTripsEveryRepresentablePrice) {
    PriceGrid g;
    g.init_from(5'000'000);
    for (Ticks t = ob::kMinTick; t <= ob::kMaxTick; ++t) {
        const Price4 px = g.price_at(t);
        ASSERT_EQ(g.index_of(px), t) << "tick " << t << " price " << px;
    }
}

TEST(PriceGrid, AdjacentCentsAreAdjacentTicks) {
    PriceGrid g;
    g.init_from(5'000'000);
    EXPECT_EQ(g.index_of(5'000'100) - g.index_of(5'000'000), 1);
    EXPECT_EQ(g.index_of(5'000'000) - g.index_of(4'999'900), 1);
}

// The measured reality: stub quotes near $0.01 and $200,000 exist for every symbol.
// Both must be reported out of window, NOT clamped to the ladder edge, because a
// clamp would put a $200,000 ask on the same level as a real one.
TEST(PriceGrid, StubQuotePricesAreOutOfWindowRatherThanClamped) {
    PriceGrid g;
    g.init_from(5'000'000);  // a $500 stock, so both stubs fall outside
    EXPECT_EQ(g.index_of(100), PriceGrid::kOutOfWindow) << "$0.01 stub bid";
    EXPECT_EQ(g.index_of(2'000'000'000u), PriceGrid::kOutOfWindow) << "$200,000 stub ask";
    EXPECT_EQ(g.index_of(0), PriceGrid::kOutOfWindow);
}

// Exactly at the two boundaries, and exactly one step beyond each.
TEST(PriceGrid, BoundariesAreInclusiveAndOneStepBeyondIsNot) {
    PriceGrid g;
    // A $500 grid, so the bottom of the window is well above zero and `lo - 100` is a
    // real price rather than an unsigned underflow.
    g.init_from(5'000'000);
    const Price4 lo = g.price_at(ob::kMinTick);
    const Price4 hi = g.price_at(ob::kMaxTick);
    EXPECT_EQ(g.index_of(lo), ob::kMinTick);
    EXPECT_EQ(g.index_of(hi), ob::kMaxTick);
    EXPECT_EQ(g.index_of(lo - 100), PriceGrid::kOutOfWindow);
    EXPECT_EQ(g.index_of(hi + 100), PriceGrid::kOutOfWindow);
}

// A price between two ticks cannot happen at or above $1 (no sub-penny quoting), but
// the grid must not silently round if it ever does, because rounding invents a level.
TEST(PriceGrid, OffGridPriceIsRefusedNotRounded) {
    PriceGrid g;
    g.init_from(5'000'000);
    EXPECT_EQ(g.index_of(5'000'050), PriceGrid::kOutOfWindow) << "half a cent";
}

// A $200,000 first price must not make `base` negative or overflow the arithmetic.
TEST(PriceGrid, ExtremeFirstPriceDoesNotUnderflowTheBase) {
    PriceGrid g;
    g.init_from(2'000'000'000u);
    EXPECT_TRUE(g.initialized());
    EXPECT_GE(g.base(), 0);
    EXPECT_NE(g.index_of(2'000'000'000u), PriceGrid::kOutOfWindow);

    PriceGrid g2;
    g2.init_from(100);  // $0.01, so a centred base would go below zero
    EXPECT_TRUE(g2.initialized());
    EXPECT_GE(g2.base(), 0);
    EXPECT_NE(g2.index_of(100), PriceGrid::kOutOfWindow)
        << "the first price must always be representable in its own grid";
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails.**

- [ ] **Step 3: Write `include/ob/itch/price_grid.hpp`**

```cpp
// include/ob/itch/price_grid.hpp
#pragma once

// Maps a raw ITCH price (uint32, 4 implied decimals) onto the existing ladder's
// Ticks range [kMinTick, kMaxTick].
//
// THE WINDOW DOES NOT COVER THE PRICE RANGE, AND IS NOT MEANT TO. Measured over 53.8M
// real messages: the median symbol's price span is 19,999,998 cents, because market
// makers rest stub quotes near $0.01 and $200,000 to satisfy two-sided quoting
// obligations. 6,557 of 8,892 symbols exceed a 65,536-cent span.
//
// Occupancy is a different story: the worst of ten liquid symbols peaked at 5,086
// distinct live levels. So the window holds the real market comfortably and the tail
// goes to an overflow map (Task 4). `kOutOfWindow` is a normal return value taken on
// 6.9% of AMZN's adds -- it is not an error and must never be treated as one.
//
// Nothing here rounds. A price that is not exactly on the grid is refused, because
// rounding would merge two distinct price levels into one and silently corrupt depth.

#include <ob/itch/types.hpp>
#include <ob/types.hpp>

#include <cstdint>

namespace ob::itch {

class PriceGrid {
public:
    // Distinct from every valid Ticks value, which start at kMinTick == 1.
    static constexpr ob::Ticks kOutOfWindow = 0;

    static constexpr std::int32_t kCentTick    = 100;    // one cent in 1/10000 units
    static constexpr std::int32_t kRawTick     = 1;      // sub-penny, below $1
    static constexpr Price4       kOneDollar   = 10'000;

    // Establish the grid from the first priced message seen for this symbol.
    void init_from(Price4 first_px) noexcept {
        // At or above $1, SEC Rule 612 forbids sub-penny quoting, so every price is a
        // whole cent and a cent tick loses nothing. Below $1 sub-penny prices are legal
        // and were measured down to $0.0004, so the tick must stay raw there.
        tick_ = (first_px >= kOneDollar) ? kCentTick : kRawTick;

        // Centre the window so the market can move either way before overflowing.
        const std::int64_t half = static_cast<std::int64_t>(ob::kLadderSize / 2) * tick_;
        std::int64_t b = static_cast<std::int64_t>(first_px) - half;

        // A low first price would push the base below zero. Clamp to the first
        // representable multiple of the tick at or above zero: the first price must
        // always be inside its own grid, or the symbol is unusable from its first
        // message.
        if (b < 0) {
            b = 0;
        }
        // Align the base so first_px lands exactly on a tick boundary. Without this a
        // $55.36 stock whose base is not a multiple of a cent refuses every real price.
        b -= (static_cast<std::int64_t>(first_px) - b) % tick_;
        base_        = b;
        initialized_ = true;
    }

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] std::int32_t tick() const noexcept { return tick_; }
    [[nodiscard]] std::int64_t base() const noexcept { return base_; }

    // kOutOfWindow when the price is below the base, above the top, or not exactly on
    // a tick boundary. Never rounds, never clamps.
    [[nodiscard]] ob::Ticks index_of(Price4 px) const noexcept {
        if (!initialized_) {
            return kOutOfWindow;
        }
        const std::int64_t delta = static_cast<std::int64_t>(px) - base_;
        if (delta < 0 || (delta % tick_) != 0) {
            return kOutOfWindow;
        }
        const std::int64_t idx = delta / tick_;
        if (idx >= static_cast<std::int64_t>(ob::kLadderSize)) {
            return kOutOfWindow;
        }
        return static_cast<ob::Ticks>(idx + ob::kMinTick);
    }

    // Only valid for a Ticks in [kMinTick, kMaxTick] from this same grid.
    [[nodiscard]] Price4 price_at(ob::Ticks t) const noexcept {
        return static_cast<Price4>(base_ +
                                   static_cast<std::int64_t>(t - ob::kMinTick) * tick_);
    }

private:
    std::int64_t  base_        = 0;
    std::int32_t  tick_        = kCentTick;
    bool          initialized_ = false;
};

}  // namespace ob::itch
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build && ./build/tests/ob_tests '--gtest_filter=PriceGrid.*'
```

Expected: PASS, 11 tests.

Every number in these tests was checked against an executable model of the grid before
the plan was written, which is how three of them were caught using a $55.36 stock whose
base clamps to zero: the centring, stub-quote and boundary assertions were all wrong for
that grid and right for a $500 one.

- [ ] **Step 5: Commit**

```bash
git add include/ob/itch/price_grid.hpp tests/test_price_grid.cpp tests/CMakeLists.txt
git commit -m "feat(itch): per-symbol price grid over the existing ladder"
```

---

## Task 3: The order table

**Files:** Create `include/ob/itch/order_table.hpp`, `tests/test_order_table.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ob::IdIndex`, `ob::Slot`, `ob::kInvalidSlot`, `ob::Ticks`, `ob::Qty`, `ob::itch::OrderRef`.
- Produces: `ob::itch::LiveOrder` (POD: `side`, `tick`, `remaining`, `raw_price`), `ob::itch::OrderTable` with `explicit OrderTable(std::size_t capacity)`, `[[nodiscard]] bool add(OrderRef, LiveOrder)`, `[[nodiscard]] LiveOrder* find(OrderRef)`, `[[nodiscard]] const LiveOrder* find(OrderRef) const`, `bool remove(OrderRef)`, `[[nodiscard]] std::size_t size() const`, `[[nodiscard]] bool full() const`, `void reset()`.

**This is where `ob::IdIndex` from Phase 1 earns its keep.** ITCH order references are
`uint64`, exactly what `IdIndex` is keyed on, so the blocked-hash open-addressed table
with backward-shift deletion is reused unchanged.

Two measured facts drive the design:

- **References are sparse.** They ran from 42 to 65,729,048 in the first 53.8 M messages
  and will reach the hundreds of millions by the close. **An array indexed by reference
  is not an option**; it would need gigabytes to hold at most a couple of million live
  orders.
- **References are never reused.** Zero reuse in 22.9 M adds. So `add` on an existing
  reference means a bug, not a legitimate overwrite, and must be rejected loudly.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_order_table.cpp
#include <ob/itch/order_table.hpp>

#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

namespace {

using namespace ob::itch;

LiveOrder mk(ob::Ticks t, ob::Qty q) {
    LiveOrder o{};
    o.side      = ob::Side::Buy;
    o.tick      = t;
    o.remaining = q;
    o.raw_price = static_cast<Price4>(t) * 100u;
    return o;
}

TEST(OrderTable, AddFindRemoveRoundTrip) {
    OrderTable t(1024);
    EXPECT_EQ(t.size(), 0u);
    EXPECT_TRUE(t.add(42, mk(100, 500)));
    EXPECT_EQ(t.size(), 1u);
    const LiveOrder* o = t.find(42);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->tick, 100);
    EXPECT_EQ(o->remaining, 500u);
    EXPECT_TRUE(t.remove(42));
    EXPECT_EQ(t.find(42), nullptr);
    EXPECT_EQ(t.size(), 0u);
}

TEST(OrderTable, MissingReferenceIsNullNotAnEmptyRecord) {
    OrderTable t(1024);
    EXPECT_EQ(t.find(999), nullptr) << "a default record would be silently applied";
    EXPECT_FALSE(t.remove(999));
}

// Measured: zero reference reuse in 22.9M real adds. A duplicate therefore means a bug
// somewhere upstream, and overwriting would leak the first order's quantity from the
// ladder forever.
TEST(OrderTable, DuplicateReferenceIsRejectedRatherThanOverwriting) {
    OrderTable t(1024);
    EXPECT_TRUE(t.add(7, mk(100, 500)));
    EXPECT_FALSE(t.add(7, mk(200, 900)));
    const LiveOrder* o = t.find(7);
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->tick, 100) << "the original must survive";
    EXPECT_EQ(t.size(), 1u);
}

TEST(OrderTable, MutationThroughFindIsVisible) {
    OrderTable t(1024);
    EXPECT_TRUE(t.add(5, mk(100, 500)));
    t.find(5)->remaining -= 200;
    EXPECT_EQ(t.find(5)->remaining, 300u);
}

TEST(OrderTable, RefusesToExceedCapacityRatherThanCorrupting) {
    OrderTable t(64);
    std::size_t added = 0;
    for (OrderRef r = 1; r <= 1000; ++r) {
        if (t.add(r, mk(100, 1))) ++added;
    }
    EXPECT_GT(added, 0u);
    EXPECT_LT(added, 1000u) << "a bounded table must refuse, not grow or wrap";
    EXPECT_EQ(t.size(), added);
    // Everything it claimed to accept must still be findable.
    std::size_t found = 0;
    for (OrderRef r = 1; r <= 1000; ++r) found += (t.find(r) != nullptr) ? 1 : 0;
    EXPECT_EQ(found, added);
}

// Slots must be recycled, or a long replay exhausts capacity even though few orders
// are live at once. Measured peak live for the worst of ten liquid symbols: 34,248.
TEST(OrderTable, RecyclesSlotsSoALongChurnDoesNotExhaustIt) {
    OrderTable t(1024);
    for (OrderRef r = 1; r <= 100'000; ++r) {
        ASSERT_TRUE(t.add(r, mk(100, 1))) << "exhausted at " << r;
        ASSERT_TRUE(t.remove(r));
    }
    EXPECT_EQ(t.size(), 0u);
}

// The real access pattern: sparse, non-monotonic references with heavy churn, checked
// against a std::unordered_map reference model.
TEST(OrderTable, MatchesAReferenceModelUnderSparseNonMonotonicChurn) {
    OrderTable t(200'000);
    std::unordered_map<OrderRef, ob::Qty> model;
    std::mt19937_64 rng(99);
    std::vector<OrderRef> live;

    for (int i = 0; i < 400'000; ++i) {
        const bool do_add = live.empty() || (rng() % 100) < 55;
        if (do_add && model.size() < 150'000) {
            // Sparse and deliberately non-monotonic, like the real feed.
            const OrderRef r = rng() % 60'000'000ull;
            if (model.count(r)) continue;
            const ob::Qty q = static_cast<ob::Qty>(1 + rng() % 1000);
            ASSERT_TRUE(t.add(r, mk(100, q)));
            model[r] = q;
            live.push_back(r);
        } else {
            const std::size_t k = rng() % live.size();
            const OrderRef r = live[k];
            live[k] = live.back();
            live.pop_back();
            ASSERT_TRUE(t.remove(r));
            model.erase(r);
        }
        ASSERT_EQ(t.size(), model.size()) << "at step " << i;
    }
    for (const auto& [r, q] : model) {
        const LiveOrder* o = t.find(r);
        ASSERT_NE(o, nullptr) << "ref " << r;
        ASSERT_EQ(o->remaining, q);
    }
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails.**

- [ ] **Step 3: Write `include/ob/itch/order_table.hpp`**

```cpp
// include/ob/itch/order_table.hpp
#pragma once

// ITCH order reference -> live order record.
//
// Reuses ob::IdIndex from Phase 1 unchanged: it is an open-addressed table keyed on a
// uint64 with blocked hashing and backward-shift deletion, and an ITCH order reference
// is exactly a uint64.
//
// Why a hash and not an array indexed by reference: measured over the first 53.8M real
// messages, references ran from 42 to 65,729,048 and will reach the hundreds of
// millions by the close, while at most a couple of million orders are live at any
// instant. An array would need gigabytes to hold thousands of records.
//
// Why a duplicate add is an error: measured zero reference reuse across 22.9M adds. A
// duplicate means an upstream bug, and silently overwriting would strand the first
// order's quantity on the ladder with nothing left pointing at it.

#include <ob/id_index.hpp>
#include <ob/itch/price_grid.hpp>
#include <ob/itch/types.hpp>
#include <ob/types.hpp>

#include <cstddef>
#include <vector>

namespace ob::itch {

// What reconstruction needs to undo an order later. `raw_price` is kept alongside
// `tick` because an out-of-window order has no valid tick and lives in the overflow
// map, where the raw price is the only key there is.
struct LiveOrder {
    ob::Side  side      = ob::Side::Buy;
    ob::Ticks tick      = PriceGrid::kOutOfWindow;  // kOutOfWindow => in overflow
    ob::Qty   remaining = 0;
    Price4    raw_price = 0;
};

class OrderTable {
public:
    explicit OrderTable(std::size_t capacity)
        : index_(capacity), records_(capacity), free_(capacity), capacity_(capacity) {
        reset();
    }

    [[nodiscard]] bool add(OrderRef ref, LiveOrder o) noexcept {
        if (free_top_ == 0 || index_.full()) {
            return false;
        }
        if (index_.find(ref) != ob::kInvalidSlot) {
            return false;  // never legitimate; see the header comment
        }
        const ob::Slot s = free_[--free_top_];
        records_[s]      = o;
        if (!index_.insert(ref, s)) {
            free_[free_top_++] = s;  // put it back; leaking a slot here is a slow death
            return false;
        }
        return true;
    }

    [[nodiscard]] LiveOrder* find(OrderRef ref) noexcept {
        const ob::Slot s = index_.find(ref);
        return s == ob::kInvalidSlot ? nullptr : &records_[s];
    }
    [[nodiscard]] const LiveOrder* find(OrderRef ref) const noexcept {
        const ob::Slot s = index_.find(ref);
        return s == ob::kInvalidSlot ? nullptr : &records_[s];
    }

    bool remove(OrderRef ref) noexcept {
        const ob::Slot s = index_.find(ref);
        if (s == ob::kInvalidSlot) {
            return false;
        }
        index_.erase(ref);
        free_[free_top_++] = s;  // recycle, or a day-long replay exhausts capacity
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool full() const noexcept { return free_top_ == 0 || index_.full(); }

    void reset() noexcept {
        index_.reset();
        free_top_ = 0;
        // Descending, so the first allocations come off the front of the array and
        // early orders stay cache-adjacent.
        for (std::size_t i = capacity_; i-- > 0;) {
            free_[free_top_++] = static_cast<ob::Slot>(i);
        }
    }

private:
    ob::IdIndex            index_;
    std::vector<LiveOrder> records_;
    std::vector<ob::Slot>  free_;
    std::size_t            free_top_ = 0;
    std::size_t            capacity_;
};

}  // namespace ob::itch
```

- [ ] **Step 4: Run the tests.** Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add include/ob/itch/order_table.hpp tests/test_order_table.cpp tests/CMakeLists.txt
git commit -m "feat(itch): order reference table reusing the Phase 1 id index"
```

---

## Task 4: The symbol book

**Files:** Create `include/ob/itch/symbol_book.hpp`, `tests/test_symbol_book.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: everything from Tasks 2 and 3, plus `ob::PriceLadder`.
- Produces: `ob::itch::SymbolBook` with `explicit SymbolBook(std::size_t order_capacity)`, `void on_add(const AddOrder&)`, `void on_add_mpid(const AddOrderMpid&)`, `void on_executed(const OrderExecuted&)`, `void on_executed_price(const OrderExecutedPrice&)`, `void on_cancel(const OrderCancel&)`, `void on_delete(const OrderDelete&)`, `void on_replace(const OrderReplace&)`, `[[nodiscard]] Price4 best_bid() const`, `[[nodiscard]] Price4 best_ask() const`, `[[nodiscard]] QtySum qty_at(Side, Price4) const`, `[[nodiscard]] const BookCounters& counters() const`, and `kNoPrice4 = 0`.

**This is the task where the "does not match orders" rule is enforced.** Every handler
applies exactly what its message says and nothing more.

### The four hazards, each with a test

1. **Underflow.** `remaining` and level totals are unsigned. Subtracting more than
   rests wraps to a colossal number that then corrupts every depth query downstream,
   and `-fsanitize=unsigned-integer-overflow` is not on by default. Clamp and count.
2. **Unknown reference.** Normal on any start that is not the file's first byte. Count
   it, ignore the message, never fabricate an order to attach it to.
3. **Implicit delete.** An execution that empties an order removes it with no `D` to
   follow. Measured: **74% of executions** (689,810 of 937,083) do exactly this, so it
   is the common path, not an edge case.
4. **Out-of-window prices.** Measured at 6.9% of AMZN's adds. They go to the overflow
   map, and `best_*` must consult it when the ladder side is empty, or a book whose
   only resting bid is a $0.01 stub reports no bid at all.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_symbol_book.cpp
#include <ob/itch/symbol_book.hpp>

#include <gtest/gtest.h>

namespace {

using namespace ob::itch;

constexpr Price4 kRef = 5'000'000;  // $500, so both stubs fall outside the window

AddOrder add(OrderRef r, char side, ob::Qty q, Price4 px) {
    AddOrder a{};
    a.h.type    = MsgType::AddOrder;
    a.order_ref = r;
    a.buy_sell  = side;
    a.shares    = q;
    a.price     = px;
    return a;
}

TEST(SymbolBook, FirstAddEstablishesTheGridAndBecomesBothSides) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    EXPECT_EQ(b.best_bid(), kRef);
    EXPECT_EQ(b.best_ask(), SymbolBook::kNoPrice4);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 100u);
}

TEST(SymbolBook, PriceTimePriorityAggregatesAtALevel) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    b.on_add(add(2, 'B', 250, kRef));
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 350u);
}

TEST(SymbolBook, BestIsHighestBidAndLowestAsk) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    b.on_add(add(2, 'B', 100, kRef - 100));
    b.on_add(add(3, 'S', 100, kRef + 200));
    b.on_add(add(4, 'S', 100, kRef + 100));
    EXPECT_EQ(b.best_bid(), kRef);
    EXPECT_EQ(b.best_ask(), kRef + 100);
}

// X is PARTIAL. It reduces the resting quantity; only D removes the order.
TEST(SymbolBook, CancelReducesButDoesNotRemove) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 500, kRef));
    OrderCancel c{};
    c.order_ref = 1;
    c.cancelled_shares = 200;
    b.on_cancel(c);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 300u);
    EXPECT_EQ(b.best_bid(), kRef) << "the order is still resting";
}

TEST(SymbolBook, DeleteRemovesTheWholeOrder) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 500, kRef));
    OrderDelete d{};
    d.order_ref = 1;
    b.on_delete(d);
    EXPECT_EQ(b.best_bid(), SymbolBook::kNoPrice4);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 0u);
}

// 74% of real executions do this. There is no D to follow.
TEST(SymbolBook, ExecutionThatEmptiesAnOrderImplicitlyDeletesIt) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 500, kRef));
    OrderExecuted e{};
    e.order_ref = 1;
    e.executed_shares = 500;
    b.on_executed(e);
    EXPECT_EQ(b.best_bid(), SymbolBook::kNoPrice4);
    EXPECT_EQ(b.counters().implicit_deletes, 1u);
    // And the reference must be gone, so a later message for it is "unknown".
    b.on_executed(e);
    EXPECT_EQ(b.counters().unknown_ref, 1u);
}

TEST(SymbolBook, PartialExecutionLeavesTheRemainder) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 500, kRef));
    OrderExecuted e{};
    e.order_ref = 1;
    e.executed_shares = 200;
    b.on_executed(e);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 300u);
    EXPECT_EQ(b.counters().implicit_deletes, 0u);
}

// U is delete-plus-add with a NEW reference. The old one must not remain usable.
TEST(SymbolBook, ReplaceMovesTheOrderAndRetiresTheOldReference) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 500, kRef));
    OrderReplace u{};
    u.original_order_ref = 1;
    u.new_order_ref      = 2;
    u.shares             = 300;
    u.price              = kRef - 100;
    b.on_replace(u);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 0u);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef - 100), 300u);
    EXPECT_EQ(b.best_bid(), kRef - 100);

    OrderDelete d{};
    d.order_ref = 1;
    b.on_delete(d);
    EXPECT_EQ(b.counters().unknown_ref, 1u) << "the old reference must be retired";
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef - 100), 300u) << "and must not disturb the new one";
}

// Unsigned subtraction past zero is the defect that corrupts everything downstream.
TEST(SymbolBook, OverCancelAndOverExecuteClampInsteadOfUnderflowing) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    OrderCancel c{};
    c.order_ref = 1;
    c.cancelled_shares = 999'999;
    b.on_cancel(c);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 0u) << "must be 0, not 4 billion";
    EXPECT_EQ(b.counters().over_reduce, 1u);

    SymbolBook b2(1024);
    b2.on_add(add(1, 'B', 100, kRef));
    OrderExecuted e{};
    e.order_ref = 1;
    e.executed_shares = 999'999;
    b2.on_executed(e);
    EXPECT_EQ(b2.qty_at(ob::Side::Buy, kRef), 0u);
    EXPECT_EQ(b2.counters().over_reduce, 1u);
}

TEST(SymbolBook, MessagesForUnknownReferencesAreCountedAndIgnored) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    OrderDelete d{};
    d.order_ref = 4242;
    b.on_delete(d);
    OrderExecuted e{};
    e.order_ref = 4242;
    e.executed_shares = 5;
    b.on_executed(e);
    EXPECT_EQ(b.counters().unknown_ref, 2u);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef), 100u) << "the real book must be untouched";
}

// Stub quotes. Measured at 6.9% of AMZN's adds, so this path is ordinary traffic.
TEST(SymbolBook, OutOfWindowOrdersGoToOverflowAndAreStillInTheBook) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));           // establishes the $500 grid
    b.on_add(add(2, 'B', 50, 100));             // $0.01 stub bid, outside the window
    b.on_add(add(3, 'S', 50, 2'000'000'000u));  // $200,000 stub ask, outside
    EXPECT_EQ(b.counters().out_of_window, 2u);
    EXPECT_EQ(b.best_bid(), kRef) << "the real bid still wins";
    EXPECT_EQ(b.best_ask(), 2'000'000'000u) << "the stub is the only ask, so it IS best";
    EXPECT_EQ(b.qty_at(ob::Side::Buy, 100), 50u) << "overflow orders are real orders";
}

TEST(SymbolBook, OverflowOrderBecomesBestWhenTheLadderSideEmpties) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    b.on_add(add(2, 'B', 50, 100));  // $0.01 stub
    OrderDelete d{};
    d.order_ref = 1;
    b.on_delete(d);
    EXPECT_EQ(b.best_bid(), 100u)
        << "with the ladder empty the stub really is the best bid; reporting none is wrong";
}

TEST(SymbolBook, OverflowOrdersCanBeCancelledExecutedAndDeleted) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef));
    b.on_add(add(2, 'B', 500, 100));
    OrderCancel c{};
    c.order_ref = 2;
    c.cancelled_shares = 200;
    b.on_cancel(c);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, 100), 300u);
    OrderDelete d{};
    d.order_ref = 2;
    b.on_delete(d);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, 100), 0u);
    EXPECT_EQ(b.counters().unknown_ref, 0u);
}

// Reconstruction NEVER matches. A crossed book is a thing that happens in real data
// and must be reported as-is, not "fixed".
TEST(SymbolBook, ACrossedBookIsReportedNotMatched) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 100, kRef + 100));
    b.on_add(add(2, 'S', 100, kRef));
    EXPECT_EQ(b.best_bid(), kRef + 100);
    EXPECT_EQ(b.best_ask(), kRef);
    EXPECT_EQ(b.qty_at(ob::Side::Buy, kRef + 100), 100u)
        << "matching them would delete liquidity the exchange never removed";
    EXPECT_EQ(b.qty_at(ob::Side::Sell, kRef), 100u);
}

TEST(SymbolBook, ZeroShareAddIsRejected) {
    SymbolBook b(1024);
    b.on_add(add(1, 'B', 0, kRef));
    EXPECT_EQ(b.best_bid(), SymbolBook::kNoPrice4);
    EXPECT_EQ(b.counters().rejected, 1u);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails.**

- [ ] **Step 3: Write `include/ob/itch/symbol_book.hpp`**

Key structure, with the handlers that carry the hazards:

```cpp
// include/ob/itch/symbol_book.hpp
#pragma once

// One symbol's reconstructed book.
//
// THIS DOES NOT MATCH ORDERS. The exchange already decided; the feed reports what
// happened. Only E, C, X, D and U remove liquidity, and only by the amount stated.
// A crossed book is reported as a crossed book. `P` (non-cross trade) is not handled
// here at all: it reports a trade against non-displayed liquidity and applying it
// would double-count.
//
// Structure: a flat ladder per side for the window around the market, plus a per-side
// overflow std::map for prices outside it. See spec 6.3 -- overflow is ordinary
// traffic (6.9% of AMZN's adds), not a safety net.

#include <ob/itch/order_table.hpp>
#include <ob/itch/price_grid.hpp>
#include <ob/itch/types.hpp>
#include <ob/price_ladder.hpp>
#include <ob/types.hpp>

#include <map>

namespace ob::itch {

struct BookCounters {
    std::uint64_t applied          = 0;
    std::uint64_t unknown_ref      = 0;  // normal when not starting at byte 0
    std::uint64_t implicit_deletes = 0;  // executions that emptied an order (~74%)
    std::uint64_t out_of_window    = 0;  // went to overflow
    std::uint64_t over_reduce      = 0;  // clamped instead of underflowing
    std::uint64_t rejected         = 0;  // zero shares, table full
    std::uint64_t table_full       = 0;
};

class SymbolBook {
public:
    static constexpr Price4 kNoPrice4 = 0;

    explicit SymbolBook(std::size_t order_capacity) : orders_(order_capacity) {}

    void on_add(const AddOrder& a) { add_impl(a.order_ref, a.buy_sell, a.shares, a.price); }
    void on_add_mpid(const AddOrderMpid& a) {
        add_impl(a.order_ref, a.buy_sell, a.shares, a.price);
    }

    void on_executed(const OrderExecuted& e) { reduce(e.order_ref, e.executed_shares, true); }
    void on_executed_price(const OrderExecutedPrice& e) {
        // The execution PRICE on a C message is the price the trade printed at, which
        // can differ from the resting price. The BOOK is adjusted at the resting
        // price; the print price is a signal input, not a book location.
        reduce(e.order_ref, e.executed_shares, true);
    }
    void on_cancel(const OrderCancel& c) { reduce(c.order_ref, c.cancelled_shares, false); }

    void on_delete(const OrderDelete& d) {
        LiveOrder* o = orders_.find(d.order_ref);
        if (o == nullptr) { ++counters_.unknown_ref; return; }
        remove_from_structure(*o, o->remaining);
        (void)orders_.remove(d.order_ref);
        ++counters_.applied;
    }

    // U is delete-plus-add under a NEW reference, and it loses time priority.
    void on_replace(const OrderReplace& u) {
        LiveOrder* o = orders_.find(u.original_order_ref);
        if (o == nullptr) { ++counters_.unknown_ref; return; }
        const ob::Side side = o->side;
        remove_from_structure(*o, o->remaining);
        (void)orders_.remove(u.original_order_ref);
        add_impl(u.new_order_ref, side == ob::Side::Buy ? 'B' : 'S', u.shares, u.price);
    }

    [[nodiscard]] Price4 best_bid() const { return best_side(ob::Side::Buy); }
    [[nodiscard]] Price4 best_ask() const { return best_side(ob::Side::Sell); }
    [[nodiscard]] ob::QtySum qty_at(ob::Side s, Price4 px) const;
    [[nodiscard]] const BookCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const PriceGrid& grid() const noexcept { return grid_; }

private:
    void add_impl(OrderRef ref, char side_ch, ob::Qty shares, Price4 px);
    void reduce(OrderRef ref, ob::Qty by, bool is_execution);
    void remove_from_structure(const LiveOrder& o, ob::Qty qty);
    [[nodiscard]] Price4 best_side(ob::Side s) const;

    PriceGrid                  grid_;
    ob::PriceLadder<ob::Side::Buy>  bids_;
    ob::PriceLadder<ob::Side::Sell> asks_;
    ob::OrderPool              pool_;
    OrderTable                 orders_;
    // Overflow: raw price -> aggregate quantity. Ordered, so best_side() can take
    // begin()/rbegin() without a scan.
    std::map<Price4, ob::QtySum> ovf_bids_, ovf_asks_;
    BookCounters               counters_;
};

}  // namespace ob::itch
```

- [ ] **Step 4: Implement `add_impl`** — the grid-establishing and overflow-routing path.

```cpp
void SymbolBook::add_impl(OrderRef ref, char side_ch, ob::Qty shares, Price4 px) {
    if (shares == 0 || (side_ch != 'B' && side_ch != 'S')) {
        ++counters_.rejected;
        return;
    }
    if (!grid_.initialized()) {
        grid_.init_from(px);
    }
    const ob::Side  side = (side_ch == 'B') ? ob::Side::Buy : ob::Side::Sell;
    const ob::Ticks t    = grid_.index_of(px);

    LiveOrder o{};
    o.side      = side;
    o.tick      = t;
    o.remaining = shares;
    o.raw_price = px;
    if (!orders_.add(ref, o)) {
        ++counters_.rejected;
        ++counters_.table_full;
        return;
    }
    if (t == PriceGrid::kOutOfWindow) {
        ++counters_.out_of_window;
        (side == ob::Side::Buy ? ovf_bids_ : ovf_asks_)[px] += shares;
    } else {
        // The ladder's push_back wants a pool slot; reconstruction does not need
        // per-order FIFO position, only aggregates, so the level total is enough.
        (side == ob::Side::Buy ? bids_ : asks_).add_qty(t, shares);
    }
    ++counters_.applied;
}
```

**Note for the implementer:** `PriceLadder` as written in Phase 1 threads a per-order
FIFO through `OrderPool`, because a matching engine must know *which* order is next.
Reconstruction does not: it only needs level aggregates, and it already has the order
records in `OrderTable`. Add a small `add_qty(Ticks, Qty)` / `sub_qty(Ticks, Qty)` pair
to `PriceLadder` that maintains `total`, `count` and the occupancy bitmap without
touching the FIFO links. Keep the existing `push_back`/`unlink` untouched so the Phase
1–4 engine and its 222 tests are unaffected.

- [ ] **Step 5: Implement `reduce` and `remove_from_structure`** — where the clamping lives.

```cpp
void SymbolBook::reduce(OrderRef ref, ob::Qty by, bool is_execution) {
    LiveOrder* o = orders_.find(ref);
    if (o == nullptr) {
        ++counters_.unknown_ref;
        return;
    }
    // Clamp. `remaining` and the level totals are unsigned, so subtracting more than
    // rests wraps to a colossal number that corrupts every later depth query, and
    // UBSan does not flag unsigned wrap by default.
    ob::Qty n = by;
    if (n > o->remaining) {
        n = o->remaining;
        ++counters_.over_reduce;
    }
    remove_from_structure(*o, n);
    o->remaining -= n;
    ++counters_.applied;

    if (o->remaining == 0) {
        // An execution that empties an order deletes it, with no D to follow. Measured
        // at 74% of executions, so this is the common path.
        if (is_execution) {
            ++counters_.implicit_deletes;
        }
        (void)orders_.remove(ref);
    }
}

void SymbolBook::remove_from_structure(const LiveOrder& o, ob::Qty qty) {
    if (qty == 0) {
        return;
    }
    if (o.tick == PriceGrid::kOutOfWindow) {
        auto& m  = (o.side == ob::Side::Buy) ? ovf_bids_ : ovf_asks_;
        auto  it = m.find(o.raw_price);
        if (it != m.end()) {
            it->second -= qty;
            if (it->second == 0) {
                m.erase(it);  // or best_side() reports an empty level as the best
            }
        }
        return;
    }
    (o.side == ob::Side::Buy ? bids_ : asks_).sub_qty(o.tick, qty);
}
```

- [ ] **Step 6: Implement `best_side` and `qty_at`** — where the overflow map must be consulted.

```cpp
Price4 SymbolBook::best_side(ob::Side s) const {
    const bool buy = (s == ob::Side::Buy);
    const auto& ladder = buy ? bids_ : asks_;
    if (!ladder.empty()) {
        return grid_.price_at(ladder.best());
    }
    // The ladder side is empty, so an overflow order -- typically a stub quote -- is
    // genuinely the best price. Reporting "none" here would hide a real resting order.
    const auto& m = buy ? ovf_bids_ : ovf_asks_;
    if (m.empty()) {
        return kNoPrice4;
    }
    return buy ? m.rbegin()->first : m.begin()->first;
}

ob::QtySum SymbolBook::qty_at(ob::Side s, Price4 px) const {
    const ob::Ticks t = grid_.index_of(px);
    if (t != PriceGrid::kOutOfWindow) {
        return (s == ob::Side::Buy ? bids_ : asks_).level(t).total;
    }
    const auto& m  = (s == ob::Side::Buy) ? ovf_bids_ : ovf_asks_;
    const auto  it = m.find(px);
    return it == m.end() ? 0 : it->second;
}
```

- [ ] **Step 7: Run the tests.** Expected: PASS, 15 tests.

- [ ] **Step 8: Run under ASan and UBSan.**

```bash
cmake --build build-asan && ./build-asan/tests/ob_tests '--gtest_filter=SymbolBook.*'
```

- [ ] **Step 9: Commit**

```bash
git add include/ob/itch/symbol_book.hpp include/ob/price_ladder.hpp \
        tests/test_symbol_book.cpp tests/CMakeLists.txt
git commit -m "feat(itch): per-symbol book reconstruction with overflow levels"
```

---

## Task 5: The replay driver and the feed-consistency oracle

**Files:** Create `include/ob/itch/replay.hpp`, `tests/test_replay_oracle.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: Tasks 1–4 and the Phase 5 decoder.
- Produces: `ob::itch::ReplayStats`, `ob::itch::OracleReport`, `ob::itch::Replay` with `Replay(SymbolRouter, std::size_t order_capacity)`, `void feed(std::span<const std::byte>)`, `[[nodiscard]] const SymbolBook& book(BookSlot) const`, `[[nodiscard]] const ReplayStats& stats() const`, `[[nodiscard]] const OracleReport& oracle() const`, `void set_oracle_enabled(bool)`.

### Why this oracle is the whole point of using real data

A synthetic test can only check the reconstruction against the same assumptions that
built it. Real ITCH carries the exchange's own execution reports, and an execution tells
us something the reconstruction never used as input: **the price it happened at.**

In a price-time-priority book, a resting order can only execute when it is at the best
price on its side. So for every `E` and `C`:

> `resting order's price == best price on that order's side, immediately before the fill`

If reconstruction has drifted — a missed cancel, a level left occupied, an order on the
wrong side — this fires. It is an end-to-end check on every component at once, and it
costs nothing to run because the data is already there.

**It has been measured, so the threshold is known rather than guessed.**

| Scope | Executions checked | At the best price |
|---|---|---|
| Ten liquid symbols (`AAPL MSFT SPY AMZN INTC QQQ TSLA F BAC AMD`) | 78,683 | **78,683 — 100.00000%** |
| All 8,906 symbols | 937,083 | 937,074 — 99.9990% (**9 exceptions**) |

**So: assert exactly zero mismatches for the ten tracked symbols, and count-and-report
market-wide.** Do not write `EXPECT_EQ(mismatches, 0)` for a market-wide run — it would
fail on correct code. Do not write a loose tolerance for the ten — it would hide real
bugs. The 9 market-wide exceptions were all executions at a price *worse* than the
book's best, and explaining them is a stretch goal, not a blocker.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_replay_oracle.cpp
#include <ob/itch/replay.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <vector>

namespace {

using namespace ob::itch;

Symbol sym(const char* s) {
    char pad[8];
    std::memset(pad, ' ', 8);
    std::memcpy(pad, s, std::strlen(s));
    return Symbol::from_bytes(reinterpret_cast<const std::byte*>(pad));
}

std::vector<char> slice() {
    std::ifstream f("testdata/itch_slice_10mb.bin", std::ios::binary);
    EXPECT_TRUE(f) << "run scripts/fetch_itch.sh first";
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

Replay make_replay() {
    SymbolRouter r;
    for (const char* s : {"AAPL", "MSFT", "SPY", "AMZN", "INTC",
                          "QQQ", "TSLA", "F", "BAC", "AMD"}) {
        EXPECT_TRUE(r.watch(sym(s)));
    }
    return Replay(std::move(r), 1u << 20);
}

TEST(ReplayOracle, ReplaysTheRealSliceAndResolvesEverySymbol) {
    const auto raw = slice();
    ASSERT_GT(raw.size(), 1'000'000u);
    Replay rp = make_replay();
    rp.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    EXPECT_EQ(rp.stats().decode_errors, 0u);
    EXPECT_EQ(rp.stats().messages, 354'869u)
        << "the committed slice decoded to this many messages when measured";
    EXPECT_TRUE(rp.router().all_resolved());
}

// The headline assertion. Zero tolerance, because zero is what was measured.
TEST(ReplayOracle, EveryExecutionHappensAtTheBestPriceOnItsSide) {
    const auto raw = slice();
    Replay rp = make_replay();
    rp.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    const OracleReport& o = rp.oracle();
    EXPECT_EQ(o.mismatches, 0u)
        << "first at ts=" << o.first_mismatch_ts
        << " exec_px=" << o.first_mismatch_exec_px
        << " best=" << o.first_mismatch_best;
}

// A book must never report a total that no order backs. This catches unsigned
// underflow, the defect that clamping in Task 4 exists to prevent.
TEST(ReplayOracle, EveryLevelTotalIsBackedByLiveOrders) {
    const auto raw = slice();
    Replay rp = make_replay();
    rp.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    for (BookSlot s = 0; s < rp.book_count(); ++s) {
        EXPECT_TRUE(rp.book(s).audit_totals_match_orders())
            << "symbol " << rp.router().symbol_at(s).str();
    }
}

TEST(ReplayOracle, NoUnderflowEverOccurred) {
    const auto raw = slice();
    Replay rp = make_replay();
    rp.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    for (BookSlot s = 0; s < rp.book_count(); ++s) {
        EXPECT_EQ(rp.book(s).counters().over_reduce, 0u)
            << rp.router().symbol_at(s).str()
            << ": the real feed never reports reducing more than rests";
    }
}

// Starting at byte 0, every reference is known. This is what makes the counter a
// useful signal rather than noise: a nonzero value means a real problem here.
TEST(ReplayOracle, StartingAtByteZeroThereAreNoUnknownReferences) {
    const auto raw = slice();
    Replay rp = make_replay();
    rp.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});
    for (BookSlot s = 0; s < rp.book_count(); ++s) {
        EXPECT_EQ(rp.book(s).counters().unknown_ref, 0u)
            << rp.router().symbol_at(s).str();
    }
}

// ...and starting mid-stream, unknown references are EXPECTED. The replay must survive
// it rather than treating it as corruption.
TEST(ReplayOracle, StartingMidStreamSurvivesWithCountedUnknownReferences) {
    const auto raw = slice();
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                                  raw.size());
    // Walk to a message boundary well past the directory block.
    std::size_t off = 0;
    for (int i = 0; i < 300'000 && off < in.size(); ++i) {
        ItchMessage m{};
        const auto r = decode(in.subspan(off), m);
        if (r.consumed == 0) break;
        off += r.consumed;
    }
    Replay rp = make_replay();
    rp.feed(in.subspan(off));
    EXPECT_EQ(rp.stats().decode_errors, 0u) << "framing must still be intact";
    // No directory messages were seen, so nothing resolves and nothing is tracked.
    EXPECT_EQ(rp.router().tracked_count(), 0u);
    EXPECT_FALSE(rp.router().all_resolved())
        << "a run that misses the directory block must be able to say so";
}

// Feeding in arbitrary chunks must give byte-identical results to feeding in one go.
// Task 7 streams the file, so a decoder that mishandles a split message would corrupt
// only the threaded run and pass every single-buffer test.
TEST(ReplayOracle, ChunkedFeedMatchesASingleFeed) {
    const auto raw = slice();
    Replay whole = make_replay();
    whole.feed({reinterpret_cast<const std::byte*>(raw.data()), raw.size()});

    Replay chunked = make_replay();
    const std::byte* p = reinterpret_cast<const std::byte*>(raw.data());
    for (std::size_t off = 0; off < raw.size();) {
        const std::size_t n = std::min<std::size_t>(1237, raw.size() - off);  // a prime
        chunked.feed({p + off, n});
        off += n;
    }
    EXPECT_EQ(chunked.stats().messages, whole.stats().messages);
    EXPECT_EQ(chunked.stats().decode_errors, 0u);
    for (BookSlot s = 0; s < whole.book_count(); ++s) {
        EXPECT_EQ(chunked.book(s).best_bid(), whole.book(s).best_bid())
            << rp_name(whole, s);
        EXPECT_EQ(chunked.book(s).best_ask(), whole.book(s).best_ask())
            << rp_name(whole, s);
    }
}

}  // namespace
```

Add a small `rp_name(const Replay&, BookSlot)` helper in the anonymous namespace that
returns `rp.router().symbol_at(s).str()`.

- [ ] **Step 2: Add to the build, run, verify it fails.**

- [ ] **Step 3: Write `include/ob/itch/replay.hpp`**

The oracle check, which is the part that must be exactly right:

```cpp
// Called for every E and C, BEFORE the book is mutated.
void Replay::check_oracle(const SymbolBook& b, const LiveOrder& o) {
    if (!oracle_enabled_) {
        return;
    }
    const Price4 best = (o.side == ob::Side::Buy) ? b.best_bid() : b.best_ask();
    if (best == SymbolBook::kNoPrice4) {
        return;  // nothing to compare against
    }
    ++oracle_.checked;
    if (o.raw_price == best) {
        return;
    }
    if (oracle_.mismatches == 0) {
        oracle_.first_mismatch_ts      = current_ts_;
        oracle_.first_mismatch_exec_px = o.raw_price;
        oracle_.first_mismatch_best    = best;
    }
    ++oracle_.mismatches;
}
```

**The ordering is the whole trick and is easy to get wrong.** The check must run
*before* the fill is applied. Afterwards the level may already be gone and the
comparison is against the book's next state, which silently always passes.

`Replay::feed` must also **buffer a partial trailing message across calls**, because
Task 7 streams the file in chunks and a message will straddle a boundary. Keep a small
`std::array<std::byte, 2 + kMaxFrameLength>` carry buffer; the `ChunkedFeedMatchesASingleFeed`
test is what proves it works.

- [ ] **Step 4: Run the tests.** Expected: PASS, 7 tests, with the oracle at **zero** mismatches.

- [ ] **Step 5: Run the oracle market-wide as a tool, not a test**

```bash
./build/tools/ob_replay --in data/12302019.NASDAQ_ITCH50 --all-symbols --oracle
```

Expected: about 99.999% agreement, a handful of exceptions. **This is a report, not a
gate**, for the reason given above. Record the number in `docs/RECONSTRUCTION.md`.

- [ ] **Step 6: Commit**

```bash
git add include/ob/itch/replay.hpp tests/test_replay_oracle.cpp tests/CMakeLists.txt
git commit -m "feat(itch): replay driver with the feed-consistency oracle"
```

---

## Task 6: The SPSC ring

**Files:** Create `include/ob/spsc_ring.hpp`, `tests/test_spsc_ring.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Produces: `ob::SpscRing<T, Capacity>` with `[[nodiscard]] bool try_push(const T&)`, `[[nodiscard]] bool try_pop(T&)`, `[[nodiscard]] std::size_t size_approx() const`, `[[nodiscard]] bool empty() const`, and a `kCapacity` static.

One producer (decode), one consumer (book). A power-of-two capacity so the modulo is a
mask. The two indices sit on **separate cache lines**, using the same
`kCacheLine` constant and the same GCC `-Winterference-size` suppression that
`seqlock.hpp` already carries — reuse it rather than writing a second copy.

**Memory ordering, which is the only hard part.** The producer publishes with
`release` on `head_` and the consumer reads it `acquire`; the consumer publishes with
`release` on `tail_` and the producer reads it `acquire`. A `relaxed` load of the index
the *same* thread owns is correct and cheaper. Getting this wrong produces a queue that
works on x86, where the hardware hides it, and corrupts data on arm64 — so **CI must
run this under TSan on both architectures**, and the local arm64 Mac is the better
test machine of the two.

- [ ] **Step 1: Write the failing test**, covering: empty pop fails; fill to capacity then push fails; FIFO order preserved; wraparound past capacity many times; `size_approx` bounded by capacity; and a **two-thread test pushing 5,000,000 sequence numbers and asserting the consumer sees them strictly in order with none lost**.

- [ ] **Step 2: Run, verify it fails.**

- [ ] **Step 3: Implement the ring.**

- [ ] **Step 4: Run the tests, then run the two-thread test under TSan.**

```bash
cmake --build build-tsan && ./build-tsan/tests/ob_tests '--gtest_filter=SpscRing.*'
```

Expected: PASS with no TSan report. Note the Phase 4 lesson recorded in
`include/ob/sanitizer.hpp`: a program cannot both replace the global allocator and run
under a sanitizer that owns it. That guard is already in place and must not be undone.

- [ ] **Step 5: Commit.**

---

## Task 7: Two-thread replay and the benchmark

**Files:** Create `tools/ob_replay.cpp`, `bench/bench_replay.cpp`. Modify `tools/CMakeLists.txt`, `bench/CMakeLists.txt`.

**Interfaces:** Produces the `ob_replay` and `bench_replay` executables.

Thread A reads the file, decodes, filters by `stock_locate`, and pushes accepted
messages into the ring. Thread B pops and applies them. A `--single-thread` flag runs
both inline, which is what the correctness tests use.

### Measure it the way Phases 1–4 established

The methodology is already settled and its defects already found. **Re-read
`docs/BENCHMARK-METHODOLOGY.md` before writing a line of this.** The four traps that
were paid for once and must not be paid for again:

1. **Do not include the harness's own clock reads in the per-operation cost.** That
   defect reported 66 ns for a 29 ns operation. Use a separate untimestamped loop for
   the throughput figure.
2. **Subtracting clock overhead can produce a negative percentile.** Clamp at zero.
3. **Report the median of the clock-resolution samples, not the minimum.** The minimum
   is outlier-sensitive and understates the error bars by about 2.5x.
4. **Use the serialized clock read.** Two bare `mrs` can retire out of order, which is
   what made the measured resolution wander between 1 ns and 42 ns.

What to report:

| Metric | How |
|---|---|
| Messages/sec, decode + filter only | Untimestamped loop over the full file |
| Messages/sec, decode + book | Same, with books enabled |
| Per-update latency p50/p99/p99.9 | Timestamped loop, nearest-rank, clock overhead subtracted and clamped |
| Ring occupancy | Sampled, to show whether the consumer is the bottleneck |
| Peak live orders per symbol | From the books |

**Sizing note from the measurements.** Peak live orders across all 8,906 symbols was
1,731,096 at 10:23 and still rising slowly; for the ten liquid symbols the worst was
AMZN at 34,248. Size the per-symbol `OrderTable` from the tracked set, not from the
market-wide figure, or the id index becomes tens of megabytes of mostly-empty table and
the Phase 2 result applies: **a 4.3x slowdown from index size alone**, because the
engine is memory-bound on that structure.

- [ ] **Step 1: Write `tools/ob_replay.cpp`** with `--in`, `--symbols`, `--single-thread`, `--oracle`, `--snapshot-every`, `--limit`.
- [ ] **Step 2: Verify single-thread and two-thread runs agree** on every book's best bid/ask and every counter. A disagreement is a ring bug, and this is the only test that finds it.
- [ ] **Step 3: Write `bench/bench_replay.cpp`** following the methodology above.
- [ ] **Step 4: Run it, record the numbers in `bench/results/`.**
- [ ] **Step 5: Run under TSan**, full two-thread replay over at least 10 M messages.
- [ ] **Step 6: Commit.**

---

## Task 8: Profile, CI and documentation

**Files:** Modify `.github/workflows/ci.yml`, `README.md`, `docs/`. Create `docs/RECONSTRUCTION.md`.

- [ ] **Step 1: Profile with `perf`** on the full-file replay; record the top ten symbols by cycles. The expectation from Phase 2 is that the order table dominates. **Write down whether it did.** A profile that only confirms what was assumed was not worth running.
- [ ] **Step 2: Add a CI job** that replays the committed 10 MB slice and asserts: zero decode errors, zero oracle mismatches, zero `over_reduce`, zero `unknown_ref`. It must not download the multi-gigabyte file.
- [ ] **Step 3: Extend the Cachegrind gate** to the replay path, arch-keyed as the existing one is.
- [ ] **Step 4: Write `docs/RECONSTRUCTION.md`** — the reconstruction-vs-matching distinction, the oracle and its measured rates, the window-plus-overflow structure and its measured cost per symbol.
- [ ] **Step 5: Update `README.md`** with the measured messages/sec and p99.
- [ ] **Step 6: Update the phase index. Commit.**

---

## Self-review

**Spec coverage.** Spec 6.1 (reconstruction, not matching) is the rule stated up front
and enforced by `ACrossedBookIsReportedNotMatched`. Spec 6.2 (order references are not
monotonic) is Task 3, whose design follows directly from the measurement. Spec 6.3 (the
per-symbol grid) is Tasks 2 and 4, including the corrected overflow behaviour. Spec 6.3b
(locate consistency) is what licenses per-book order tables in Tasks 1 and 3. Spec 6.4
(the feed's own trades as the oracle) is Task 5. The SPSC queue and the benchmarks are
Tasks 6 and 7. Phases 7 and 8 own the bindings, the backtest and the MCP server.

**Placeholder scan.** Tasks 1–5 carry complete code. Tasks 6–8 are step lists with
concrete acceptance criteria rather than full listings, which is deliberate: the ring is
a well-known structure whose *only* difficulty is the memory ordering, and that is
spelled out; the benchmark's difficulty is methodology, and that is spelled out with the
four specific traps already paid for. No step says "add error handling" or "write tests
for the above".

**Type consistency.** `BookSlot`, `kUntracked`, `LiveOrder`, `PriceGrid::kOutOfWindow`,
`SymbolBook::kNoPrice4`, `BookCounters` field names, and `OrderTable`'s methods are
spelled identically everywhere they appear across Tasks 1–7. `kOutOfWindow` is `0`,
which is distinct from every valid `Ticks` because `ob::kMinTick` is 1.

**One gap, stated rather than hidden.** Task 4 asks for `add_qty`/`sub_qty` on
`PriceLadder`, which touches a file the Phase 1–4 engine depends on. The instruction is
to add methods beside the existing ones rather than change `push_back`/`unlink`, so the
222 existing tests stay meaningful — but the executor should run the full suite after
that edit, not just the new tests.

---

## Verification of this plan

Two kinds of verification were done.

**The complete code was compiled and run.** Tasks 1, 2 and 3 are complete headers, so
they were extracted from this plan, built and executed against the real 10 MB slice:

| Check | Result |
|---|---|
| Apple Clang, `-std=c++20 -O2 -Wall -Wextra -Werror` | clean |
| GCC 13, `-std=c++20 -O2 -Wall -Wextra -Werror` | clean |
| Task 1-3 tests, both compilers | **1,211,422 assertions, 0 failures** |
| ASan + UBSan | clean |

That caught two defects, fixed above:

1. **Three `PriceGrid` tests were written against a $55.36 stock**, whose centred base
   clamps to zero. The centring, stub-quote and boundary assertions were all wrong for
   that grid and right for a $500 one. They now use $500, and the clamping behaviour —
   which is correct, not a degradation — has a test of its own.
2. **`SymbolRouter::watch()` is `[[nodiscard]]` and nine test call sites ignored it**,
   which is nine `-Wunused-result` warnings and, since CI builds with `-Werror`, a
   build failure rather than a warning. Every call site now asserts the result.

**The facts the design rests on were measured**, by replaying real Nasdaq data rather
than reading documentation:

| Fact the design depends on | Measured | Where it is used |
|---|---|---|
| `stock_locate` is stable across an order's lifetime | **1,421,624 checked, 0 mismatches** | Per-book order tables (Tasks 1, 3) |
| Executions happen at the best price | **78,683 / 78,683 on 10 symbols; 937,074 / 937,083 market-wide** | The oracle (Task 5) |
| Order references are sparse and non-monotonic | 42 to 65,729,048; 26.5% non-increasing | Hash, not array (Task 3) |
| Order references are never reused | **0 reuse in 22.9 M adds** | Duplicate add is an error (Task 3) |
| Executions that empty an order | **689,810 of 937,083 = 74%** | Implicit delete is the common path (Task 4) |
| The feed never over-executes | **0 in 937,083** | Clamp is a guard, not a workaround (Task 4) |
| Per-symbol price span | median **19,999,998 cents** | Window cannot cover it (Task 2) |
| Peak distinct live levels | **5,086** worst of ten | Window comfortably holds it (Task 2) |
| Out-of-window add rate | 0.002% (BAC) to **6.851%** (AMZN) | Overflow is ordinary traffic (Task 4) |
| Peak live orders | 1,731,096 market-wide; **34,248** worst of ten | Table sizing (Task 7) |

**The measurement changed the design twice.**

The spec originally claimed a maximum per-symbol spread of 9,600 cents and concluded
the overflow path would "rarely be taken". Both were artefacts of a pre-market slice.
At market open the median symbol spans $199,999.98, because every symbol carries stub
quotes near $0.01 and $200,000. The window-plus-overflow structure survived, but
overflow was promoted from safety net to ordinary operating mode, and `best_side()`
gained a requirement it did not have: **consult the overflow map when the ladder side
is empty**, or a book whose only bid is a stub reports no bid at all.

Separately, the arithmetic in Task 2 was checked against an executable model before
being written down, which caught three tests that used a $55.36 stock. That grid's base
clamps to zero, so the centring, stub-quote and boundary assertions were all wrong for
it and right for a $500 one. They now use $500 and the clamp has a test of its own.

**Three hazards for the executor.**

1. **Reconstruction must never match.** A crossed book is real output. Code that
   "helpfully" resolves it deletes liquidity the exchange never removed, and no local
   test catches it because the result still looks like a book.
2. **The oracle must be checked BEFORE the fill is applied.** Afterwards the level may
   already be gone, the comparison is against the next state, and it silently always
   passes — a test that can never fail is worse than no test.
3. **Unsigned underflow is the quiet killer.** `remaining` and level totals are
   unsigned, UBSan does not flag unsigned wrap by default, and one unclamped subtraction
   turns a level total into roughly 1.8e19 that then corrupts every depth query
   downstream.
