// The Expression nodes' span bytecode must give exactly what tinyexpr's tree walk gives, so
// switching to it changes no saved project's output.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <array>
#include <random>

#include "nodes/converter/Expression.h"

namespace {

// Every expression used by the examples and the infrared preset, plus grammar corner cases.
const char* const kCorpus[] = {
    "0.2126*r + 0.7152*g + 0.0722*b",
    "abs(in1 - in2)",
    "b/(in1+0.0001)",
    "clamp(0.03+0.84*pow(clamp(r,0,1),1.05),0,1)",
    "clamp(0.06+0.86*pow(clamp(r,0,1),0.95)+0.28*b*pow(1-r,2),0,1)",
    "clamp(max(in1, in2 * 1.5), 0, 1)",
    "in1 + 0.25*max(in2 - 0.6, 0)",
    "max(r, g * (1 - b))",
    "mix(mix(0.05 + 0.85*pow(max(r,0),0.75), 0.1 + 0.9*sqrt(max(r,0)), g), -0.12 + 0.62*r, b)",
    "mix(r, g, 0.5*b)",
    "smoothstep(0.05, 0.2, in1) * (1 - smoothstep(0.06, 0.14, max(max(r,g),b)))",
    "smoothstep(0.10, 0.22, (max(max(r,g),b) - min(min(r,g),b)) / (max(max(r,g),b) + 0.06)) * "
    "smoothstep(-10, 15, in1*360) * (1 - smoothstep(170, 195, in1*360)) * (1 - in2)",
    "smoothstep(185, 205, in1*360) * (1 - smoothstep(250, 265, in1*360)) * smoothstep(0.08, 0.2, "
    "(max(max(r,g),b) - min(min(r,g),b)) / (max(max(r,g),b) + 0.06)) * smoothstep(0.2, 0.4, in2)",
    "(r + g + b) / 3",
    "0.5",
    "pi",
    "-2^2",
    "2^3^2",
    "-r^2 + -(g)",
    "r % 0.3",
    "r > g && b <= 0.5 || !a",
    "!!r - -!g + !(b == in1) + (r != g)",
    "(r, g)",
    "sin x + cos(y) * tan(u) + atan2(v, u)",
    "exp(-r) + ln(g + 1) + log(b + 1) + log10(in1 + 2)",
    "floor(x / 7) + ceil(y / 3) + fract(u * 10) + step(0.5, v)",
    "sqrt(r - 0.5)",  // NaN for part of the range
    "1 / (r - r)",     // inf / NaN
    "fac(4) + ncr(5, 2) + npr(5, 2) + sinh(r) + cosh(g) + tanh(b) + asin(r) + acos(g) + atan(b)",
    "w * h + x - y",
    "e",
};

bool sameBits(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return std::memcmp(&a, &b, sizeof a) == 0;
}

}  // namespace

TEST_CASE("Expression bytecode matches tinyexpr exactly") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> unit(-0.25, 1.25);
    for (const char* src : kCorpus) {
        CAPTURE(src);
        exprvm::Program p(src);
        REQUIRE(p.ok());
        exprvm::Workspace ws;
        p.bind(ws);
        const int n = 200;  // not a multiple of anything convenient
        std::vector<std::array<double, exprvm::kVarCount>> vars(n);
        for (int i = 0; i < n; ++i) {
            auto& v = vars[size_t(i)];
            for (int k = exprvm::R; k <= exprvm::In2; ++k) v[size_t(k)] = unit(rng);
            v[exprvm::X] = i, v[exprvm::Y] = 3, v[exprvm::U] = (i + 0.5) / n, v[exprvm::V] = 0.4;
            v[exprvm::W] = n, v[exprvm::H] = 9;
            for (int k = 0; k < exprvm::kVarCount; ++k)
                if (double* d = p.var(ws, exprvm::Var(k))) d[i] = v[size_t(k)];
        }
        const double* out = p.run(ws, n);
        int mismatches = 0;
        for (int i = 0; i < n; ++i)
            if (!sameBits(out[i], exprvm::interpret(src, vars[size_t(i)].data()))) ++mismatches;
        CHECK(mismatches == 0);
    }
}

TEST_CASE("Expression bytecode shares common subexpressions and folds constants") {
    // max(max(r,g),b) appears twice but is computed once: 2 max + 1 min... + sub, add, div
    exprvm::Program p("(max(max(r,g),b) - min(min(r,g),b)) / (max(max(r,g),b) + 0.06)");
    REQUIRE(p.ok());
    CHECK(p.instructionCount() == 7);
    exprvm::Program c("2 * pi + 1");
    REQUIRE(c.ok());
    CHECK(c.instructionCount() == 0);
    CHECK(!c.uses(exprvm::R));
    exprvm::Program bad("r +* 2");
    CHECK(!bad.ok());
}
