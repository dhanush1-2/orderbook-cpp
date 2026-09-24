// bench/histogram.hpp
#pragma once

// Exact-percentile recorder.
//
// Stores raw samples rather than bucketing them: 10^7 samples at 4 bytes is 40 MB,
// which costs nothing here, and approximation buys nothing when memory is not the
// constraint. HdrHistogram exists for the case where it is.
//
// Values are UNIT-AGNOSTIC. The latency harness records raw counter TICKS and
// converts to nanoseconds only at report time, because a floating-point multiply
// inside the measured region would be measuring the harness.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <span>
#include <vector>

namespace ob::bench {

class Histogram {
public:
    explicit Histogram(std::size_t capacity) { samples_.reserve(capacity); }

    // Never reallocates: capacity is reserved up front and exceeding it is a
    // caller error, exactly as with ob::EventBuffer.
    void record(std::uint64_t value) noexcept {
        assert(samples_.size() < samples_.capacity() &&
               "Histogram overflow: reserve the full sample count up front");
        constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
        if (value > kMax) {
            ++saturated_;
            value = kMax;
        }
        samples_.push_back(static_cast<std::uint32_t>(value));
        sorted_ = false;
    }

    [[nodiscard]] std::size_t count() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return samples_.capacity(); }
    [[nodiscard]] std::size_t saturated() const noexcept { return saturated_; }
    [[nodiscard]] bool        empty() const noexcept { return samples_.empty(); }

    // Nearest-rank: index = ceil(p/100 * N) - 1, clamped to [0, N-1]. Stated
    // explicitly because tools disagree on this and the disagreement is invisible.
    [[nodiscard]] std::uint32_t percentile(double p) {
        assert(!samples_.empty() && "percentile of an empty histogram");
        ensure_sorted();
        const double n    = static_cast<double>(samples_.size());
        double       rank = std::ceil(p / 100.0 * n) - 1.0;
        rank              = std::clamp(rank, 0.0, n - 1.0);
        return samples_[static_cast<std::size_t>(rank)];
    }

    [[nodiscard]] std::uint32_t min() {
        ensure_sorted();
        return samples_.front();
    }
    [[nodiscard]] std::uint32_t max() {
        ensure_sorted();
        return samples_.back();
    }

    [[nodiscard]] double mean() const {
        if (samples_.empty()) {
            return 0.0;
        }
        const std::uint64_t sum =
            std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0},
                            [](std::uint64_t a, std::uint32_t b) { return a + b; });
        return static_cast<double>(sum) / static_cast<double>(samples_.size());
    }

    void merge(const Histogram& other) {
        samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end());
        saturated_ += other.saturated_;
        sorted_ = false;
    }

    void clear() noexcept {
        samples_.clear();
        saturated_ = 0;
        sorted_    = false;
    }

    // Raw samples are committed alongside published figures so a reader can
    // recompute any percentile themselves rather than trusting the summary.
    void write_raw(const char* path) const {
        std::FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
            return;
        }
        std::fwrite(samples_.data(), sizeof(std::uint32_t), samples_.size(), f);
        std::fclose(f);
    }

    [[nodiscard]] std::span<const std::uint32_t> samples() const noexcept {
        return {samples_.data(), samples_.size()};
    }

private:
    void ensure_sorted() {
        if (!sorted_) {
            std::sort(samples_.begin(), samples_.end());
            sorted_ = true;
        }
    }

    std::vector<std::uint32_t> samples_;
    std::size_t                saturated_ = 0;
    bool                       sorted_    = false;
};

}  // namespace ob::bench
