// bench/alloc_counter.hpp
#pragma once

// Global operator new/delete replacement that counts allocations.
//
// Linked ONLY into benchmark executables, never into the library or the main test
// binary, so it cannot perturb anything else. Its purpose is to turn "no
// allocation on the hot path" from a claim in a README into an assertion that
// fails a build.

#include <cstddef>

namespace ob::bench {

std::size_t alloc_count() noexcept;
void        reset_alloc_count() noexcept;

}  // namespace ob::bench
