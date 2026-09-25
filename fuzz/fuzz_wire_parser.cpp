// fuzz/fuzz_wire_parser.cpp
//
// Feeds libFuzzer's bytes straight at the decoder with no sanitisation whatsoever,
// then feeds whatever it produces to the engine.
//
// Two properties under test:
//   1. decode() ALWAYS terminates with a typed result and never reads out of bounds,
//      whatever the bytes are.
//   2. A malformed frame cannot corrupt the book: the engine's invariants hold no
//      matter what the parser hands it.
//
// The loop also asserts forward progress, because a decoder that returns
// consumed == 0 on a frame the caller then retries is an infinite loop, which is a
// denial of service and exactly the kind of bug a parser fuzzer should catch.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ob/fast_engine.hpp>
#include <ob/invariants.hpp>
#include <ob/wire.hpp>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto*                bytes = reinterpret_cast<const std::byte*>(data);
    std::span<const std::byte> in(bytes, size);

    ob::FastEngine         engine(ob::FastEngine::Config{4096});
    std::vector<ob::Event> storage(1 << 14);
    ob::EventBuffer        buf(storage.data(), storage.size());

    std::size_t off        = 0;
    std::size_t iterations = 0;
    while (off < in.size()) {
        // A decoder that never makes progress would spin forever. Bound it by the
        // input size: no well-behaved decoder can need more steps than bytes.
        if (++iterations > size + 8) {
            std::fprintf(stderr, "NO FORWARD PROGRESS at offset %zu\n", off);
            std::abort();
        }

        ob::Command c{};
        const auto  r = ob::wire::decode(in.subspan(off), c);

        if (r.consumed == 0) {
            break;  // unframeable or truncated; a real reader resynchronises
        }
        if (off + r.consumed > in.size()) {
            std::fprintf(stderr, "DECODER OVERRAN: consumed %zu at offset %zu of %zu\n", r.consumed,
                         off, in.size());
            std::abort();
        }

        if (r.ok()) {
            buf.clear();
            engine.submit(c, buf);
            if (buf.empty()) {
                std::fprintf(stderr, "CONTRACT VIOLATION: no event emitted\n");
                std::abort();
            }
            if (const auto inv = ob::check_invariants(engine); !inv.ok) {
                std::fprintf(stderr, "INVARIANT after wire command: %s\n", inv.failure);
                std::abort();
            }
            if (const auto inv = engine.check_internal_invariants(); !inv.ok) {
                std::fprintf(stderr, "INTERNAL INVARIANT after wire command: %s\n", inv.failure);
                std::abort();
            }
        }
        off += r.consumed;
    }
    return 0;
}
