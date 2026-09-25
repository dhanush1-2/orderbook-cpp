// tools/wire_ingest.cpp
//
// Framed-stream ingest: read into a fixed buffer, decode every complete message,
// compact the partial tail, refill, repeat. No allocation in the loop, and the
// buffer never grows.
//
// The partial-frame handling is the part that matters and the part that is easy to
// get wrong: a message can straddle two reads, so the loop must carry the remainder
// forward rather than discarding it. A socket would slot in at the read call
// unchanged; this reads a file so the path is testable and deterministic.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/fast_engine.hpp>
#include <ob/wire.hpp>
#include <vector>

int main(int argc, char** argv) {
    using namespace ob;

    const char* in_path  = "orders.bin";
    std::size_t buf_size = 64 * 1024;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
            in_path = argv[++i];
        } else if (std::strcmp(argv[i], "--buffer") == 0 && i + 1 < argc) {
            buf_size = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--in FILE] [--buffer BYTES]\n", argv[0]);
            return 1;
        }
    }
    if (buf_size < wire::kMaxMsgSize) {
        buf_size = wire::kMaxMsgSize;  // must hold at least one whole message
    }

    std::FILE* f = std::fopen(in_path, "rb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", in_path);
        return 1;
    }

    FastEngine         engine;
    std::vector<Event> storage(1 << 16);
    EventBuffer        ev(storage.data(), storage.size());

    std::vector<std::byte> buf(buf_size);
    std::size_t            filled = 0;
    std::uint64_t          msgs = 0, trades = 0, straddles = 0;
    std::uint64_t          err[5] = {0, 0, 0, 0, 0};

    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const std::size_t got = std::fread(buf.data() + filled, 1, buf.size() - filled, f);
        filled += got;
        if (filled == 0) {
            break;  // nothing buffered and nothing left to read
        }

        std::size_t off = 0;
        for (;;) {
            Command    c{};
            const auto r =
                wire::decode(std::span<const std::byte>(buf.data() + off, filled - off), c);

            if (r.error == wire::DecodeError::Truncated && r.consumed == 0) {
                break;  // partial frame: carry it forward and refill
            }
            if (r.consumed == 0) {
                // Unframeable. A real reader resynchronises; here, report and stop
                // rather than spin forever on the same byte.
                ++err[static_cast<std::size_t>(r.error)];
                std::fprintf(stderr, "unframeable message at offset %zu: %s\n", off,
                             wire::to_string(r.error));
                off = filled;
                break;
            }
            if (r.ok()) {
                ev.clear();
                engine.submit(c, ev);
                ++msgs;
                for (const Event& e : ev) {
                    if (e.type == EventType::Trade) {
                        ++trades;
                    }
                }
            } else {
                ++err[static_cast<std::size_t>(r.error)];
            }
            off += r.consumed;
        }

        // Compact the partial tail to the front. This is the straddle case.
        const std::size_t rest = filled - off;
        if (rest > 0 && off > 0) {
            std::memmove(buf.data(), buf.data() + off, rest);
            ++straddles;
        }
        filled = rest;

        if (got == 0) {
            if (filled > 0) {
                std::fprintf(stderr, "%zu trailing bytes: the stream ends mid-message\n", filled);
            }
            break;
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fclose(f);

    std::printf("messages decoded  %llu\n", static_cast<unsigned long long>(msgs));
    std::printf("trades            %llu\n", static_cast<unsigned long long>(trades));
    std::printf("buffer straddles  %llu\n", static_cast<unsigned long long>(straddles));
    std::printf(
        "decode errors     truncated=%llu bad_length=%llu unknown_type=%llu "
        "unknown_version=%llu\n",
        static_cast<unsigned long long>(err[1]), static_cast<unsigned long long>(err[2]),
        static_cast<unsigned long long>(err[3]), static_cast<unsigned long long>(err[4]));
    std::printf("seconds           %.3f\n", secs);
    std::printf("msgs/sec          %.0f\n", secs > 0 ? static_cast<double>(msgs) / secs : 0.0);
    std::printf("resting orders    %zu\n", engine.live_order_count());
    std::printf("best bid / ask    %d / %d\n", engine.best_bid(), engine.best_ask());
    const auto inv = engine.check_internal_invariants();
    std::printf("invariants        %s\n", inv.ok ? "hold" : inv.failure);
    return inv.ok ? 0 : 1;
}
