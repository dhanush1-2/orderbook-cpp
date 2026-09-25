// include/ob/sanitizer.hpp
#pragma once

// Is this a sanitizer build?
//
// Sanitizers slow execution by 20-50x and, in TSan's case, deliberately perturb
// thread scheduling. Assertions about THROUGHPUT RATIOS or iteration COUNTS under
// those conditions measure the sanitizer, not the code, and fail intermittently on
// a shared CI runner with fewer cores than the developer's machine.
//
// Correctness assertions still run everywhere. Only the performance-shaped ones are
// relaxed, and the tests say so where they do it.

namespace ob {

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || \
    __has_feature(memory_sanitizer)
#define OB_SANITIZER_BUILD 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define OB_SANITIZER_BUILD 1
#endif

#if defined(OB_SANITIZER_BUILD)
inline constexpr bool kSanitizerBuild = true;
#else
inline constexpr bool kSanitizerBuild = false;
#endif

}  // namespace ob
