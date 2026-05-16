#include "entity/ui/MathInput.hpp"

#include <gtest/gtest.h>

#include <cmath>

using entity::ui::evaluateExpression;

namespace {

constexpr double kEps = 1e-9;

void expectClose(const char* expr, double expected) {
    auto v = evaluateExpression(expr);
    ASSERT_TRUE(v.has_value()) << "Expected `" << expr << "` to parse";
    EXPECT_NEAR(*v, expected, kEps) << "Expression: " << expr;
}

void expectFail(const char* expr) {
    auto v = evaluateExpression(expr);
    EXPECT_FALSE(v.has_value())
        << "Expected `" << expr << "` to fail but got " << (v ? *v : 0.0);
}

}  // namespace

// ---- Plain numbers ------------------------------------------------------

TEST(MathInputParser, PlainInteger) {
    expectClose("3", 3.0);
    expectClose("0", 0.0);
    expectClose("42", 42.0);
}

TEST(MathInputParser, PlainDecimal) {
    expectClose("3.14", 3.14);
    expectClose("0.5", 0.5);
    expectClose(".5", 0.5);
    expectClose("3.", 3.0);
}

TEST(MathInputParser, NegativeLiteral) {
    expectClose("-3", -3.0);
    expectClose("-3.5", -3.5);
}

TEST(MathInputParser, UnaryPlus) {
    expectClose("+3", 3.0);
    expectClose("+3.5", 3.5);
}

TEST(MathInputParser, DoubleUnaryMinus) {
    expectClose("--3", 3.0);
    expectClose("---3", -3.0);
}

TEST(MathInputParser, ScientificNotation) {
    expectClose("1e2", 100.0);
    expectClose("3.5e-2", 0.035);
    expectClose("1.5e+3", 1500.0);
}

// ---- Binary operators ---------------------------------------------------

TEST(MathInputParser, Addition) {
    expectClose("1+2", 3.0);
    expectClose("1.5 + 2.5", 4.0);
}

TEST(MathInputParser, Subtraction) {
    expectClose("5-3", 2.0);
    expectClose("0.5 - 0.25", 0.25);
}

TEST(MathInputParser, Multiplication) {
    expectClose("4*2", 8.0);
    expectClose("1.5 * 2", 3.0);
}

TEST(MathInputParser, Division) {
    expectClose("6.74/2", 3.37);
    expectClose("10 / 4", 2.5);
}

TEST(MathInputParser, Precedence) {
    expectClose("1+2*3", 7.0);
    expectClose("2*3+1", 7.0);
    expectClose("10-2*3", 4.0);
    expectClose("20/4-2", 3.0);
}

TEST(MathInputParser, LeftAssociativity) {
    expectClose("10-3-2", 5.0);
    expectClose("8/4/2", 1.0);
}

// ---- Parentheses --------------------------------------------------------

TEST(MathInputParser, Parentheses) {
    expectClose("(1+2)*3", 9.0);
    expectClose("2*(3+4)", 14.0);
    expectClose("(1+2)*(3+4)", 21.0);
}

TEST(MathInputParser, NestedParentheses) {
    expectClose("((1+2)*3)", 9.0);
    expectClose("(1+(2*3))", 7.0);
}

TEST(MathInputParser, UnaryMinusOnParens) {
    expectClose("-(1+2)", -3.0);
    expectClose("-(1+2)*3", -9.0);
}

// ---- Whitespace ---------------------------------------------------------

TEST(MathInputParser, Whitespace) {
    expectClose("  3.14  ", 3.14);
    expectClose("  6.74  /  2  ", 3.37);
    expectClose("1 + 2 * 3", 7.0);
}

TEST(MathInputParser, EmptyOrWhitespaceOnly) {
    expectFail("");
    expectFail("   ");
    expectFail("\t\n ");
}

// ---- Trailing format decoration -----------------------------------------

TEST(MathInputParser, TrailingFormatDecoration) {
    // PropertyWindow uses "%.1f deg" for the rotation field; when the
    // user enters edit mode the seed buffer reads "45.0 deg". The
    // parser must tolerate the suffix.
    expectClose("45.0 deg", 45.0);
    expectClose("3.37 px", 3.37);
    expectClose("1.5 + 0.5 sec", 2.0);
}

// ---- Failure modes ------------------------------------------------------

TEST(MathInputParser, GarbageInput) {
    expectFail("abc");
    expectFail("hello world");
}

TEST(MathInputParser, TrailingOperator) {
    expectFail("3 +");
    expectFail("3 *");
    expectFail("3 /");
    expectFail("3 -");
}

TEST(MathInputParser, LeadingBinaryOperator) {
    expectFail("*3");
    expectFail("/3");
}

TEST(MathInputParser, DivisionByZero) {
    expectFail("1/0");
    expectFail("3.14 / 0.0");
    expectFail("5 / (1-1)");
}

TEST(MathInputParser, UnbalancedParens) {
    expectFail("(1+2");
    expectFail("1+2)");
    expectFail("((1+2)");
    expectFail("()");
}

TEST(MathInputParser, MultipleDecimalPoints) {
    expectFail("3..14");
    expectFail("1.2.3");
}

TEST(MathInputParser, OperatorRunsOnInternal) {
    // "1**2" — '*' then '*' as factor — second '*' isn't a valid factor
    // start, so parseFactor fails.
    expectFail("1**2");
    expectFail("1//2");
}

TEST(MathInputParser, NonFiniteResult) {
    // Overflow → from_chars returns out_of_range → nullopt.
    expectFail("1e500");
    expectFail("1e500 * 1e500");
}

// ---- Real-world examples from PropertyWindow / OutputsWindow ------------

TEST(MathInputParser, PropertyWindowExamples) {
    // Halving a value (a common motivation for this feature).
    expectClose("6.74 / 2", 3.37);
    // Centering relative to a screen dimension.
    expectClose("1920 / 2", 960.0);
    // Composing offsets.
    expectClose("0.5 + 0.1", 0.6);
    // Rotation arithmetic.
    expectClose("180 - 45", 135.0);
}

TEST(MathInputParser, OutputsWindowCornerExamples) {
    // Corner positions are in NDC roughly [-1, 1].
    expectClose("-1 + 0.05", -0.95);
    expectClose("(0.5 + 0.3) / 2", 0.4);
}

TEST(MathInputParser, CueModalFadeSeconds) {
    expectClose("0.5 * 2", 1.0);
    expectClose("60 / 24", 2.5);  // frames at 24fps → seconds-ish
}
