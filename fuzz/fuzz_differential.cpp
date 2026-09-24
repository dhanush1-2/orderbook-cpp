// fuzz/fuzz_differential.cpp
//
// libFuzzer target. Decodes the input as a command stream and asserts that
// ReferenceEngine and FastEngine agree, event for event, plus every invariant.
//
// The decoder deliberately does NOT sanitise: prices span the full int32 range,
// quantities include 0, ids are arbitrary and non-monotonic. Producing inputs the
// engine must REJECT is the point, because that is where the flat ladder's bounds
// checking lives.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <ob/invariants.hpp>
#include <ob/reference_engine.hpp>
#include <vector>

namespace {

// 12 bytes per command, chosen so a single flipped byte is a meaningful mutation.
constexpr std::size_t kBytesPerCommand = 12;

bool decode(const std::uint8_t* data, std::size_t size, std::size_t i, ob::Command& out) {
    const std::size_t off = i * kBytesPerCommand;
    if (off + kBytesPerCommand > size) {
        return false;
    }
    const std::uint8_t* p = data + off;

    std::uint32_t id_lo;
    std::int32_t  price;
    std::uint16_t qty;
    std::memcpy(&id_lo, p, 4);
    std::memcpy(&price, p + 4, 4);
    std::memcpy(&qty, p + 8, 2);
    const std::uint8_t flags = p[10];
    // p[11] is spare: it exists so the record size stays a round 12 bytes.

    out            = ob::Command{};
    out.id         = id_lo;
    out.price      = price;
    out.qty        = qty;
    out.side       = (flags & 0x01) ? ob::Side::Sell : ob::Side::Buy;
    out.order_type = static_cast<ob::OrderType>((flags >> 1) % 5);
    out.type       = (flags & 0x40) ? ob::CommandType::Cancel : ob::CommandType::New;
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kCapacity = 4096;
    ob::ReferenceEngine   ref(kCapacity);
    ob::FastEngine        fast(ob::FastEngine::Config{kCapacity});

    std::vector<ob::Event> rs(1 << 16), fs(1 << 16);
    ob::EventBuffer        rb(rs.data(), rs.size());
    ob::EventBuffer        fb(fs.data(), fs.size());

    const std::size_t n = size / kBytesPerCommand;
    for (std::size_t i = 0; i < n; ++i) {
        ob::Command c{};
        if (!decode(data, size, i, c)) {
            break;
        }

        rb.clear();
        fb.clear();
        ref.submit(c, rb);
        fast.submit(c, fb);

        if (rb.size() != fb.size()) {
            std::fprintf(stderr,
                         "DIVERGENCE at command %zu: reference emitted %zu events, "
                         "fast emitted %zu\n",
                         i, rb.size(), fb.size());
            std::abort();
        }
        for (std::size_t k = 0; k < rb.size(); ++k) {
            if (!(rb[k] == fb[k])) {
                std::fprintf(stderr, "DIVERGENCE at command %zu event %zu\n", i, k);
                std::abort();
            }
        }
        if (rb.empty()) {
            std::fprintf(stderr, "CONTRACT VIOLATION at command %zu: no event emitted\n", i);
            std::abort();
        }
        if (ref.best_bid() != fast.best_bid() || ref.best_ask() != fast.best_ask() ||
            ref.live_order_count() != fast.live_order_count()) {
            std::fprintf(stderr, "BOOK STATE DIVERGENCE at command %zu\n", i);
            std::abort();
        }

        if (const auto r = ob::check_invariants(fast); !r.ok) {
            std::fprintf(stderr, "FAST INVARIANT at command %zu: %s\n", i, r.failure);
            std::abort();
        }
        if (const auto r = fast.check_internal_invariants(); !r.ok) {
            std::fprintf(stderr, "FAST INTERNAL at command %zu: %s\n", i, r.failure);
            std::abort();
        }
        if (const auto r = ob::check_invariants(ref); !r.ok) {
            std::fprintf(stderr, "REF INVARIANT at command %zu: %s\n", i, r.failure);
            std::abort();
        }
    }
    return 0;
}
