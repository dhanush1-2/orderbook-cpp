#include "alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>
#include <ob/sanitizer.hpp>

namespace {
// Relaxed atomic rather than a plain size_t: runtime startup can allocate from
// threads this harness does not control, and a data race would be UB even though
// the count itself is only read single-threaded.
std::atomic<std::size_t> g_allocs{0};
}  // namespace

namespace ob::bench {
std::size_t alloc_count() noexcept {
    return g_allocs.load(std::memory_order_relaxed);
}
void reset_alloc_count() noexcept {
    g_allocs.store(0, std::memory_order_relaxed);
}
}  // namespace ob::bench

// A PROGRAM CANNOT BOTH REPLACE THE GLOBAL ALLOCATOR AND RUN UNDER A SANITIZER
// THAT OWNS IT. libclang_rt.tsan_cxx and libclang_rt.asan_cxx define these same
// operators, so defining them here too is a multiple-definition link error on GNU
// ld. (macOS's two-level namespace resolves it silently, which is why this only
// surfaced on Linux CI.)
//
// Under a sanitizer the replacements are compiled out entirely: alloc_count() then
// reports 0 and every caller skips its assertion via ob::kSanitizerBuild. That is
// the honest behaviour anyway - the sanitizer allocates during the measured window,
// so a zero-allocation assertion there would be meaningless even if it linked.
#if !defined(OB_SANITIZER_BUILD)

// The full replaceable set. Missing one would pair a counted new with an
// uncounted delete, which on some libraries is undefined behavior rather than
// merely a wrong count.
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](std::size_t n) {
    return ::operator new(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept {
    return ::operator new(n, t);
}
void* operator new(std::size_t n, std::align_val_t a) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    std::size_t align = static_cast<std::size_t>(a);
    if (align < sizeof(void*)) {
        align = sizeof(void*);
    }
    void* p = nullptr;
    if (posix_memalign(&p, align, n == 0 ? 1 : n) != 0) {
        throw std::bad_alloc{};
    }
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

#endif  // !OB_SANITIZER_BUILD
