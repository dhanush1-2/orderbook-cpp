#include <gtest/gtest.h>

#include <cstdint>

TEST(Smoke, CompilerIsCpp20OrLater) {
    static_assert(__cplusplus >= 202002L, "C++20 required");
    EXPECT_GE(__cplusplus, 202002L);
}

TEST(Smoke, IntegerTypesAreTheExpectedWidths) {
    static_assert(sizeof(std::int32_t) == 4);
    static_assert(sizeof(std::uint64_t) == 8);
    EXPECT_EQ(sizeof(void*), 8u);  // 64-bit only; the design assumes it
}
