// include/ob/level_bitmap.hpp
#pragma once

// Three-level occupancy bitmap over the price ladder. This is what replaces a
// red-black tree walk with two or three ctz/clz instructions.
//
//   L2:     1 word  (16 bits used)   each bit: "the L1 word below me is non-empty"
//   L1:    16 words (1024 bits)      each bit: "the L0 word below me is non-empty"
//   L0:  1024 words (65536 bits)     each bit: "this price level has orders"
//
// Cost of next_set_at_or_above / prev_set_at_or_below is bounded at three word
// loads no matter how far the next occupied level is.
//
// THE TRAP: `~0ull << 64` is undefined behavior, and the case is reached whenever
// the index lands on a word boundary. Both mask helpers guard it explicitly.

#include <ob/types.hpp>

#include <cassert>
#include <cstdint>

namespace ob {

class LevelBitmap {
public:
    static constexpr std::uint32_t kBits     = static_cast<std::uint32_t>(kLadderSize);
    static constexpr std::uint32_t kNotFound = 0xFFFF'FFFFu;

    static_assert(kBits % 64 == 0, "kBits must be a whole number of words");
    static_assert((kBits / 64) % 64 == 0, "L0 word count must be a whole L1 word");
    static_assert(kBits / 64 / 64 <= 64, "hierarchy needs exactly one L2 word");

    void set(std::uint32_t i) noexcept {
        assert(i < kBits);
        const std::uint32_t w0 = i >> 6;
        const std::uint32_t w1 = w0 >> 6;
        l0_[w0] |= bit(i & 63);
        l1_[w1] |= bit(w0 & 63);
        l2_ |= bit(w1 & 63);
    }

    void clear(std::uint32_t i) noexcept {
        assert(i < kBits);
        const std::uint32_t w0 = i >> 6;
        l0_[w0] &= ~bit(i & 63);
        if (l0_[w0] != 0) {
            return;  // the summary bits above are still correct
        }
        const std::uint32_t w1 = w0 >> 6;
        l1_[w1] &= ~bit(w0 & 63);
        if (l1_[w1] != 0) {
            return;
        }
        l2_ &= ~bit(w1 & 63);
    }

    [[nodiscard]] bool test(std::uint32_t i) const noexcept {
        assert(i < kBits);
        return (l0_[i >> 6] & bit(i & 63)) != 0;
    }

    [[nodiscard]] bool empty() const noexcept { return l2_ == 0; }

    [[nodiscard]] std::uint32_t popcount() const noexcept {
        std::uint32_t n = 0;
        for (const std::uint64_t w : l0_) {
            n += static_cast<std::uint32_t>(__builtin_popcountll(w));
        }
        return n;
    }

    // Lowest set bit at index >= i, or kNotFound.
    [[nodiscard]] std::uint32_t next_set_at_or_above(std::uint32_t i) const noexcept {
        if (i >= kBits) {
            return kNotFound;
        }
        const std::uint32_t w0 = i >> 6;
        if (const std::uint64_t m = l0_[w0] & mask_at_or_above(i & 63); m != 0) {
            return (w0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(m));
        }
        const std::uint32_t w1 = w0 >> 6;
        if (const std::uint64_t m1 = l1_[w1] & mask_above(w0 & 63); m1 != 0) {
            const std::uint32_t nw0 = (w1 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(m1));
            return (nw0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l0_[nw0]));
        }
        if (const std::uint64_t m2 = l2_ & mask_above(w1 & 63); m2 != 0) {
            const std::uint32_t nw1 = static_cast<std::uint32_t>(__builtin_ctzll(m2));
            const std::uint32_t nw0 = (nw1 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l1_[nw1]));
            return (nw0 << 6) | static_cast<std::uint32_t>(__builtin_ctzll(l0_[nw0]));
        }
        return kNotFound;
    }

    // Highest set bit at index <= i, or kNotFound.
    [[nodiscard]] std::uint32_t prev_set_at_or_below(std::uint32_t i) const noexcept {
        if (i >= kBits) {
            i = kBits - 1;
        }
        const std::uint32_t w0 = i >> 6;
        if (const std::uint64_t m = l0_[w0] & mask_at_or_below(i & 63); m != 0) {
            return (w0 << 6) | top_bit(m);
        }
        const std::uint32_t w1 = w0 >> 6;
        if (const std::uint64_t m1 = l1_[w1] & mask_below(w0 & 63); m1 != 0) {
            const std::uint32_t pw0 = (w1 << 6) | top_bit(m1);
            return (pw0 << 6) | top_bit(l0_[pw0]);
        }
        if (const std::uint64_t m2 = l2_ & mask_below(w1 & 63); m2 != 0) {
            const std::uint32_t pw1 = top_bit(m2);
            const std::uint32_t pw0 = (pw1 << 6) | top_bit(l1_[pw1]);
            return (pw0 << 6) | top_bit(l0_[pw0]);
        }
        return kNotFound;
    }

    void reset() noexcept {
        for (std::uint64_t& w : l0_) {
            w = 0;
        }
        for (std::uint64_t& w : l1_) {
            w = 0;
        }
        l2_ = 0;
    }

private:
    static constexpr std::uint32_t kL0Words = kBits / 64;     // 1024
    static constexpr std::uint32_t kL1Words = kL0Words / 64;  // 16

    static constexpr std::uint64_t bit(std::uint32_t b) noexcept {
        return std::uint64_t{1} << b;
    }
    // Bits b..63. Safe: b is always < 64, so no shift-by-64 here.
    static constexpr std::uint64_t mask_at_or_above(std::uint32_t b) noexcept {
        return ~std::uint64_t{0} << b;
    }
    // Bits b+1..63. GUARDED: b == 63 would shift by 64, which is UB.
    static constexpr std::uint64_t mask_above(std::uint32_t b) noexcept {
        return b == 63 ? std::uint64_t{0} : (~std::uint64_t{0} << (b + 1));
    }
    // Bits 0..b. GUARDED for the same reason.
    static constexpr std::uint64_t mask_at_or_below(std::uint32_t b) noexcept {
        return b == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (b + 1)) - 1);
    }
    // Bits 0..b-1. GUARDED: b == 0 would shift by a negative amount.
    static constexpr std::uint64_t mask_below(std::uint32_t b) noexcept {
        return b == 0 ? std::uint64_t{0} : ((std::uint64_t{1} << b) - 1);
    }
    // Index of the highest set bit. Precondition: w != 0 (clzll(0) is UB).
    static std::uint32_t top_bit(std::uint64_t w) noexcept {
        assert(w != 0);
        return 63u - static_cast<std::uint32_t>(__builtin_clzll(w));
    }

    std::uint64_t l0_[kL0Words]{};
    std::uint64_t l1_[kL1Words]{};
    std::uint64_t l2_ = 0;
};

}  // namespace ob
