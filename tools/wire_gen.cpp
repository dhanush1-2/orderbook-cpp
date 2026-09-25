// tools/wire_gen.cpp
//
// Writes a scenario out as a stream of binary wire messages, so the ingest path can
// be exercised against a real file rather than an in-process vector.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ob/wire.hpp>
#include <vector>

#include "scenarios.hpp"

int main(int argc, char** argv) {
    using namespace ob;
    using namespace ob::bench;

    Scenario      sc   = Scenario::MixedRealistic;
    std::size_t   ops  = 200'000;
    std::uint64_t seed = 20260922;
    const char*   out  = "orders.bin";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            const char* want  = argv[++i];
            bool        found = false;
            for (const Scenario s : kAllScenarios) {
                if (std::strcmp(want, name(s)) == 0) {
                    sc    = s;
                    found = true;
                }
            }
            if (!found) {
                std::fprintf(stderr, "unknown scenario '%s'\n", want);
                return 1;
            }
        } else if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            ops = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = std::strtoull(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s [--scenario NAME] [--ops N] [--seed S] [--out FILE]\n",
                         argv[0]);
            return 1;
        }
    }

    const auto stream = build(sc, seed, ops);
    std::FILE* f      = std::fopen(out, "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot open %s for writing\n", out);
        return 1;
    }

    std::array<std::byte, wire::kMaxMsgSize> buf{};
    std::size_t                              bytes = 0;
    for (const Command& c : stream) {
        const std::size_t n = wire::encode(c, buf);
        if (n == 0 || std::fwrite(buf.data(), 1, n, f) != n) {
            std::fprintf(stderr, "write failed\n");
            std::fclose(f);
            return 1;
        }
        bytes += n;
    }
    std::fclose(f);
    std::printf("wrote %s: %zu messages, %zu bytes (%.1f bytes/msg)\n", out, stream.size(), bytes,
                static_cast<double>(bytes) / static_cast<double>(stream.size()));
    return 0;
}
