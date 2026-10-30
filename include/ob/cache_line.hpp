// include/ob/cache_line.hpp
#pragma once

#include <cstddef>
#include <new>

namespace ob::detail {

// std::hardware_destructive_interference_size is not provided by every standard
// library (libc++ gated it for years). Prefer it when the feature-test macro says
// it exists; otherwise fall back to 128, which is correct on Apple Silicon and
// merely generous on x86-64.
//
// GCC additionally warns about it (-Winterference-size): the value is ABI-sensitive,
// so a type whose layout depends on it is not stable across compilers or standard
// library versions. That is a real hazard when shipping a binary interface. It is
// not one here: these types are internal to this project and every translation unit that
// sees them is compiled together, from source, with one toolchain. The warning is
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

}  // namespace ob::detail
