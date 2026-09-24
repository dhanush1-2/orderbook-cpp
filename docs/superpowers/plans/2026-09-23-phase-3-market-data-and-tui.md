# Phase 3: Market Data and Live Demo — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish top-N book depth out of the matching thread without ever blocking it, and render it live in a terminal from replayed order flow.

**Architecture:** The matching thread writes an `L2Snapshot` into a **seqlock**: a single-writer, multi-reader protocol where the writer never waits and readers retry on a torn read. A separate render thread reads the newest snapshot at a fixed frame rate and drops whatever it missed. This is the market-data discipline of a real venue in miniature, and it is the reason a TUI is safe to attach to a latency-sensitive engine at all.

**Tech Stack:** C++20, no new dependencies. The TUI is hand-rolled ANSI escapes rather than a widget library: the engine and harness depend on the standard library alone and this phase keeps that true. The cost is ~150 lines of terminal handling; the benefit is no dependency and full control over the one thing that must not go wrong, which is restoring the terminal on exit.

**Spec:** [`../specs/2026-09-22-order-book-matching-engine-design.md`](../specs/2026-09-22-order-book-matching-engine-design.md) — success criterion **S8**, and section 7.1's architecture diagram.

**Prerequisite:** Phase 2 complete and merged. `FastEngine` must be the measured engine, because demonstrating an unoptimized one demonstrates nothing.

## Global Constraints

Everything in [`README.md`](README.md) still applies. The ones this phase turns on:

- **The matching thread must never block on a reader.** The writer takes no lock and never waits. A test asserts writer throughput is unchanged with the reader stopped.
- **`snapshot_l2` must not allocate.** It fills a caller-provided `L2Snapshot`.
- **This phase introduces the project's first real concurrency**, so **ThreadSanitizer joins the verification set.** A seqlock deliberately races on the data words and is correct only because of the sequence protocol, so the data words must be read and written in a way TSan accepts — see Task 1.
- **The terminal must be restored on every exit path**, including SIGINT and an exception. A tool that leaves the terminal in raw mode with the cursor hidden is user-hostile.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `include/ob/seqlock.hpp` | Single-writer multi-reader seqlock | 1 |
| `include/ob/l2_snapshot.hpp` | Top-N depth value type and its formatting | 2 |
| `include/ob/fast_engine.hpp` | gains `snapshot_l2(L2Snapshot&) const` | 2 |
| `tools/replay.cpp` | Feeds a generated or file-backed stream through the engine and publishes snapshots | 3 |
| `tools/tui.cpp` | Terminal depth ladder, reads the seqlock | 4 |
| `tests/test_seqlock.cpp`, `tests/test_l2_snapshot.cpp`, `tests/test_publisher_isolation.cpp` | | 1, 2, 5 |

---

## Task 1: The seqlock

**Files:** Create `include/ob/seqlock.hpp`, `tests/test_seqlock.cpp`. Modify `tests/CMakeLists.txt`.

**Produces:** `ob::Seqlock<T>` with `store(const T&)`, `try_load(T&) const -> bool`, `load(T&) const` (retries until success), `sequence() const -> std::uint64_t`.

**The protocol.** The writer increments the sequence to an odd value, writes the payload, then increments to the next even value. A reader samples the sequence, rejects an odd value (a write is in progress), copies the payload, samples again, and retries if it changed. The writer never waits for anything, which is the entire point: **a stalled reader cannot slow the matching thread.**

**Why `std::atomic` on the payload, not a plain copy.** A seqlock intentionally allows a reader to observe a torn payload and then discard it. In C++ that is a data race, which is undefined behavior, and ThreadSanitizer will correctly report it. The standard-conforming way is to make the payload words atomic and access them with `memory_order_relaxed`: the sequence's acquire/release edges give the ordering, and the relaxed payload accesses make the race well-defined rather than UB. The cost is nil on ARM and x86 for word-sized relaxed loads.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_seqlock.cpp
#include <ob/seqlock.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

struct Payload {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
};

TEST(Seqlock, StartsAtAnEvenSequenceAndLoadsTheInitialValue) {
    ob::Seqlock<Payload> s;
    EXPECT_EQ(s.sequence() % 2, 0u);
    Payload p{1, 2, 3};
    EXPECT_TRUE(s.try_load(p));
    EXPECT_EQ(p.a, 0u);
}

TEST(Seqlock, StoreThenLoadRoundTrips) {
    ob::Seqlock<Payload> s;
    s.store(Payload{7, 8, 9});
    Payload p{};
    ASSERT_TRUE(s.try_load(p));
    EXPECT_EQ(p.a, 7u);
    EXPECT_EQ(p.b, 8u);
    EXPECT_EQ(p.c, 9u);
}

TEST(Seqlock, SequenceAdvancesByTwoPerStore) {
    ob::Seqlock<Payload> s;
    const std::uint64_t before = s.sequence();
    s.store(Payload{1, 1, 1});
    EXPECT_EQ(s.sequence(), before + 2);
    s.store(Payload{2, 2, 2});
    EXPECT_EQ(s.sequence(), before + 4);
}

// The property that matters: a reader NEVER observes a half-written payload. The
// writer only ever publishes triples where a == b == c, so any reader that sees
// them differ has observed a tear that the protocol failed to reject.
TEST(Seqlock, ReadersNeverObserveATornPayload) {
    ob::Seqlock<Payload> s;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> reads{0};

    std::thread writer([&] {
        for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            s.store(Payload{i, i, i});
        }
    });

    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            Payload p{};
            while (!stop.load(std::memory_order_relaxed)) {
                if (s.try_load(p)) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                    if (p.a != p.b || p.b != p.c) {
                        torn.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (std::thread& t : readers) {
        t.join();
    }

    EXPECT_GT(reads.load(), 1000u) << "readers barely ran; the test proved nothing";
    EXPECT_EQ(torn.load(), 0u) << "a reader observed a torn payload";
}

// load() must make progress even under a hot writer.
TEST(Seqlock, BlockingLoadEventuallySucceedsUnderContention) {
    ob::Seqlock<Payload> s;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
            s.store(Payload{i, i, i});
        }
    });
    Payload p{};
    for (int i = 0; i < 1000; ++i) {
        s.load(p);
        ASSERT_EQ(p.a, p.c);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails** (`'ob/seqlock.hpp' file not found`).

- [ ] **Step 3: Write `include/ob/seqlock.hpp`**

```cpp
// include/ob/seqlock.hpp
#pragma once

// Single-writer, multi-reader seqlock.
//
// The writer NEVER WAITS. That is the whole point: this sits between the matching
// thread and anything that wants to look at the book, and a slow or stopped reader
// must not be able to slow matching down. A reader that catches a write in progress
// simply retries and sees a newer value.
//
// WHY THE PAYLOAD IS ATOMIC. A seqlock deliberately lets a reader copy a payload
// that is being written and then discard it. In C++ that is a data race, which is
// undefined behavior, and ThreadSanitizer is right to report it. Making the payload
// words atomic and accessing them with memory_order_relaxed makes the race
// well-defined: the sequence counter's release/acquire pair supplies the ordering,
// and the relaxed payload accesses supply legality. On arm64 and x86-64 a relaxed
// word-sized load or store is the same instruction as a plain one, so this costs
// nothing at runtime and buys standard conformance plus a clean TSan run.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ob {

template <class T>
class Seqlock {
public:
    static_assert(std::is_trivially_copyable_v<T>, "payload must be trivially copyable");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0,
                  "payload size must be a whole number of 64-bit words");

    Seqlock() {
        const T zero{};
        store(zero);
        seq_.store(0, std::memory_order_release);
    }

    // Writer side. Single writer only; two concurrent writers would corrupt state.
    void store(const T& value) noexcept {
        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);

        const auto* src = reinterpret_cast<const std::uint64_t*>(&value);
        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(src[i], std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);  // even: complete
    }

    // Reader side. Returns false when a write was in progress or landed mid-copy.
    [[nodiscard]] bool try_load(T& out) const noexcept {
        const std::uint64_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            return false;  // writer is mid-update
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        auto* dst = reinterpret_cast<std::uint64_t*>(&out);
        for (std::size_t i = 0; i < kWords; ++i) {
            dst[i] = words_[i].load(std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        return seq_.load(std::memory_order_acquire) == before;
    }

    // Retries until a consistent snapshot is obtained. Only safe because the writer
    // always finishes: it never blocks, so a retry loop cannot starve indefinitely.
    void load(T& out) const noexcept {
        while (!try_load(out)) {
            // spin: the writer is a handful of stores away from finishing
        }
    }

    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);

    // Padded so the sequence counter and the payload do not share a cache line with
    // whatever the enclosing object puts next to them. 128 bytes on Apple Silicon.
    alignas(std::hardware_destructive_interference_size) std::atomic<std::uint64_t> seq_{0};
    alignas(std::hardware_destructive_interference_size)
        std::atomic<std::uint64_t> words_[kWords]{};
};

}  // namespace ob
```

- [ ] **Step 4: Run the tests** — expect PASS, 5 tests.

- [ ] **Step 5: Run under ThreadSanitizer, which is the real test here**

```bash
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DOB_WARNINGS_AS_ERRORS=ON
cmake --build build-tsan
./build-tsan/tests/ob_tests --gtest_filter='Seqlock*'
```

Expected: PASS with **no TSan reports**. A `data race` report on `words_` means the payload was made a plain array rather than atomics; that is the mistake this design exists to avoid, and the fix is the atomics, not suppressing the warning.

- [ ] **Step 6: Commit**

```bash
git add include/ob/seqlock.hpp tests/test_seqlock.cpp tests/CMakeLists.txt
git commit -m "feat: single-writer seqlock where the writer never waits"
```

---

## Task 2: L2 snapshot and the engine's publisher

**Files:** Create `include/ob/l2_snapshot.hpp`, `tests/test_l2_snapshot.cpp`. Modify `include/ob/fast_engine.hpp`, `include/ob/reference_engine.hpp`, `tests/CMakeLists.txt`.

**Produces:** `ob::L2Level{Ticks price; QtySum qty; std::uint32_t orders;}`, `ob::L2Snapshot` with `kDepth`, `seq`, `bid_levels`, `ask_levels`, `bids[]`, `asks[]`, plus `best_bid()`, `best_ask()`, `spread()`, `total_bid_qty()`. Both engines gain `snapshot_l2(L2Snapshot&) const`.

`L2Snapshot` must be trivially copyable and a whole number of 64-bit words, because `Seqlock` requires both. `kDepth = 15` keeps it small enough that a snapshot is a handful of cache lines.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_l2_snapshot.cpp
#include <ob/fast_engine.hpp>
#include <ob/l2_snapshot.hpp>
#include <ob/reference_engine.hpp>
#include <ob/seqlock.hpp>

#include <gtest/gtest.h>

namespace {

using ob::OrderType;
using ob::Side;

template <class E>
void feed(E& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(L2Snapshot, IsSeqlockCompatible) {
    static_assert(std::is_trivially_copyable_v<ob::L2Snapshot>);
    static_assert(sizeof(ob::L2Snapshot) % sizeof(std::uint64_t) == 0);
    SUCCEED();
}

TEST(L2Snapshot, EmptyBookProducesAnEmptySnapshot) {
    const ob::FastEngine e;
    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    EXPECT_EQ(s.bid_levels, 0u);
    EXPECT_EQ(s.ask_levels, 0u);
    EXPECT_EQ(s.best_bid(), ob::kNoPrice);
    EXPECT_EQ(s.best_ask(), ob::kNoPrice);
}

TEST(L2Snapshot, AggregatesQuantityAndOrderCountPerLevel) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 25));
    feed(e, ob::make_new(3, Side::Buy, OrderType::Limit, 9990, 5));

    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    ASSERT_EQ(s.bid_levels, 2u);
    EXPECT_EQ(s.bids[0].price, 10000);
    EXPECT_EQ(s.bids[0].qty, 35u);
    EXPECT_EQ(s.bids[0].orders, 2u);
    EXPECT_EQ(s.bids[1].price, 9990);
    EXPECT_EQ(s.bids[1].qty, 5u);
}

TEST(L2Snapshot, LevelsAreOrderedBestFirstOnBothSides) {
    ob::FastEngine e;
    for (ob::Ticks px = 9990; px <= 9999; ++px) {
        feed(e, ob::make_new(static_cast<ob::OrderId>(px), Side::Buy, OrderType::Limit, px, 1));
    }
    for (ob::Ticks px = 10010; px >= 10001; --px) {
        feed(e, ob::make_new(static_cast<ob::OrderId>(px), Side::Sell, OrderType::Limit, px, 1));
    }
    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    for (std::uint32_t i = 1; i < s.bid_levels; ++i) {
        EXPECT_LT(s.bids[i].price, s.bids[i - 1].price) << "bids must descend";
    }
    for (std::uint32_t i = 1; i < s.ask_levels; ++i) {
        EXPECT_GT(s.asks[i].price, s.asks[i - 1].price) << "asks must ascend";
    }
    EXPECT_LT(s.best_bid(), s.best_ask());
    EXPECT_EQ(s.spread(), s.best_ask() - s.best_bid());
}

TEST(L2Snapshot, TruncatesAtKDepthWithoutOverrunning) {
    ob::FastEngine e;
    // Twice the snapshot depth, so truncation is exercised.
    for (std::uint32_t i = 0; i < ob::L2Snapshot::kDepth * 2; ++i) {
        const ob::Ticks px = 10000 - static_cast<ob::Ticks>(i);
        feed(e, ob::make_new(static_cast<ob::OrderId>(i + 1), Side::Buy, OrderType::Limit, px, 1));
    }
    ob::L2Snapshot s{};
    e.snapshot_l2(s);
    EXPECT_EQ(s.bid_levels, ob::L2Snapshot::kDepth);
    EXPECT_EQ(s.bids[0].price, 10000) << "truncation must keep the BEST levels";
}

// Both engines must agree, for the same reason their event streams must.
TEST(L2Snapshot, BothEnginesProduceIdenticalSnapshots) {
    ob::FastEngine fast;
    ob::ReferenceEngine ref;
    for (ob::OrderId id = 1; id <= 200; ++id) {
        const ob::Command c = ob::make_new(
            id, id % 2 ? Side::Buy : Side::Sell, OrderType::Limit,
            id % 2 ? 10000 - static_cast<ob::Ticks>(id % 20)
                   : 10010 + static_cast<ob::Ticks>(id % 20),
            static_cast<ob::Qty>(id));
        feed(fast, c);
        feed(ref, c);
    }
    ob::L2Snapshot a{}, b{};
    fast.snapshot_l2(a);
    ref.snapshot_l2(b);
    ASSERT_EQ(a.bid_levels, b.bid_levels);
    ASSERT_EQ(a.ask_levels, b.ask_levels);
    for (std::uint32_t i = 0; i < a.bid_levels; ++i) {
        EXPECT_EQ(a.bids[i].price, b.bids[i].price) << "bid level " << i;
        EXPECT_EQ(a.bids[i].qty, b.bids[i].qty) << "bid level " << i;
        EXPECT_EQ(a.bids[i].orders, b.bids[i].orders) << "bid level " << i;
    }
    for (std::uint32_t i = 0; i < a.ask_levels; ++i) {
        EXPECT_EQ(a.asks[i].price, b.asks[i].price) << "ask level " << i;
        EXPECT_EQ(a.asks[i].qty, b.asks[i].qty) << "ask level " << i;
    }
}

TEST(L2Snapshot, RoundTripsThroughASeqlock) {
    ob::FastEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 42));
    ob::L2Snapshot s{};
    e.snapshot_l2(s);

    ob::Seqlock<ob::L2Snapshot> lock;
    lock.store(s);
    ob::L2Snapshot got{};
    ASSERT_TRUE(lock.try_load(got));
    EXPECT_EQ(got.bids[0].price, 10000);
    EXPECT_EQ(got.bids[0].qty, 42u);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails.**

- [ ] **Step 3: Write `include/ob/l2_snapshot.hpp`**

```cpp
// include/ob/l2_snapshot.hpp
#pragma once

// Aggregated top-of-book depth, as a venue's level-2 feed publishes it.
//
// Trivially copyable and a whole number of 64-bit words, because Seqlock requires
// both. kDepth is small on purpose: a snapshot is a handful of cache lines, so the
// matching thread can publish one cheaply and a reader can copy one without
// noticeably widening the window in which a tear can happen.

#include <ob/types.hpp>

#include <cstdint>
#include <type_traits>

namespace ob {

struct L2Level {
    Ticks         price   = kNoPrice;  // 4
    std::uint32_t orders  = 0;         // 4
    QtySum        qty     = 0;         // 8
};
static_assert(sizeof(L2Level) == 16);
static_assert(std::is_trivially_copyable_v<L2Level>);

struct L2Snapshot {
    static constexpr std::uint32_t kDepth = 15;

    Seq           seq        = 0;  // 8. engine sequence at publication
    std::uint32_t bid_levels = 0;  // 4
    std::uint32_t ask_levels = 0;  // 4
    L2Level       bids[kDepth]{};  // best first, descending price
    L2Level       asks[kDepth]{};  // best first, ascending price

    [[nodiscard]] Ticks best_bid() const noexcept {
        return bid_levels == 0 ? kNoPrice : bids[0].price;
    }
    [[nodiscard]] Ticks best_ask() const noexcept {
        return ask_levels == 0 ? kNoPrice : asks[0].price;
    }
    // kNoPrice when either side is empty: there is no spread without two sides.
    [[nodiscard]] Ticks spread() const noexcept {
        return (bid_levels == 0 || ask_levels == 0) ? kNoPrice
                                                   : asks[0].price - bids[0].price;
    }
    [[nodiscard]] QtySum total_bid_qty() const noexcept { return side_total(bids, bid_levels); }
    [[nodiscard]] QtySum total_ask_qty() const noexcept { return side_total(asks, ask_levels); }

private:
    static QtySum side_total(const L2Level* lv, std::uint32_t n) noexcept {
        QtySum t = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            t += lv[i].qty;
        }
        return t;
    }
};

static_assert(std::is_trivially_copyable_v<L2Snapshot>);
static_assert(sizeof(L2Snapshot) % sizeof(std::uint64_t) == 0,
              "Seqlock copies the payload in 64-bit words");

}  // namespace ob
```

- [ ] **Step 4: Add `snapshot_l2` to both engines**

`FastEngine`, using `for_each_level` so it walks levels rather than orders — O(depth), not O(orders):

```cpp
    // Fills a caller-provided snapshot. Allocates nothing, walks at most kDepth
    // levels per side, and stops early. Safe to call from the matching thread.
    void snapshot_l2(L2Snapshot& out) const noexcept {
        out.seq        = seq_;
        out.bid_levels = 0;
        out.ask_levels = 0;
        bids_.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (out.bid_levels >= L2Snapshot::kDepth) {
                return false;
            }
            out.bids[out.bid_levels++] = L2Level{px, lv.count, lv.total};
            return true;
        });
        asks_.for_each_level([&](Ticks px, const PriceLevel& lv) {
            if (out.ask_levels >= L2Snapshot::kDepth) {
                return false;
            }
            out.asks[out.ask_levels++] = L2Level{px, lv.count, lv.total};
            return true;
        });
    }
```

`ReferenceEngine`, walking its maps — present only so the two can be compared:

```cpp
    void snapshot_l2(L2Snapshot& out) const {
        out.seq        = seq_;
        out.bid_levels = 0;
        out.ask_levels = 0;
        for (const auto& [px, level] : bids_) {
            if (out.bid_levels >= L2Snapshot::kDepth) {
                break;
            }
            QtySum total = 0;
            for (const RefOrder& o : level) {
                total += o.remaining;
            }
            out.bids[out.bid_levels++] =
                L2Level{px, static_cast<std::uint32_t>(level.size()), total};
        }
        for (const auto& [px, level] : asks_) {
            if (out.ask_levels >= L2Snapshot::kDepth) {
                break;
            }
            QtySum total = 0;
            for (const RefOrder& o : level) {
                total += o.remaining;
            }
            out.asks[out.ask_levels++] =
                L2Level{px, static_cast<std::uint32_t>(level.size()), total};
        }
    }
```

Both need `#include <ob/l2_snapshot.hpp>`.

- [ ] **Step 5: Run the tests** — expect PASS, 7 tests, including the cross-engine agreement.

- [ ] **Step 6: Commit.**

---

## Task 3: The replay tool

**Files:** Create `tools/replay.cpp`, `tools/CMakeLists.txt`. Modify `CMakeLists.txt`.

**Produces:** the `ob_replay` executable. It drives a scenario through `FastEngine` at a chosen rate, publishing an `L2Snapshot` into a `Seqlock` as it goes, and prints a summary. With `--tui` it renders (Task 4).

Rate limiting exists so a human can watch: unthrottled, the engine finishes 2 M orders before a frame is drawn.

- [ ] **Step 1: Write `tools/replay.cpp`** — driver with `--scenario`, `--ops`, `--rate`, `--seed`, `--tui`, publishing a snapshot every `--publish-every` commands (default 1) and printing final book state plus event counts.

- [ ] **Step 2: Add `tools/CMakeLists.txt` behind `OB_BUILD_TOOLS`, wire into the root.**

- [ ] **Step 3: Run it headless and confirm the summary matches what the scenario should produce.**

- [ ] **Step 4: Commit.**

---

## Task 4: The TUI

**Files:** Create `tools/tui.hpp`. Modify `tools/replay.cpp`.

**Produces:** `ob::tui::Terminal` (RAII alternate screen + cursor restore) and `ob::tui::render(const L2Snapshot&, ...) -> std::string`.

**Hand-rolled ANSI rather than a widget library.** The engine, harness and tools depend on the standard library alone, and this phase keeps that true. The risk that justifies care is not layout, it is **leaving the terminal broken**: raw mode, hidden cursor and the alternate screen must all be undone on every exit path including SIGINT.

- [ ] **Step 1: Write the render test first** — `render` returns a `std::string` precisely so it is testable without a terminal:

```cpp
TEST(Tui, RendersBothSidesWithPricesAndQuantities) { /* assert substrings */ }
TEST(Tui, ClampsToATinyTerminalWithoutOverrunning) { /* 1x1, 3x10 */ }
TEST(Tui, RendersAnEmptyBookWithoutCrashing) { /* empty snapshot */ }
```

- [ ] **Step 2: Write `tools/tui.hpp`** — `Terminal` enters the alternate screen in its constructor and restores in its destructor, installs a SIGINT handler that sets an atomic flag (no I/O in the handler), and queries size with `ioctl(TIOCGWINSZ)` per frame so resize needs no signal handling. `render` draws a depth ladder with a bar chart scaled to the largest level, the spread, and totals.

- [ ] **Step 3: Verify manually**, then confirm the terminal is sane afterwards:

```bash
./build/tools/ob_replay --scenario mixed_realistic --ops 200000 --rate 5000 --tui
# after exit:
tput cnorm; echo "cursor visible, terminal restored"
```

- [ ] **Step 4: Commit.**

---

## Task 5: Prove the reader cannot slow the writer

**Files:** Create `tests/test_publisher_isolation.cpp`. Modify `tests/CMakeLists.txt`.

This is the task that justifies attaching a TUI to a latency-sensitive engine at all. Without it, "the writer never blocks" is a claim in a comment.

- [ ] **Step 1: Write the test**

Measure engine throughput while publishing a snapshot per command, under three conditions: no reader, a reader spinning as fast as it can, and a reader that **stops entirely** mid-run. Assert the throughput of all three is within a tolerance of each other, and that the stalled-reader case is not slower than the no-reader case beyond noise.

```cpp
TEST(PublisherIsolation, AStalledReaderDoesNotSlowTheWriter) { /* three conditions */ }
TEST(PublisherIsolation, SnapshotPublicationCostIsBounded) {
    // Publishing every command must not dominate. Compare throughput with
    // publish-every-1 against publish-every-0 and assert the ratio is sane.
}
```

- [ ] **Step 2: Run it, and run it under TSan.**

- [ ] **Step 3: Commit.**

---

## Task 6: Documentation and CI

- [ ] **Step 1: Add a TSan job to `.github/workflows/ci.yml`.** This phase introduces the project's first concurrency, so TSan joins ASan and UBSan permanently.
- [ ] **Step 2: Update `README.md`** — status, the architecture diagram showing the seqlock boundary, a TUI screenshot or transcript, and the isolation result.
- [ ] **Step 3: Update the phase index.**
- [ ] **Step 4: Commit.**

---

## Self-review

**Spec coverage.** S8 (demonstrable live) is Tasks 3 to 5. Section 7.1's seqlock boundary is Task 1. Section 10's "TUI thread stalls or is killed" row is Task 5. Section 5.8's note that `Engine` gains `snapshot_l2` in this phase is Task 2.

**Hazards for the executor.**
1. **`Seqlock`'s payload must be atomic words, not a plain struct copy.** A plain copy is a data race and therefore UB, and TSan will say so. Task 1 Step 5 is the check.
2. **`L2Snapshot` must stay trivially copyable and word-sized**, or `Seqlock`'s static asserts fire. Adding a `std::string` or a vector to it breaks the design, not just the build.
3. **The terminal must be restored on every exit path.** Test by running the TUI, killing it with Ctrl-C, and confirming the shell is usable.
