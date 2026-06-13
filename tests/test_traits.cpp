// ─── function_traits tests ─────────────────────────────────────────────────
// Platform-independent — exercises pure compile-time reflection.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/traits.h>

#include <type_traits>

namespace {

int nullary();
void unary(int);
double binary(int, float);
void ten_arity(int, int, int, int, int, int, int, int, int, int);

} // namespace

TEST(FunctionTraits, arity_is_correct_for_zero_args) {
    EXPECT_EQ((splice::arity_v<decltype(&nullary)>), 0u);
}

TEST(FunctionTraits, arity_is_correct_for_one_arg) {
    EXPECT_EQ((splice::arity_v<decltype(&unary)>), 1u);
}

TEST(FunctionTraits, arity_is_correct_for_two_args) {
    EXPECT_EQ((splice::arity_v<decltype(&binary)>), 2u);
}

TEST(FunctionTraits, arity_is_correct_for_ten_args) {
    EXPECT_EQ((splice::arity_v<decltype(&ten_arity)>), 10u);
}

TEST(FunctionTraits, return_type_deduction_is_correct) {
    using ZeroRet  = splice::return_type_t<decltype(&nullary)>;
    using UnaryRet = splice::return_type_t<decltype(&unary)>;
    using BinRet   = splice::return_type_t<decltype(&binary)>;

    static_assert(std::is_same_v<ZeroRet, int>);
    static_assert(std::is_same_v<UnaryRet, void>);
    static_assert(std::is_same_v<BinRet, double>);
    SUCCEED();
}

TEST(FunctionTraits, function_type_round_trips) {
    using F = splice::function_traits<decltype(&binary)>::function_type;
    static_assert(std::is_same_v<F, double (*)(int, float)>);
    SUCCEED();
}
