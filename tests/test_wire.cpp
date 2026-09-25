#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <ob/fast_engine.hpp>
#include <ob/wire.hpp>
#include <vector>

#include "../bench/scenarios.hpp"

namespace {

using ob::wire::decode;
using ob::wire::DecodeError;
using ob::wire::encode;

std::vector<std::byte> enc(const ob::Command& c) {
    std::array<std::byte, ob::wire::kMaxMsgSize> buf{};
    const std::size_t                            n = encode(c, buf);
    EXPECT_GT(n, 0u);
    return {buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)};
}

TEST(Wire, RoundTripsEveryOrderTypeAndSide) {
    for (const ob::Side side : {ob::Side::Buy, ob::Side::Sell}) {
        for (const ob::OrderType t :
             {ob::OrderType::Limit, ob::OrderType::Market, ob::OrderType::Ioc, ob::OrderType::Fok,
              ob::OrderType::PostOnly}) {
            const ob::Command in    = ob::make_new(4242, side, t, 10050, 777);
            const auto        bytes = enc(in);
            EXPECT_EQ(bytes.size(), ob::wire::kNewSize);

            ob::Command out{};
            const auto  r = decode(bytes, out);
            ASSERT_TRUE(r.ok()) << ob::wire::to_string(r.error);
            EXPECT_EQ(r.consumed, ob::wire::kNewSize);
            EXPECT_EQ(out.type, in.type);
            EXPECT_EQ(out.id, in.id);
            EXPECT_EQ(out.price, in.price);
            EXPECT_EQ(out.qty, in.qty);
            EXPECT_EQ(out.side, in.side);
            EXPECT_EQ(out.order_type, in.order_type);
        }
    }
}

TEST(Wire, RoundTripsCancel) {
    const ob::Command in    = ob::make_cancel(999999);
    const auto        bytes = enc(in);
    EXPECT_EQ(bytes.size(), ob::wire::kCancelSize);

    ob::Command out{};
    const auto  r = decode(bytes, out);
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(out.type, ob::CommandType::Cancel);
    EXPECT_EQ(out.id, 999999u);
    EXPECT_EQ(out.price, ob::kNoPrice);
}

TEST(Wire, ExtremeFieldValuesSurviveTheRoundTrip) {
    ob::Command in = ob::make_new(0xFFFF'FFFF'FFFF'FFFFull, ob::Side::Sell, ob::OrderType::Fok,
                                  ob::kMaxTick, ob::kMaxOrderQty);
    ob::Command out{};
    const auto  bytes = enc(in);
    ASSERT_TRUE(decode(bytes, out).ok());
    EXPECT_EQ(out.id, in.id);
    EXPECT_EQ(out.price, in.price);
    EXPECT_EQ(out.qty, in.qty);

    in = ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, std::numeric_limits<ob::Ticks>::min(),
                      0);
    const auto b2 = enc(in);
    ASSERT_TRUE(decode(b2, out).ok()) << "the wire layer must carry values the ENGINE "
                                         "will reject; validation belongs to the engine";
    EXPECT_EQ(out.price, std::numeric_limits<ob::Ticks>::min());
    EXPECT_EQ(out.qty, 0u);
}

// EVERY prefix must be reported as truncated, never read past, never crash.
TEST(Wire, EveryTruncatedPrefixIsReportedNotRead) {
    const auto full = enc(ob::make_new(7, ob::Side::Buy, ob::OrderType::Limit, 10000, 5));
    for (std::size_t n = 0; n < full.size(); ++n) {
        ob::Command out{};
        const auto  r = decode(std::span<const std::byte>(full.data(), n), out);
        EXPECT_FALSE(r.ok()) << "prefix length " << n << " decoded as valid";
        EXPECT_EQ(r.error, DecodeError::Truncated) << "prefix length " << n;
        EXPECT_EQ(r.consumed, 0u) << "a truncated frame must consume nothing";
    }
}

TEST(Wire, EncodeRefusesABufferThatIsTooSmall) {
    const ob::Command c = ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 1);
    for (std::size_t n = 0; n < ob::wire::kNewSize; ++n) {
        // `volatile` keeps the size opaque to constant folding. Without it GCC
        // unrolls this loop, inlines encode against a known 23-byte buffer, fails to
        // propagate encode's own size guard through std::span, and reports the frame
        // copy as out of bounds. Opaque sizes also make this a better test: it
        // exercises the real runtime path rather than a constant-folded one.
        volatile std::size_t   opaque = n;
        std::vector<std::byte> small(opaque);
        EXPECT_EQ(encode(c, small), 0u) << "buffer size " << n;
    }
}

TEST(Wire, LengthBelowTheHeaderIsUnframeableAndConsumesNothing) {
    for (std::uint16_t len : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{3}}) {
        std::array<std::byte, 24> buf{};
        std::memcpy(buf.data(), &len, 2);
        buf[2] = std::byte{1};
        buf[3] = std::byte{1};
        ob::Command out{};
        const auto  r = decode(buf, out);
        EXPECT_EQ(r.error, DecodeError::BadLength) << "len " << len;
        EXPECT_EQ(r.consumed, 0u) << "an unframeable message cannot be skipped";
    }
}

TEST(Wire, LengthAboveTheMaximumIsUnframeable) {
    std::array<std::byte, 24> buf{};
    const std::uint16_t       len = 9999;
    std::memcpy(buf.data(), &len, 2);
    buf[2] = std::byte{1};
    buf[3] = std::byte{1};
    ob::Command out{};
    const auto  r = decode(buf, out);
    EXPECT_EQ(r.error, DecodeError::BadLength);
    EXPECT_EQ(r.consumed, 0u);
}

// A well-framed message of an unknown type must be SKIPPABLE. That is the entire
// reason the length lives in the header rather than being implied by the type.
TEST(Wire, UnknownTypeIsSkippableWithoutLosingFrameSync) {
    std::array<std::byte, 24> buf{};
    const std::uint16_t       len = 16;
    std::memcpy(buf.data(), &len, 2);
    buf[2] = std::byte{99};  // a type this build has never heard of
    buf[3] = std::byte{1};
    ob::Command out{};
    const auto  r = decode(buf, out);
    EXPECT_EQ(r.error, DecodeError::UnknownType);
    EXPECT_EQ(r.consumed, 16u) << "the caller must be able to skip and stay in sync";
}

TEST(Wire, UnknownVersionIsSkippable) {
    auto bytes = enc(ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 1));
    bytes[3]   = std::byte{200};
    ob::Command out{};
    const auto  r = decode(bytes, out);
    EXPECT_EQ(r.error, DecodeError::UnknownVersion);
    EXPECT_EQ(r.consumed, ob::wire::kNewSize);
}

TEST(Wire, LengthDisagreeingWithTheTypeIsRejected) {
    auto                bytes = enc(ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 1));
    const std::uint16_t wrong = ob::wire::kCancelSize;  // New message claiming Cancel's size
    std::memcpy(bytes.data(), &wrong, 2);
    ob::Command out{};
    const auto  r = decode(bytes, out);
    EXPECT_EQ(r.error, DecodeError::BadLength);
}

// An out-of-range enum value is undefined behaviour the moment anything switches on
// it, so the decoder validates rather than trusts.
TEST(Wire, OutOfRangeEnumValuesAreRejectedNotCastBlindly) {
    auto bytes = enc(ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 1));
    bytes[20]  = std::byte{7};  // side
    ob::Command out{};
    EXPECT_EQ(decode(bytes, out).error, DecodeError::BadLength);

    bytes     = enc(ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 1));
    bytes[21] = std::byte{42};  // order type
    EXPECT_EQ(decode(bytes, out).error, DecodeError::BadLength);
}

// A stream of mixed messages must decode in order, including skipping the bad ones.
TEST(Wire, DecodesAStreamAndSkipsUnknownFramesInTheMiddle) {
    std::vector<std::byte> stream;
    const auto             append = [&stream](const std::vector<std::byte>& b) {
        stream.insert(stream.end(), b.begin(), b.end());
    };
    append(enc(ob::make_new(1, ob::Side::Buy, ob::OrderType::Limit, 10000, 10)));
    // An unknown-but-well-framed message wedged into the middle.
    std::vector<std::byte> unknown(16);
    const std::uint16_t    ul = 16;
    std::memcpy(unknown.data(), &ul, 2);
    unknown[2] = std::byte{77};
    unknown[3] = std::byte{1};
    append(unknown);
    append(enc(ob::make_cancel(1)));

    std::vector<ob::OrderId> got;
    std::size_t              skipped = 0, off = 0;
    while (off < stream.size()) {
        ob::Command c{};
        const auto  r = decode(std::span<const std::byte>(stream).subspan(off), c);
        if (r.consumed == 0) {
            break;  // unframeable; a real reader would resynchronise
        }
        if (r.ok()) {
            got.push_back(c.id);
        } else {
            ++skipped;
        }
        off += r.consumed;
    }
    EXPECT_EQ(got, (std::vector<ob::OrderId>{1, 1}));
    EXPECT_EQ(skipped, 1u);
    EXPECT_EQ(off, stream.size()) << "frame sync was lost";
}

// THE acceptance test for the whole wire layer: a scenario driven through
// encode -> a byte stream -> a small buffer with straddles -> decode -> the engine
// must leave the book in exactly the state the in-process path does. If the two ever
// disagree, the protocol is lying about what it carries.
TEST(Wire, IngestThroughAStraddlingBufferMatchesTheInProcessPath) {
    using namespace ob::bench;
    const auto stream = build(Scenario::MixedRealistic, 20260922, 50000);

    // 1. In-process reference.
    ob::FastEngine         direct;
    std::vector<ob::Event> ds(1 << 16);
    ob::EventBuffer        db(ds.data(), ds.size());
    std::uint64_t          direct_trades = 0;
    for (const ob::Command& c : stream) {
        db.clear();
        direct.submit(c, db);
        for (const ob::Event& e : db) {
            if (e.type == ob::EventType::Trade) {
                ++direct_trades;
            }
        }
    }

    // 2. Encode the whole stream to bytes.
    std::vector<std::byte>                       wirebytes;
    std::array<std::byte, ob::wire::kMaxMsgSize> tmp{};
    for (const ob::Command& c : stream) {
        const std::size_t n = encode(c, tmp);
        ASSERT_GT(n, 0u);
        wirebytes.insert(wirebytes.end(), tmp.begin(),
                         tmp.begin() + static_cast<std::ptrdiff_t>(n));
    }

    // 3. Decode through a buffer deliberately too small to hold whole messages
    //    cleanly, so frames straddle refills and the compaction path is exercised.
    constexpr std::size_t  kBuf = 100;  // not a multiple of either message size
    ob::FastEngine         viawire;
    std::vector<ob::Event> ws(1 << 16);
    ob::EventBuffer        wb(ws.data(), ws.size());

    std::vector<std::byte> buf(kBuf);
    std::size_t            filled = 0, src = 0, decoded = 0, straddles = 0;
    std::uint64_t          wire_trades = 0;
    while (src < wirebytes.size() || filled > 0) {
        const std::size_t take = std::min(buf.size() - filled, wirebytes.size() - src);
        std::memcpy(buf.data() + filled, wirebytes.data() + src, take);
        filled += take;
        src += take;

        std::size_t off = 0;
        for (;;) {
            ob::Command c{};
            const auto  r = decode(std::span<const std::byte>(buf.data() + off, filled - off), c);
            if (r.consumed == 0) {
                break;
            }
            ASSERT_TRUE(r.ok()) << ob::wire::to_string(r.error);
            wb.clear();
            viawire.submit(c, wb);
            for (const ob::Event& e : wb) {
                if (e.type == ob::EventType::Trade) {
                    ++wire_trades;
                }
            }
            ++decoded;
            off += r.consumed;
        }
        const std::size_t rest = filled - off;
        if (rest > 0 && off > 0) {
            std::memmove(buf.data(), buf.data() + off, rest);
            ++straddles;
        }
        filled = rest;
        if (take == 0 && off == 0) {
            break;  // no progress possible
        }
    }

    EXPECT_EQ(decoded, stream.size());
    EXPECT_GT(straddles, 100u) << "the straddle path was never exercised";
    EXPECT_EQ(wire_trades, direct_trades);
    EXPECT_EQ(viawire.live_order_count(), direct.live_order_count());
    EXPECT_EQ(viawire.best_bid(), direct.best_bid());
    EXPECT_EQ(viawire.best_ask(), direct.best_ask());
    EXPECT_TRUE(viawire.check_internal_invariants().ok);
}

}  // namespace
