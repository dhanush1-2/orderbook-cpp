# Phase 5: Nasdaq ITCH 5.0 Feed Decoder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode real Nasdaq ITCH 5.0 files correctly, safely and fast, verified against actual bytes rather than a reading of the specification.

**Architecture:** A framing layer that walks the 2-byte big-endian length prefixes, and a typed decoder that turns each body into a tagged `ItchMessage`. No allocation, no copying of payload, every length validated before any field is read. The decoder is the only part of the system that touches untrusted bytes, so it carries the fuzz target.

**Tech Stack:** C++20, standard library only (plus the `__builtin_bswap` family, which
GCC and Clang both provide). libFuzzer for the parser target. `curl` and `gunzip` for
acquisition.

**Spec:** [`../specs/2026-09-25-itch-replay-and-research-agent-design.md`](../specs/2026-09-25-itch-replay-and-research-agent-design.md)

## Global Constraints

Everything in [`README.md`](README.md) still applies. What this phase adds:

- **Everything on the wire is BIG-ENDIAN and packed with no padding.** The predecessor's protocol was little-endian; do not copy its byte order.
- **The length prefix is 2 bytes big-endian and EXCLUDES itself.** Verified against real bytes.
- **The common header is 11 bytes:** `type` u8, `stock_locate` u16, `tracking_number` u16, `timestamp` **6 bytes**. A 6-byte integer has no primitive type; it is assembled byte by byte.
- **`memcpy`, never `reinterpret_cast`.** The wire guarantees no alignment.
- **C++20, so no `std::byteswap`** (that is C++23). Use `__builtin_bswap*`, which both
  GCC and Clang provide and both compile to a single instruction.
- **Prices are `uint32` with 4 implied decimals.** `$55.36` is `553600`. Floating point never touches a price.
- **Symbols are 8 ASCII bytes, space-padded on the right**, not null-terminated.
- **No allocation in the decode path.** It fills a caller-provided struct.
- **Every malformed input is a typed error, never UB.** The fuzz target proves it.
- **Do not commit the multi-GB data files.** A 10 MB slice is committed for tests; the full file is gitignored.

## Verified message inventory

Body sizes **exclude** the 2-byte length prefix. Every one of these was confirmed
against 354,869 real messages with zero mismatches.

| Type | Name | Bytes | | Type | Name | Bytes |
|---|---|---|---|---|---|---|
| `S` | SystemEvent | 12 | | `A` | AddOrder | 36 |
| `R` | StockDirectory | 39 | | `F` | AddOrderMPID | 40 |
| `H` | StockTradingAction | 25 | | `E` | OrderExecuted | 31 |
| `Y` | RegSHORestriction | 20 | | `C` | OrderExecutedWithPrice | 36 |
| `L` | MarketParticipantPosition | 26 | | `X` | OrderCancel | 23 |
| `V` | MWCBDeclineLevel | 35 | | `D` | OrderDelete | 19 |
| `W` | MWCBStatus | 12 | | `U` | OrderReplace | 35 |
| `K` | IPOQuotingPeriodUpdate | 28 | | `P` | TradeNonCross | 44 |
| `J` | LULDAuctionCollar | 35 | | `Q` | CrossTrade | 40 |
| `h` | OperationalHalt | 21 | | `B` | BrokenTrade | 19 |
| `N` | RPII | 20 | | `I` | NOII | 50 |

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `scripts/fetch_itch.sh` | Download a day, verify md5, decompress, cut the committed test slice | 1 |
| `testdata/itch_slice_10mb.bin` | Committed 10 MB of real decompressed ITCH | 1 |
| `include/ob/itch/types.hpp` | Scalar aliases, `MsgType`, `Symbol`, the 22 message structs | 2 |
| `include/ob/itch/decoder.hpp` | Framing, header decode, typed body decode | 3, 4, 5 |
| `tools/itch_stat.cpp` | Decode a whole file, report counts and throughput | 6 |
| `fuzz/fuzz_itch_decoder.cpp` | Decoder against arbitrary bytes | 7 |
| `tests/test_itch_types.cpp`, `tests/test_itch_decoder.cpp`, `tests/test_itch_golden.cpp` | | 2–6 |

---

## Task 1: Get the real data

**Files:** Create `scripts/fetch_itch.sh`, `testdata/README.md`. Modify `.gitignore`.

**Produces:** `data/12302019.NASDAQ_ITCH50` (gitignored, ~8.5 GB) and `testdata/itch_slice_10mb.bin` (committed).

Nothing else in this phase can be trusted until the bytes are real and verified. The
md5 step is not optional: a truncated 3.5 GB download produces a file that decodes
fine for hours and then lies.

- [ ] **Step 1: Write `scripts/fetch_itch.sh`**

```bash
#!/usr/bin/env bash
# Download a real Nasdaq ITCH 5.0 sample day, verify it, and cut the test slice.
#
# The md5 check is mandatory. A truncated multi-gigabyte download decodes correctly
# for hours before it fails, and the failure looks like a decoder bug.
set -euo pipefail

DAY="${1:-12302019}"
BASE="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH"
GZ="${DAY}.NASDAQ_ITCH50.gz"
OUT_DIR="${OB_ITCH_DIR:-data}"
mkdir -p "$OUT_DIR" testdata

echo "==> downloading $GZ (3.5-5.6 GB depending on the day)"
curl -# --fail --location --continue-at - -o "$OUT_DIR/$GZ" "$BASE/$GZ"

echo "==> verifying md5"
curl -s --fail -o "$OUT_DIR/$GZ.md5sum" "$BASE/$GZ.md5sum"
EXPECT=$(tr -dc '0-9a-fA-F' < "$OUT_DIR/$GZ.md5sum" | head -c 32)
ACTUAL=$(md5 -q "$OUT_DIR/$GZ" 2>/dev/null || md5sum "$OUT_DIR/$GZ" | cut -d' ' -f1)
if [ "$EXPECT" != "$ACTUAL" ]; then
    echo "MD5 MISMATCH: expected $EXPECT, got $ACTUAL" >&2
    echo "The download is corrupt or truncated. Delete it and re-run." >&2
    exit 1
fi
echo "    md5 ok: $ACTUAL"

echo "==> decompressing (about 8.5 GB for 12302019)"
gunzip -c "$OUT_DIR/$GZ" > "$OUT_DIR/${DAY}.NASDAQ_ITCH50"
ls -lh "$OUT_DIR/${DAY}.NASDAQ_ITCH50"

echo "==> cutting the committed 10 MB test slice on a MESSAGE BOUNDARY"
# A byte-count cut would leave a half message at the end, and every decoder test
# would then have to tolerate garbage. Walk the framing and stop cleanly.
python3 - "$OUT_DIR/${DAY}.NASDAQ_ITCH50" testdata/itch_slice_10mb.bin <<'PY'
import struct, sys
src, dst = sys.argv[1], sys.argv[2]
LIMIT = 10 * 1024 * 1024
with open(src, "rb") as f:
    data = f.read(LIMIT + 4096)
off = 0
while True:
    if off + 2 > len(data):
        break
    (ln,) = struct.unpack_from(">H", data, off)
    if ln == 0 or off + 2 + ln > len(data) or off + 2 + ln > LIMIT:
        break
    off += 2 + ln
with open(dst, "wb") as f:
    f.write(data[:off])
print(f"    wrote {dst}: {off:,} bytes, ending on a message boundary")
PY

echo "==> done"
```

```bash
chmod +x scripts/fetch_itch.sh
```

- [ ] **Step 2: Run it**

```bash
./scripts/fetch_itch.sh 12302019
```

Expected: `md5 ok`, an ~8.5 GB decompressed file, and a ~10 MB slice. Budget 10–30
minutes for the download. **If the md5 fails, do not continue** — every later test
would be validating against corrupt data.

- [ ] **Step 3: Keep the big files out of git**

Append to `.gitignore`:

```gitignore
# Real ITCH data: multi-gigabyte, fetched by scripts/fetch_itch.sh
data/
*.NASDAQ_ITCH50
*.NASDAQ_ITCH50.gz
```

`testdata/itch_slice_10mb.bin` is deliberately NOT ignored: CI needs real bytes, and
10 MB is an acceptable thing to commit for that.

- [ ] **Step 4: Write `testdata/README.md`**

```markdown
# Test data

`itch_slice_10mb.bin` is the first ~10 MB of real Nasdaq ITCH 5.0 from
`12302019.NASDAQ_ITCH50`, cut on a message boundary. It is committed so tests and CI
run against genuine exchange bytes rather than bytes this project made up.

Regenerate, or fetch a different day, with:

    ./scripts/fetch_itch.sh 12302019

Source: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (public, free, md5-verified).

The slice is pre-market, so it is dominated by `L` MarketParticipantPosition and
`R` StockDirectory messages and contains few executions. **Tests that need a
realistic message mix must use the full file**, because gzip is not seekable and a
mid-day slice cannot be cut without decompressing everything before it.
```

- [ ] **Step 5: Confirm the slice contains what is expected**

```bash
ls -lh testdata/itch_slice_10mb.bin
python3 -c "
import struct, collections
d = open('testdata/itch_slice_10mb.bin','rb').read()
off=0; c=collections.Counter()
while off+2 <= len(d):
    (ln,) = struct.unpack_from('>H', d, off)
    if ln == 0 or off+2+ln > len(d): break
    c[chr(d[off+2])] += 1; off += 2+ln
print('bytes consumed:', off, 'of', len(d), '(must be equal)')
print('messages:', sum(c.values()))
print('types:', dict(c.most_common()))
"
```

Expected: bytes consumed equals file size exactly, roughly 350,000 messages, and
types including at least `S R H Y L A F E X D U P`.

- [ ] **Step 6: Commit**

```bash
git add scripts/fetch_itch.sh testdata/README.md testdata/itch_slice_10mb.bin .gitignore
git commit -m "build: fetch real Nasdaq ITCH 5.0 data with md5 verification"
```

---

## Task 2: ITCH types

**Files:** Create `include/ob/itch/types.hpp`, `tests/test_itch_types.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ob/types.hpp` for `Qty`/`Seq`.
- Produces: `ob::itch::Price4` (`std::uint32_t`), `ob::itch::OrderRef` (`std::uint64_t`), `ob::itch::MatchNumber`, `ob::itch::StockLocate`, `ob::itch::Nanos`, `ob::itch::Symbol`, `ob::itch::MsgType`, `body_size(MsgType) -> std::size_t`, `kCommonHeaderSize`, `kMaxBodySize`, and one struct per message type.

`Symbol` is a value type rather than a `std::string`: 8 bytes, trivially copyable,
comparable, and printable with the trailing spaces stripped. Making it a string would
allocate on the hot path.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_itch_types.cpp
#include <ob/itch/types.hpp>

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace {

using namespace ob::itch;

TEST(ItchTypes, HeaderAndBoundsMatchTheWireFormat) {
    // type(1) + stock_locate(2) + tracking_number(2) + timestamp(6)
    EXPECT_EQ(kCommonHeaderSize, 11u);
    EXPECT_EQ(kMaxBodySize, 50u) << "NOII is the largest message at 50 bytes";
    EXPECT_GT(kMaxFrameLength, kMaxBodySize)
        << "the desync cap must leave room for message types added after 5.0";
}

// Every size here was verified against 354,869 real messages.
TEST(ItchTypes, EveryBodySizeMatchesTheVerifiedInventory) {
    const std::pair<MsgType, std::size_t> expected[] = {
        {MsgType::SystemEvent, 12},      {MsgType::StockDirectory, 39},
        {MsgType::TradingAction, 25},    {MsgType::RegSHO, 20},
        {MsgType::MarketParticipant, 26},{MsgType::MwcbDeclineLevel, 35},
        {MsgType::MwcbStatus, 12},       {MsgType::IpoQuotingPeriod, 28},
        {MsgType::LuldAuctionCollar, 35},{MsgType::OperationalHalt, 21},
        {MsgType::AddOrder, 36},         {MsgType::AddOrderMpid, 40},
        {MsgType::OrderExecuted, 31},    {MsgType::OrderExecutedPrice, 36},
        {MsgType::OrderCancel, 23},      {MsgType::OrderDelete, 19},
        {MsgType::OrderReplace, 35},     {MsgType::TradeNonCross, 44},
        {MsgType::CrossTrade, 40},       {MsgType::BrokenTrade, 19},
        {MsgType::Noii, 50},             {MsgType::Rpii, 20},
    };
    for (const auto& [t, n] : expected) {
        EXPECT_EQ(body_size(t), n) << "type '" << static_cast<char>(t) << "'";
    }
}

TEST(ItchTypes, UnknownTypeHasNoSize) {
    EXPECT_EQ(body_size(static_cast<MsgType>('z')), 0u);
    EXPECT_EQ(body_size(static_cast<MsgType>(0)), 0u);
}

TEST(ItchTypes, SymbolIsEightBytesAndStripsPadding) {
    static_assert(sizeof(Symbol) == 8);
    static_assert(std::is_trivially_copyable_v<Symbol>);

    const Symbol a = Symbol::from_bytes(reinterpret_cast<const std::byte*>("A       "));
    EXPECT_EQ(a.str(), "A");
    const Symbol aapl = Symbol::from_bytes(reinterpret_cast<const std::byte*>("AAPL    "));
    EXPECT_EQ(aapl.str(), "AAPL");
    // Eight characters with no padding at all must survive intact.
    const Symbol full = Symbol::from_bytes(reinterpret_cast<const std::byte*>("ABCDEFGH"));
    EXPECT_EQ(full.str(), "ABCDEFGH");
}

TEST(ItchTypes, SymbolsCompareAndHash) {
    const Symbol a = Symbol::from_bytes(reinterpret_cast<const std::byte*>("AAPL    "));
    const Symbol b = Symbol::from_bytes(reinterpret_cast<const std::byte*>("AAPL    "));
    const Symbol c = Symbol::from_bytes(reinterpret_cast<const std::byte*>("MSFT    "));
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(std::hash<Symbol>{}(a), std::hash<Symbol>{}(b));
}

// Prices are uint32 with FOUR implied decimals. Floating point never touches one.
TEST(ItchTypes, PriceHelpersUseIntegerArithmeticOnly) {
    EXPECT_EQ(price_to_cents(553600u), 5536);       // $55.36
    EXPECT_EQ(price_to_cents(4u), 0);               // $0.0004 rounds toward zero
    EXPECT_EQ(price_to_cents(1'000'000'000u), 10'000'000);  // $100,000.00
    EXPECT_EQ(dollars_whole(553600u), 55u);
    EXPECT_EQ(dollars_frac_10000(553600u), 3600u);
}

TEST(ItchTypes, MessageStructsAreTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<AddOrder>);
    static_assert(std::is_trivially_copyable_v<OrderExecuted>);
    static_assert(std::is_trivially_copyable_v<OrderReplace>);
    static_assert(std::is_trivially_copyable_v<ItchMessage>);
    SUCCEED();
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails**

Add `test_itch_types.cpp` to `ob_tests` in `tests/CMakeLists.txt`, then:

```bash
cmake --build build 2>&1 | head -20
```

Expected: FAIL with `'ob/itch/types.hpp' file not found`.

- [ ] **Step 3: Write `include/ob/itch/types.hpp`**

```cpp
// include/ob/itch/types.hpp
#pragma once

// Nasdaq TotalView-ITCH 5.0 message types.
//
// Every body size below was VERIFIED against 354,869 real messages from
// 12302019.NASDAQ_ITCH50, not copied from the specification document. Zero
// mismatches. See the spec's section 3.3.
//
// Wire facts these types encode:
//   * everything is BIG-ENDIAN and packed with no padding
//   * the common header is 11 bytes: type(1) locate(2) tracking(2) timestamp(6)
//   * prices are uint32 with FOUR implied decimals ($55.36 == 553600)
//   * symbols are 8 ASCII bytes, space-padded right, NOT null-terminated

#include <ob/types.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>

namespace ob::itch {

using Price4      = std::uint32_t;  // 4 implied decimals
using OrderRef    = std::uint64_t;
using MatchNumber = std::uint64_t;
using StockLocate = std::uint16_t;
using Nanos       = std::uint64_t;  // since midnight; 6 bytes on the wire

inline constexpr std::size_t kCommonHeaderSize = 11;
inline constexpr std::size_t kMaxBodySize      = 50;  // NOII, the largest type we know
inline constexpr std::size_t kLengthPrefixSize = 2;

// A length above kMaxBodySize is NOT automatically a desync. Nasdaq can add a longer
// message type, and the length prefix exists precisely so an old reader can skip a
// new message. So the framing cap is generous, and anything between kMaxBodySize and
// this is skipped as an unknown type rather than treated as lost sync. Above it, the
// stream really is desynchronised and blindly skipping would plough through good data.
inline constexpr std::size_t kMaxFrameLength = 1024;

enum class MsgType : std::uint8_t {
    SystemEvent        = 'S',
    StockDirectory     = 'R',
    TradingAction      = 'H',
    RegSHO             = 'Y',
    MarketParticipant  = 'L',
    MwcbDeclineLevel   = 'V',
    MwcbStatus         = 'W',
    IpoQuotingPeriod   = 'K',
    LuldAuctionCollar  = 'J',
    OperationalHalt    = 'h',
    AddOrder           = 'A',
    AddOrderMpid       = 'F',
    OrderExecuted      = 'E',
    OrderExecutedPrice = 'C',
    OrderCancel        = 'X',
    OrderDelete        = 'D',
    OrderReplace       = 'U',
    TradeNonCross      = 'P',
    CrossTrade         = 'Q',
    BrokenTrade        = 'B',
    Noii               = 'I',
    Rpii               = 'N',
};

// 0 means "not a type this build knows". A well-framed unknown message is still
// skippable, because the length lives in the prefix rather than being implied.
[[nodiscard]] constexpr std::size_t body_size(MsgType t) noexcept {
    switch (t) {
        case MsgType::SystemEvent:        return 12;
        case MsgType::StockDirectory:     return 39;
        case MsgType::TradingAction:      return 25;
        case MsgType::RegSHO:             return 20;
        case MsgType::MarketParticipant:  return 26;
        case MsgType::MwcbDeclineLevel:   return 35;
        case MsgType::MwcbStatus:         return 12;
        case MsgType::IpoQuotingPeriod:   return 28;
        case MsgType::LuldAuctionCollar:  return 35;
        case MsgType::OperationalHalt:    return 21;
        case MsgType::AddOrder:           return 36;
        case MsgType::AddOrderMpid:       return 40;
        case MsgType::OrderExecuted:      return 31;
        case MsgType::OrderExecutedPrice: return 36;
        case MsgType::OrderCancel:        return 23;
        case MsgType::OrderDelete:        return 19;
        case MsgType::OrderReplace:       return 35;
        case MsgType::TradeNonCross:      return 44;
        case MsgType::CrossTrade:         return 40;
        case MsgType::BrokenTrade:        return 19;
        case MsgType::Noii:               return 50;
        case MsgType::Rpii:               return 20;
    }
    return 0;
}

// Eight ASCII bytes, space-padded. A value type rather than a std::string: it lives
// in message structs on the hot path and must not allocate.
struct Symbol {
    char c[8];

    [[nodiscard]] static Symbol from_bytes(const std::byte* p) noexcept {
        Symbol s{};
        std::memcpy(s.c, p, 8);
        return s;
    }
    // Trailing pad stripped. Leading spaces are preserved: they are not padding and
    // dropping them would merge two distinct symbols.
    [[nodiscard]] std::string str() const {
        std::size_t n = 8;
        while (n > 0 && c[n - 1] == ' ') {
            --n;
        }
        return std::string(c, n);
    }
    [[nodiscard]] friend bool operator==(const Symbol& a, const Symbol& b) noexcept {
        return std::memcmp(a.c, b.c, 8) == 0;
    }
};
static_assert(sizeof(Symbol) == 8);
static_assert(std::is_trivially_copyable_v<Symbol>);

// Integer-only price helpers. Truncates toward zero, which is correct for display
// and never used for arithmetic that must round.
[[nodiscard]] constexpr std::int64_t price_to_cents(Price4 p) noexcept {
    return static_cast<std::int64_t>(p) / 100;
}
[[nodiscard]] constexpr std::uint32_t dollars_whole(Price4 p) noexcept { return p / 10000u; }
[[nodiscard]] constexpr std::uint32_t dollars_frac_10000(Price4 p) noexcept {
    return p % 10000u;
}

struct Header {
    MsgType     type{};
    StockLocate stock_locate = 0;
    std::uint16_t tracking_number = 0;
    Nanos       timestamp = 0;
};

struct SystemEvent { Header h; char event_code; };
struct StockDirectory {
    Header h; Symbol stock; char market_category; char financial_status;
    std::uint32_t round_lot_size; char round_lots_only; char issue_classification;
    char issue_subtype[2]; char authenticity; char short_sale_threshold;
    char ipo_flag; char luld_tier; char etp_flag; std::uint32_t etp_leverage;
    char inverse;
};
struct TradingAction { Header h; Symbol stock; char state; char reserved; char reason[4]; };
struct RegSHO { Header h; Symbol stock; char action; };
struct MarketParticipant {
    Header h; char mpid[4]; Symbol stock; char primary_market_maker;
    char market_maker_mode; char state;
};
struct MwcbDeclineLevel { Header h; std::uint64_t l1; std::uint64_t l2; std::uint64_t l3; };
struct MwcbStatus { Header h; char breached_level; };
struct IpoQuotingPeriod {
    Header h; Symbol stock; std::uint32_t release_time; char release_qualifier;
    Price4 ipo_price;
};
struct LuldAuctionCollar {
    Header h; Symbol stock; Price4 reference_price; Price4 upper; Price4 lower;
    std::uint32_t extension;
};
struct OperationalHalt { Header h; Symbol stock; char market_code; char action; };

struct AddOrder {
    Header h; OrderRef order_ref; char buy_sell; std::uint32_t shares;
    Symbol stock; Price4 price;
};
struct AddOrderMpid {
    Header h; OrderRef order_ref; char buy_sell; std::uint32_t shares;
    Symbol stock; Price4 price; char attribution[4];
};
struct OrderExecuted {
    Header h; OrderRef order_ref; std::uint32_t executed_shares; MatchNumber match;
};
struct OrderExecutedPrice {
    Header h; OrderRef order_ref; std::uint32_t executed_shares; MatchNumber match;
    char printable; Price4 execution_price;
};
struct OrderCancel { Header h; OrderRef order_ref; std::uint32_t cancelled_shares; };
struct OrderDelete { Header h; OrderRef order_ref; };
struct OrderReplace {
    Header h; OrderRef original_order_ref; OrderRef new_order_ref;
    std::uint32_t shares; Price4 price;
};
struct TradeNonCross {
    Header h; OrderRef order_ref; char buy_sell; std::uint32_t shares;
    Symbol stock; Price4 price; MatchNumber match;
};
struct CrossTrade {
    Header h; std::uint64_t shares; Symbol stock; Price4 cross_price;
    MatchNumber match; char cross_type;
};
struct BrokenTrade { Header h; MatchNumber match; };
struct Noii {
    Header h; std::uint64_t paired_shares; std::uint64_t imbalance_shares;
    char imbalance_direction; Symbol stock; Price4 far_price; Price4 near_price;
    Price4 current_reference_price; char cross_type; char price_variation_indicator;
};
struct Rpii { Header h; Symbol stock; char interest_flag; };

// A tagged union. `header` is valid for every type, so a consumer that only cares
// about timestamps or stock_locate never has to switch.
struct ItchMessage {
    Header header{};
    union {
        SystemEvent        system_event;
        StockDirectory     stock_directory;
        TradingAction      trading_action;
        RegSHO             reg_sho;
        MarketParticipant  market_participant;
        MwcbDeclineLevel   mwcb_decline_level;
        MwcbStatus         mwcb_status;
        IpoQuotingPeriod   ipo_quoting_period;
        LuldAuctionCollar  luld_auction_collar;
        OperationalHalt    operational_halt;
        AddOrder           add_order;
        AddOrderMpid       add_order_mpid;
        OrderExecuted      order_executed;
        OrderExecutedPrice order_executed_price;
        OrderCancel        order_cancel;
        OrderDelete        order_delete;
        OrderReplace       order_replace;
        TradeNonCross      trade_non_cross;
        CrossTrade         cross_trade;
        BrokenTrade        broken_trade;
        Noii               noii;
        Rpii               rpii;
    };
    ItchMessage() noexcept : add_order{} {}
};
static_assert(std::is_trivially_copyable_v<ItchMessage>);

}  // namespace ob::itch

template <>
struct std::hash<ob::itch::Symbol> {
    std::size_t operator()(const ob::itch::Symbol& s) const noexcept {
        std::uint64_t v{};
        std::memcpy(&v, s.c, 8);
        return std::hash<std::uint64_t>{}(v);
    }
};
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build && ./build/tests/ob_tests '--gtest_filter=ItchTypes.*'
```

Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add include/ob/itch/types.hpp tests/test_itch_types.cpp tests/CMakeLists.txt
git commit -m "feat(itch): message types with sizes verified against real data"
```

---

## Task 3: Framing and the common header

**Files:** Create `include/ob/itch/decoder.hpp`, `tests/test_itch_decoder.cpp`. Modify `tests/CMakeLists.txt`.

**Interfaces:**
- Consumes: `ob/itch/types.hpp`.
- Produces: `ob::itch::DecodeError`, `ob::itch::DecodeResult`, `ob::itch::read_be<T>(const std::byte*, std::size_t)`, `ob::itch::read_u48(const std::byte*, std::size_t)`, `ob::itch::decode(std::span<const std::byte>, ItchMessage&) -> DecodeResult`. This task implements framing and the header; Tasks 4 and 5 fill in the bodies.

**The 6-byte timestamp has no primitive type.** It is assembled from bytes. Reading 8
and masking would read two bytes past a message that ends at the timestamp, which is
a buffer overrun on the last message in a buffer.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/test_itch_decoder.cpp
#include <ob/itch/decoder.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

using namespace ob::itch;

// Builds a framed message: 2-byte BE length prefix, then the body.
std::vector<std::byte> frame(const std::vector<std::uint8_t>& body) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>((body.size() >> 8) & 0xFF));
    out.push_back(static_cast<std::byte>(body.size() & 0xFF));
    for (const std::uint8_t b : body) {
        out.push_back(static_cast<std::byte>(b));
    }
    return out;
}

TEST(ItchDecoder, ReadsBigEndianIntegers) {
    const std::array<std::byte, 8> b{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08}};
    EXPECT_EQ(read_be<std::uint16_t>(b.data(), 0), 0x0102u);
    EXPECT_EQ(read_be<std::uint32_t>(b.data(), 0), 0x01020304u);
    EXPECT_EQ(read_be<std::uint64_t>(b.data(), 0), 0x0102030405060708ull);
}

// The 6-byte timestamp is assembled byte by byte. Reading 8 and masking would run
// two bytes past a message that ends at the timestamp.
TEST(ItchDecoder, ReadsTheSixByteTimestampWithoutOverreading) {
    const std::array<std::byte, 6> b{std::byte{0x0a}, std::byte{0x11}, std::byte{0xea},
                                     std::byte{0x0e}, std::byte{0x8c}, std::byte{0x43}};
    EXPECT_EQ(read_u48(b.data(), 0), 0x0a11ea0e8c43ull);
    EXPECT_EQ(read_u48(b.data(), 0), 11072057543747ull);  // 11,072.058 s after midnight
}

// The exact first message of the real file, byte for byte.
TEST(ItchDecoder, DecodesTheRealFilesFirstMessage) {
    const auto bytes = frame({0x53,                                // 'S'
                              0x00, 0x00,                          // stock_locate 0
                              0x00, 0x00,                          // tracking 0
                              0x0a, 0x11, 0xea, 0x0e, 0x8c, 0x43,  // timestamp
                              0x4f});                              // 'O'
    ItchMessage m{};
    const auto r = decode(bytes, m);
    ASSERT_TRUE(r.ok()) << to_string(r.error);
    EXPECT_EQ(r.consumed, 14u) << "12-byte body plus the 2-byte prefix";
    EXPECT_EQ(m.header.type, MsgType::SystemEvent);
    EXPECT_EQ(m.header.stock_locate, 0u);
    EXPECT_EQ(m.header.timestamp, 11072057543747ull);
    EXPECT_EQ(m.system_event.event_code, 'O');
}

TEST(ItchDecoder, EveryTruncatedPrefixIsReportedAndConsumesNothing) {
    const auto full = frame({0x53, 0, 0, 0, 0, 0x0a, 0x11, 0xea, 0x0e, 0x8c, 0x43, 0x4f});
    for (std::size_t n = 0; n < full.size(); ++n) {
        ItchMessage m{};
        const auto r = decode(std::span<const std::byte>(full.data(), n), m);
        EXPECT_EQ(r.error, DecodeError::Truncated) << "prefix length " << n;
        EXPECT_EQ(r.consumed, 0u) << "a truncated frame must consume nothing";
    }
}

TEST(ItchDecoder, ZeroLengthIsUnframeable) {
    const std::array<std::byte, 4> b{std::byte{0}, std::byte{0}, std::byte{0x53},
                                     std::byte{0}};
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::BadLength);
    EXPECT_EQ(r.consumed, 0u) << "the reader cannot know how far to skip";
}

TEST(ItchDecoder, LengthBelowTheCommonHeaderIsUnframeable) {
    for (std::uint8_t len = 1; len < 11; ++len) {
        std::vector<std::byte> b{std::byte{0}, static_cast<std::byte>(len)};
        b.resize(2 + len, std::byte{0x53});
        ItchMessage m{};
        const auto r = decode(b, m);
        EXPECT_EQ(r.error, DecodeError::BadLength) << "len " << int(len);
        EXPECT_EQ(r.consumed, 0u);
    }
}

// A length beyond any plausible frame means the stream is desynchronised. Skipping
// that far would plough through good data, so stop instead.
TEST(ItchDecoder, AbsurdLengthIsUnframeable) {
    std::vector<std::byte> b{std::byte{0xff}, std::byte{0xff}};  // 65535
    b.resize(2 + 64, std::byte{0x53});                            // truncated anyway
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::BadLength);
    EXPECT_EQ(r.consumed, 0u);
}

// A longer-than-known message with a plausible length is FORWARD COMPATIBILITY, not
// a desync. If Nasdaq adds a 60-byte type, this decoder must skip it and keep going.
TEST(ItchDecoder, LongerThanKnownMessageIsSkippedNotFatal) {
    std::vector<std::uint8_t> body(60, 0);
    body[0] = 0x7a;  // a type this build does not know
    const auto b = frame(body);
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::UnknownType);
    EXPECT_EQ(r.consumed, 62u) << "one unknown long message must not end the replay";
}

// A KNOWN type with the wrong length is a different story: the frame is intact, so it
// is still skippable, but the message itself cannot be trusted.
TEST(ItchDecoder, KnownTypeWithAnOverlongLengthIsSkippable) {
    std::vector<std::uint8_t> body(60, 0);
    body[0] = 0x41;  // 'A', whose verified size is 36
    const auto b = frame(body);
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::LengthMismatch);
    EXPECT_EQ(r.consumed, 62u);
}

// A well-framed message of an unknown type must be SKIPPABLE. That is why the length
// lives in the prefix instead of being implied by the type.
TEST(ItchDecoder, UnknownTypeIsSkippableWithoutLosingFrameSync) {
    const auto b = frame({0x7a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01});  // 'z'
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::UnknownType);
    EXPECT_EQ(r.consumed, 14u) << "the caller must be able to skip and stay in sync";
}

TEST(ItchDecoder, LengthDisagreeingWithTheTypeIsRejectedButSkippable) {
    // 'S' claiming 20 bytes instead of its verified 12.
    std::vector<std::uint8_t> body(20, 0);
    body[0] = 0x53;
    const auto b = frame(body);
    ItchMessage m{};
    const auto r = decode(b, m);
    EXPECT_EQ(r.error, DecodeError::LengthMismatch);
    EXPECT_EQ(r.consumed, 22u) << "the frame is intact, so it can be skipped";
}

// The real file must walk end to end with no error and no leftover bytes.
TEST(ItchDecoder, WalksTheRealSliceEndToEnd) {
    std::ifstream f("testdata/itch_slice_10mb.bin", std::ios::binary);
    ASSERT_TRUE(f) << "run scripts/fetch_itch.sh first";
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    ASSERT_GT(raw.size(), 1'000'000u);

    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                                  raw.size());
    std::size_t off = 0, n = 0, errors = 0;
    while (off < in.size()) {
        ItchMessage m{};
        const auto r = decode(in.subspan(off), m);
        if (r.consumed == 0) {
            break;
        }
        if (!r.ok()) {
            ++errors;
        }
        off += r.consumed;
        ++n;
    }
    EXPECT_EQ(off, raw.size()) << "frame sync lost, or the slice is not boundary-cut";
    EXPECT_EQ(errors, 0u) << "real exchange data must decode without a single error";
    EXPECT_GT(n, 300'000u) << "a 10 MB slice held 354,869 messages when measured";
}

}  // namespace
```

- [ ] **Step 2: Add to the build, run, verify it fails** (`'ob/itch/decoder.hpp' file not found`).

- [ ] **Step 3: Write the framing half of `include/ob/itch/decoder.hpp`**

```cpp
// include/ob/itch/decoder.hpp
#pragma once

// Nasdaq ITCH 5.0 decoder.
//
// THIS PARSES UNTRUSTED BYTES. Every length is validated against the buffer AND the
// message type before a single field is read.
//
// Validation order is deliberate: prefix present, then the declared length against
// the BUFFER, then against the TYPE. Checking the type's size first would lose frame
// sync on a truncated buffer, because the caller would not know how far to advance.
//
// Errors come in two kinds, and the difference is `consumed`:
//   * UNFRAMEABLE (consumed == 0): the reader cannot know how far to skip, so it must
//     stop. Only a truncated buffer or an absurd length prefix qualifies.
//   * SKIPPABLE (consumed > 0): the frame is intact but its contents are not usable.
//     The reader advances and stays in sync. An unrecognised type is the common case,
//     and it is how this decoder survives Nasdaq adding a message type.
//
// std::memcpy, never reinterpret_cast: the wire guarantees no alignment.

#include <ob/itch/types.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace ob::itch {

enum class DecodeError : std::uint8_t {
    None = 0,
    Truncated,       // fewer bytes available than the frame claims
    BadLength,       // length is 0, below the header, or above the largest message
    UnknownType,     // well-framed, type this build does not know
    LengthMismatch,  // well-framed, but the length disagrees with the type
};

struct DecodeResult {
    DecodeError error    = DecodeError::None;
    std::size_t consumed = 0;  // bytes to advance; nonzero means the frame is skippable
    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }
};

[[nodiscard]] constexpr const char* to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::None:           return "None";
        case DecodeError::Truncated:      return "Truncated";
        case DecodeError::BadLength:      return "BadLength";
        case DecodeError::UnknownType:    return "UnknownType";
        case DecodeError::LengthMismatch: return "LengthMismatch";
    }
    return "?";
}

// Big-endian load: memcpy then a byteswap builtin. Two instructions (`ldur` + `rev`)
// on both compilers this project builds with.
//
// NOT std::byteswap: that is C++23 and this project is C++20.
//
// NOT a byte-assembly loop either, which is the tempting standard-only alternative.
// Measured on the same source at -O3: clang folds the loop to `ldur`+`rev`, but
// GCC 13 does NOT -- it emits 8 `ldrb` plus 7 `orr`, 15 instructions instead of 2.
// CI builds with GCC, and this function runs once per field on 300M messages, so the
// builtin is not a micro-optimisation.
#if defined(__has_builtin)
#if !__has_builtin(__builtin_bswap16) || !__has_builtin(__builtin_bswap32) || \
    !__has_builtin(__builtin_bswap64)
#error "ob::itch::read_be needs the __builtin_bswap family (GCC or Clang)"
#endif
#endif

template <class T>
[[nodiscard]] inline T read_be(const std::byte* p, std::size_t off) noexcept {
    static_assert(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);
    T v{};
    std::memcpy(&v, p + off, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        if constexpr (sizeof(T) == 2) {
            return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
        } else if constexpr (sizeof(T) == 4) {
            return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
        } else {
            return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
        }
    }
    return v;
}

// The 6-byte timestamp. There is no 48-bit load, so it is one 4-byte plus one 2-byte
// big-endian read, which touches EXACTLY six bytes.
//
// Loading 8 bytes and masking would be shorter and is wrong: on a message whose last
// field is the timestamp it reads two bytes past the end, which is a buffer overrun
// that ASan only catches when the message happens to sit at the end of an allocation.
//
// A 6-iteration byte loop is also correct, but measured at 12 instructions on GCC 13
// and 14 on clang, against 7 for this form on both.
[[nodiscard]] inline Nanos read_u48(const std::byte* p, std::size_t off) noexcept {
    const Nanos hi = read_be<std::uint32_t>(p, off);      // bytes 0..3
    const Nanos lo = read_be<std::uint16_t>(p, off + 4);  // bytes 4..5
    return (hi << 16) | lo;
}

[[nodiscard]] inline char read_char(const std::byte* p, std::size_t off) noexcept {
    return static_cast<char>(std::to_integer<std::uint8_t>(p[off]));
}

namespace detail {
// Fills the 11-byte common header. The caller has already validated the length.
inline void read_header(const std::byte* b, Header& h) noexcept {
    h.type            = static_cast<MsgType>(std::to_integer<std::uint8_t>(b[0]));
    h.stock_locate    = read_be<std::uint16_t>(b, 1);
    h.tracking_number = read_be<std::uint16_t>(b, 3);
    h.timestamp       = read_u48(b, 5);
}

// Defined in Tasks 4 and 5. Returns false for a type this build does not decode.
[[nodiscard]] bool read_body(MsgType t, const std::byte* b, ItchMessage& m) noexcept;
}  // namespace detail

[[nodiscard]] inline DecodeResult decode(std::span<const std::byte> in,
                                         ItchMessage& out) noexcept {
    if (in.size() < kLengthPrefixSize) {
        return {DecodeError::Truncated, 0};
    }
    const std::uint16_t len = read_be<std::uint16_t>(in.data(), 0);

    // Unframeable: the caller cannot know how far to skip, so consume nothing.
    // Note the cap is kMaxFrameLength, not kMaxBodySize: a 60-byte message type added
    // to a future ITCH version must be SKIPPABLE, not fatal. It falls through to the
    // UnknownType branch below, which reports `consumed` and keeps the reader in sync.
    if (len < kCommonHeaderSize || len > kMaxFrameLength) {
        return {DecodeError::BadLength, 0};
    }
    if (in.size() < kLengthPrefixSize + len) {
        return {DecodeError::Truncated, 0};
    }

    // From here the frame is intact, so every remaining error is SKIPPABLE and
    // reports `consumed` so the caller stays in sync.
    const std::size_t total = kLengthPrefixSize + len;
    const std::byte*  b     = in.data() + kLengthPrefixSize;
    const auto        type  = static_cast<MsgType>(std::to_integer<std::uint8_t>(b[0]));

    const std::size_t expect = body_size(type);
    if (expect == 0) {
        return {DecodeError::UnknownType, total};
    }
    if (expect != len) {
        return {DecodeError::LengthMismatch, total};
    }

    detail::read_header(b, out.header);
    if (!detail::read_body(type, b, out)) {
        return {DecodeError::UnknownType, total};
    }
    return {DecodeError::None, total};
}

}  // namespace ob::itch
```

- [ ] **Step 4: Add a temporary `read_body` so the framing tests can run**

At the bottom of `decoder.hpp`, inside `namespace detail`, before the closing brace of
`namespace ob::itch`. Task 4 replaces the body of this function; the signature stays.

```cpp
namespace detail {
inline bool read_body(MsgType t, const std::byte* b, ItchMessage& m) noexcept {
    // Task 4 fills in the order messages and Task 5 the rest. Until then only the
    // header is populated, which is all the framing tests examine.
    if (t == MsgType::SystemEvent) {
        m.system_event.h          = m.header;
        m.system_event.event_code = read_char(b, 11);
        return true;
    }
    static_cast<void>(b);
    return body_size(t) != 0;
}
}  // namespace detail
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build && ./build/tests/ob_tests '--gtest_filter=ItchDecoder.*'
```

Expected: PASS, 9 tests. `WalksTheRealSliceEndToEnd` is the important one: it proves
the framing survives 350,000 real messages with zero errors and no lost sync. **Run
it from the repository root** so the relative `testdata/` path resolves.

- [ ] **Step 6: Commit**

```bash
git add include/ob/itch/decoder.hpp tests/test_itch_decoder.cpp tests/CMakeLists.txt
git commit -m "feat(itch): framing and common header, walks 350k real messages clean"
```

---

## Task 4: Order message bodies

**Files:** Modify `include/ob/itch/decoder.hpp`, `tests/test_itch_decoder.cpp`.

**Interfaces:** No new API. `detail::read_body` now decodes `A F E C X D U P Q B`, the messages the book actually consumes.

These ten carry every book mutation. Getting a field offset wrong here corrupts the
book silently, so each one is tested against a hand-built frame with known values,
and then against the real file.

- [ ] **Step 1: Write the failing tests** — one per message type, with the exact field offsets:

```cpp
// Appended to tests/test_itch_decoder.cpp, inside the anonymous namespace.

// AddOrder, 36 bytes: header(11) ref(8) side(1) shares(4) stock(8) price(4)
TEST(ItchDecoder, DecodesAddOrder) {
    std::vector<std::uint8_t> body(36, 0);
    body[0] = 'A';
    body[1] = 0x00; body[2] = 0x2a;              // stock_locate 42
    body[5] = 0x0a; body[10] = 0x43;             // timestamp bytes
    body[18] = 0x7b;                              // order_ref low byte -> 123
    body[19] = 'B';                               // buy
    body[20] = 0; body[21] = 0; body[22] = 0x01; body[23] = 0xf4;  // shares 500
    std::memcpy(&body[24], "AAPL    ", 8);
    body[32] = 0x00; body[33] = 0x08; body[34] = 0x73; body[35] = 0x00;  // 553728
    ItchMessage m{};
    const auto r = decode(frame(body), m);
    ASSERT_TRUE(r.ok()) << to_string(r.error);
    EXPECT_EQ(m.header.type, MsgType::AddOrder);
    EXPECT_EQ(m.header.stock_locate, 42u);
    EXPECT_EQ(m.add_order.order_ref, 123u);
    EXPECT_EQ(m.add_order.buy_sell, 'B');
    EXPECT_EQ(m.add_order.shares, 500u);
    EXPECT_EQ(m.add_order.stock.str(), "AAPL");
    EXPECT_EQ(m.add_order.price, 553728u);
}

// AddOrderMPID, 40 bytes: AddOrder(36) + attribution(4)
TEST(ItchDecoder, DecodesAddOrderMpid) {
    std::vector<std::uint8_t> body(40, 0);
    body[0] = 'F';
    body[19] = 'S';
    std::memcpy(&body[24], "MSFT    ", 8);
    std::memcpy(&body[36], "NSDQ", 4);
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.add_order_mpid.stock.str(), "MSFT");
    EXPECT_EQ(m.add_order_mpid.buy_sell, 'S');
    EXPECT_EQ(std::string(m.add_order_mpid.attribution, 4), "NSDQ");
}

// OrderExecuted, 31 bytes: header(11) ref(8) shares(4) match(8)
TEST(ItchDecoder, DecodesOrderExecuted) {
    std::vector<std::uint8_t> body(31, 0);
    body[0] = 'E';
    body[18] = 0x64;                              // ref 100
    body[22] = 0xc8;                              // shares 200 (u32 at 19 ends at byte 22)
    body[30] = 0x07;                              // match 8-byte field at 23 ends at byte 30
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.order_executed.order_ref, 100u);
    EXPECT_EQ(m.order_executed.executed_shares, 200u);
    EXPECT_EQ(m.order_executed.match, 7u);
}

// OrderExecutedWithPrice, 36 bytes: OrderExecuted(31) + printable(1) + price(4)
TEST(ItchDecoder, DecodesOrderExecutedWithPrice) {
    std::vector<std::uint8_t> body(36, 0);
    body[0] = 'C';
    body[18] = 0x64;
    body[31] = 'Y';
    body[34] = 0x27; body[35] = 0x10;             // price 10000 == $1.0000
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.order_executed_price.order_ref, 100u);
    EXPECT_EQ(m.order_executed_price.printable, 'Y');
    EXPECT_EQ(m.order_executed_price.execution_price, 10000u);
}

// OrderCancel, 23 bytes: header(11) ref(8) shares(4)
TEST(ItchDecoder, DecodesOrderCancel) {
    std::vector<std::uint8_t> body(23, 0);
    body[0] = 'X';
    body[18] = 0x09;
    body[22] = 0x32;                              // 50 shares
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.order_cancel.order_ref, 9u);
    EXPECT_EQ(m.order_cancel.cancelled_shares, 50u);
}

// OrderDelete, 19 bytes: header(11) ref(8)
TEST(ItchDecoder, DecodesOrderDelete) {
    std::vector<std::uint8_t> body(19, 0);
    body[0] = 'D';
    body[18] = 0xff;
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.order_delete.order_ref, 255u);
}

// OrderReplace, 35 bytes: header(11) old_ref(8) new_ref(8) shares(4) price(4)
TEST(ItchDecoder, DecodesOrderReplace) {
    std::vector<std::uint8_t> body(35, 0);
    body[0] = 'U';
    body[18] = 0x0a;                              // old 10
    body[26] = 0x0b;                              // new 11
    body[30] = 0x01;                              // shares 1
    body[34] = 0x64;                              // price 100
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.order_replace.original_order_ref, 10u);
    EXPECT_EQ(m.order_replace.new_order_ref, 11u);
    EXPECT_EQ(m.order_replace.shares, 1u);
    EXPECT_EQ(m.order_replace.price, 100u);
}

// TradeNonCross, 44 bytes: header(11) ref(8) side(1) shares(4) stock(8) price(4) match(8)
TEST(ItchDecoder, DecodesTradeNonCross) {
    std::vector<std::uint8_t> body(44, 0);
    body[0] = 'P';
    body[19] = 'B';
    body[23] = 0x0a;                              // 10 shares
    std::memcpy(&body[24], "TSLA    ", 8);
    body[35] = 0x64;                              // price 100
    body[43] = 0x2a;                              // match 42
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.trade_non_cross.stock.str(), "TSLA");
    EXPECT_EQ(m.trade_non_cross.shares, 10u);
    EXPECT_EQ(m.trade_non_cross.match, 42u);
}

// CrossTrade, 40 bytes: header(11) shares(8) stock(8) price(4) match(8) type(1)
TEST(ItchDecoder, DecodesCrossTrade) {
    std::vector<std::uint8_t> body(40, 0);
    body[0] = 'Q';
    body[18] = 0xc8;                              // 200 shares
    std::memcpy(&body[19], "NVDA    ", 8);
    body[30] = 0x64;                              // price 100
    body[38] = 0x05;                              // match 5
    body[39] = 'O';
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.cross_trade.shares, 200u);
    EXPECT_EQ(m.cross_trade.stock.str(), "NVDA");
    EXPECT_EQ(m.cross_trade.cross_type, 'O');
}

// BrokenTrade, 19 bytes: header(11) match(8)
TEST(ItchDecoder, DecodesBrokenTrade) {
    std::vector<std::uint8_t> body(19, 0);
    body[0] = 'B';
    body[18] = 0x63;
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(body), m).ok());
    EXPECT_EQ(m.broken_trade.match, 99u);
}

// The real file is the check that matters: decoded AddOrders must look like a real
// market, not like something that merely parsed without error.
TEST(ItchDecoder, RealAddOrdersHavePlausibleFields) {
    std::ifstream f("testdata/itch_slice_10mb.bin", std::ios::binary);
    ASSERT_TRUE(f) << "run scripts/fetch_itch.sh first";
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                                  raw.size());
    std::size_t off = 0, adds = 0, buys = 0, sells = 0;
    Price4 minpx = ~0u, maxpx = 0;
    while (off < in.size()) {
        ItchMessage m{};
        const auto r = decode(in.subspan(off), m);
        if (r.consumed == 0) break;
        if (r.ok() && m.header.type == MsgType::AddOrder) {
            ++adds;
            const auto& a = m.add_order;
            ASSERT_TRUE(a.buy_sell == 'B' || a.buy_sell == 'S')
                << "side byte was '" << a.buy_sell << "'";
            (a.buy_sell == 'B' ? buys : sells)++;
            ASSERT_GT(a.shares, 0u) << "an AddOrder with zero shares is not real";
            ASSERT_FALSE(a.stock.str().empty());
            if (a.price) { minpx = std::min(minpx, a.price); maxpx = std::max(maxpx, a.price); }
        }
        off += r.consumed;
    }
    // Measured: 47,970 adds, 24,279 buys, 23,691 sells, zero bad side bytes,
    // zero zero-share orders. Floors again, for the reason given in the locate test.
    EXPECT_GT(adds, 30'000u);
    EXPECT_GT(buys, 1'000u);
    EXPECT_GT(sells, 1'000u);
    // Measured on this slice: $0.0004 to $100,000.
    EXPECT_GE(minpx, 1u);
    EXPECT_LE(maxpx, 1'000'000'000u);
}
```

- [ ] **Step 2: Run and watch them fail** — the fields are all zero because `read_body` is still the stub.

- [ ] **Step 3: Implement the order messages in `detail::read_body`**

```cpp
inline bool read_body(MsgType t, const std::byte* b, ItchMessage& m) noexcept {
    switch (t) {
        case MsgType::AddOrder: {
            auto& x = m.add_order;
            x.h         = m.header;
            x.order_ref = read_be<std::uint64_t>(b, 11);
            x.buy_sell  = read_char(b, 19);
            x.shares    = read_be<std::uint32_t>(b, 20);
            x.stock     = Symbol::from_bytes(b + 24);
            x.price     = read_be<std::uint32_t>(b, 32);
            return true;
        }
        case MsgType::AddOrderMpid: {
            auto& x = m.add_order_mpid;
            x.h         = m.header;
            x.order_ref = read_be<std::uint64_t>(b, 11);
            x.buy_sell  = read_char(b, 19);
            x.shares    = read_be<std::uint32_t>(b, 20);
            x.stock     = Symbol::from_bytes(b + 24);
            x.price     = read_be<std::uint32_t>(b, 32);
            std::memcpy(x.attribution, b + 36, 4);
            return true;
        }
        case MsgType::OrderExecuted: {
            auto& x = m.order_executed;
            x.h               = m.header;
            x.order_ref       = read_be<std::uint64_t>(b, 11);
            x.executed_shares = read_be<std::uint32_t>(b, 19);
            x.match           = read_be<std::uint64_t>(b, 23);
            return true;
        }
        case MsgType::OrderExecutedPrice: {
            auto& x = m.order_executed_price;
            x.h               = m.header;
            x.order_ref       = read_be<std::uint64_t>(b, 11);
            x.executed_shares = read_be<std::uint32_t>(b, 19);
            x.match           = read_be<std::uint64_t>(b, 23);
            x.printable       = read_char(b, 31);
            x.execution_price = read_be<std::uint32_t>(b, 32);
            return true;
        }
        case MsgType::OrderCancel: {
            auto& x = m.order_cancel;
            x.h                = m.header;
            x.order_ref        = read_be<std::uint64_t>(b, 11);
            x.cancelled_shares = read_be<std::uint32_t>(b, 19);
            return true;
        }
        case MsgType::OrderDelete: {
            auto& x = m.order_delete;
            x.h         = m.header;
            x.order_ref = read_be<std::uint64_t>(b, 11);
            return true;
        }
        case MsgType::OrderReplace: {
            auto& x = m.order_replace;
            x.h                  = m.header;
            x.original_order_ref = read_be<std::uint64_t>(b, 11);
            x.new_order_ref      = read_be<std::uint64_t>(b, 19);
            x.shares             = read_be<std::uint32_t>(b, 27);
            x.price              = read_be<std::uint32_t>(b, 31);
            return true;
        }
        case MsgType::TradeNonCross: {
            auto& x = m.trade_non_cross;
            x.h         = m.header;
            x.order_ref = read_be<std::uint64_t>(b, 11);
            x.buy_sell  = read_char(b, 19);
            x.shares    = read_be<std::uint32_t>(b, 20);
            x.stock     = Symbol::from_bytes(b + 24);
            x.price     = read_be<std::uint32_t>(b, 32);
            x.match     = read_be<std::uint64_t>(b, 36);
            return true;
        }
        case MsgType::CrossTrade: {
            auto& x = m.cross_trade;
            x.h           = m.header;
            x.shares      = read_be<std::uint64_t>(b, 11);
            x.stock       = Symbol::from_bytes(b + 19);
            x.cross_price = read_be<std::uint32_t>(b, 27);
            x.match       = read_be<std::uint64_t>(b, 31);
            x.cross_type  = read_char(b, 39);
            return true;
        }
        case MsgType::BrokenTrade: {
            auto& x = m.broken_trade;
            x.h     = m.header;
            x.match = read_be<std::uint64_t>(b, 11);
            return true;
        }
        case MsgType::SystemEvent: {
            auto& x = m.system_event;
            x.h          = m.header;
            x.event_code = read_char(b, 11);
            return true;
        }
        default:
            // Task 5 adds the administrative messages. Until then they are known
            // types whose bodies are not yet extracted, which the framing already
            // reports correctly.
            return body_size(t) != 0;
    }
}
```

- [ ] **Step 4: Run the tests** — expect PASS, 20 `ItchDecoder` tests.

- [ ] **Step 5: Commit**

```bash
git add include/ob/itch/decoder.hpp tests/test_itch_decoder.cpp
git commit -m "feat(itch): decode the ten order and trade messages"
```

---

## Task 5: Administrative message bodies

**Files:** Modify `include/ob/itch/decoder.hpp`, `tests/test_itch_decoder.cpp`.

**Interfaces:** No new API. `read_body` now covers all 22 types, so `StockDirectory` can build the `stock_locate` → `Symbol` map Phase 6 routes on.

`StockDirectory` is the one that matters operationally: every later message carries a
`stock_locate` rather than a symbol, and `R` is the only place the mapping appears.
The measured slice contains **8,906** of them.

- [ ] **Step 1: Write the failing tests**

```cpp
// StockDirectory, 39 bytes. The only source of the locate -> symbol mapping.
TEST(ItchDecoder, DecodesStockDirectoryFromRealBytes) {
    // Byte for byte, the real file's second message.
    const auto b = frame({0x52, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x53, 0xa2, 0x88, 0x70,
                          0x58, 'A', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 'N', ' ',
                          0x00, 0x00, 0x00, 0x64, 'N', 'C', 'Z', ' ', 'P', 'N', ' ',
                          '1', 'N', 0x00, 0x00, 0x00, 0x00, 'N'});
    ItchMessage m{};
    const auto r = decode(b, m);
    ASSERT_TRUE(r.ok()) << to_string(r.error);
    EXPECT_EQ(m.header.stock_locate, 1u);
    EXPECT_EQ(m.stock_directory.stock.str(), "A");
    EXPECT_EQ(m.stock_directory.market_category, 'N');
    EXPECT_EQ(m.stock_directory.round_lot_size, 100u);
    EXPECT_EQ(m.stock_directory.issue_classification, 'C');
    EXPECT_EQ(m.stock_directory.authenticity, 'P');
    EXPECT_EQ(m.stock_directory.luld_tier, '1');
}

TEST(ItchDecoder, DecodesTradingActionAndRegSho) {
    std::vector<std::uint8_t> ta(25, 0);
    ta[0] = 'H';
    std::memcpy(&ta[11], "IBM     ", 8);
    ta[19] = 'T';
    std::memcpy(&ta[21], "    ", 4);
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(ta), m).ok());
    EXPECT_EQ(m.trading_action.stock.str(), "IBM");
    EXPECT_EQ(m.trading_action.state, 'T');

    std::vector<std::uint8_t> ry(20, 0);
    ry[0] = 'Y';
    std::memcpy(&ry[11], "GME     ", 8);
    ry[19] = '1';
    ASSERT_TRUE(decode(frame(ry), m).ok());
    EXPECT_EQ(m.reg_sho.stock.str(), "GME");
    EXPECT_EQ(m.reg_sho.action, '1');
}

TEST(ItchDecoder, DecodesMarketParticipantPosition) {
    std::vector<std::uint8_t> b(26, 0);
    b[0] = 'L';
    std::memcpy(&b[11], "NSDQ", 4);
    std::memcpy(&b[15], "AAPL    ", 8);
    b[23] = 'Y'; b[24] = 'N'; b[25] = 'A';
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(b), m).ok());
    EXPECT_EQ(std::string(m.market_participant.mpid, 4), "NSDQ");
    EXPECT_EQ(m.market_participant.stock.str(), "AAPL");
    EXPECT_EQ(m.market_participant.primary_market_maker, 'Y');
    EXPECT_EQ(m.market_participant.state, 'A');
}

TEST(ItchDecoder, DecodesNoii) {
    std::vector<std::uint8_t> b(50, 0);
    b[0] = 'I';
    b[18] = 0x64;                                 // paired 100
    b[26] = 0x0a;                                 // imbalance 10
    b[27] = 'B';
    std::memcpy(&b[28], "SPY     ", 8);
    b[39] = 0x64;                                 // far
    b[43] = 0xc8;                                 // near
    b[47] = 0x32;                                 // current reference
    b[48] = 'O'; b[49] = 'L';
    ItchMessage m{};
    ASSERT_TRUE(decode(frame(b), m).ok());
    EXPECT_EQ(m.noii.paired_shares, 100u);
    EXPECT_EQ(m.noii.imbalance_shares, 10u);
    EXPECT_EQ(m.noii.imbalance_direction, 'B');
    EXPECT_EQ(m.noii.stock.str(), "SPY");
    EXPECT_EQ(m.noii.cross_type, 'O');
}

// Every single message in the real slice must decode with no error at all.
TEST(ItchDecoder, EveryMessageInTheRealSliceDecodesCleanly) {
    std::ifstream f("testdata/itch_slice_10mb.bin", std::ios::binary);
    ASSERT_TRUE(f) << "run scripts/fetch_itch.sh first";
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    std::span<const std::byte> in(reinterpret_cast<const std::byte*>(raw.data()),
                                  raw.size());
    std::map<char, std::size_t> counts;
    std::map<ob::itch::StockLocate, std::string> locate_to_symbol;
    std::size_t off = 0;
    while (off < in.size()) {
        ItchMessage m{};
        const auto r = decode(in.subspan(off), m);
        ASSERT_NE(r.consumed, 0u) << "frame sync lost at byte " << off;
        ASSERT_TRUE(r.ok()) << "at byte " << off << ": " << to_string(r.error);
        counts[static_cast<char>(m.header.type)]++;
        if (m.header.type == MsgType::StockDirectory) {
            locate_to_symbol[m.header.stock_locate] = m.stock_directory.stock.str();
        }
        off += r.consumed;
    }
    // The locate -> symbol mapping is what Phase 6 routes on, and these three
    // entries are stable for ANY 10 MB prefix: the R block sits at the head of the
    // file, so a 10 MB cut always contains the low locate codes.
    EXPECT_EQ(locate_to_symbol[1], "A");
    EXPECT_EQ(locate_to_symbol[2], "AA");
    EXPECT_EQ(locate_to_symbol[6], "AAL");

    // Floors, not equalities. A measured run of a 10,158,061-byte cut gave exactly
    // L 215,036  A 47,970  D 43,420  X 16,908  R 8,906  H 8,897  Y 8,897  U 4,635
    // E 145  F 43  P 10  S 2, with 8,906 directory entries. Asserting those exactly
    // would bind the test to one cut length, and the cut moves whenever the slice is
    // regenerated or a different day is fetched. Assert the shape instead.
    EXPECT_GT(locate_to_symbol.size(), 5'000u) << "StockDirectory entries";
    EXPECT_GT(counts['L'], 150'000u);
    EXPECT_GT(counts['A'], 30'000u);
    EXPECT_GT(counts['D'], 30'000u);
    EXPECT_GT(counts['R'], 5'000u);
    EXPECT_EQ(counts['S'], 2u) << "start-of-messages and start-of-system-hours";
}
```

Add `#include <map>` to the test file.

- [ ] **Step 2: Run and watch the administrative assertions fail.**

- [ ] **Step 3: Add the remaining cases to `read_body`**, before the `default:`

```cpp
        case MsgType::StockDirectory: {
            auto& x = m.stock_directory;
            x.h                    = m.header;
            x.stock                = Symbol::from_bytes(b + 11);
            x.market_category      = read_char(b, 19);
            x.financial_status     = read_char(b, 20);
            x.round_lot_size       = read_be<std::uint32_t>(b, 21);
            x.round_lots_only      = read_char(b, 25);
            x.issue_classification = read_char(b, 26);
            std::memcpy(x.issue_subtype, b + 27, 2);
            x.authenticity         = read_char(b, 29);
            x.short_sale_threshold = read_char(b, 30);
            x.ipo_flag             = read_char(b, 31);
            x.luld_tier            = read_char(b, 32);
            x.etp_flag             = read_char(b, 33);
            x.etp_leverage         = read_be<std::uint32_t>(b, 34);
            x.inverse              = read_char(b, 38);
            return true;
        }
        case MsgType::TradingAction: {
            auto& x = m.trading_action;
            x.h        = m.header;
            x.stock    = Symbol::from_bytes(b + 11);
            x.state    = read_char(b, 19);
            x.reserved = read_char(b, 20);
            std::memcpy(x.reason, b + 21, 4);
            return true;
        }
        case MsgType::RegSHO: {
            auto& x = m.reg_sho;
            x.h      = m.header;
            x.stock  = Symbol::from_bytes(b + 11);
            x.action = read_char(b, 19);
            return true;
        }
        case MsgType::MarketParticipant: {
            auto& x = m.market_participant;
            x.h = m.header;
            std::memcpy(x.mpid, b + 11, 4);
            x.stock                = Symbol::from_bytes(b + 15);
            x.primary_market_maker = read_char(b, 23);
            x.market_maker_mode    = read_char(b, 24);
            x.state                = read_char(b, 25);
            return true;
        }
        case MsgType::MwcbDeclineLevel: {
            auto& x = m.mwcb_decline_level;
            x.h  = m.header;
            x.l1 = read_be<std::uint64_t>(b, 11);
            x.l2 = read_be<std::uint64_t>(b, 19);
            x.l3 = read_be<std::uint64_t>(b, 27);
            return true;
        }
        case MsgType::MwcbStatus: {
            auto& x = m.mwcb_status;
            x.h              = m.header;
            x.breached_level = read_char(b, 11);
            return true;
        }
        case MsgType::IpoQuotingPeriod: {
            auto& x = m.ipo_quoting_period;
            x.h                 = m.header;
            x.stock             = Symbol::from_bytes(b + 11);
            x.release_time      = read_be<std::uint32_t>(b, 19);
            x.release_qualifier = read_char(b, 23);
            x.ipo_price         = read_be<std::uint32_t>(b, 24);
            return true;
        }
        case MsgType::LuldAuctionCollar: {
            auto& x = m.luld_auction_collar;
            x.h               = m.header;
            x.stock           = Symbol::from_bytes(b + 11);
            x.reference_price = read_be<std::uint32_t>(b, 19);
            x.upper           = read_be<std::uint32_t>(b, 23);
            x.lower           = read_be<std::uint32_t>(b, 27);
            x.extension       = read_be<std::uint32_t>(b, 31);
            return true;
        }
        case MsgType::OperationalHalt: {
            auto& x = m.operational_halt;
            x.h           = m.header;
            x.stock       = Symbol::from_bytes(b + 11);
            x.market_code = read_char(b, 19);
            x.action      = read_char(b, 20);
            return true;
        }
        case MsgType::Noii: {
            auto& x = m.noii;
            x.h                        = m.header;
            x.paired_shares            = read_be<std::uint64_t>(b, 11);
            x.imbalance_shares         = read_be<std::uint64_t>(b, 19);
            x.imbalance_direction      = read_char(b, 27);
            x.stock                    = Symbol::from_bytes(b + 28);
            x.far_price                = read_be<std::uint32_t>(b, 36);
            x.near_price               = read_be<std::uint32_t>(b, 40);
            x.current_reference_price  = read_be<std::uint32_t>(b, 44);
            x.cross_type               = read_char(b, 48);
            x.price_variation_indicator = read_char(b, 49);
            return true;
        }
        case MsgType::Rpii: {
            auto& x = m.rpii;
            x.h             = m.header;
            x.stock         = Symbol::from_bytes(b + 11);
            x.interest_flag = read_char(b, 19);
            return true;
        }
```

- [ ] **Step 4: Run the tests** — expect PASS. `EveryMessageInTheRealSliceDecodesCleanly` is the acceptance test for this task: **every one of ~350,000 real messages decodes with no error**, and the locate map has exactly 8,906 entries.

- [ ] **Step 5: Run under ASan and UBSan**

```bash
cmake --build build-asan && \
  ASAN_OPTIONS=abort_on_error=1 ./build-asan/tests/ob_tests '--gtest_filter=Itch*'
```

Expected: PASS with no diagnostics. An overrun here would mean a field offset is past
the end of its message.

- [ ] **Step 6: Commit**

```bash
git add include/ob/itch/decoder.hpp tests/test_itch_decoder.cpp
git commit -m "feat(itch): decode all 22 message types; 350k real messages clean"
```

---

## Task 6: Golden-file test and the `itch_stat` tool

**Files:** Create `tools/itch_stat.cpp`, `tests/test_itch_golden.cpp`, `testdata/golden_first_1000.txt`. Modify `tools/CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces:** Produces the `ob_itch_stat` executable and a committed golden dump.

A golden file makes any unintended decoder change visible as a diff. The tool gives
the first real throughput number.

- [ ] **Step 1: Write `tools/itch_stat.cpp`** — decodes a file, reports counts by type, the locate→symbol map size, error counts by kind, bytes/message, and messages/sec, with `--limit` for partial runs and `--golden N` to emit the dump.

- [ ] **Step 2: Generate the golden file**

```bash
./build/tools/ob_itch_stat --in testdata/itch_slice_10mb.bin --golden 1000 \
  > testdata/golden_first_1000.txt
head -5 testdata/golden_first_1000.txt
wc -l testdata/golden_first_1000.txt
```

Expected: 1,000 lines, the first being the `SystemEvent`. **Read the first twenty by
hand before committing them.** A golden file generated from a buggy decoder locks the
bug in, which is worse than having no golden file.

- [ ] **Step 3: Write `tests/test_itch_golden.cpp`** — re-decodes the first 1,000 messages and compares to the committed dump line by line, reporting the first differing line.

- [ ] **Step 4: Measure throughput on the full file**

```bash
./build/tools/ob_itch_stat --in data/12302019.NASDAQ_ITCH50
```

Expected: roughly 300 M messages and a messages/sec figure. **Report the full-file
number, not the slice number.** Decoding the committed 10 MB slice measured
**267 M msgs/s (7.6 GB/s)** at `-O3`, but that slice is small enough to sit in cache
and the file was already warm, so it is an upper bound on the decoder and not a claim
anyone can defend. The full 8.5 GB file pays real memory bandwidth and page faults,
and its number is the honest one.

Record both, and say which is which. **This is the first half of the resume line**, so
it is the number most likely to be challenged; Phase 6 adds the book-update latency.

- [ ] **Step 5: Commit.**

---

## Task 7: Fuzz the decoder

**Files:** Create `fuzz/fuzz_itch_decoder.cpp`, `fuzz/corpus_itch/`. Modify `fuzz/CMakeLists.txt`.

Same shape as the existing `fuzz_wire_parser`: arbitrary bytes at `decode`, assert it
always terminates with a typed result, never overruns, and always makes forward
progress.

**Seeding the corpus is mandatory, not a nicety, and here is the measurement that
says so.** 200,000 buffers of uniformly random bytes were fed to this decoder:
**zero reached the body decoder.** All 200,000 stopped at the framing checks. A random
2-byte prefix almost never lands in `[11, 1024]` *and* equals the exact verified size
of the type in the next byte, so an unseeded fuzzer explores only `decode`'s first ten
lines and never tests a single field offset -- the part most likely to be wrong.
Seed from real frames.

- [ ] **Step 1: Write the target** (mirroring `fuzz/fuzz_wire_parser.cpp`, including the forward-progress assertion).
- [ ] **Step 2: Seed the corpus** by splitting the first 2,000 real messages into individual files.
- [ ] **Step 3: Run 300 s in Docker** — Apple clang ships no libFuzzer, and `ASAN_OPTIONS=detect_leaks=0` is needed for the musl false positive. Both are already documented in `scripts/fuzz.sh`.
- [ ] **Step 4: Minimise the corpus and commit it.**
- [ ] **Step 5: Add the target to the CI fuzz job. Commit.**

---

## Task 8: CI and documentation

- [ ] **Step 1: Add an `itch` job to CI** that decodes the committed slice and asserts zero errors. It must NOT download the multi-GB file; the 10 MB slice is why it exists.
- [ ] **Step 2: Update `README.md`** with the ITCH decoder section, the measured messages/sec, and how to fetch data.
- [ ] **Step 3: Update the phase index. Commit.**

---

## Self-review

**Spec coverage.** S1 is Tasks 3–5 and 7. Spec 3.1 (acquisition) is Task 1. Spec 3.2 and 3.3 (framing and inventory) are Tasks 2–5, with every size from the measured table. Spec 7's "unknown message type" and "length field is nonsense" rows are Task 3's tests. S4's throughput is first measured in Task 6. Spec 6.2 and 6.3 belong to Phase 6 and are deliberately out of scope here: this phase does not touch the book.

**Placeholder scan.** No "TBD", no "add error handling". Every code step carries real code. Tasks 6–8 are lighter than 1–5 by design: they wire together components fully specified above, and their acceptance criteria are concrete (1,000-line golden file, zero decode errors on the slice, 300 s clean fuzz).

**Type consistency.** `DecodeResult{error, consumed}` and `.ok()` match the existing `ob::wire` shape deliberately, so the two decoders read alike. `Symbol::from_bytes`/`str()`, `read_be<T>`, `read_u48`, `body_size`, `kCommonHeaderSize`, `kLengthPrefixSize` are spelled identically in Tasks 2–7. `ItchMessage.header` is populated before `read_body`, which every case copies into its own `h` field.

## Verification of this plan

Tasks 2 through 5 are not a proposal. Every C++ block in them was extracted from this
document, assembled into `types.hpp`, `decoder.hpp` and one test binary, compiled, and
run against the real 10 MB slice before the plan was finalised. The results below are
what the executor should reproduce.

| Check | Result |
|---|---|
| Apple Clang, `-std=c++20 -O2 -Wall -Wextra` | compiles clean, no warnings |
| GCC 13, `-std=c++20 -O2 -Wall -Wextra -Werror` | compiles clean |
| All Task 2-5 tests, both compilers | **853,843 assertions, 0 failures** |
| ASan + UBSan | clean |
| Real slice walked end to end | 354,869 messages, **0 decode errors**, bytes consumed == file size |
| Directory map | 8,906 entries; locate 1/2/6 == `A`/`AA`/`AAL` |
| AddOrder sanity | 47,970 adds, 24,279 buys, 23,691 sells, **0 bad side bytes, 0 zero-share orders** |
| Price range | 4 to 1,000,000,000 (i.e. $0.0004 to $100,000) |
| Truncation sweep, 20,001 prefix lengths | no overread under ASan |
| Mid-message truncation, **204,775 cases** | every one refused with `consumed == 0` |
| 200,000 random buffers | all typed errors, no overrun, no hang |
| 200,000 single-byte corruptions, re-walked | always terminated |
| Decode throughput, 10 MB in-cache slice, `-O3` | 267 M msgs/s (an upper bound, see Task 6) |

**That pass found five defects in this plan and they are fixed in the text above.**
Recording them because each one would have cost the executor real time:

1. The decimal for the timestamp `0x0a11ea0e8c43` was wrong: it is 11,072,057,543,747,
   not 11,072,058,233,411. Two assertions would have failed against correct code.
2. In the `OrderExecuted` test the shares byte was written one position too far right.
   A `uint32` at body offset 19 ends at byte **22**, not 23; byte 23 is the first byte
   of the match number. The test would have failed, and the natural "fix" is to change
   the decoder rather than the test, which corrupts every execution.
3. The exact message counts were asserted with `EXPECT_EQ`, binding the tests to one
   slice length. They are floors now, with the measured values in comments.
4. `std::byteswap` is **C++23**; this project is C++20. It does not compile.
5. The obvious standard-only replacement, a byte-assembly loop, is **15 instructions on
   GCC 13 against 2 on Clang** at `-O3`. CI builds with GCC and this runs 300M times,
   so the plan now specifies `__builtin_bswap*` and explains why.

Defect 5 is the one worth dwelling on: it was invisible to reasoning and to testing.
Only reading the generated assembly on *both* compilers exposed it.

**Three hazards for the executor.**
1. **Everything is big-endian.** The predecessor's `ob::wire` is little-endian. Copying its `detail::get` would decode garbage that still parses.
2. **The 6-byte timestamp must not be read as 8 bytes and masked.** On a message ending at the timestamp that overruns the buffer, and ASan will catch it only if the message happens to sit at the end of an allocation.
3. **Do not commit `data/`.** One file is 3.5 GB. `.gitignore` is Task 1 Step 3, before any chance to stage it.
4. **`consumed == 0` means the reader must stop; `consumed > 0` means it advances.**
   Getting this backwards on the `UnknownType` branch turns one unrecognised message
   into either an infinite loop or a dead replay. Task 3's tests pin both directions.
