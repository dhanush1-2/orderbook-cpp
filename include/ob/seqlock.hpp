// include/ob/seqlock.hpp
#pragma once

// Single-writer, multi-reader seqlock.
//
// The writer NEVER WAITS. That is the whole point: this sits between the matching
// thread and anything that wants to look at the book, and a slow or stopped reader
// must not be able to slow matching down. A reader that catches a write in progress
// simply retries and sees a newer value.
//
// WHY THE PAYLOAD IS ATOMIC WORDS. A seqlock deliberately lets a reader copy a
// payload that is being written and then discard it. In C++ that is a data race,
// which is undefined behavior, and ThreadSanitizer is right to report it. Storing
// the payload as atomic words accessed with memory_order_relaxed makes the race
// well-defined: the sequence counter's release/acquire pair supplies the ordering
// and the relaxed payload accesses supply legality. On arm64 and x86-64 a relaxed
// word-sized load or store is the same instruction as a plain one, so this costs
// nothing at runtime and buys standard conformance plus a clean TSan run.
//
// std::memcpy rather than reinterpret_cast for the payload/word conversion: the
// cast would be a strict-aliasing violation, and every compiler elides the memcpy.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace ob {

namespace detail {
// std::hardware_destructive_interference_size is not provided by every standard
// library (libc++ gated it for years). Prefer it when the feature-test macro says
// it exists; otherwise fall back to 128, which is correct on Apple Silicon and
// merely generous on x86-64.
//
// GCC additionally warns about it (-Winterference-size): the value is ABI-sensitive,
// so a type whose layout depends on it is not stable across compilers or standard
// library versions. That is a real hazard when shipping a binary interface. It is
// not one here: Seqlock is internal to this project and every translation unit that
// sees it is compiled together, from source, with one toolchain. The warning is
// suppressed deliberately and narrowly rather than dodged by hardcoding 64, which
// would be wrong on the 128-byte-line hardware this project targets.
#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t kCacheLine = std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
inline constexpr std::size_t kCacheLine = 128;
#endif
}  // namespace detail

template <class T>
class Seqlock {
public:
    static_assert(std::is_trivially_copyable_v<T>, "payload must be trivially copyable");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0,
                  "payload size must be a whole number of 64-bit words");

    Seqlock() noexcept {
        const T zero{};
        store(zero);
        seq_.store(0, std::memory_order_release);
    }

    // Writer side. SINGLE writer only: two concurrent writers would corrupt state.
    void store(const T& value) noexcept {
        std::uint64_t buf[kWords];
        std::memcpy(buf, &value, sizeof(T));

        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);  // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);

        for (std::size_t i = 0; i < kWords; ++i) {
            words_[i].store(buf[i], std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);  // even: complete
    }

    // Reader side. Returns false when a write was in progress or landed mid-copy.
    [[nodiscard]] bool try_load(T& out) const noexcept {
        const std::uint64_t before = seq_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) {
            return false;  // writer is mid-update
        }
        std::atomic_thread_fence(std::memory_order_acquire);

        std::uint64_t buf[kWords];
        for (std::size_t i = 0; i < kWords; ++i) {
            buf[i] = words_[i].load(std::memory_order_relaxed);
        }

        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_acquire) != before) {
            return false;
        }
        std::memcpy(&out, buf, sizeof(T));
        return true;
    }

    // Retries until a consistent snapshot is obtained. Only safe because the writer
    // always finishes: it never blocks, so a retry loop cannot starve indefinitely.
    void load(T& out) const noexcept {
        while (!try_load(out)) {
            // spin: the writer is a handful of stores away from finishing
        }
    }

    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);

    // Padded so the sequence counter and the payload do not share a cache line with
    // each other or with whatever the enclosing object places next to them.
    alignas(detail::kCacheLine) std::atomic<std::uint64_t> seq_{0};
    alignas(detail::kCacheLine) std::atomic<std::uint64_t> words_[kWords]{};
};

}  // namespace ob
