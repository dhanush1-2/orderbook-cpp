# Phase 1: Foundation and Correctness Oracle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a correct, exhaustively tested limit-order matching engine with green CI, plus the random-stream generator and shrinker that will act as the correctness oracle for the fast engine in Phase 2.

**Architecture:** Header-only C++20 library. Value-type `Command` in, sequenced `Event` stream out through a caller-owned `EventBuffer`. `ReferenceEngine` uses `std::map` and `std::list` deliberately: it exists to be obviously correct, never to be fast, and it is never optimized. A compile-time `Engine` concept lets every test in this plan be reused verbatim against `FastEngine` in Phase 2.

**Tech Stack:** C++20, CMake >= 3.25 + Ninja, GoogleTest v1.15.2 (FetchContent, test-only), GitHub Actions.

**Spec:** [`docs/superpowers/specs/2026-09-22-order-book-matching-engine-design.md`](../specs/2026-09-22-order-book-matching-engine-design.md)

## Global Constraints

See [`docs/superpowers/plans/README.md`](README.md) for the full list, which applies to every task here. The ones this plan touches most:

- **C++20**, `CMAKE_CXX_EXTENSIONS OFF`. CMake >= 3.25.
- **Prices are `std::int32_t` ticks.** Floating point never touches a price or quantity, anywhere, including tests.
- `kMinTick = 1`, `kMaxTick = 65536`. Every externally supplied price is range-checked **before** being used as an index.
- **Validation order is fixed:** quantity, then price range, then duplicate ID, then capacity. A command bad in two ways always reports the first in that order.
- **Trades execute at the resting (maker) order's price.**
- **Crossing is inclusive:** a buy crosses when `price >= best_ask`.
- **Order IDs must be strictly increasing** and are retired permanently. Duplicate
  detection is `id <= high_water_`, a single comparison, not a set lookup. `FastEngine`
  cannot hold an unbounded set of retired IDs on a no-allocation hot path, so both
  engines use the high-water mark. A *rejected* command does not advance the mark.
  ID 0 is rejected automatically because the mark starts at 0.
- **A rejected command leaves the book bit-for-bit unchanged.**
- **Every command produces at least one event.** There is no silent drop.
- Warnings are errors: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align`. Expect to write explicit `static_cast`s; that is the point.
- Conventional Commits. **Nothing is pushed to a remote without explicit approval.**

## Event ordering contract

Fixed here because every test in this plan and Phase 2 depends on it byte-for-byte.

For a `New` command that passes validation:

```
Accepted(order_id)
  then, per fill, in match order:
    Trade(order_id=taker, maker_id=maker, price=maker's price, qty=fill)
    Filled(maker_id)            <- only if that fill consumed the maker entirely
  then exactly one terminal event for the taker, if it did not rest:
    Filled(order_id)            <- remaining == 0
    Cancelled(order_id, reason) <- Market/Ioc remainder, or Fok unfillable
  and no terminal event if the taker rested (Accepted already conveyed that).
```

For a `New` that fails validation: exactly one `Rejected(order_id, reason)`.
For `Cancel`: exactly one `Cancelled(order_id, UserRequested, qty=removed)` or one `Rejected(order_id, UnknownOrderId)`.

## File Structure

| File | Responsibility | Created in |
|---|---|---|
| `CMakeLists.txt` | Root build: options, warning and sanitizer interface targets, header-only `ob` target | Task 1 |
| `.gitignore`, `.clang-format` | Hygiene | Task 1 |
| `tests/CMakeLists.txt` | GoogleTest via FetchContent SYSTEM, test executable, `gtest_discover_tests` | Task 1 |
| `.github/workflows/ci.yml` | CI. Minimal in Task 1, full matrix in Task 13 | Task 1, 13 |
| `include/ob/types.hpp` | Scalar aliases, enums, ladder bounds, size assertions | Task 2 |
| `include/ob/command.hpp` | `Command` value type + factories | Task 3 |
| `include/ob/events.hpp` | `Event` value type, equality, GoogleTest printer | Task 3 |
| `include/ob/engine_concept.hpp` | `EventBuffer`, `Engine` concept | Task 4 |
| `include/ob/reference_engine.hpp` | The oracle matcher | Tasks 5–9 |
| `include/ob/invariants.hpp` | Whole-book invariant checker | Task 11 |
| `tests/model/scenario_gen.hpp` | Seeded random command-stream generator + delta-debugging shrinker | Task 12 |
| `tests/*.cpp` | One test file per concern | Tasks 2–13 |
| `README.md` | Project front page | Task 13 |

---

## Task 1: Build system, test harness, and CI skeleton

**Files:**
- Create: `CMakeLists.txt`, `.gitignore`, `.clang-format`, `tests/CMakeLists.txt`, `tests/test_smoke.cpp`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake target `ob` (INTERFACE, header-only, include dir `include/`), `ob_warnings` (INTERFACE), `ob_sanitize` (INTERFACE), test executable `ob_tests`. CMake options `OB_BUILD_TESTS`, `OB_BUILD_BENCH`, `OB_BUILD_FUZZ`, `OB_BUILD_TOOLS`, `OB_SANITIZE`, `OB_ENABLE_INVARIANTS`.

- [ ] **Step 1: Install the missing toolchain**

`cmake` is not installed on this machine; Homebrew is. Run:

```bash
brew install cmake
cmake --version
```

Expected: version 3.25 or newer. `ninja` is already present.

- [ ] **Step 2: Write `.gitignore`**

```gitignore
build/
build-*/
.cache/
compile_commands.json
*.o
*.a
*.dSYM/
perf.data*
callgrind.out.*
cachegrind.out.*
bench/results/*.raw
.DS_Store
```

- [ ] **Step 3: Write `.clang-format`**

```yaml
---
Language: Cpp
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
AccessModifierOffset: -4
PointerAlignment: Left
AlignAfterOpenBracket: Align
AllowShortFunctionsOnASingleLine: Inline
IncludeBlocks: Regroup
```

- [ ] **Step 4: Write the root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.25)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_BUILD_TESTS       "Build tests"                            ON)
option(OB_BUILD_BENCH       "Build benchmarks"                       OFF)
option(OB_BUILD_FUZZ        "Build libFuzzer targets"                OFF)
option(OB_BUILD_TOOLS       "Build replay and TUI tools"             OFF)
option(OB_SANITIZE          "Enable ASan + UBSan"                    OFF)
option(OB_ENABLE_INVARIANTS "Assert book invariants after every op"   OFF)

# Warning set. Applied only to our own targets, never to dependencies.
add_library(ob_warnings INTERFACE)
target_compile_options(ob_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wold-style-cast -Wcast-align -Wunused -Wnon-virtual-dtor
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
if(OB_WARNINGS_AS_ERRORS)
    target_compile_options(ob_warnings INTERFACE -Werror)
endif()

add_library(ob_sanitize INTERFACE)
if(OB_SANITIZE)
    target_compile_options(ob_sanitize INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
    target_link_options(ob_sanitize INTERFACE -fsanitize=address,undefined)
endif()

# The library is header-only: the hot path depends on cross-module inlining and
# LTO is not guaranteed across every compiler in the CI matrix.
add_library(ob INTERFACE)
target_include_directories(ob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ob INTERFACE ob_warnings ob_sanitize)
if(OB_ENABLE_INVARIANTS)
    target_compile_definitions(ob INTERFACE OB_ENABLE_INVARIANTS=1)
endif()

if(OB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 5: Write `tests/CMakeLists.txt`**

`SYSTEM` on the `FetchContent_Declare` is load-bearing: without it GoogleTest's own headers trip our `-Wold-style-cast` and the build fails on code we do not own.

```cmake
include(FetchContent)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    SYSTEM)
FetchContent_MakeAvailable(googletest)

add_executable(ob_tests
    test_smoke.cpp
)
target_link_libraries(ob_tests PRIVATE ob GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(ob_tests)
```

- [ ] **Step 6: Write the failing smoke test**

This test asserts the toolchain is what the spec requires, which is a real thing to verify once rather than assume forever.

```cpp
// tests/test_smoke.cpp
#include <gtest/gtest.h>

TEST(Smoke, CompilerIsCpp20OrLater) {
    static_assert(__cplusplus >= 202002L, "C++20 required");
    EXPECT_GE(__cplusplus, 202002L);
}

TEST(Smoke, IntegerTypesAreTheExpectedWidths) {
    static_assert(sizeof(std::int32_t) == 4);
    static_assert(sizeof(std::uint64_t) == 8);
    EXPECT_EQ(sizeof(void*), 8u);  // 64-bit only; the design assumes it
}
```

- [ ] **Step 7: Configure and run, verify it passes**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: configure succeeds (fetching GoogleTest), 2 tests pass. If `-Wold-style-cast` errors appear inside GoogleTest headers, `SYSTEM` did not take effect; check the CMake version is >= 3.25.

- [ ] **Step 8: Write the minimal CI workflow**

The full matrix arrives in Task 13. This proves CI runs at all.

```yaml
# .github/workflows/ci.yml
name: CI
on:
  push:
    branches: [main]
  pull_request:

jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install Ninja
        run: sudo apt-get update && sudo apt-get install -y ninja-build
      - name: Configure
        run: cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON
      - name: Build
        run: cmake --build build
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt .gitignore .clang-format tests/CMakeLists.txt tests/test_smoke.cpp .github/workflows/ci.yml
git commit -m "build: cmake skeleton, googletest harness, minimal CI"
```

---

## Task 2: Core types

**Files:**
- Create: `include/ob/types.hpp`, `tests/test_types.cpp`
- Modify: `tests/CMakeLists.txt` (add `test_types.cpp` to `ob_tests` sources)

**Interfaces:**
- Consumes: nothing.
- Produces: `ob::OrderId`, `ob::Ticks`, `ob::Qty`, `ob::QtySum`, `ob::Seq`, `ob::Slot`, `ob::kInvalidSlot`, `ob::kNoPrice`, `ob::kMinTick`, `ob::kMaxTick`, `ob::kLadderSize`, `ob::kMaxOrderQty`, `ob::Side`, `ob::OrderType`, `ob::RejectReason`, `ob::CancelReason`, `ob::opposite(Side)`, `ob::price_in_range(Ticks)`, `ob::qty_valid(Qty)`.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_types.cpp
#include <ob/types.hpp>

#include <gtest/gtest.h>

namespace {

TEST(Types, OppositeSideFlips) {
    EXPECT_EQ(ob::opposite(ob::Side::Buy), ob::Side::Sell);
    EXPECT_EQ(ob::opposite(ob::Side::Sell), ob::Side::Buy);
}

TEST(Types, PriceRangeIsInclusiveOfBothBounds) {
    EXPECT_TRUE(ob::price_in_range(ob::kMinTick));
    EXPECT_TRUE(ob::price_in_range(ob::kMaxTick));
    EXPECT_FALSE(ob::price_in_range(ob::kMinTick - 1));
    EXPECT_FALSE(ob::price_in_range(ob::kMaxTick + 1));
}

// Edge cases E12, E13: the ladder is a flat array, so an out-of-range price is an
// out-of-bounds write unless it is rejected first. These two assertions are the
// first line of that defence.
TEST(Types, ExtremePricesAreOutOfRange) {
    EXPECT_FALSE(ob::price_in_range(std::numeric_limits<ob::Ticks>::min()));
    EXPECT_FALSE(ob::price_in_range(std::numeric_limits<ob::Ticks>::max()));
    EXPECT_FALSE(ob::price_in_range(0));      // tick 0 is deliberately not valid
    EXPECT_FALSE(ob::price_in_range(-1));
}

TEST(Types, LadderSizeCoversExactlyTheValidTickRange) {
    EXPECT_EQ(ob::kLadderSize, static_cast<std::size_t>(ob::kMaxTick - ob::kMinTick + 1));
}

// Edge cases E10, E11.
TEST(Types, QuantityValidityRejectsZeroAndOversize) {
    EXPECT_FALSE(ob::qty_valid(0));
    EXPECT_TRUE(ob::qty_valid(1));
    EXPECT_TRUE(ob::qty_valid(ob::kMaxOrderQty));
    EXPECT_FALSE(ob::qty_valid(ob::kMaxOrderQty + 1));
}

TEST(Types, NoPriceSentinelIsNotAValidPrice) {
    EXPECT_FALSE(ob::price_in_range(ob::kNoPrice));
}

TEST(Types, EnumsAreOneByte) {
    static_assert(sizeof(ob::Side) == 1);
    static_assert(sizeof(ob::OrderType) == 1);
    static_assert(sizeof(ob::RejectReason) == 1);
    static_assert(sizeof(ob::CancelReason) == 1);
    SUCCEED();
}

}  // namespace
```

- [ ] **Step 2: Add the test file to the build and run it to verify it fails**

In `tests/CMakeLists.txt`, change the `add_executable` block to:

```cmake
add_executable(ob_tests
    test_smoke.cpp
    test_types.cpp
)
```

Run:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL at compile time with `fatal error: 'ob/types.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/types.hpp`**

```cpp
// include/ob/types.hpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace ob {

using OrderId = std::uint64_t;
using Ticks   = std::int32_t;   // price, in whole ticks. never floating point.
using Qty     = std::uint32_t;  // per-order quantity
using QtySum  = std::uint64_t;  // aggregates; wider so level overflow is unreachable
using Seq     = std::uint64_t;  // event sequence number
using Slot    = std::uint32_t;  // index into the order pool (Phase 2)

// Ladder bounds. Tick 0 is deliberately invalid so that a zero-initialised or
// default-constructed price can never be mistaken for a real one.
inline constexpr Ticks kMinTick = 1;
inline constexpr Ticks kMaxTick = 65536;
inline constexpr std::size_t kLadderSize =
    static_cast<std::size_t>(kMaxTick - kMinTick + 1);

inline constexpr Slot  kInvalidSlot = 0xFFFF'FFFFu;
inline constexpr Ticks kNoPrice     = std::numeric_limits<Ticks>::min();

// Bounded so that a level's QtySum total cannot overflow: 2^31 per order across
// at most 2^32 orders still fits in 2^63.
inline constexpr Qty kMaxOrderQty = 1u << 31;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : std::uint8_t { Limit = 0, Market, Ioc, Fok, PostOnly };

enum class RejectReason : std::uint8_t {
    None = 0,
    InvalidQuantity,
    PriceOutOfRange,
    DuplicateOrderId,
    UnknownOrderId,
    WouldCross,
    EngineCapacity,
};

enum class CancelReason : std::uint8_t {
    None = 0,
    UserRequested,
    NoLiquidity,
    Unfillable,
    IocRemainder,
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] constexpr bool price_in_range(Ticks p) noexcept {
    return p >= kMinTick && p <= kMaxTick;
}

[[nodiscard]] constexpr bool qty_valid(Qty q) noexcept {
    return q > 0 && q <= kMaxOrderQty;
}

// True when `taker_price` at `taker_side` crosses a resting order at `book_price`.
// Crossing is INCLUSIVE of equality (edge case E30).
[[nodiscard]] constexpr bool crosses(Side taker_side, Ticks taker_price,
                                     Ticks book_price) noexcept {
    return taker_side == Side::Buy ? taker_price >= book_price
                                   : taker_price <= book_price;
}

}  // namespace ob
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 9 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/types.hpp tests/test_types.cpp tests/CMakeLists.txt
git commit -m "feat: core scalar types, ladder bounds, and validation predicates"
```

---

## Task 3: Command and Event value types

**Files:**
- Create: `include/ob/command.hpp`, `include/ob/events.hpp`, `tests/test_command_events.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/types.hpp`.
- Produces: `ob::CommandType`, `ob::Command`, `ob::make_new(OrderId, Side, OrderType, Ticks, Qty)`, `ob::make_cancel(OrderId)`, `ob::EventType`, `ob::Event`, `operator==(const Event&, const Event&)`, `PrintTo(const Event&, std::ostream*)`.

The `PrintTo` overload is not decoration. Differential testing compares long event streams, and a failure that prints `Event` as a hex blob costs hours. GoogleTest picks up `PrintTo` by ADL.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_command_events.cpp
#include <ob/command.hpp>
#include <ob/events.hpp>

#include <gtest/gtest.h>

#include <sstream>

namespace {

TEST(Command, NewFactorySetsEveryField) {
    const ob::Command c = ob::make_new(42, ob::Side::Buy, ob::OrderType::Limit, 10050, 350);
    EXPECT_EQ(c.type, ob::CommandType::New);
    EXPECT_EQ(c.id, 42u);
    EXPECT_EQ(c.side, ob::Side::Buy);
    EXPECT_EQ(c.order_type, ob::OrderType::Limit);
    EXPECT_EQ(c.price, 10050);
    EXPECT_EQ(c.qty, 350u);
}

TEST(Command, CancelFactoryLeavesPriceAndQtyNeutral) {
    const ob::Command c = ob::make_cancel(42);
    EXPECT_EQ(c.type, ob::CommandType::Cancel);
    EXPECT_EQ(c.id, 42u);
    EXPECT_EQ(c.price, ob::kNoPrice);
    EXPECT_EQ(c.qty, 0u);
}

TEST(Event, EqualityComparesEveryField) {
    ob::Event a{};
    a.seq = 1; a.type = ob::EventType::Trade; a.order_id = 9;
    a.maker_id = 7; a.price = 10050; a.qty = 300;
    ob::Event b = a;
    EXPECT_EQ(a, b);

    b.qty = 299;
    EXPECT_NE(a, b);

    b = a; b.maker_id = 8;
    EXPECT_NE(a, b);

    b = a; b.seq = 2;
    EXPECT_NE(a, b);
}

TEST(Event, PrintToProducesReadableOutput) {
    ob::Event e{};
    e.seq = 5; e.type = ob::EventType::Trade; e.order_id = 99;
    e.maker_id = 7; e.price = 10050; e.qty = 300;

    std::ostringstream os;
    PrintTo(e, &os);
    const std::string s = os.str();

    EXPECT_NE(s.find("Trade"), std::string::npos);
    EXPECT_NE(s.find("99"), std::string::npos);
    EXPECT_NE(s.find("10050"), std::string::npos);
}

TEST(Event, RejectedPrintIncludesTheReasonName) {
    ob::Event e{};
    e.type = ob::EventType::Rejected;
    e.order_id = 3;
    e.reject = ob::RejectReason::PriceOutOfRange;

    std::ostringstream os;
    PrintTo(e, &os);
    EXPECT_NE(os.str().find("PriceOutOfRange"), std::string::npos);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_command_events.cpp` to the `ob_tests` sources in `tests/CMakeLists.txt`, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/command.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/command.hpp`**

```cpp
// include/ob/command.hpp
#pragma once

#include <ob/types.hpp>

namespace ob {

enum class CommandType : std::uint8_t { New = 0, Cancel };

// A command is a value: trivially copyable, no ownership, no allocation.
struct Command {
    CommandType type       = CommandType::New;
    Side        side       = Side::Buy;
    OrderType   order_type = OrderType::Limit;
    OrderId     id         = 0;
    Ticks       price      = kNoPrice;
    Qty         qty        = 0;
};

static_assert(std::is_trivially_copyable_v<Command>);

[[nodiscard]] constexpr Command make_new(OrderId id, Side side, OrderType type,
                                        Ticks price, Qty qty) noexcept {
    Command c{};
    c.type = CommandType::New;
    c.side = side;
    c.order_type = type;
    c.id = id;
    c.price = price;
    c.qty = qty;
    return c;
}

[[nodiscard]] constexpr Command make_cancel(OrderId id) noexcept {
    Command c{};
    c.type = CommandType::Cancel;
    c.id = id;
    c.price = kNoPrice;
    c.qty = 0;
    return c;
}

}  // namespace ob
```

Add `#include <type_traits>` at the top for `is_trivially_copyable_v`.

- [ ] **Step 4: Write `include/ob/events.hpp`**

```cpp
// include/ob/events.hpp
#pragma once

#include <ob/types.hpp>

#include <ostream>
#include <type_traits>

namespace ob {

enum class EventType : std::uint8_t { Accepted = 0, Rejected, Trade, Cancelled, Filled };

struct Event {
    Seq          seq      = 0;
    EventType    type     = EventType::Accepted;
    OrderId      order_id = 0;   // for Trade this is the TAKER
    OrderId      maker_id = 0;   // Trade only, otherwise 0
    Ticks        price    = kNoPrice;  // Trade: the MAKER's price
    Qty          qty      = 0;   // Trade: filled qty. Cancelled: qty removed.
    RejectReason reject   = RejectReason::None;
    CancelReason cancel   = CancelReason::None;
};

static_assert(std::is_trivially_copyable_v<Event>);

[[nodiscard]] constexpr bool operator==(const Event& a, const Event& b) noexcept {
    return a.seq == b.seq && a.type == b.type && a.order_id == b.order_id &&
           a.maker_id == b.maker_id && a.price == b.price && a.qty == b.qty &&
           a.reject == b.reject && a.cancel == b.cancel;
}

[[nodiscard]] constexpr const char* to_string(EventType t) noexcept {
    switch (t) {
        case EventType::Accepted:  return "Accepted";
        case EventType::Rejected:  return "Rejected";
        case EventType::Trade:     return "Trade";
        case EventType::Cancelled: return "Cancelled";
        case EventType::Filled:    return "Filled";
    }
    return "?";
}

[[nodiscard]] constexpr const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:             return "None";
        case RejectReason::InvalidQuantity:  return "InvalidQuantity";
        case RejectReason::PriceOutOfRange:  return "PriceOutOfRange";
        case RejectReason::DuplicateOrderId: return "DuplicateOrderId";
        case RejectReason::UnknownOrderId:   return "UnknownOrderId";
        case RejectReason::WouldCross:       return "WouldCross";
        case RejectReason::EngineCapacity:   return "EngineCapacity";
    }
    return "?";
}

[[nodiscard]] constexpr const char* to_string(CancelReason r) noexcept {
    switch (r) {
        case CancelReason::None:          return "None";
        case CancelReason::UserRequested: return "UserRequested";
        case CancelReason::NoLiquidity:   return "NoLiquidity";
        case CancelReason::Unfillable:    return "Unfillable";
        case CancelReason::IocRemainder:  return "IocRemainder";
    }
    return "?";
}

// Found by ADL. GoogleTest uses it to print Event in failure messages, which is
// what makes a differential-test failure readable instead of a hex dump.
inline void PrintTo(const Event& e, std::ostream* os) {
    *os << "Event{seq=" << e.seq << " " << to_string(e.type)
        << " id=" << e.order_id;
    if (e.type == EventType::Trade) {
        *os << " maker=" << e.maker_id;
    }
    if (e.price != kNoPrice) {
        *os << " px=" << e.price;
    }
    if (e.qty != 0) {
        *os << " qty=" << e.qty;
    }
    if (e.reject != RejectReason::None) {
        *os << " reject=" << to_string(e.reject);
    }
    if (e.cancel != CancelReason::None) {
        *os << " cancel=" << to_string(e.cancel);
    }
    *os << "}";
}

inline std::ostream& operator<<(std::ostream& os, const Event& e) {
    PrintTo(e, &os);
    return os;
}

}  // namespace ob
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 14 tests total.

- [ ] **Step 6: Commit**

```bash
git add include/ob/command.hpp include/ob/events.hpp tests/test_command_events.cpp tests/CMakeLists.txt
git commit -m "feat: Command and Event value types with readable test printers"
```

---

## Task 4: EventBuffer and the Engine concept

**Files:**
- Create: `include/ob/engine_concept.hpp`, `tests/test_event_buffer.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/command.hpp`, `ob/events.hpp`.
- Produces: `ob::EventBuffer` with `EventBuffer(Event*, std::size_t)`, `push(const Event&)`, `size()`, `capacity()`, `empty()`, `clear()`, `view() -> std::span<const Event>`, `operator[](std::size_t)`, `begin()`, `end()`; `ob::FixedEventBuffer<N>`; the `ob::Engine` concept.

`EventBuffer` never owns memory: that is what keeps "no allocation on the hot path" true. Overflow is a caller programming error (spec E47), so it asserts rather than growing or truncating, because a truncating buffer would silently break the "every command produces at least one event" contract.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_event_buffer.cpp
#include <ob/engine_concept.hpp>

#include <gtest/gtest.h>

#include <array>

namespace {

ob::Event mk(ob::Seq s, ob::EventType t) {
    ob::Event e{};
    e.seq = s;
    e.type = t;
    return e;
}

TEST(EventBuffer, StartsEmpty) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_EQ(buf.capacity(), 4u);
}

TEST(EventBuffer, PushAppendsInOrder) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.push(mk(2, ob::EventType::Trade));

    ASSERT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf[0].seq, 1u);
    EXPECT_EQ(buf[1].type, ob::EventType::Trade);
}

TEST(EventBuffer, ClearResetsSizeButNotCapacity) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.clear();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.capacity(), 4u);
}

// Edge case E46: a buffer sized exactly to the event count must work.
TEST(EventBuffer, FillingToExactCapacitySucceeds) {
    std::array<ob::Event, 2> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    buf.push(mk(2, ob::EventType::Filled));
    EXPECT_EQ(buf.size(), 2u);
    EXPECT_EQ(buf.view().size(), 2u);
}

TEST(EventBuffer, ViewIsIterableAndRangeBased) {
    std::array<ob::Event, 4> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    buf.push(mk(7, ob::EventType::Accepted));
    buf.push(mk(8, ob::EventType::Filled));

    ob::Seq total = 0;
    for (const ob::Event& e : buf) {
        total += e.seq;
    }
    EXPECT_EQ(total, 15u);
}

TEST(FixedEventBuffer, OwnsItsStorageAndReportsCapacity) {
    ob::FixedEventBuffer<8> buf;
    EXPECT_EQ(buf.capacity(), 8u);
    buf.push(mk(1, ob::EventType::Accepted));
    EXPECT_EQ(buf.size(), 1u);
}

// Edge case E47: overflow is a programming error and must abort, not truncate.
// A truncating buffer would silently violate "every command produces at least
// one event", which is the contract the whole test suite leans on.
TEST(EventBufferDeathTest, OverflowAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::array<ob::Event, 1> storage{};
    ob::EventBuffer buf(storage.data(), storage.size());
    buf.push(mk(1, ob::EventType::Accepted));
    EXPECT_DEATH(buf.push(mk(2, ob::EventType::Filled)), "");
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_event_buffer.cpp` to `ob_tests` sources, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/engine_concept.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/engine_concept.hpp`**

Note the deliberate `assert` rather than an exception: the hot path must not throw.

```cpp
// include/ob/engine_concept.hpp
#pragma once

#include <ob/command.hpp>
#include <ob/events.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <span>

namespace ob {

// A non-owning, fixed-capacity output buffer. The engine never owns output
// memory; that is what makes "no allocation on the hot path" a true statement
// rather than an aspiration.
class EventBuffer {
public:
    EventBuffer(Event* data, std::size_t capacity) noexcept
        : data_(data), capacity_(capacity) {}

    void push(const Event& e) noexcept {
        // Overflow is a caller programming error (spec E47). Asserting keeps the
        // "at least one event per command" contract honest; truncating would not.
        assert(n_ < capacity_ && "EventBuffer overflow: caller under-sized the buffer");
        data_[n_++] = e;
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return n_ == 0; }
    void clear() noexcept { n_ = 0; }

    [[nodiscard]] const Event& operator[](std::size_t i) const noexcept {
        assert(i < n_);
        return data_[i];
    }

    [[nodiscard]] std::span<const Event> view() const noexcept { return {data_, n_}; }

    [[nodiscard]] const Event* begin() const noexcept { return data_; }
    [[nodiscard]] const Event* end() const noexcept { return data_ + n_; }

private:
    Event*      data_     = nullptr;
    std::size_t capacity_ = 0;
    std::size_t n_        = 0;
};

namespace detail {
// Holds the storage so that it is initialised BEFORE the EventBuffer base below,
// because base classes are initialised in declaration order while members are
// initialised after all bases. Passing a member's address to a base constructor
// is the classic "base-from-member" problem: legal, but -Wuninitialized flags it
// and the warning is right to. Making the storage its own base fixes the ordering
// instead of arguing with the compiler about it.
//
// Found during execution: the original form here failed the build under -Werror
// with "field 'storage_' is uninitialized when used here".
template <std::size_t N>
struct EventStorage {
    std::array<Event, N> data{};
};
}  // namespace detail

// Convenience for tests and tools: an EventBuffer that owns inline storage.
// Not used on a measured hot path.
template <std::size_t N>
class FixedEventBuffer : private detail::EventStorage<N>, public EventBuffer {
public:
    FixedEventBuffer() noexcept
        : EventBuffer(detail::EventStorage<N>::data.data(), N) {}

    FixedEventBuffer(const FixedEventBuffer&) = delete;
    FixedEventBuffer& operator=(const FixedEventBuffer&) = delete;
};

// The compile-time engine interface. A concept rather than a virtual base class:
// the benchmark must not measure vtable dispatch, and the optimizer has to be
// able to inline across this boundary. Phase 3 extends this with snapshot_l2.
template <class E>
concept Engine = requires(E e, const Command& c, EventBuffer& out) {
    { e.submit(c, out) } -> std::same_as<void>;
    { std::as_const(e).best_bid() } -> std::same_as<Ticks>;
    { std::as_const(e).best_ask() } -> std::same_as<Ticks>;
    { e.reset() } -> std::same_as<void>;
};

}  // namespace ob
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: with a Release build the death test FAILS, because `NDEBUG` deletes the `assert` it relies on. That is the correct reason, and it was confirmed during execution. Fix it by keeping assertions in the test binary; add to `tests/CMakeLists.txt`:

```cmake
# Death tests rely on assert(), which NDEBUG would delete. Release adds -DNDEBUG
# via CMAKE_CXX_FLAGS_RELEASE; target options land after it on the command line,
# so this undefines it for the test binary only.
target_compile_options(ob_tests PRIVATE -UNDEBUG)
```

Then re-run: PASS, 21 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/engine_concept.hpp tests/test_event_buffer.cpp tests/CMakeLists.txt
git commit -m "feat: non-owning EventBuffer and compile-time Engine concept"
```

---

## Task 5: ReferenceEngine validation

**Files:**
- Create: `include/ob/reference_engine.hpp`, `tests/test_reference_validation.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/engine_concept.hpp`.
- Produces: `ob::ReferenceEngine` with `explicit ReferenceEngine(std::size_t capacity = 1'000'000)`, `submit(const Command&, EventBuffer&)`, `best_bid() const -> Ticks`, `best_ask() const -> Ticks`, `reset()`, `live_order_count() const -> std::size_t`. Satisfies the `ob::Engine` concept.

This task builds only the reject paths. A `New` that passes validation emits `Accepted` and is then dropped on the floor: resting arrives in Task 6, matching in Task 7. That is a deliberate TDD increment, not an oversight, and no test here asserts anything about a surviving order.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_reference_validation.cpp
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

using ob::CancelReason;
using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

// Submits one command and returns the events it produced.
std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

TEST(RefValidation, SatisfiesTheEngineConcept) {
    static_assert(ob::Engine<ob::ReferenceEngine>);
    SUCCEED();
}

TEST(RefValidation, ValidLimitOrderIsAccepted) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].order_id, 1u);
    EXPECT_EQ(ev[0].seq, 0u);
}

// E10
TEST(RefValidation, ZeroQuantityIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

// E11
TEST(RefValidation, OversizeQuantityIsRejected) {
    ob::ReferenceEngine e;
    const auto ev =
        run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, ob::kMaxOrderQty + 1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

// E12, E13
TEST(RefValidation, PriceOutsideLadderIsRejected) {
    ob::ReferenceEngine e;
    for (const ob::Ticks bad : {ob::Ticks{0}, ob::Ticks{-1}, ob::kMinTick - 1,
                                ob::kMaxTick + 1, std::numeric_limits<ob::Ticks>::max()}) {
        const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, bad, 100));
        ASSERT_EQ(ev.size(), 1u) << "price " << bad;
        EXPECT_EQ(ev[0].reject, RejectReason::PriceOutOfRange) << "price " << bad;
    }
}

TEST(RefValidation, BothLadderBoundsAreAccepted) {
    ob::ReferenceEngine e;
    EXPECT_EQ(run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, ob::kMinTick, 1))[0].type,
              EventType::Accepted);
    EXPECT_EQ(run_one(e, ob::make_new(2, Side::Sell, OrderType::Limit, ob::kMaxTick, 1))[0].type,
              EventType::Accepted);
}

// E15: Market ignores the price field entirely; it is not validated.
TEST(RefValidation, MarketOrderPriceIsIgnoredNotValidated) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Market,
                                            std::numeric_limits<ob::Ticks>::max(), 100));
    ASSERT_GE(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
}

// E16, E17: IDs are retired permanently, so reuse is rejected whether the original
// is still live or long gone.
TEST(RefValidation, DuplicateOrderIdIsRejected) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, 10000, 100))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(7, Side::Sell, OrderType::Limit, 20000, 5));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E48: strictly increasing ids. An id at or below the high-water mark is a
// duplicate even if it was never used.
TEST(RefValidation, IdBelowHighWaterIsRejectedEvenIfNeverUsed) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(100, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(50, Side::Buy, OrderType::Limit, 10000, 10));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E49: id 0 falls out of the same rule, because the mark starts at 0.
TEST(RefValidation, OrderIdZeroIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(0, Side::Buy, OrderType::Limit, 10000, 10));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// Only an Accepted advances the mark, so a rejected id stays usable.
TEST(RefValidation, RejectedCommandDoesNotAdvanceTheHighWaterMark) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, -1, 10))[0].reject,
              RejectReason::PriceOutOfRange);
    EXPECT_EQ(run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
}

// Validation order: a command that is bad in two ways reports the FIRST failure in
// the fixed order (quantity, price, duplicate, capacity, would-cross).
TEST(RefValidation, QuantityIsCheckedBeforePrice) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, -5, 0));
    EXPECT_EQ(ev[0].reject, RejectReason::InvalidQuantity);
}

TEST(RefValidation, PriceIsCheckedBeforeDuplicateId) {
    ob::ReferenceEngine e;
    ASSERT_EQ(run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, 10000, 100))[0].type,
              EventType::Accepted);
    const auto ev = run_one(e, ob::make_new(7, Side::Buy, OrderType::Limit, -5, 100));
    EXPECT_EQ(ev[0].reject, RejectReason::PriceOutOfRange);
}

// E18: cancel of an ID that never existed.
TEST(RefValidation, CancelOfUnknownIdIsRejected) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_cancel(999));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

TEST(RefValidation, EveryCommandProducesAtLeastOneEvent) {
    ob::ReferenceEngine e;
    for (const ob::Command c : {ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100),
                                ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0),
                                ob::make_cancel(12345),
                                ob::make_new(2, Side::Sell, OrderType::Market, 0, 50)}) {
        EXPECT_GE(run_one(e, c).size(), 1u);
    }
}

TEST(RefValidation, SequenceNumbersAreMonotonicAndGapFree) {
    ob::ReferenceEngine e;
    ob::Seq expected = 0;
    for (ob::OrderId id = 1; id <= 10; ++id) {
        for (const ob::Event& ev :
             run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))) {
            EXPECT_EQ(ev.seq, expected++);
        }
    }
}

TEST(RefValidation, EmptyBookReportsNoPriceOnBothSides) {
    ob::ReferenceEngine e;
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

TEST(RefValidation, RejectedCommandLeavesTheBookUnchanged) {
    ob::ReferenceEngine e;
    const std::size_t before = e.live_order_count();
    run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 0));
    EXPECT_EQ(e.live_order_count(), before);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_reference_validation.cpp` to `ob_tests` sources, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/reference_engine.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/reference_engine.hpp`**

```cpp
// include/ob/reference_engine.hpp
#pragma once

// The correctness oracle. This engine is DELIBERATELY SIMPLE and is NEVER
// OPTIMIZED. std::map, std::list and std::set are used on purpose: the value of
// this file is that a reader can confirm it is right by reading it. Every
// optimization in Phase 2 is validated by differential testing against this.
//
// Do not "improve" the performance of anything in this file.

#include <ob/engine_concept.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <map>

namespace ob {

class ReferenceEngine {
public:
    // This engine can afford an arrival sequence per order, so the invariant
    // checker's FIFO check is enabled for it. FastEngine sets this to false.
    static constexpr bool kTracksArrival = true;

    explicit ReferenceEngine(std::size_t capacity = 1'000'000) : capacity_(capacity) {}

    void submit(const Command& c, EventBuffer& out) {
        if (c.type == CommandType::Cancel) {
            submit_cancel(c, out);
        } else {
            submit_new(c, out);
        }
    }

    [[nodiscard]] Ticks best_bid() const noexcept {
        return bids_.empty() ? kNoPrice : bids_.begin()->first;
    }
    [[nodiscard]] Ticks best_ask() const noexcept {
        return asks_.empty() ? kNoPrice : asks_.begin()->first;
    }
    [[nodiscard]] std::size_t live_order_count() const noexcept { return live_.size(); }

    void reset() {
        bids_.clear();
        asks_.clear();
        live_.clear();
        high_water_ = 0;
        seq_ = 0;
    }

private:
    struct RefOrder {
        OrderId id;
        Qty     remaining;
    };
    using Level = std::list<RefOrder>;

    // Comparators chosen so begin() is always the best price on that side.
    using BidBook = std::map<Ticks, Level, std::greater<Ticks>>;
    using AskBook = std::map<Ticks, Level, std::less<Ticks>>;

    struct Loc {
        Side  side;
        Ticks price;
    };

    [[nodiscard]] Seq next_seq() noexcept { return seq_++; }

    Event base(EventType t, OrderId id) {
        Event e{};
        e.seq = next_seq();
        e.type = t;
        e.order_id = id;
        return e;
    }

    void emit_rejected(EventBuffer& out, OrderId id, RejectReason r) {
        Event e = base(EventType::Rejected, id);
        e.reject = r;
        out.push(e);
    }

    // Validation. Order is FIXED so reason codes are deterministic:
    // quantity, price range, duplicate id, capacity, post-only would-cross.
    // Returns None when the command is acceptable.
    [[nodiscard]] RejectReason validate_new(const Command& c) const {
        if (!qty_valid(c.qty)) {
            return RejectReason::InvalidQuantity;
        }
        // A Market order's price field is ignored, not validated (E15).
        if (c.order_type != OrderType::Market && !price_in_range(c.price)) {
            return RejectReason::PriceOutOfRange;
        }
        // Order IDs must be strictly increasing (spec E48). This single comparison
        // replaces an unbounded set of retired ids, which FastEngine could not hold
        // without allocating. ID 0 is rejected automatically because the mark
        // starts at 0 (spec E49).
        if (c.id <= high_water_) {
            return RejectReason::DuplicateOrderId;
        }
        // Capacity is checked only for types that can rest, conservatively, before
        // it is known whether the order would have fully filled. Both engines must
        // apply this identical rule or differential testing diverges on a full book.
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        if (can_rest && live_.size() >= capacity_) {
            return RejectReason::EngineCapacity;
        }
        if (c.order_type == OrderType::PostOnly && would_cross(c)) {
            return RejectReason::WouldCross;
        }
        return RejectReason::None;
    }

    [[nodiscard]] bool would_cross(const Command& c) const {
        const Ticks opp = (c.side == Side::Buy) ? best_ask() : best_bid();
        return opp != kNoPrice && crosses(c.side, c.price, opp);
    }

    void submit_new(const Command& c, EventBuffer& out) {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark
        // Resting arrives in Task 6, matching in Task 7.
    }

    void submit_cancel(const Command& c, EventBuffer& out) {
        const auto it = live_.find(c.id);
        if (it == live_.end()) {
            emit_rejected(out, c.id, RejectReason::UnknownOrderId);
            return;
        }
        // Removal arrives in Task 8.
        emit_rejected(out, c.id, RejectReason::UnknownOrderId);
    }

    BidBook bids_;
    AskBook asks_;
    std::map<OrderId, Loc> live_;  // live orders only
    OrderId high_water_ = 0;       // highest accepted id; ids must strictly increase
    std::size_t capacity_;
    Seq seq_ = 0;
};

static_assert(Engine<ReferenceEngine>);

}  // namespace ob
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 39 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/reference_engine.hpp tests/test_reference_validation.cpp tests/CMakeLists.txt
git commit -m "feat: ReferenceEngine validation with fixed reason-code ordering"
```

---

## Task 6: ReferenceEngine resting and best prices

**Files:**
- Modify: `include/ob/reference_engine.hpp`
- Create: `tests/test_reference_resting.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Task 5.
- Produces: no new public API. `submit_new` now rests non-crossing limit orders, so `best_bid()` and `best_ask()` become meaningful, and `live_order_count()` grows.

Covers E1, E7, E8, E31, E39.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_reference_resting.cpp
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<256> buf;
    e.submit(c, buf);
}

// E1
TEST(RefResting, LimitIntoEmptyBookRestsAndBecomesBest) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 1u);
}

TEST(RefResting, BetterBidReplacesBest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10010, 100));
    feed(e, ob::make_new(3, Side::Buy, OrderType::Limit, 9990, 100));
    EXPECT_EQ(e.best_bid(), 10010);
}

TEST(RefResting, BetterAskIsTheLowerPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10100, 100));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10200, 100));
    EXPECT_EQ(e.best_ask(), 10050);
}

// E7, E8
TEST(RefResting, OrdersRestAtBothLadderExtremes) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, ob::kMinTick, 1));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, ob::kMaxTick, 1));
    EXPECT_EQ(e.best_bid(), ob::kMinTick);
    EXPECT_EQ(e.best_ask(), ob::kMaxTick);
}

// E31: one tick apart does not cross. The spread is minimal but uncrossed.
TEST(RefResting, OneTickApartDoesNotCross) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10001, 100));
    const auto ev = run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(ev.size(), 1u) << "should rest, not trade";
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.best_ask(), 10001);
    EXPECT_LT(e.best_bid(), e.best_ask());
}

TEST(RefResting, ManyOrdersAtOnePriceAllRest) {
    ob::ReferenceEngine e;
    for (ob::OrderId id = 1; id <= 50; ++id) {
        feed(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10));
    }
    EXPECT_EQ(e.best_bid(), 10000);
    EXPECT_EQ(e.live_order_count(), 50u);
}

// E39: capacity is a rejection, never a crash, and the engine stays usable.
TEST(RefResting, CapacityExhaustionRejectsAndTheEngineStaysUsable) {
    ob::ReferenceEngine e(3);
    for (ob::OrderId id = 1; id <= 3; ++id) {
        ASSERT_EQ(run_one(e, ob::make_new(id, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
                  EventType::Accepted);
    }
    const auto rejected = run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].reject, RejectReason::EngineCapacity);
    EXPECT_EQ(e.live_order_count(), 3u);
    EXPECT_EQ(e.best_bid(), 10000);
}

TEST(RefResting, ResetEmptiesEverythingIncludingRetiredIds) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    e.reset();
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
    // After reset, id 1 is reusable and the sequence restarts at 0.
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].seq, 0u);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_reference_resting.cpp` to `ob_tests`, then:

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -30
```

Expected: FAIL. `LimitIntoEmptyBookRestsAndBecomesBest` reports `best_bid()` as `kNoPrice` because nothing rests yet.

- [ ] **Step 3: Add the resting path to `ReferenceEngine`**

Add this private method:

```cpp
    void rest(const Command& c, Qty remaining) {
        if (c.side == Side::Buy) {
            bids_[c.price].push_back(RefOrder{c.id, remaining});
        } else {
            asks_[c.price].push_back(RefOrder{c.id, remaining});
        }
        live_[c.id] = Loc{c.side, c.price};
    }
```

Then replace the body of `submit_new` with:

```cpp
    void submit_new(const Command& c, EventBuffer& out) {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        // Matching arrives in Task 7. For now every accepted order that can rest
        // does so at its full quantity.
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        if (can_rest) {
            rest(c, c.qty);
        }
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 47 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/reference_engine.hpp tests/test_reference_resting.cpp tests/CMakeLists.txt
git commit -m "feat: ReferenceEngine rests limit orders and reports best prices"
```

---

## Task 7: ReferenceEngine matching

**Files:**
- Modify: `include/ob/reference_engine.hpp`
- Create: `tests/test_reference_matching.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 6.
- Produces: no new public API. `submit` now emits `Trade` and `Filled` events and maintains an uncrossed book.

Covers E26, E27, E28, E29, E30, E32, E38, and the maker-price rule.

- [ ] **Step 1: Write the failing test**

This is the worked example from the spec, turned into a test. The `maker_id` and `price` assertions are the ones that catch subtly wrong FIFO order.

```cpp
// tests/test_reference_matching.cpp
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

using ob::EventType;
using ob::OrderType;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

// Builds the book from the spec's worked example:
//   asks: 100.50 -> [300 (id 2)][100 (id 3)],  101.00 -> [200 (id 4)]
void build_example_book(ob::ReferenceEngine& e) {
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10050, 300));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10050, 100));
    feed(e, ob::make_new(4, Side::Sell, OrderType::Limit, 10100, 200));
}

// The spec's worked example, verbatim.
TEST(RefMatching, WorkedExampleFromTheSpec) {
    ob::ReferenceEngine e;
    build_example_book(e);

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 350));

    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[0].order_id, 99u);

    EXPECT_EQ(ev[1].type, EventType::Trade);
    EXPECT_EQ(ev[1].order_id, 99u);
    EXPECT_EQ(ev[1].maker_id, 2u);      // id 2 arrived first, so it fills first
    EXPECT_EQ(ev[1].price, 10050);      // the MAKER's price
    EXPECT_EQ(ev[1].qty, 300u);

    EXPECT_EQ(ev[2].type, EventType::Filled);
    EXPECT_EQ(ev[2].order_id, 2u);      // maker fully consumed

    EXPECT_EQ(ev[3].type, EventType::Trade);
    EXPECT_EQ(ev[3].maker_id, 3u);
    EXPECT_EQ(ev[3].qty, 50u);

    EXPECT_EQ(ev[4].type, EventType::Filled);
    EXPECT_EQ(ev[4].order_id, 99u);     // taker fully filled

    // 101.00 was never touched; id 3 keeps 50 and its time priority.
    EXPECT_EQ(e.best_ask(), 10050);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

// E32: the same order, larger, sweeps the level and rests the remainder.
TEST(RefMatching, LargerOrderSweepsThenRestsRemainder) {
    ob::ReferenceEngine e;
    build_example_book(e);

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10050, 1000));

    // Accepted, Trade(2)+Filled(2), Trade(3)+Filled(3) = 5 events; no Filled(99).
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[4].type, EventType::Filled);
    EXPECT_EQ(ev[4].order_id, 3u);

    EXPECT_EQ(e.best_bid(), 10050);   // 600 remainder rested
    EXPECT_EQ(e.best_ask(), 10100);   // 101.00 untouched
    EXPECT_LT(e.best_bid(), e.best_ask());
}

// E30: crossing is inclusive of equality.
TEST(RefMatching, EqualPriceCrosses) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    const auto ev = run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev[1].type, EventType::Trade);
    EXPECT_EQ(ev[1].price, 10000);
}

// E26
TEST(RefMatching, ExactQuantityMatchLeavesNothing) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
}

// E27: a partially filled resting order keeps its place at the front of the queue.
TEST(RefMatching, PartiallyFilledMakerKeepsTimePriority) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));  // first
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10000, 100));  // second
    run_one(e, ob::make_new(3, Side::Buy, OrderType::Limit, 10000, 30)); // takes 30 of id 1

    // id 1 has 70 left and must still fill before id 2.
    const auto ev = run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 70));
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev[1].maker_id, 1u);
}

// E28
TEST(RefMatching, FifoOrderWithinAPriceLevel) {
    ob::ReferenceEngine e;
    for (ob::OrderId id = 1; id <= 4; ++id) {
        feed(e, ob::make_new(id, Side::Sell, OrderType::Limit, 10000, 10));
    }
    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10000, 40));

    std::vector<ob::OrderId> makers;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            makers.push_back(x.maker_id);
        }
    }
    EXPECT_EQ(makers, (std::vector<ob::OrderId>{1, 2, 3, 4}));
}

// E29: levels are consumed best-price-first, and each trade prints at its own
// maker's price, not at one blended price.
TEST(RefMatching, SweepsLevelsInPriceOrderAtEachMakersPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10020, 10));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 10010, 10));

    const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10020, 30));

    std::vector<ob::Ticks> prices;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            prices.push_back(x.price);
        }
    }
    EXPECT_EQ(prices, (std::vector<ob::Ticks>{10000, 10010, 10020}));
}

// The book must never be observably crossed.
TEST(RefMatching, BookIsNeverCrossedAfterAnyOperation) {
    ob::ReferenceEngine e;
    ob::OrderId id = 1;
    for (int i = 0; i < 200; ++i) {
        const Side s = (i % 2 == 0) ? Side::Buy : Side::Sell;
        const ob::Ticks px = 10000 + static_cast<ob::Ticks>(i % 7) - 3;
        feed(e, ob::make_new(id++, s, OrderType::Limit, px, 10));
        if (e.best_bid() != ob::kNoPrice && e.best_ask() != ob::kNoPrice) {
            ASSERT_LT(e.best_bid(), e.best_ask()) << "crossed at i=" << i;
        }
    }
}

// E4, E5: emptying a side must produce the sentinel, not a stale or zero price.
TEST(RefMatching, EmptyingASideYieldsTheNoPriceSentinel) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    ASSERT_EQ(e.best_ask(), 10000);
    run_one(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -30
```

Expected: FAIL. `WorkedExampleFromTheSpec` gets 1 event (`Accepted`) instead of 5, because matching does not exist yet.

- [ ] **Step 3: Add matching to `ReferenceEngine`**

Add these private members. `match_into` is a template because the two books have different comparator types.

```cpp
    Event trade_event(OrderId taker, OrderId maker, Ticks px, Qty qty) {
        Event e = base(EventType::Trade, taker);
        e.maker_id = maker;
        e.price = px;
        e.qty = qty;
        return e;
    }

    // Matches `c` against `book` (the opposite side), consuming `remaining`.
    // Emits Trade per fill, and Filled for each maker it fully consumes.
    template <class BookMap>
    void match_into(BookMap& book, const Command& c, Qty& remaining, EventBuffer& out) {
        while (remaining > 0 && !book.empty()) {
            const auto lit = book.begin();  // best price on this side
            const Ticks level_px = lit->first;

            // A Market order ignores price entirely; everything else must cross.
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, level_px)) {
                break;
            }

            Level& level = lit->second;
            while (remaining > 0 && !level.empty()) {
                RefOrder& maker = level.front();  // FIFO: oldest fills first
                const Qty fill = std::min(remaining, maker.remaining);

                remaining -= fill;
                maker.remaining -= fill;
                out.push(trade_event(c.id, maker.id, level_px, fill));

                if (maker.remaining == 0) {
                    out.push(base(EventType::Filled, maker.id));
                    live_.erase(maker.id);
                    level.pop_front();
                }
            }
            if (level.empty()) {
                book.erase(lit);
            }
        }
    }
```

Then replace `submit_new` with:

```cpp
    void submit_new(const Command& c, EventBuffer& out) {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        Qty remaining = c.qty;

        // PostOnly never matches: validation already rejected it if it would cross.
        if (c.order_type != OrderType::PostOnly) {
            if (c.side == Side::Buy) {
                match_into(asks_, c, remaining, out);
            } else {
                match_into(bids_, c, remaining, out);
            }
        }

        if (remaining == 0) {
            out.push(base(EventType::Filled, c.id));
            return;
        }

        // Market/Ioc/Fok remainders are handled in Task 9. For now only the
        // resting types have a defined remainder behavior.
        const bool can_rest =
            c.order_type == OrderType::Limit || c.order_type == OrderType::PostOnly;
        if (can_rest) {
            rest(c, remaining);
        }
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 56 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/reference_engine.hpp tests/test_reference_matching.cpp tests/CMakeLists.txt
git commit -m "feat: ReferenceEngine price-time priority matching at the maker price"
```

---

## Task 8: ReferenceEngine cancel

**Files:**
- Modify: `include/ob/reference_engine.hpp`
- Create: `tests/test_reference_cancel.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 7.
- Produces: no new public API. `Cancel` now removes the order and emits `Cancelled(UserRequested, qty=removed)`.

Covers E19, E20, E21, E22, E23, E24, E25, and E39's "freeing capacity makes it available again".

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_reference_cancel.cpp
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

using ob::CancelReason;
using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

TEST(RefCancel, CancelRemovesTheOrderAndReportsTheQuantityRemoved) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Cancelled);
    EXPECT_EQ(ev[0].order_id, 1u);
    EXPECT_EQ(ev[0].cancel, CancelReason::UserRequested);
    EXPECT_EQ(ev[0].qty, 100u);

    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
    EXPECT_EQ(e.live_order_count(), 0u);
}

// E21: only the remaining quantity is removed; prior fills stand.
TEST(RefCancel, CancelOfAPartiallyFilledOrderRemovesOnlyTheRemainder) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 40));  // fills 40 of id 1

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].qty, 60u);
}

// E19
TEST(RefCancel, CancelOfAFullyFilledOrderIsUnknown) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_cancel(1));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

// E20: double cancel is an error, deliberately not idempotent.
TEST(RefCancel, DoubleCancelIsRejected) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    ASSERT_EQ(run_one(e, ob::make_cancel(1))[0].type, EventType::Cancelled);

    const auto ev = run_one(e, ob::make_cancel(1));
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::UnknownOrderId);
}

// E17: a cancelled id is retired and cannot be reused.
TEST(RefCancel, CancelledIdCannotBeReused) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    feed(e, ob::make_cancel(1));

    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    EXPECT_EQ(ev[0].reject, RejectReason::DuplicateOrderId);
}

// E22, E23, E25: cancel from head, tail and middle of a level, and confirm the
// surviving FIFO order by draining the level afterwards.
TEST(RefCancel, CancelFromHeadMiddleAndTailPreservesRemainingFifoOrder) {
    for (const ob::OrderId victim : {ob::OrderId{1}, ob::OrderId{2}, ob::OrderId{3}}) {
        ob::ReferenceEngine e;
        for (ob::OrderId id = 1; id <= 3; ++id) {
            feed(e, ob::make_new(id, Side::Sell, OrderType::Limit, 10000, 10));
        }
        ASSERT_EQ(run_one(e, ob::make_cancel(victim))[0].type, EventType::Cancelled)
            << "victim " << victim;

        const auto ev = run_one(e, ob::make_new(99, Side::Buy, OrderType::Limit, 10000, 20));
        std::vector<ob::OrderId> makers;
        for (const ob::Event& x : ev) {
            if (x.type == EventType::Trade) {
                makers.push_back(x.maker_id);
            }
        }
        std::vector<ob::OrderId> expected;
        for (ob::OrderId id = 1; id <= 3; ++id) {
            if (id != victim) {
                expected.push_back(id);
            }
        }
        EXPECT_EQ(makers, expected) << "victim " << victim;
    }
}

// E24: cancelling the only order at a level removes the level and moves the best.
TEST(RefCancel, CancellingTheOnlyOrderAtTheBestLevelMovesTheBest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10010, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(e.best_bid(), 10010);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(e.best_bid(), 10000);
}

// E39 continued: freeing an order makes capacity available again.
TEST(RefCancel, CancelFreesCapacity) {
    ob::ReferenceEngine e(2);
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Buy, OrderType::Limit, 10000, 10));
    ASSERT_EQ(run_one(e, ob::make_new(3, Side::Buy, OrderType::Limit, 10000, 10))[0].reject,
              RejectReason::EngineCapacity);

    feed(e, ob::make_cancel(1));
    EXPECT_EQ(run_one(e, ob::make_new(4, Side::Buy, OrderType::Limit, 10000, 10))[0].type,
              EventType::Accepted);
}

TEST(RefCancel, RejectedCancelLeavesTheBookBitForBitUnchanged) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Buy, OrderType::Limit, 10000, 100));
    const ob::Ticks bid = e.best_bid();
    const std::size_t n = e.live_order_count();

    run_one(e, ob::make_cancel(4242));
    EXPECT_EQ(e.best_bid(), bid);
    EXPECT_EQ(e.live_order_count(), n);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -30
```

Expected: FAIL. `CancelRemovesTheOrder...` gets `Rejected(UnknownOrderId)` because `submit_cancel` is still the Task 5 stub.

- [ ] **Step 3: Implement cancel**

Add this private helper:

```cpp
    // Removes `id` from `book` at `px`. Returns the quantity that was removed.
    template <class BookMap>
    Qty erase_order(BookMap& book, Ticks px, OrderId id) {
        const auto lit = book.find(px);
        assert(lit != book.end() && "live_ pointed at a level that does not exist");
        Level& level = lit->second;

        const auto oit = std::find_if(level.begin(), level.end(),
                                      [id](const RefOrder& o) { return o.id == id; });
        assert(oit != level.end() && "live_ pointed at an order that is not in its level");

        const Qty removed = oit->remaining;
        level.erase(oit);
        if (level.empty()) {
            book.erase(lit);
        }
        return removed;
    }
```

Then replace `submit_cancel` with:

```cpp
    void submit_cancel(const Command& c, EventBuffer& out) {
        const auto it = live_.find(c.id);
        if (it == live_.end()) {
            // Covers an id that never existed (E18), one already fully filled
            // (E19) and one already cancelled (E20). Deliberately not idempotent.
            emit_rejected(out, c.id, RejectReason::UnknownOrderId);
            return;
        }
        const Loc loc = it->second;
        const Qty removed = (loc.side == Side::Buy) ? erase_order(bids_, loc.price, c.id)
                                                    : erase_order(asks_, loc.price, c.id);
        live_.erase(it);

        Event e = base(EventType::Cancelled, c.id);
        e.cancel = CancelReason::UserRequested;
        e.qty = removed;
        e.price = loc.price;
        out.push(e);
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 65 tests total.

- [ ] **Step 5: Commit**

```bash
git add include/ob/reference_engine.hpp tests/test_reference_cancel.cpp tests/CMakeLists.txt
git commit -m "feat: ReferenceEngine cancel with permanent id retirement"
```

---

## Task 9: Market, IOC, FOK and PostOnly

**Files:**
- Modify: `include/ob/reference_engine.hpp`
- Create: `tests/test_reference_order_types.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 8.
- Produces: no new public API. All five `OrderType` values are now fully implemented.

Covers E2, E3, E33, E34, E35, E36, E37. **E36 is the highest-risk case in the plan:** a `Fok` that cannot fill must leave the book bit-for-bit unchanged, which is why `fillable_qty` is `const` and runs before any mutation.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_reference_order_types.cpp
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

using ob::CancelReason;
using ob::EventType;
using ob::OrderType;
using ob::RejectReason;
using ob::Side;

std::vector<ob::Event> run_one(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
    return {buf.begin(), buf.end()};
}

void feed(ob::ReferenceEngine& e, const ob::Command& c) {
    ob::FixedEventBuffer<4096> buf;
    e.submit(c, buf);
}

std::size_t count_trades(const std::vector<ob::Event>& ev) {
    std::size_t n = 0;
    for (const ob::Event& x : ev) {
        if (x.type == EventType::Trade) {
            ++n;
        }
    }
    return n;
}

// ---- Market ----------------------------------------------------------------

// E2: a market order into an empty book is a cancel, not an error and not a crash.
TEST(RefOrderTypes, MarketIntoEmptyBookCancelsWithNoLiquidity) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Market, ob::kNoPrice, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(ev[1].type, EventType::Cancelled);
    EXPECT_EQ(ev[1].cancel, CancelReason::NoLiquidity);
    EXPECT_EQ(ev[1].qty, 100u);
    EXPECT_EQ(e.live_order_count(), 0u);
}

TEST(RefOrderTypes, MarketIgnoresPriceAndSweepsEveryLevel) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 20000, 10));
    feed(e, ob::make_new(3, Side::Sell, OrderType::Limit, 30000, 10));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Market, ob::kNoPrice, 30));
    EXPECT_EQ(count_trades(ev), 3u);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

TEST(RefOrderTypes, MarketNeverRestsAndReportsIocRemainderAfterAPartialFill) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 10));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Market, ob::kNoPrice, 50));
    ASSERT_EQ(ev.back().type, EventType::Cancelled);
    EXPECT_EQ(ev.back().cancel, CancelReason::IocRemainder);
    EXPECT_EQ(ev.back().qty, 40u);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice) << "a market order must never rest";
}

// ---- IOC -------------------------------------------------------------------

// E37
TEST(RefOrderTypes, IocFillsWhatItCanAndCancelsTheRest) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 30));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Ioc, 10000, 100));
    EXPECT_EQ(count_trades(ev), 1u);
    EXPECT_EQ(ev.back().type, EventType::Cancelled);
    EXPECT_EQ(ev.back().cancel, CancelReason::IocRemainder);
    EXPECT_EQ(ev.back().qty, 70u);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

TEST(RefOrderTypes, IocRespectsItsLimitPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10010, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Ioc, 10000, 100));
    EXPECT_EQ(count_trades(ev), 0u);
    EXPECT_EQ(ev.back().cancel, CancelReason::NoLiquidity);
    EXPECT_EQ(e.best_ask(), 10010) << "the resting ask must be untouched";
}

// ---- FOK -------------------------------------------------------------------

// E3
TEST(RefOrderTypes, FokIntoEmptyBookIsUnfillable) {
    ob::ReferenceEngine e;
    const auto ev = run_one(e, ob::make_new(1, Side::Buy, OrderType::Fok, 10000, 100));
    ASSERT_EQ(ev.size(), 2u);
    EXPECT_EQ(ev[1].type, EventType::Cancelled);
    EXPECT_EQ(ev[1].cancel, CancelReason::Unfillable);
}

// E35
TEST(RefOrderTypes, FokFillsWhenExactlyFillable) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 40));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    EXPECT_EQ(count_trades(ev), 2u);
    EXPECT_EQ(ev.back().type, EventType::Filled);
    EXPECT_EQ(ev.back().order_id, 9u);
    EXPECT_EQ(e.best_ask(), ob::kNoPrice);
}

// E36: the highest-risk case. One unit short means ZERO mutation.
TEST(RefOrderTypes, FokOneUnitShortMutatesNothing) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 60));
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 10010, 39));  // 99 available

    const ob::Ticks ask_before = e.best_ask();
    const std::size_t live_before = e.live_order_count();

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10010, 100));
    EXPECT_EQ(count_trades(ev), 0u) << "a partial fill here would be the worst bug possible";
    EXPECT_EQ(ev.back().cancel, CancelReason::Unfillable);
    EXPECT_EQ(e.best_ask(), ask_before);
    EXPECT_EQ(e.live_order_count(), live_before);

    // And the liquidity is still fully there afterwards.
    const auto after = run_one(e, ob::make_new(10, Side::Buy, OrderType::Limit, 10010, 99));
    EXPECT_EQ(count_trades(after), 2u);
}

TEST(RefOrderTypes, FokIgnoresLiquidityBeyondItsLimitPrice) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 50));
    // 20000 is inside the ladder but outside the Fok's limit price. Using a price
    // beyond kMaxTick here would be REJECTED outright, and the test would then
    // pass for the wrong reason.
    feed(e, ob::make_new(2, Side::Sell, OrderType::Limit, 20000, 50));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Fok, 10000, 100));
    EXPECT_EQ(count_trades(ev), 0u);
    EXPECT_EQ(ev.back().cancel, CancelReason::Unfillable);
}

// ---- PostOnly --------------------------------------------------------------

// E33: a would-cross PostOnly is Rejected, so Rejected is its ONLY event.
TEST(RefOrderTypes, PostOnlyThatWouldCrossIsRejectedWithNoAccepted) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Rejected);
    EXPECT_EQ(ev[0].reject, RejectReason::WouldCross);
    EXPECT_EQ(e.best_ask(), 10000);
    EXPECT_EQ(e.best_bid(), ob::kNoPrice);
}

// E34
TEST(RefOrderTypes, PostOnlyThatWouldNotCrossRestsNormally) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10010, 100));

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100));
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, EventType::Accepted);
    EXPECT_EQ(e.best_bid(), 10000);
}

// A rejected PostOnly still retires nothing: its id stays reusable, because the
// command never got an Accepted.
TEST(RefOrderTypes, RejectedPostOnlyDoesNotRetireItsId) {
    ob::ReferenceEngine e;
    feed(e, ob::make_new(1, Side::Sell, OrderType::Limit, 10000, 100));
    ASSERT_EQ(run_one(e, ob::make_new(9, Side::Buy, OrderType::PostOnly, 10000, 100))[0].reject,
              RejectReason::WouldCross);

    const auto ev = run_one(e, ob::make_new(9, Side::Buy, OrderType::Limit, 9000, 100));
    EXPECT_EQ(ev[0].type, EventType::Accepted);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

```bash
cmake --build build && ctest --test-dir build --output-on-failure 2>&1 | tail -40
```

Expected: FAIL on the Market/Ioc/Fok tests. A market order into an empty book currently produces only `Accepted` (1 event), because the remainder branch does not exist.

- [ ] **Step 3: Implement the remaining order types**

Add the non-mutating pre-scan. `const` is load-bearing here: it is what makes E36 structurally impossible to get wrong.

```cpp
    // Non-mutating. Returns the quantity `c` could fill right now, stopping early
    // once it reaches c.qty. Being const is what makes the Fok "mutate nothing"
    // guarantee (E36) structural rather than a matter of care.
    template <class BookMap>
    [[nodiscard]] QtySum fillable_qty(const BookMap& book, const Command& c) const {
        QtySum total = 0;
        for (const auto& [px, level] : book) {
            if (c.order_type != OrderType::Market && !crosses(c.side, c.price, px)) {
                break;
            }
            for (const RefOrder& o : level) {
                total += o.remaining;
                if (total >= c.qty) {
                    return total;
                }
            }
        }
        return total;
    }

    [[nodiscard]] bool fok_is_fillable(const Command& c) const {
        const QtySum available = (c.side == Side::Buy) ? fillable_qty(asks_, c)
                                                       : fillable_qty(bids_, c);
        return available >= c.qty;
    }
```

Then replace `submit_new` with its final form:

```cpp
    void submit_new(const Command& c, EventBuffer& out) {
        const RejectReason r = validate_new(c);
        if (r != RejectReason::None) {
            emit_rejected(out, c.id, r);
            return;
        }
        out.push(base(EventType::Accepted, c.id));
        high_water_ = c.id;  // only an Accepted advances the mark

        // Fok decides before touching anything (E36).
        if (c.order_type == OrderType::Fok && !fok_is_fillable(c)) {
            Event e = base(EventType::Cancelled, c.id);
            e.cancel = CancelReason::Unfillable;
            e.qty = c.qty;
            out.push(e);
            return;
        }

        Qty remaining = c.qty;

        // PostOnly never matches: validation rejected it already if it would cross.
        if (c.order_type != OrderType::PostOnly) {
            if (c.side == Side::Buy) {
                match_into(asks_, c, remaining, out);
            } else {
                match_into(bids_, c, remaining, out);
            }
        }

        if (remaining == 0) {
            out.push(base(EventType::Filled, c.id));
            return;
        }

        switch (c.order_type) {
            case OrderType::Limit:
            case OrderType::PostOnly:
                // Rests. Accepted already conveyed this, so no terminal event.
                rest(c, remaining);
                return;

            case OrderType::Market:
            case OrderType::Ioc: {
                Event e = base(EventType::Cancelled, c.id);
                // NoLiquidity when nothing filled at all, IocRemainder otherwise.
                e.cancel = (remaining == c.qty) ? CancelReason::NoLiquidity
                                                : CancelReason::IocRemainder;
                e.qty = remaining;
                out.push(e);
                return;
            }

            case OrderType::Fok:
                // Unreachable: the pre-scan guarantees a Fok that gets here fills
                // completely, so `remaining` is 0 and we returned above.
                assert(false && "Fok reached the remainder branch: pre-scan disagreed "
                                "with the match loop");
                return;
        }
    }
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, 77 tests total.

- [ ] **Step 5: Verify the ReferenceEngine is feature-complete**

```bash
grep -c "OrderType::" include/ob/reference_engine.hpp
grep -n "TODO\|FIXME\|Task [0-9]" include/ob/reference_engine.hpp
```

Expected: the second command prints nothing. Every "arrives in Task N" comment from the earlier increments must now be gone. If any remain, the file is lying about its own completeness.

- [ ] **Step 6: Commit**

```bash
git add include/ob/reference_engine.hpp tests/test_reference_order_types.cpp tests/CMakeLists.txt
git commit -m "feat: Market, IOC, FOK and PostOnly with a non-mutating FOK pre-scan"
```

---

## Task 10: The data-driven edge-case suite

**Files:**
- Create: `tests/cases/edge_cases.hpp`, `tests/test_edge_cases.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/reference_engine.hpp`, `ob/engine_concept.hpp`.
- Produces: `obtest::Expect`, `obtest::Case`, `obtest::all_edge_cases() -> std::vector<Case>`, and the builders `obtest::buy`, `obtest::sell`, `obtest::cxl`, `obtest::acc`, `obtest::rej`, `obtest::trd`, `obtest::fil`, `obtest::can`. The test itself is a `TYPED_TEST_SUITE` over `EngineTypes`.

Why data-driven rather than 40 hand-written tests: **the table is the specification.** Each row names its spec edge case, its setup, its subject command and its exact expected event sequence, so a reviewer can diff the table against spec section 5.3 line by line. And `EngineTypes` is a one-line change in Phase 2 to run all of it against `FastEngine`.

A case asserts events from the **subject command only**. Setup events are ignored, which is what keeps each row to one readable line group.

- [ ] **Step 1: Write the case table**

```cpp
// tests/cases/edge_cases.hpp
#pragma once

// The edge-case table IS the specification. Every row cites the spec edge case it
// covers (docs/superpowers/specs/...-design.md section 5.3). Keep them in sync.

#include <ob/command.hpp>
#include <ob/events.hpp>

#include <optional>
#include <vector>

namespace obtest {

using namespace ob;  // NOLINT(google-build-using-namespace) - test-local convenience

// Expected event, without `seq`. Sequence monotonicity is asserted separately so
// that inserting a case does not renumber every later expectation.
struct Expect {
    EventType    type{};
    OrderId      id = 0;
    OrderId      maker = 0;
    Ticks        price = kNoPrice;
    Qty          qty = 0;
    RejectReason reject = RejectReason::None;
    CancelReason cancel = CancelReason::None;
};

struct Case {
    const char*          name;
    std::vector<Command> setup;    // events ignored
    Command              subject;  // the command under test
    std::vector<Expect>  expect;   // exactly the events `subject` must produce
    std::optional<Ticks> bid;      // expected best_bid() afterwards, if checked
    std::optional<Ticks> ask;      // expected best_ask() afterwards, if checked
};

// ---- command builders ----
inline Command buy(OrderId id, Ticks px, Qty q, OrderType t = OrderType::Limit) {
    return make_new(id, Side::Buy, t, px, q);
}
inline Command sell(OrderId id, Ticks px, Qty q, OrderType t = OrderType::Limit) {
    return make_new(id, Side::Sell, t, px, q);
}
inline Command cxl(OrderId id) { return make_cancel(id); }

// ---- expectation builders ----
inline Expect acc(OrderId id) { return {EventType::Accepted, id}; }
inline Expect fil(OrderId id) { return {EventType::Filled, id}; }
inline Expect rej(OrderId id, RejectReason r) {
    Expect e{EventType::Rejected, id};
    e.reject = r;
    return e;
}
inline Expect trd(OrderId taker, OrderId maker, Ticks px, Qty q) {
    Expect e{EventType::Trade, taker};
    e.maker = maker;
    e.price = px;
    e.qty = q;
    return e;
}
inline Expect can(OrderId id, CancelReason r, Qty q) {
    Expect e{EventType::Cancelled, id};
    e.cancel = r;
    e.qty = q;
    return e;
}

inline constexpr Ticks NONE = kNoPrice;

// The example book used by several cases:
//   asks  10050 -> [300 (id 1)][100 (id 2)],  10100 -> [200 (id 3)]
inline std::vector<Command> example_book() {
    return {sell(1, 10050, 300), sell(2, 10050, 100), sell(3, 10100, 200)};
}

inline std::vector<Case> all_edge_cases() {
    return {
        // ---- empty and boundary book states ----
        {"E1_limit_into_empty_book_rests",
         {}, buy(1, 10000, 100), {acc(1)}, 10000, NONE},

        {"E2_market_into_empty_book_cancels_no_liquidity",
         {}, buy(1, NONE, 100, OrderType::Market),
         {acc(1), can(1, CancelReason::NoLiquidity, 100)}, NONE, NONE},

        {"E3_fok_into_empty_book_unfillable",
         {}, buy(1, 10000, 100, OrderType::Fok),
         {acc(1), can(1, CancelReason::Unfillable, 100)}, NONE, NONE},

        {"E4_last_order_filled_yields_sentinel",
         {sell(1, 10000, 100)}, buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)}, NONE, NONE},

        {"E5_last_order_cancelled_yields_sentinel",
         {sell(1, 10000, 100)}, cxl(1),
         {can(1, CancelReason::UserRequested, 100)}, NONE, NONE},

        {"E6_both_sides_emptied_then_reused",
         {sell(1, 10000, 100), buy(2, 10000, 100)}, buy(3, 9000, 10),
         {acc(3)}, 9000, NONE},

        {"E7_rests_at_lowest_valid_tick",
         {}, buy(1, kMinTick, 1), {acc(1)}, kMinTick, NONE},

        {"E8_rests_at_highest_valid_tick",
         {}, sell(1, kMaxTick, 1), {acc(1)}, NONE, kMaxTick},

        // ---- price and quantity validation ----
        {"E10_zero_quantity_rejected",
         {}, buy(1, 10000, 0), {rej(1, RejectReason::InvalidQuantity)}, NONE, NONE},

        {"E11_oversize_quantity_rejected",
         {}, buy(1, 10000, kMaxOrderQty + 1),
         {rej(1, RejectReason::InvalidQuantity)}, NONE, NONE},

        {"E12_price_below_ladder_rejected",
         {}, buy(1, kMinTick - 1, 100),
         {rej(1, RejectReason::PriceOutOfRange)}, NONE, NONE},

        {"E13_price_above_ladder_rejected",
         {}, buy(1, kMaxTick + 1, 100),
         {rej(1, RejectReason::PriceOutOfRange)}, NONE, NONE},

        {"E15_market_price_field_ignored_not_validated",
         {}, buy(1, std::numeric_limits<Ticks>::max(), 100, OrderType::Market),
         {acc(1), can(1, CancelReason::NoLiquidity, 100)}, NONE, NONE},

        // ---- identity and lifecycle ----
        {"E16_duplicate_live_id_rejected",
         {buy(1, 10000, 100)}, sell(1, 20000, 5),
         {rej(1, RejectReason::DuplicateOrderId)}, 10000, NONE},

        {"E17_duplicate_retired_id_rejected",
         {sell(1, 10000, 100), buy(2, 10000, 100)}, sell(1, 30000, 5),
         {rej(1, RejectReason::DuplicateOrderId)}, NONE, NONE},

        {"E48_id_at_or_below_high_water_rejected_even_if_never_used",
         {buy(5, 10000, 100)}, buy(3, 10000, 100),
         {rej(3, RejectReason::DuplicateOrderId)}, 10000, NONE},

        {"E48b_rejected_command_does_not_advance_the_high_water_mark",
         {buy(5, 10000, 100), buy(3, 10000, 100)}, buy(6, 10000, 100),
         {acc(6)}, 10000, NONE},

        {"E49_order_id_zero_rejected",
         {}, buy(0, 10000, 100),
         {rej(0, RejectReason::DuplicateOrderId)}, NONE, NONE},

        {"E18_cancel_unknown_id_rejected",
         {}, cxl(999), {rej(999, RejectReason::UnknownOrderId)}, NONE, NONE},

        {"E19_cancel_filled_id_rejected",
         {sell(1, 10000, 100), buy(2, 10000, 100)}, cxl(1),
         {rej(1, RejectReason::UnknownOrderId)}, NONE, NONE},

        {"E20_double_cancel_rejected",
         {buy(1, 10000, 100), cxl(1)}, cxl(1),
         {rej(1, RejectReason::UnknownOrderId)}, NONE, NONE},

        {"E21_cancel_partially_filled_removes_remainder_only",
         {sell(1, 10000, 100), buy(2, 10000, 40)}, cxl(1),
         {can(1, CancelReason::UserRequested, 60)}, NONE, NONE},

        {"E22_cancel_fifo_head",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)}, cxl(1),
         {can(1, CancelReason::UserRequested, 10)}, NONE, 10000},

        {"E23_cancel_fifo_tail",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)}, cxl(3),
         {can(3, CancelReason::UserRequested, 10)}, NONE, 10000},

        {"E24_cancel_only_order_at_best_level_moves_best",
         {buy(1, 10010, 10), buy(2, 10000, 10)}, cxl(1),
         {can(1, CancelReason::UserRequested, 10)}, 10000, NONE},

        {"E25_cancel_fifo_middle",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10)}, cxl(2),
         {can(2, CancelReason::UserRequested, 10)}, NONE, 10000},

        // ---- matching mechanics ----
        {"E26_exact_quantity_match",
         {sell(1, 10000, 100)}, buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)}, NONE, NONE},

        {"E27_incoming_smaller_maker_survives_without_filled_event",
         {sell(1, 10000, 100)}, buy(2, 10000, 30),
         {acc(2), trd(2, 1, 10000, 30), fil(2)}, NONE, 10000},

        {"E28_fifo_within_a_level",
         {sell(1, 10000, 10), sell(2, 10000, 10), sell(3, 10000, 10), sell(4, 10000, 10)},
         buy(9, 10000, 40),
         {acc(9), trd(9, 1, 10000, 10), fil(1), trd(9, 2, 10000, 10), fil(2),
          trd(9, 3, 10000, 10), fil(3), trd(9, 4, 10000, 10), fil(4), fil(9)},
         NONE, NONE},

        {"E29_sweeps_levels_best_price_first_at_each_makers_price",
         {sell(1, 10020, 10), sell(2, 10000, 10), sell(3, 10010, 10)},
         buy(9, 10020, 30),
         {acc(9), trd(9, 2, 10000, 10), fil(2), trd(9, 3, 10010, 10), fil(3),
          trd(9, 1, 10020, 10), fil(1), fil(9)},
         NONE, NONE},

        {"E30_equal_price_crosses",
         {sell(1, 10000, 100)}, buy(2, 10000, 100),
         {acc(2), trd(2, 1, 10000, 100), fil(1), fil(2)}, NONE, NONE},

        {"E31_one_tick_apart_does_not_cross",
         {sell(1, 10001, 100)}, buy(2, 10000, 100), {acc(2)}, 10000, 10001},

        {"E32_sweeps_level_then_rests_remainder",
         example_book(), buy(9, 10050, 1000),
         {acc(9), trd(9, 1, 10050, 300), fil(1), trd(9, 2, 10050, 100), fil(2)},
         10050, 10100},

        {"E33_post_only_that_would_cross_is_rejected_with_no_accepted",
         {sell(1, 10000, 100)}, buy(2, 10000, 100, OrderType::PostOnly),
         {rej(2, RejectReason::WouldCross)}, NONE, 10000},

        {"E34_post_only_that_does_not_cross_rests",
         {sell(1, 10010, 100)}, buy(2, 10000, 100, OrderType::PostOnly),
         {acc(2)}, 10000, 10010},

        {"E35_fok_exactly_fillable",
         {sell(1, 10000, 60), sell(2, 10010, 40)}, buy(9, 10010, 100, OrderType::Fok),
         {acc(9), trd(9, 1, 10000, 60), fil(1), trd(9, 2, 10010, 40), fil(2), fil(9)},
         NONE, NONE},

        {"E36_fok_one_unit_short_mutates_nothing",
         {sell(1, 10000, 60), sell(2, 10010, 39)}, buy(9, 10010, 100, OrderType::Fok),
         {acc(9), can(9, CancelReason::Unfillable, 100)}, NONE, 10000},

        {"E37_ioc_partial_fill_then_cancel_remainder",
         {sell(1, 10000, 30)}, buy(9, 10000, 100, OrderType::Ioc),
         {acc(9), trd(9, 1, 10000, 30), fil(1), can(9, CancelReason::IocRemainder, 70)},
         NONE, NONE},

        {"E38_equal_price_ties_broken_by_arrival_sequence",
         {sell(1, 10000, 10), sell(2, 10000, 10)}, buy(9, 10000, 20),
         {acc(9), trd(9, 1, 10000, 10), fil(1), trd(9, 2, 10000, 10), fil(2), fil(9)},
         NONE, NONE},

        // ---- worked example from the spec ----
        {"spec_worked_example_350_at_10050",
         example_book(), buy(99, 10050, 350),
         {acc(99), trd(99, 1, 10050, 300), fil(1), trd(99, 2, 10050, 50), fil(99)},
         NONE, 10050},
    };
}

}  // namespace obtest
```

- [ ] **Step 2: Write the typed test runner**

```cpp
// tests/test_edge_cases.cpp
#include "cases/edge_cases.hpp"

#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

// Phase 2 adds ob::FastEngine to this list, and every case below then runs against
// it unchanged. That one-line extension is the whole reason the table exists.
using EngineTypes = ::testing::Types<ob::ReferenceEngine>;

template <class E>
class EdgeCases : public ::testing::Test {};

TYPED_TEST_SUITE(EdgeCases, EngineTypes);

TYPED_TEST(EdgeCases, AllCasesProduceTheSpecifiedEvents) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;

        ob::FixedEventBuffer<4096> scratch;
        for (const ob::Command& c : tc.setup) {
            scratch.clear();
            engine.submit(c, scratch);
        }

        ob::FixedEventBuffer<4096> out;
        engine.submit(tc.subject, out);

        ASSERT_EQ(out.size(), tc.expect.size()) << "event count for " << tc.name;
        for (std::size_t i = 0; i < tc.expect.size(); ++i) {
            const ob::Event& got = out[i];
            const obtest::Expect& want = tc.expect[i];
            EXPECT_EQ(got.type, want.type) << "event " << i;
            EXPECT_EQ(got.order_id, want.id) << "event " << i;
            EXPECT_EQ(got.reject, want.reject) << "event " << i;
            EXPECT_EQ(got.cancel, want.cancel) << "event " << i;
            if (want.type == ob::EventType::Trade) {
                EXPECT_EQ(got.maker_id, want.maker) << "event " << i;
                EXPECT_EQ(got.price, want.price) << "event " << i;
            }
            if (want.qty != 0) {
                EXPECT_EQ(got.qty, want.qty) << "event " << i;
            }
        }

        if (tc.bid.has_value()) {
            EXPECT_EQ(engine.best_bid(), *tc.bid) << "best_bid for " << tc.name;
        }
        if (tc.ask.has_value()) {
            EXPECT_EQ(engine.best_ask(), *tc.ask) << "best_ask for " << tc.name;
        }
    }
}

// Asserted separately from the table so that inserting a case does not renumber
// every later expectation.
TYPED_TEST(EdgeCases, SequenceNumbersAreMonotonicAndGapFreeAcrossEveryCase) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;
        ob::Seq expected = 0;
        ob::FixedEventBuffer<4096> buf;

        for (const ob::Command& c : tc.setup) {
            buf.clear();
            engine.submit(c, buf);
            for (const ob::Event& e : buf) {
                ASSERT_EQ(e.seq, expected++);
            }
        }
        buf.clear();
        engine.submit(tc.subject, buf);
        for (const ob::Event& e : buf) {
            ASSERT_EQ(e.seq, expected++);
        }
    }
}

TYPED_TEST(EdgeCases, TheBookIsNeverCrossedAfterAnyCase) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        TypeParam engine;
        ob::FixedEventBuffer<4096> buf;
        for (const ob::Command& c : tc.setup) {
            buf.clear();
            engine.submit(c, buf);
        }
        buf.clear();
        engine.submit(tc.subject, buf);

        if (engine.best_bid() != ob::kNoPrice && engine.best_ask() != ob::kNoPrice) {
            EXPECT_LT(engine.best_bid(), engine.best_ask());
        }
    }
}

TYPED_TEST(EdgeCases, EveryCaseProducesAtLeastOneEvent) {
    for (const obtest::Case& tc : obtest::all_edge_cases()) {
        SCOPED_TRACE(tc.name);
        EXPECT_GE(tc.expect.size(), 1u) << "the table itself is wrong for " << tc.name;
    }
}

}  // namespace
```

- [ ] **Step 3: Add to the build and run**

Add `test_edge_cases.cpp` to `ob_tests` sources and add the include dir:

```cmake
target_include_directories(ob_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS. 40 cases x 4 typed tests, bringing the suite to 81 tests.

- [ ] **Step 4: Write the coverage-accounting note**

Not every spec edge case fits a single-subject-command table. Record where the rest live, so coverage is auditable rather than assumed. Append to `tests/cases/edge_cases.hpp`:

```cpp
// Coverage accounting for edge cases NOT in the table above. Every spec case in
// section 5.3 appears either in all_edge_cases() or in this list.
//
//   E9  ladder fully occupied      -> tests/test_stress.cpp (Task 12): 65,536 levels
//   E14 price not a whole tick     -> not reachable through this API; the API takes
//                                     integer ticks. Enforced at the file boundary by
//                                     the replay parser (Phase 3).
//   E39 order pool exhausted       -> test_reference_resting.cpp,
//                                     test_reference_cancel.cpp (capacity ctor)
//   E40 id index at capacity       -> Phase 2, tests/test_id_index.cpp
//   E41 level quantity overflow    -> invariants.hpp (Task 11), asserted continuously
//   E42 sequence number overflow   -> accepted, not guarded (spec 5.3); ~5800 years
//   E43 replay determinism         -> tests/test_determinism.cpp (Task 12)
//   E44 determinism across builds  -> CI matrix golden-file comparison (Task 13)
//   E45 reference vs fast engine   -> Phase 2, tests/test_differential.cpp
//   E46 buffer sized exactly       -> test_event_buffer.cpp (Task 4)
//   E47 buffer overflow aborts     -> test_event_buffer.cpp death test (Task 4)
//   E48 strictly increasing ids    -> all_edge_cases() rows E48, E48b, and
//                                     test_reference_validation.cpp
//   E49 order id 0 rejected        -> all_edge_cases() row E49
```

- [ ] **Step 5: Commit**

```bash
git add tests/cases/edge_cases.hpp tests/test_edge_cases.cpp tests/CMakeLists.txt
git commit -m "test: data-driven edge-case suite covering spec E1-E38, typed over engines"
```

---

## Task 11: Invariant checker

**Files:**
- Create: `include/ob/invariants.hpp`, `tests/test_invariants.cpp`
- Modify: `include/ob/reference_engine.hpp` (add `arrival` to `RefOrder`, add `for_each_resting`), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/engine_concept.hpp`.
- Produces: `ob::Inspectable` concept; `ob::RestingOrder{Side side; Ticks price; OrderId id; Qty remaining; Seq arrival;}`; `ob::InvariantResult{bool ok; const char* failure;}`; `ob::check_invariants(const E&) -> InvariantResult`. `ReferenceEngine` gains `template <class Fn> void for_each_resting(Fn&&) const`.

The checker is generic over any engine that can enumerate its resting orders, so Phase 2's `FastEngine` gets the same scrutiny for free. It returns a result rather than asserting, so the fuzzer can report which invariant broke.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_invariants.cpp
#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

#include <random>

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

// The checker must be able to fail. A checker that can only return ok is not a
// checker, and this is the test that proves it works.
TEST(Invariants, DetectAnInjectedFifoViolation) {
    struct BrokenEngine {
        static constexpr bool kTracksArrival = true;
        void submit(const ob::Command&, ob::EventBuffer&) {}
        [[nodiscard]] ob::Ticks best_bid() const { return 10000; }
        [[nodiscard]] ob::Ticks best_ask() const { return ob::kNoPrice; }
        void reset() {}
        [[nodiscard]] std::size_t live_order_count() const { return 2; }

        template <class Fn>
        void for_each_resting(Fn&& fn) const {
            // arrival sequence 5 before 2 at the same level: FIFO is violated.
            fn(ob::RestingOrder{Side::Buy, 10000, 1, 10, 5});
            fn(ob::RestingOrder{Side::Buy, 10000, 2, 10, 2});
        }
    };
    const BrokenEngine b;
    const auto r = ob::check_invariants(b);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string(r.failure).find("FIFO"), std::string::npos) << r.failure;
}

TEST(Invariants, DetectAnInjectedCrossedBook) {
    struct CrossedEngine {
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
    const CrossedEngine c;
    const auto r = ob::check_invariants(c);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(std::string(r.failure).find("crossed"), std::string::npos) << r.failure;
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_invariants.cpp` to `ob_tests`, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/invariants.hpp' file not found`.

- [ ] **Step 3: Add `arrival` and `for_each_resting` to `ReferenceEngine`**

Change `RefOrder` and `rest`, and add the visitor. The `arrival` field is what makes FIFO a checkable invariant rather than a hope.

```cpp
    struct RefOrder {
        OrderId id;
        Qty     remaining;
        Seq     arrival;   // strictly increasing; makes FIFO order checkable
    };
```

```cpp
    void rest(const Command& c, Qty remaining) {
        const Seq arrival = arrival_counter_++;
        if (c.side == Side::Buy) {
            bids_[c.price].push_back(RefOrder{c.id, remaining, arrival});
        } else {
            asks_[c.price].push_back(RefOrder{c.id, remaining, arrival});
        }
        live_[c.id] = Loc{c.side, c.price};
    }
```

Add the member `Seq arrival_counter_ = 0;` and reset it in `reset()`.

Add this public method (it needs `#include <ob/invariants.hpp>`'s `RestingOrder`, so declare `RestingOrder` in `types.hpp` instead to avoid a circular include):

```cpp
    // Enumerates every resting order, side by side, each side in best-to-worst
    // price order, and within a level in FIFO order. The invariant checker and the
    // L2 publisher (Phase 3) both rely on exactly that ordering.
    template <class Fn>
    void for_each_resting(Fn&& fn) const {
        for (const auto& [px, level] : bids_) {
            for (const RefOrder& o : level) {
                fn(RestingOrder{Side::Buy, px, o.id, o.remaining, o.arrival});
            }
        }
        for (const auto& [px, level] : asks_) {
            for (const RefOrder& o : level) {
                fn(RestingOrder{Side::Sell, px, o.id, o.remaining, o.arrival});
            }
        }
    }
```

And add `RestingOrder` to `include/ob/types.hpp`:

```cpp
// A resting order as seen by an external inspector. Used by the invariant checker
// and the L2 publisher so neither needs access to engine internals.
struct RestingOrder {
    Side    side;
    Ticks   price;
    OrderId id;
    Qty     remaining;
    // Arrival order, strictly increasing. Meaningful only when the engine sets
    // E::kTracksArrival; FastEngine reports 0 because its Order struct is exactly
    // 32 bytes with no room for it, and its FIFO order is instead established by
    // the intrusive list structure plus differential testing against this engine.
    Seq     arrival;
};
```

- [ ] **Step 4: Write `include/ob/invariants.hpp`**

```cpp
// include/ob/invariants.hpp
#pragma once

// Whole-book invariant checker. Generic over any engine that can enumerate its
// resting orders, so the FastEngine in Phase 2 gets the same scrutiny for free.
//
// Returns a result rather than asserting, so the fuzzer can report WHICH invariant
// broke instead of just dying.

#include <ob/engine_concept.hpp>

#include <cstddef>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace ob {

template <class E>
concept Inspectable = Engine<E> && requires(const E e) {
    { e.live_order_count() } -> std::same_as<std::size_t>;
    e.for_each_resting([](const RestingOrder&) {});
    // Whether RestingOrder::arrival is meaningful. Compile-time, so the FIFO check
    // below simply does not exist for engines that cannot afford to track it.
    { std::bool_constant<E::kTracksArrival>{} } -> std::same_as<std::bool_constant<E::kTracksArrival>>;
};

struct InvariantResult {
    bool        ok = true;
    const char* failure = nullptr;
};

inline constexpr InvariantResult kInvariantsHold{};

template <Inspectable E>
[[nodiscard]] InvariantResult check_invariants(const E& engine) {
    std::vector<RestingOrder> orders;
    engine.for_each_resting([&orders](const RestingOrder& o) { orders.push_back(o); });

    // 1. The reported live count matches what enumeration finds.
    if (orders.size() != engine.live_order_count()) {
        return {false, "live_order_count disagrees with for_each_resting"};
    }

    std::unordered_set<OrderId> ids;
    Ticks max_bid = kNoPrice;
    Ticks min_ask = kNoPrice;

    // Per-side, per-level FIFO tracking.
    Side  cur_side = Side::Buy;
    Ticks cur_price = kNoPrice;
    Seq   last_arrival = 0;
    bool  in_level = false;

    Ticks prev_bid_px = kNoPrice;
    Ticks prev_ask_px = kNoPrice;

    for (const RestingOrder& o : orders) {
        // 2. No resting order may have zero remaining quantity.
        if (o.remaining == 0) {
            return {false, "resting order has zero remaining quantity"};
        }
        // 3. Every resting price is inside the ladder.
        if (!price_in_range(o.price)) {
            return {false, "resting order price is outside the ladder"};
        }
        // 4. Order ids are unique among live orders.
        if (!ids.insert(o.id).second) {
            return {false, "duplicate order id among resting orders"};
        }

        // 5. Within a price level, arrival sequence strictly increases (FIFO).
        //    Only checkable on engines that track arrival order. For engines that
        //    do not, FIFO is established by their own structural invariants plus
        //    differential testing, not here.
        if (in_level && o.side == cur_side && o.price == cur_price) {
            if constexpr (E::kTracksArrival) {
                if (o.arrival <= last_arrival) {
                    return {false, "FIFO order violated within a price level"};
                }
            }
        } else {
            // 6. Levels are visited best-to-worst within each side.
            if (o.side == Side::Buy) {
                if (prev_bid_px != kNoPrice && o.price >= prev_bid_px) {
                    return {false, "bid levels not enumerated in descending price order"};
                }
                prev_bid_px = o.price;
            } else {
                if (prev_ask_px != kNoPrice && o.price <= prev_ask_px) {
                    return {false, "ask levels not enumerated in ascending price order"};
                }
                prev_ask_px = o.price;
            }
            cur_side = o.side;
            cur_price = o.price;
            in_level = true;
        }
        last_arrival = o.arrival;

        if (o.side == Side::Buy) {
            if (max_bid == kNoPrice || o.price > max_bid) {
                max_bid = o.price;
            }
        } else {
            if (min_ask == kNoPrice || o.price < min_ask) {
                min_ask = o.price;
            }
        }
    }

    // 7. best_bid()/best_ask() agree with the resting orders that exist.
    if (engine.best_bid() != max_bid) {
        return {false, "best_bid disagrees with the resting bid orders"};
    }
    if (engine.best_ask() != min_ask) {
        return {false, "best_ask disagrees with the resting ask orders"};
    }

    // 8. The book is never observably crossed.
    if (max_bid != kNoPrice && min_ask != kNoPrice && max_bid >= min_ask) {
        return {false, "book is crossed: best_bid >= best_ask"};
    }

    return kInvariantsHold;
}

}  // namespace ob
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS. The two injected-failure tests are the important ones: they prove the checker can actually fail.

- [ ] **Step 6: Commit**

```bash
git add include/ob/invariants.hpp include/ob/types.hpp include/ob/reference_engine.hpp tests/test_invariants.cpp tests/CMakeLists.txt
git commit -m "feat: generic whole-book invariant checker with self-tests that prove it can fail"
```

---

## Task 12: Scenario generator, shrinker, determinism and stress

**Files:**
- Create: `tests/model/scenario_gen.hpp`, `tests/test_scenario_gen.cpp`, `tests/test_determinism.cpp`, `tests/test_stress.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ob/command.hpp`, `ob/invariants.hpp`.
- Produces: `obtest::Xoshiro256ss{explicit Xoshiro256ss(std::uint64_t seed); std::uint64_t next(); std::uint64_t bounded(std::uint64_t n);}`; `obtest::GenConfig`; `obtest::generate_stream(std::uint64_t seed, std::size_t n, const GenConfig&) -> std::vector<Command>`; `obtest::shrink(std::vector<Command>, const std::function<bool(const std::vector<Command>&)>& still_fails) -> std::vector<Command>`; `obtest::run_stream<E>(const std::vector<Command>&) -> std::vector<Event>`.

This is the oracle machinery Phase 2 consumes. In Phase 1 it earns its keep two ways: random streams checked against the invariants find bugs no hand-written case would, and the golden files prove determinism (E43, E44).

The shrinker matters more than it looks. A differential failure at operation 4,000,000 is unusable; the same failure reduced to six commands is a test you commit.

- [ ] **Step 1: Write the failing test for the generator and shrinker**

```cpp
// tests/test_scenario_gen.cpp
#include "model/scenario_gen.hpp"

#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

TEST(Xoshiro, IsDeterministicForAGivenSeed) {
    obtest::Xoshiro256ss a(12345), b(12345);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next(), b.next());
    }
}

TEST(Xoshiro, DifferentSeedsDiverge) {
    obtest::Xoshiro256ss a(1), b(2);
    bool differed = false;
    for (int i = 0; i < 10; ++i) {
        if (a.next() != b.next()) {
            differed = true;
        }
    }
    EXPECT_TRUE(differed);
}

TEST(Xoshiro, BoundedStaysInRange) {
    obtest::Xoshiro256ss r(7);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_LT(r.bounded(10), 10u);
    }
    EXPECT_EQ(r.bounded(1), 0u);
}

TEST(Generator, IsReproducibleFromItsSeed) {
    const obtest::GenConfig cfg;
    const auto a = obtest::generate_stream(99, 500, cfg);
    const auto b = obtest::generate_stream(99, 500, cfg);
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].type, b[i].type) << i;
        EXPECT_EQ(a[i].id, b[i].id) << i;
        EXPECT_EQ(a[i].price, b[i].price) << i;
        EXPECT_EQ(a[i].qty, b[i].qty) << i;
    }
}

TEST(Generator, ProducesEveryOrderTypeAndBothSidesAndCancels) {
    obtest::GenConfig cfg;
    const auto s = obtest::generate_stream(4, 5000, cfg);

    std::array<bool, 5> saw_type{};
    bool saw_cancel = false, saw_buy = false, saw_sell = false;
    for (const ob::Command& c : s) {
        if (c.type == ob::CommandType::Cancel) {
            saw_cancel = true;
            continue;
        }
        saw_type[static_cast<std::size_t>(c.order_type)] = true;
        (c.side == ob::Side::Buy ? saw_buy : saw_sell) = true;
    }
    for (std::size_t i = 0; i < saw_type.size(); ++i) {
        EXPECT_TRUE(saw_type[i]) << "order type " << i << " never generated";
    }
    EXPECT_TRUE(saw_cancel);
    EXPECT_TRUE(saw_buy);
    EXPECT_TRUE(saw_sell);
}

// The generator must produce a workload that actually trades. A generator that
// silently degenerates into "everything rests" would make every later benchmark
// meaningless while looking fine.
TEST(Generator, ActuallyProducesTradesNotJustRestingOrders) {
    const auto s = obtest::generate_stream(11, 5000, obtest::GenConfig{});
    const auto events = obtest::run_stream<ob::ReferenceEngine>(s);

    std::size_t trades = 0;
    for (const ob::Event& e : events) {
        if (e.type == ob::EventType::Trade) {
            ++trades;
        }
    }
    EXPECT_GT(trades, 100u) << "generated workload barely trades; it is not realistic";
}

TEST(Shrinker, ReducesToAMinimalFailingStream) {
    const auto stream = obtest::generate_stream(3, 200, obtest::GenConfig{});
    ASSERT_GE(stream.size(), 10u);

    // Predicate: "fails" iff the stream still contains the command at original
    // index 7's order id. The minimal failing input is therefore one command.
    const ob::OrderId target = stream[7].id;
    const auto still_fails = [target](const std::vector<ob::Command>& s) {
        for (const ob::Command& c : s) {
            if (c.id == target) {
                return true;
            }
        }
        return false;
    };
    ASSERT_TRUE(still_fails(stream));

    const auto minimal = obtest::shrink(stream, still_fails);
    EXPECT_TRUE(still_fails(minimal));
    EXPECT_LT(minimal.size(), stream.size());
    EXPECT_LE(minimal.size(), 2u) << "shrinker did not reduce far enough";
}

TEST(Shrinker, LeavesAnAlreadyMinimalStreamAlone) {
    const std::vector<ob::Command> one{ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit,
                                                    10000, 10)};
    const auto minimal = obtest::shrink(one, [](const std::vector<ob::Command>& s) {
        return !s.empty();
    });
    EXPECT_EQ(minimal.size(), 1u);
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_scenario_gen.cpp` to `ob_tests`, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'model/scenario_gen.hpp' file not found`.

- [ ] **Step 3: Write `tests/model/scenario_gen.hpp`**

`xoshiro256**` rather than `std::mt19937_64` because the standard library's distributions are not specified to produce identical values across implementations, and a golden file that changes between libstdc++ and libc++ is worse than no golden file.

```cpp
// tests/model/scenario_gen.hpp
#pragma once

// Deterministic command-stream generator and a delta-debugging shrinker.
//
// The PRNG is hand-rolled on purpose: std::uniform_int_distribution is NOT
// specified to produce identical values across standard library implementations,
// so a golden file generated with it would differ between libstdc++ and libc++.
// A golden file that changes with the toolchain is worse than no golden file.

#include <ob/command.hpp>
#include <ob/engine_concept.hpp>
#include <ob/events.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace obtest {

class Xoshiro256ss {
public:
    explicit Xoshiro256ss(std::uint64_t seed) {
        // SplitMix64 to spread a single seed across the 256-bit state.
        for (std::uint64_t& w : s_) {
            seed += 0x9E37'79B9'7F4A'7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
            w = z ^ (z >> 31);
        }
    }

    std::uint64_t next() {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Unbiased enough for test generation, and identical on every platform.
    std::uint64_t bounded(std::uint64_t n) { return n == 0 ? 0 : next() % n; }

private:
    static std::uint64_t rotl(std::uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t s_[4]{};
};

struct GenConfig {
    ob::Ticks   centre      = 10000;  // prices cluster around here
    ob::Ticks   half_width  = 25;     // +/- this many ticks
    ob::Qty     max_qty     = 100;
    std::uint64_t cancel_pct = 30;    // share of commands that are cancels
    std::uint64_t market_pct = 3;     // of New commands
    std::uint64_t ioc_pct     = 5;
    std::uint64_t fok_pct     = 3;
    std::uint64_t postonly_pct = 5;
};

// Generates `n` commands. Cancels always target an id the generator previously
// issued as a resting type, which is what makes the stream exercise the cancel
// path instead of producing a flood of UnknownOrderId rejections.
inline std::vector<ob::Command> generate_stream(std::uint64_t seed, std::size_t n,
                                               const GenConfig& cfg) {
    Xoshiro256ss rng(seed);
    std::vector<ob::Command> out;
    out.reserve(n);

    std::vector<ob::OrderId> cancellable;
    ob::OrderId next_id = 1;  // strictly increasing, as spec E48 requires

    for (std::size_t i = 0; i < n; ++i) {
        const bool do_cancel =
            !cancellable.empty() && rng.bounded(100) < cfg.cancel_pct;

        if (do_cancel) {
            const std::size_t k = static_cast<std::size_t>(rng.bounded(cancellable.size()));
            out.push_back(ob::make_cancel(cancellable[k]));
            cancellable.erase(cancellable.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }

        const ob::Side side = (rng.bounded(2) == 0) ? ob::Side::Buy : ob::Side::Sell;
        const std::uint64_t roll = rng.bounded(100);
        ob::OrderType type = ob::OrderType::Limit;
        if (roll < cfg.market_pct) {
            type = ob::OrderType::Market;
        } else if (roll < cfg.market_pct + cfg.ioc_pct) {
            type = ob::OrderType::Ioc;
        } else if (roll < cfg.market_pct + cfg.ioc_pct + cfg.fok_pct) {
            type = ob::OrderType::Fok;
        } else if (roll < cfg.market_pct + cfg.ioc_pct + cfg.fok_pct + cfg.postonly_pct) {
            type = ob::OrderType::PostOnly;
        }

        const ob::Ticks span = cfg.half_width * 2 + 1;
        const ob::Ticks px =
            cfg.centre - cfg.half_width +
            static_cast<ob::Ticks>(rng.bounded(static_cast<std::uint64_t>(span)));
        const ob::Qty qty =
            1 + static_cast<ob::Qty>(rng.bounded(static_cast<std::uint64_t>(cfg.max_qty)));

        out.push_back(ob::make_new(next_id, side, type, px, qty));
        if (type == ob::OrderType::Limit || type == ob::OrderType::PostOnly) {
            cancellable.push_back(next_id);
        }
        ++next_id;
    }
    return out;
}

// Runs a stream through a fresh engine and returns every event it produced.
template <class E>
std::vector<ob::Event> run_stream(const std::vector<ob::Command>& cmds) {
    E engine;
    std::vector<ob::Event> all;
    all.reserve(cmds.size() * 2);

    // Sized for the worst single-command sweep in a generated stream.
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (const ob::Command& c : cmds) {
        buf.clear();
        engine.submit(c, buf);
        all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
}

// Delta debugging. Repeatedly tries removing chunks, keeping any removal that
// preserves the failure, halving the chunk size when a pass makes no progress.
// A differential failure at operation 4,000,000 is unusable; the same failure
// reduced to six commands is a test you commit.
inline std::vector<ob::Command> shrink(
    std::vector<ob::Command> stream,
    const std::function<bool(const std::vector<ob::Command>&)>& still_fails) {
    std::size_t chunk = stream.size() / 2;
    while (chunk >= 1) {
        bool progressed = false;
        std::size_t i = 0;
        while (i < stream.size()) {
            const std::size_t take = std::min(chunk, stream.size() - i);
            std::vector<ob::Command> candidate;
            candidate.reserve(stream.size() - take);
            candidate.insert(candidate.end(), stream.begin(),
                             stream.begin() + static_cast<std::ptrdiff_t>(i));
            candidate.insert(candidate.end(),
                             stream.begin() + static_cast<std::ptrdiff_t>(i + take),
                             stream.end());
            if (!candidate.empty() && still_fails(candidate)) {
                stream = std::move(candidate);
                progressed = true;
            } else {
                i += take;
            }
        }
        if (!progressed) {
            if (chunk == 1) {
                break;
            }
            chunk /= 2;
        }
    }
    return stream;
}

}  // namespace obtest
```

Add `#include <algorithm>` for `std::min`.

- [ ] **Step 4: Run the generator tests to verify they pass**

```bash
cmake --build build && ctest --test-dir build -R "Xoshiro|Generator|Shrinker" --output-on-failure
```

Expected: PASS, 8 tests.

- [ ] **Step 5: Write the determinism and stress tests**

```cpp
// tests/test_determinism.cpp
#include "model/scenario_gen.hpp"

#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

// E43: the same input replayed gives a byte-identical event stream.
TEST(Determinism, ReplayingTheSameStreamProducesIdenticalEvents) {
    const auto stream = obtest::generate_stream(20260922, 20000, obtest::GenConfig{});
    const auto a = obtest::run_stream<ob::ReferenceEngine>(stream);
    const auto b = obtest::run_stream<ob::ReferenceEngine>(stream);

    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a[i], b[i]) << "divergence at event " << i;
    }
}

TEST(Determinism, ResetMakesAnEngineIndistinguishableFromAFreshOne) {
    const auto stream = obtest::generate_stream(7, 2000, obtest::GenConfig{});

    ob::ReferenceEngine reused;
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
    }
    reused.reset();

    std::vector<ob::Event> after_reset;
    for (const ob::Command& c : stream) {
        buf.clear();
        reused.submit(c, buf);
        after_reset.insert(after_reset.end(), buf.begin(), buf.end());
    }
    const auto fresh = obtest::run_stream<ob::ReferenceEngine>(stream);

    ASSERT_EQ(after_reset.size(), fresh.size());
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        ASSERT_EQ(after_reset[i], fresh[i]) << "divergence at event " << i;
    }
}

// E44 in part: a fingerprint of the event stream, printed so CI can compare it
// across compilers and optimization levels (Task 13 wires up that comparison).
TEST(Determinism, EventStreamFingerprintIsStable) {
    const auto stream = obtest::generate_stream(20260922, 50000, obtest::GenConfig{});
    const auto events = obtest::run_stream<ob::ReferenceEngine>(stream);

    // FNV-1a over the meaningful fields. Not cryptographic; just a stable digest.
    std::uint64_t h = 0xCBF2'9CE4'8422'2325ULL;
    const auto mix = [&h](std::uint64_t v) {
        for (int b = 0; b < 8; ++b) {
            h ^= (v >> (b * 8)) & 0xFF;
            h *= 0x0000'0100'0000'01B3ULL;
        }
    };
    for (const ob::Event& e : events) {
        mix(e.seq);
        mix(static_cast<std::uint64_t>(e.type));
        mix(e.order_id);
        mix(e.maker_id);
        mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(e.price)));
        mix(e.qty);
        mix(static_cast<std::uint64_t>(e.reject));
        mix(static_cast<std::uint64_t>(e.cancel));
    }

    std::printf("FINGERPRINT events=%zu digest=%016llx\n", events.size(),
                static_cast<unsigned long long>(h));
    EXPECT_GT(events.size(), 50000u);
    // The digest is compared ACROSS builds by CI, not pinned here: pinning it in
    // source would just encode one toolchain's result as if it were the spec.
}

}  // namespace
```

```cpp
// tests/test_stress.cpp
#include "model/scenario_gen.hpp"

#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>

#include <gtest/gtest.h>

namespace {

// Random streams with the invariants checked after EVERY operation. This is the
// test that finds what the hand-written cases did not.
TEST(Stress, InvariantsHoldAcrossManySeeds) {
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    for (std::uint64_t seed = 1; seed <= 25; ++seed) {
        ob::ReferenceEngine engine;
        const auto stream = obtest::generate_stream(seed, 4000, obtest::GenConfig{});

        for (std::size_t i = 0; i < stream.size(); ++i) {
            buf.clear();
            engine.submit(stream[i], buf);
            ASSERT_GE(buf.size(), 1u) << "seed " << seed << " op " << i;

            const auto r = ob::check_invariants(engine);
            ASSERT_TRUE(r.ok) << "seed " << seed << " op " << i << ": " << r.failure;
        }
    }
}

// E9: every tick in the ladder occupied. Too large for the edge-case table, so it
// lives here. Uses a narrow ladder span to stay fast while still hitting both
// extremes and every bitmap word boundary that Phase 2 will care about.
TEST(Stress, EveryTickInARangeCanBeOccupiedIncludingBothExtremes) {
    ob::ReferenceEngine engine(200'000);
    std::vector<ob::Event> storage(8192);
    ob::EventBuffer buf(storage.data(), storage.size());

    ob::OrderId id = 1;
    // Bids on the low half, asks on the high half, so the book never crosses.
    const ob::Ticks mid = ob::kMaxTick / 2;
    for (ob::Ticks px = ob::kMinTick; px <= mid; ++px) {
        buf.clear();
        engine.submit(ob::make_new(id++, ob::Side::Buy, ob::OrderType::Limit, px, 1), buf);
        ASSERT_EQ(buf[0].type, ob::EventType::Accepted) << "px " << px;
    }
    for (ob::Ticks px = mid + 1; px <= ob::kMaxTick; ++px) {
        buf.clear();
        engine.submit(ob::make_new(id++, ob::Side::Sell, ob::OrderType::Limit, px, 1), buf);
        ASSERT_EQ(buf[0].type, ob::EventType::Accepted) << "px " << px;
    }

    EXPECT_EQ(engine.best_bid(), mid);
    EXPECT_EQ(engine.best_ask(), mid + 1);
    EXPECT_EQ(engine.live_order_count(), static_cast<std::size_t>(ob::kMaxTick));

    const auto r = ob::check_invariants(engine);
    EXPECT_TRUE(r.ok) << r.failure;
}

}  // namespace
```

- [ ] **Step 6: Add both to the build and run the full suite**

Add `test_determinism.cpp` and `test_stress.cpp` to `ob_tests`, then:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS. `Stress.InvariantsHoldAcrossManySeeds` runs 100,000 operations with a full invariant check after each, so it takes tens of seconds in a Release build and minutes under sanitizers. If it fails, use the shrinker before debugging: reproduce with the failing seed, wrap the failure as a predicate, and shrink.

- [ ] **Step 7: Commit**

```bash
git add tests/model/scenario_gen.hpp tests/test_scenario_gen.cpp tests/test_determinism.cpp tests/test_stress.cpp tests/CMakeLists.txt
git commit -m "test: deterministic stream generator, delta-debugging shrinker, determinism and stress suites"
```

---

## Task 13: CI matrix, determinism gate, and README

**Files:**
- Modify: `.github/workflows/ci.yml`
- Create: `README.md`, `docs/METHODOLOGY.md`

**Interfaces:**
- Consumes: everything.
- Produces: a CI matrix over gcc/clang, Linux/macOS, Release and sanitizer builds, plus a cross-build determinism comparison. No new code interfaces.

- [ ] **Step 1: Replace `.github/workflows/ci.yml`**

The determinism job is the interesting one: it extracts the fingerprint line that `Determinism.EventStreamFingerprintIsStable` prints from four different builds and asserts they are identical. That is E44 turned into a gate, and it catches undefined behavior that happens to be benign at one optimization level.

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:

jobs:
  build-and-test:
    name: ${{ matrix.name }}
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - name: linux-gcc-release
            os: ubuntu-latest
            cc: gcc
            cxx: g++
            build_type: Release
            extra: ""
          - name: linux-clang-release
            os: ubuntu-latest
            cc: clang
            cxx: clang++
            build_type: Release
            extra: ""
          - name: linux-clang-asan-ubsan
            os: ubuntu-latest
            cc: clang
            cxx: clang++
            build_type: Debug
            extra: "-DOB_SANITIZE=ON"
          - name: linux-gcc-debug-invariants
            os: ubuntu-latest
            cc: gcc
            cxx: g++
            build_type: Debug
            extra: "-DOB_ENABLE_INVARIANTS=ON"
          - name: macos-clang-release
            os: macos-latest
            cc: clang
            cxx: clang++
            build_type: Release
            extra: ""
    steps:
      - uses: actions/checkout@v4

      - name: Install Ninja (Linux)
        if: runner.os == 'Linux'
        run: sudo apt-get update && sudo apt-get install -y ninja-build

      - name: Install Ninja (macOS)
        if: runner.os == 'macOS'
        run: brew install ninja

      - name: Configure
        env:
          CC: ${{ matrix.cc }}
          CXX: ${{ matrix.cxx }}
        run: >
          cmake -S . -B build -G Ninja
          -DCMAKE_BUILD_TYPE=${{ matrix.build_type }}
          -DOB_WARNINGS_AS_ERRORS=ON
          ${{ matrix.extra }}

      - name: Build
        run: cmake --build build

      - name: Test
        env:
          ASAN_OPTIONS: detect_leaks=1:abort_on_error=1
          UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
        run: ctest --test-dir build --output-on-failure --timeout 900

  determinism:
    name: determinism across builds (E44)
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y ninja-build clang

      # Four builds that differ in compiler, standard library behaviour and
      # optimization level. The event-stream fingerprint must be identical in all
      # four, or the engine depends on something it must not depend on.
      - name: Build and fingerprint each configuration
        run: |
          set -euo pipefail
          : > fingerprints.txt
          for cfg in "gcc:g++:Release" "gcc:g++:Debug" "clang:clang++:Release" "clang:clang++:Debug"; do
            cc="${cfg%%:*}"; rest="${cfg#*:}"; cxx="${rest%%:*}"; bt="${rest#*:}"
            rm -rf "build-$cc-$bt"
            CC="$cc" CXX="$cxx" cmake -S . -B "build-$cc-$bt" -G Ninja \
              -DCMAKE_BUILD_TYPE="$bt" -DOB_WARNINGS_AS_ERRORS=ON
            cmake --build "build-$cc-$bt"
            "./build-$cc-$bt/tests/ob_tests" \
              --gtest_filter=Determinism.EventStreamFingerprintIsStable \
            | grep '^FINGERPRINT' | tee -a fingerprints.txt
          done
          cat fingerprints.txt

      - name: Assert every build agrees
        run: |
          set -euo pipefail
          distinct=$(sort -u fingerprints.txt | wc -l)
          if [ "$distinct" -ne 1 ]; then
            echo "::error::event stream is not deterministic across builds"
            sort -u fingerprints.txt
            exit 1
          fi
          echo "all four builds agree: $(head -1 fingerprints.txt)"

  format:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y clang-format
      - run: |
          find include tests -name '*.hpp' -o -name '*.cpp' \
            | xargs clang-format --dry-run --Werror
```

- [ ] **Step 2: Verify the determinism job logic locally before trusting CI**

```bash
set -euo pipefail
: > /tmp/fp.txt
for bt in Release Debug; do
  cmake -S . -B "build-$bt" -G Ninja -DCMAKE_BUILD_TYPE="$bt" -DOB_WARNINGS_AS_ERRORS=ON
  cmake --build "build-$bt"
  "./build-$bt/tests/ob_tests" --gtest_filter=Determinism.EventStreamFingerprintIsStable \
    | grep '^FINGERPRINT' | tee -a /tmp/fp.txt
done
sort -u /tmp/fp.txt | wc -l
```

Expected: `1`. Two different optimization levels must produce the same digest. If this prints `2`, there is a real bug: something in the engine depends on optimization level, which almost always means undefined behavior. **Do not proceed to Phase 2 until this prints 1.**

- [ ] **Step 3: Write `docs/METHODOLOGY.md`**

The reader-facing version of spec section 8. It exists now, before any benchmark, so that the numbers in Phase 2 land on top of a stated method rather than the method being reverse-engineered to fit the numbers.

```markdown
# Measurement methodology

## Why this document exists

Benchmark numbers without a method are decoration. This file states what is
measured, how, and what could not be engineered away, so a skeptical reader can
attack the method directly.

## Hardware and toolchain

Recorded per result, not assumed. The development machine:

- Apple M4 Pro, arm64, 8 performance cores + 4 efficiency cores
- Cache line 128 bytes, L1d 64 KB, L2 4 MB
- Apple clang 21.0.0, C++20, `-O3`
- macOS 15 (Darwin 25.6)

## The clock, measured rather than assumed

| Source | Nominal | Measured minimum delta | Read overhead |
|---|---|---|---|
| `mach_absolute_time` | 41.6667 ns/tick (numer 125, denom 3) | 1 tick = 41.67 ns | — |
| `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` | nanoseconds | 41 ns | 11.52 ns |
| `CNTVCT_EL0` | `CNTFRQ` claims 1 GHz | ~42 (1 GHz units are fiction) | 0.32 ns amortized |

**The finest timestamp granularity available on this machine is ~41.67 ns.** The
target operation costs a few hundred nanoseconds. That ratio is the central
constraint on everything below.

Consequences:

1. **Per-operation cost is measured in batches** (time N operations, divide by N).
   Quantization cancels; the distribution is lost.
2. **The distribution is measured per operation**, with the 41.67 ns resolution
   floor stated next to every figure.
3. **Both are cross-checked on x86-64 Linux**, where `rdtsc` resolves below a
   nanosecond, to confirm the distribution's shape is a property of the code and
   not of the clock.
4. **The CI regression gate uses no clock at all.** It counts instructions with
   Cachegrind, which is deterministic on a shared runner.

## Errors this harness avoids, and how

| Error | Handling |
|---|---|
| Clock resolution | Batched measurement for per-op cost; explicit resolution floor on distributions; x86 cross-check |
| Clock call overhead | Measured at startup with a dependency-chained loop, not a throughput loop (a throughput loop under-reports because the reads pipeline), printed, and subtracted |
| Compiler eliding the work | Explicit barriers, plus a guard test asserting a deliberately dead benchmark body does *not* report ~0 ns |
| Coordinated omission | Open-loop driver: commands issued on a schedule computed in advance, latency measured from *intended* issue time |
| Cold cache and branch predictor | 100,000 discarded warm-up operations; the count is published |
| First-touch page faults | Pool, ladder and index fully pre-faulted before measurement |
| P-core vs E-core migration | `QOS_CLASS_USER_INTERACTIVE`. macOS offers no hard affinity, so this is a stated limitation, not a solved problem |
| Thermal throttling | Median of 5 independent process runs; inter-run spread published; >10% spread invalidates the run |
| Discarding outliers | Not done. The tail is the product. Max is always published |
| Flattering input | Six scenarios including an explicit worst case |
| No baseline | `ReferenceEngine` benchmarked under the identical harness; speedups are ratios against a real baseline |

## Limitations that remain

Stated rather than buried:

- **41.67 ns quantization on per-operation latency.** A p50 near 250 ns carries
  roughly 8% granularity error on this machine. The x86 cross-check exists because
  of this, not in spite of it.
- **No hardware performance counters.** `perf` does not exist on macOS, Apple
  Silicon exposes no userspace PMU, and GitHub runners have none either. Cache-miss
  and branch-misprediction counts come from Cachegrind simulation, not real silicon.
- **No hard CPU affinity on macOS.** A run that migrates cores shows up as a
  bimodal distribution; such runs are flagged and repeated, not quietly dropped.
- **No Instruments locally.** Only Command Line Tools are installed, so local
  profiling is `sample` plus manual instrumentation. `perf` profiles come from
  Linux CI.
```

- [ ] **Step 4: Write `README.md`**

The numbers section is deliberately empty and says so. An empty, honest results table is worth more than an aspirational one, and Phase 2 fills it.

```markdown
# Order book and matching engine

A limit order book and matching engine in C++20. Price-time priority, five order
types, deterministic event output, and a measurement harness built to survive
someone attacking it.

**Status: Phase 1 complete.** The engine is correct and exhaustively tested. It is
not yet fast, and it is not yet claimed to be: `ReferenceEngine` uses `std::map`
and `std::list` on purpose. Phase 2 adds `FastEngine` and the measured results.

## What it does

- Five order types: `Limit`, `Market`, `Ioc`, `Fok`, `PostOnly`, plus `Cancel`
- Price-time priority. Trades print at the **maker's** price
- Integer tick prices. No floating point touches a price or a quantity, anywhere
- Deterministic, gap-free, sequenced event stream. Identical across compilers and
  optimization levels, which CI enforces
- Every failure is a typed reason code, and a failed command leaves the book
  bit-for-bit unchanged

## How correctness is established

Five layers, weakest to strongest:

1. Unit tests per module
2. A **data-driven edge-case table** covering spec cases E1–E38, where the table
   is the specification and each row cites the case it covers
3. A **whole-book invariant checker** run after every operation, with self-tests
   that prove the checker can actually fail
4. **Random-stream stress testing** with invariants checked after each of 100,000
   operations across 25 seeds, plus a delta-debugging shrinker that reduces any
   failure to a minimal committable reproducer
5. **Cross-build determinism**: four builds (gcc/clang x Release/Debug) must
   produce an identical event-stream fingerprint, which catches undefined
   behaviour that happens to be benign at one optimization level

Phase 2 adds a sixth: differential testing of `FastEngine` against
`ReferenceEngine` over more than 10^7 generated operations, plus libFuzzer.

## Performance

Not yet measured. Deliberately blank rather than aspirational.

The methodology is written first, in [`docs/METHODOLOGY.md`](docs/METHODOLOGY.md),
including the awkward parts: the development machine's finest timestamp
granularity is 41.67 ns while the target operation costs a few hundred, so per-op
cost is measured in batches, distributions carry an explicit resolution floor, and
the CI regression gate counts instructions with Cachegrind rather than trusting
wall-clock time on a shared runner.

## Building

```bash
brew install cmake ninja          # macOS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Useful configurations:

```bash
# sanitizers
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON

# invariants asserted after every operation
cmake -S . -B build-inv -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOB_ENABLE_INVARIANTS=ON
```

## Design

The full design document, including the alternatives that were rejected and why,
the enumerated edge cases, the failure scenarios and an adversarial review
section, is in
[`docs/superpowers/specs/`](docs/superpowers/specs/2026-09-22-order-book-matching-engine-design.md).
```

- [ ] **Step 5: Run the complete suite one final time**

```bash
rm -rf build && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DOB_WARNINGS_AS_ERRORS=ON
cmake --build build 2>&1 | tail -5
ctest --test-dir build --output-on-failure 2>&1 | tail -20
```

Expected: a clean build with zero warnings and every test passing. **If any warning appears, fix it rather than lowering the flag.** The warning set is chosen to be demanding, and `-Wconversion` plus `-Wsign-conversion` in particular will have forced explicit `static_cast`s throughout, which is the intended outcome.

- [ ] **Step 6: Verify against the spec's success criteria**

```bash
ctest --test-dir build -N | tail -3            # total test count
grep -c '{"E' tests/cases/edge_cases.hpp       # edge-case table rows
```

Confirm by hand: S1 partially met (differential arrives in Phase 2, generator and shrinker exist now), S2 met, S7 met for build commands, S3–S6 and S8 belong to later plans. Record anything unmet in the commit message rather than claiming completion.

- [ ] **Step 7: Commit**

```bash
git add .github/workflows/ci.yml README.md docs/METHODOLOGY.md
git commit -m "ci: full build matrix, cross-build determinism gate, and project README"
```

- [ ] **Step 8: Confirm before any push**

```bash
git log --oneline
git remote -v
```

Per the global constraints and spec O3, **nothing is pushed without explicit approval.** Report the commit list and ask before adding a remote or pushing.

---

## Self-review

Run after the plan is complete, before execution.

**Spec coverage.** Spec section 5.3 lists E1–E47. E1–E8 and E10–E38 are rows in the Task 10 table. E9 is `Stress.EveryTickInARange...` (Task 12). E39 is in Tasks 6 and 8. E43 and E44 are Task 12 and the Task 13 determinism job. E46 and E47 are Task 4. E14, E40, E41, E42 and E45 are accounted for in the Task 10 coverage note, four of them deferred to Phase 2 by design and one unreachable through this API. Spec sections 8 (measurement), 5.6 (performance) and 5.8's L2 additions are Phases 2 and 3 and are out of this plan's scope by construction.

**Placeholder scan.** No "TBD", no "add appropriate error handling", no "similar to Task N". Every code step carries the actual code it needs.

**Compile-time capability flag.** `E::kTracksArrival` is required by `Inspectable` and consumed by an `if constexpr` in `check_invariants`. `ReferenceEngine` sets it true. Phase 2's `FastEngine` sets it false, because `Order` is exactly 32 bytes with no room for an arrival sequence, and adding a parallel array would put a cold-line store on the hot path for the benefit of a debug-only check. FastEngine's FIFO order is established instead by its own intrusive-list structural invariants and by differential testing against this engine.

**Type consistency.** `submit(const Command&, EventBuffer&)`, `best_bid()`, `best_ask()`, `reset()`, `live_order_count()`, `for_each_resting(Fn&&)` are spelled identically in the concept (Task 4), the engine (Tasks 5–11), the invariant checker (Task 11) and the harness (Task 12). `RestingOrder` is declared in `types.hpp`, not `invariants.hpp`, specifically to avoid a circular include between the engine and the checker. `Expect`/`Case` field names match their use in the Task 10 runner. `GenConfig` field names match `generate_stream`'s body.

**Known ordering hazard for the executor.** Task 11 changes `RefOrder`'s shape and `rest`'s body. If Task 11 is executed before Tasks 6–9 are complete, `rest` will not exist yet. Execute the tasks in order.
