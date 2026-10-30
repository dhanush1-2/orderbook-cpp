// tools/ring_stress.cpp
//
// Long-running SpscRing soak: both threads fold every message into an order-sensitive
// checksum, and the run fails unless they agree and every message arrives intact.

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/spsc_ring.hpp>
#include <thread>

namespace {

struct Message {
    std::array<std::uint64_t, 8> words;
};

Message make_message(std::uint64_t seq) noexcept {
    Message m{};
    for (std::size_t i = 0; i < m.words.size(); ++i) {
        m.words[i] = seq ^ (0x9E37'79B9'7F4A'7C15ull * (i + 1));
    }
    return m;
}

std::uint64_t fold(std::uint64_t sum, const Message& m) noexcept {
    for (const std::uint64_t w : m.words) {
        sum = (sum ^ w) * 0x0000'0100'0000'01B3ull;  // FNV-1a prime
    }
    return sum;
}

constexpr std::uint64_t kChecksumSeed = 0xCBF2'9CE4'8422'2325ull;

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t items = 1'000'000'000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--items") == 0 && i + 1 < argc) {
            items = std::strtoull(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--items N]\n", argv[0]);
            return 2;
        }
    }

    static ob::SpscRing<Message, 1024> ring;
    std::uint64_t                      produced_sum = kChecksumSeed;
    const auto                         start        = std::chrono::steady_clock::now();

    std::thread producer([&] {
        std::uint64_t sum = kChecksumSeed;
        for (std::uint64_t seq = 0; seq < items; ++seq) {
            const Message m = make_message(seq);
            sum             = fold(sum, m);
            while (!ring.try_push(m)) {
                std::this_thread::yield();
            }
        }
        produced_sum = sum;
    });

    std::uint64_t consumed_sum = kChecksumSeed;
    std::uint64_t corrupt      = 0;
    for (std::uint64_t seq = 0; seq < items;) {
        Message m{};
        if (!ring.try_pop(m)) {
            std::this_thread::yield();
            continue;
        }
        if (m.words != make_message(seq).words) {
            ++corrupt;
        }
        consumed_sum = fold(consumed_sum, m);
        ++seq;
    }
    producer.join();

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const bool ok = corrupt == 0 && produced_sum == consumed_sum && ring.empty();
    std::printf(
        "items      %llu\ncorrupt    %llu\nchecksums  %016llx %016llx\n"
        "seconds    %.3f\nM items/s  %.1f\nresult     %s\n",
        static_cast<unsigned long long>(items), static_cast<unsigned long long>(corrupt),
        static_cast<unsigned long long>(produced_sum),
        static_cast<unsigned long long>(consumed_sum), elapsed.count(),
        static_cast<double>(items) / elapsed.count() / 1e6, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
