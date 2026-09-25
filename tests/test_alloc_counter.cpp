// Built as its OWN executable linking bench/alloc_counter.cpp, because replacing
// global operator new inside the main test binary would perturb GoogleTest itself.
#include <cstdio>
#include <ob/fast_engine.hpp>
#include <ob/sanitizer.hpp>
#include <vector>

#include "../bench/alloc_counter.hpp"
#include "../bench/clock.hpp"

#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main() {
    // The allocator replacement is compiled out under a sanitizer, because the
    // sanitizer runtime defines the same operators and owns the allocator. Skipping
    // is the correct outcome, not a gap: the zero-allocation claim is verified by
    // the ordinary build, which is where it is meaningful.
    if (ob::kSanitizerBuild) {
        std::printf("SKIP: sanitizer build owns the allocator\n");
        return 0;
    }

    // 1. The counter must actually count. The barrier is load-bearing: at -O3 an
    //    unused vector is elided entirely and the allocation never happens, which
    //    made this check fail for the right reason on the first run.
    ob::bench::reset_alloc_count();
    {
        std::vector<int> v;
        v.reserve(1000);
        v.push_back(1);
        ob::bench::do_not_optimize(v.data());
    }
    CHECK(ob::bench::alloc_count() > 0, "counter did not observe a vector allocation");

    // 2. FastEngine::submit must allocate nothing, ever. This is the claim.
    ob::FastEngine         e(ob::FastEngine::Config{1 << 16});
    std::vector<ob::Event> storage(1 << 14);
    ob::EventBuffer        buf(storage.data(), storage.size());

    // Warm up outside the measured window so construction-time allocation and any
    // lazy runtime initialisation do not count.
    for (ob::OrderId id = 1; id <= 1000; ++id) {
        buf.clear();
        e.submit(ob::make_new(id, ob::Side::Buy, ob::OrderType::Limit, 9000, 10), buf);
    }

    ob::bench::reset_alloc_count();
    for (ob::OrderId id = 1001; id <= 51000; ++id) {
        buf.clear();
        e.submit(ob::make_new(id, ob::Side::Sell, ob::OrderType::Limit, 11000, 10), buf);
        buf.clear();
        e.submit(
            ob::make_new(id + 1'000'000, ob::Side::Buy, ob::OrderType::Market, ob::kNoPrice, 5),
            buf);
        buf.clear();
        e.submit(ob::make_cancel(id), buf);
    }
    const std::size_t allocs = ob::bench::alloc_count();
    std::printf("allocations during 150000 submits: %zu\n", allocs);
    CHECK(allocs == 0, "FastEngine::submit allocated on the hot path");

    std::printf("PASS\n");
    return 0;
}
