// Expression nodes: per-pixel math typed as text.
//
// Variables: r g b a (the Image input), in1 in2 (channel inputs), x y (pixel), u v (0..1 across
// the image), w h (size), plus tinyexpr's pi, e. Functions: tinyexpr built-ins (sin, cos, pow,
// sqrt, abs, floor, ceil, log, ln, exp, atan2, ...) and min, max, clamp, mix, step, smoothstep, fract.
//
// tinyexpr parses and folds constants; its tree is then compiled into span bytecode: each
// instruction processes a run of pixels at once, instead of walking the tree for every pixel.
// That's much faster, and the arithmetic is the same double math in the same order, so results match
// tinyexpr exactly. Anything the compiler doesn't recognise runs tinyexpr's own function per value.
#include "nodes/converter/Expression.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>

extern "C" {
#include <tinyexpr.h>
}

#include "gpu/Device.h"
#include "gpu/PointOp.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

double fnMin(double a, double b) { return a < b ? a : b; }
double fnMax(double a, double b) { return a > b ? a : b; }
double fnClamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double fnMix(double a, double b, double t) { return a + (b - a) * t; }
double fnStep(double edge, double x) { return x < edge ? 0.0 : 1.0; }
double fnSmoothstep(double e0, double e1, double x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    double t = fnClamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double fnFract(double x) { return x - std::floor(x); }

// Order matches exprvm::Var.
constexpr const char* kVarNames[exprvm::kVarCount] = {"r", "g", "b", "a", "in1", "in2", "x", "y", "u", "v", "w", "h"};

// Compiles with tinyexpr, variables bound to `slots` (the compiler identifies variables by address).
te_expr* teCompile(const std::string& src, double* slots, int* errPos) {
    std::vector<te_variable> table;
    for (int i = 0; i < exprvm::kVarCount; ++i) table.push_back({kVarNames[i], &slots[i], TE_VARIABLE, nullptr});
    const te_variable fns[] = {
        {"min", (const void*)fnMin, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"max", (const void*)fnMax, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"clamp", (const void*)fnClamp, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"mix", (const void*)fnMix, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"step", (const void*)fnStep, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
        {"smoothstep", (const void*)fnSmoothstep, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
        {"fract", (const void*)fnFract, TE_FUNCTION1 | TE_FLAG_PURE, nullptr},
    };
    table.insert(table.end(), std::begin(fns), std::end(fns));
    int err = 0;
    te_expr* e = te_compile(src.c_str(), table.data(), int(table.size()), &err);
    if (errPos) *errPos = err;
    return e;
}

constexpr int kTeConstant = 1;  // tinyexpr.c's private TE_CONSTANT
int teKind(const te_expr* n) { return n->type & 0x1F; }
int teArity(const te_expr* n) { return (n->type & (TE_FUNCTION0 | TE_CLOSURE0)) ? (n->type & 7) : 0; }

using exprvm::Op;

// tinyexpr's operators are private functions; compiling "x+y" etc. reveals their addresses.
struct OpTable {
    std::map<const void*, Op> ops;
    std::map<const void*, std::string> glsl;  // tinyexpr's other functions -> GLSL (kGlsl helpers)
    OpTable() {
        double slots[exprvm::kVarCount] = {};
        const std::pair<const char*, Op> probes[] = {
            {"x+y", Op::Add}, {"x-y", Op::Sub}, {"x*y", Op::Mul}, {"x/y", Op::Div}, {"-x", Op::Neg},
            {"x<y", Op::Lt}, {"x<=y", Op::Le}, {"x>y", Op::Gt}, {"x>=y", Op::Ge}, {"x==y", Op::Eq},
            {"x!=y", Op::Ne}, {"x&&y", Op::And}, {"x||y", Op::Or}, {"!x", Op::Not}, {"!!x", Op::NotNot},
            {"-!x", Op::NegNot}, {"-!!x", Op::NegNotNot}, {"(x,y)", Op::Comma}, {"abs(x)", Op::Abs},
            {"floor(x)", Op::Floor}, {"ceil(x)", Op::Ceil}, {"sqrt(x)", Op::Sqrt},
        };
        for (const auto& [src, op] : probes) {
            te_expr* e = teCompile(src, slots, nullptr);
            if (e && (e->type & (TE_FUNCTION0 | TE_CLOSURE0))) ops.emplace(e->function, op);
            te_free(e);
        }
        ops[(const void*)fnMin] = Op::Min;
        ops[(const void*)fnMax] = Op::Max;
        ops[(const void*)fnClamp] = Op::Clamp;
        ops[(const void*)fnMix] = Op::Mix;
        ops[(const void*)fnStep] = Op::Step;
        ops[(const void*)fnSmoothstep] = Op::Smoothstep;
        ops[(const void*)fnFract] = Op::Fract;
        const std::pair<const char*, const char*> fns[] = {
            {"acos(x)", "c_acos"}, {"asin(x)", "c_asin"}, {"atan(x)", "atan"}, {"atan2(x,y)", "c_atan2"},
            {"cos(x)", "cos"}, {"cosh(x)", "cosh"}, {"exp(x)", "exp"}, {"ln(x)", "c_ln"}, {"log10(x)", "c_log10"},
            {"pow(x,y)", "c_pow"}, {"x%y", "c_fmod"}, {"sin(x)", "sin"}, {"sinh(x)", "sinh"}, {"tan(x)", "tan"},
            {"tanh(x)", "c_tanh"},
        };
        for (const auto& [src, name] : fns) {
            te_expr* e = teCompile(src, slots, nullptr);
            if (e && (e->type & (TE_FUNCTION0 | TE_CLOSURE0))) glsl.emplace(e->function, name);
            te_free(e);
        }
    }
};

const OpTable& opTable() {
    static const OpTable t;
    return t;
}

}  // namespace

namespace exprvm {

Program::Program(const std::string& src) {
    for (int& r : varReg_) r = -1;
    double slots[kVarCount] = {};
    te_expr* root = teCompile(src, slots, &errPos_);
    if (!root) return;
    ok_ = true;
    // Common subexpressions share a register: e.g. max(max(r,g),b) written twice runs once.
    std::map<std::tuple<int, const void*, std::vector<int>>, int> memo;
    std::map<uint64_t, int> constRegs;
    const OpTable& table = opTable();

    std::function<int(const te_expr*)> emit = [&](const te_expr* n) -> int {
        const int kind = teKind(n);
        if (kind == kTeConstant) {
            uint64_t bits;
            std::memcpy(&bits, &n->value, sizeof bits);
            auto [it, fresh] = constRegs.emplace(bits, regs_);
            if (fresh) consts_.push_back({regs_++, n->value});
            return it->second;
        }
        if (kind == TE_VARIABLE) {
            const int v = int(n->bound - slots);
            if (varReg_[v] < 0) varReg_[v] = regs_++;
            return varReg_[v];
        }
        if (kind >= TE_CLOSURE0) {
            // Not produced with our variable table; keep the contract anyway.
            throw std::runtime_error("Expression: closures are not supported");
        }
        const int arity = teArity(n);
        std::vector<int> args;
        for (int i = 0; i < arity; ++i) args.push_back(emit(static_cast<const te_expr*>(n->parameters[i])));
        auto it = table.ops.find(n->function);
        const Op op = it != table.ops.end() ? it->second : Op::Call;
        if (op == Op::Comma) return args[1];
        auto key = std::make_tuple(int(op), op == Op::Call ? n->function : nullptr, args);
        if (auto m = memo.find(key); m != memo.end()) return m->second;
        Instr ins;
        ins.op = op;
        ins.fn = n->function;
        ins.arity = arity;
        ins.dst = regs_++;
        for (int i = 0; i < arity && i < 7; ++i) ins.args[i] = args[size_t(i)];
        code_.push_back(ins);
        memo.emplace(key, ins.dst);
        return ins.dst;
    };
    result_ = emit(root);
    te_free(root);
}

bool Program::uses(Var v) const { return varReg_[v] >= 0; }

void Program::bind(Workspace& ws) const {
    ws.regs.assign(size_t(std::max(1, regs_)) * kSpan, 0.0);
    for (const auto& [r, v] : consts_) std::fill_n(ws.regs.data() + size_t(r) * kSpan, kSpan, v);
}

double* Program::var(Workspace& ws, Var v) const {
    return varReg_[v] < 0 ? nullptr : ws.regs.data() + size_t(varReg_[v]) * kSpan;
}

const double* Program::run(Workspace& ws, int n) const {
    double* R = ws.regs.data();
    auto reg = [&](int r) { return R + size_t(r) * kSpan; };
    for (const Instr& in : code_) {
        double* __restrict d = reg(in.dst);
        const double* __restrict a = in.arity > 0 ? reg(in.args[0]) : nullptr;
        const double* __restrict b = in.arity > 1 ? reg(in.args[1]) : nullptr;
        const double* __restrict c = in.arity > 2 ? reg(in.args[2]) : nullptr;
        // Each case is the exact expression of the function it replaces (see fnMin etc. and
        // tinyexpr.c), so the result is bit-identical to calling it.
        switch (in.op) {
            case Op::Add: for (int i = 0; i < n; ++i) d[i] = a[i] + b[i]; break;
            case Op::Sub: for (int i = 0; i < n; ++i) d[i] = a[i] - b[i]; break;
            case Op::Mul: for (int i = 0; i < n; ++i) d[i] = a[i] * b[i]; break;
            case Op::Div: for (int i = 0; i < n; ++i) d[i] = a[i] / b[i]; break;
            case Op::Neg: for (int i = 0; i < n; ++i) d[i] = -a[i]; break;
            case Op::Lt: for (int i = 0; i < n; ++i) d[i] = a[i] < b[i]; break;
            case Op::Le: for (int i = 0; i < n; ++i) d[i] = a[i] <= b[i]; break;
            case Op::Gt: for (int i = 0; i < n; ++i) d[i] = a[i] > b[i]; break;
            case Op::Ge: for (int i = 0; i < n; ++i) d[i] = a[i] >= b[i]; break;
            case Op::Eq: for (int i = 0; i < n; ++i) d[i] = a[i] == b[i]; break;
            case Op::Ne: for (int i = 0; i < n; ++i) d[i] = a[i] != b[i]; break;
            case Op::And: for (int i = 0; i < n; ++i) d[i] = a[i] != 0.0 && b[i] != 0.0; break;
            case Op::Or: for (int i = 0; i < n; ++i) d[i] = a[i] != 0.0 || b[i] != 0.0; break;
            case Op::Not: for (int i = 0; i < n; ++i) d[i] = a[i] == 0.0; break;
            case Op::NotNot: for (int i = 0; i < n; ++i) d[i] = a[i] != 0.0; break;
            case Op::NegNot: for (int i = 0; i < n; ++i) d[i] = -(a[i] == 0.0); break;
            case Op::NegNotNot: for (int i = 0; i < n; ++i) d[i] = -(a[i] != 0.0); break;
            case Op::Abs: for (int i = 0; i < n; ++i) d[i] = std::fabs(a[i]); break;
            case Op::Floor: for (int i = 0; i < n; ++i) d[i] = std::floor(a[i]); break;
            case Op::Ceil: for (int i = 0; i < n; ++i) d[i] = std::ceil(a[i]); break;
            case Op::Sqrt: for (int i = 0; i < n; ++i) d[i] = std::sqrt(a[i]); break;
            case Op::Min: for (int i = 0; i < n; ++i) d[i] = a[i] < b[i] ? a[i] : b[i]; break;
            case Op::Max: for (int i = 0; i < n; ++i) d[i] = a[i] > b[i] ? a[i] : b[i]; break;
            case Op::Clamp:
                for (int i = 0; i < n; ++i) d[i] = a[i] < b[i] ? b[i] : (a[i] > c[i] ? c[i] : a[i]);
                break;
            case Op::Mix: for (int i = 0; i < n; ++i) d[i] = a[i] + (b[i] - a[i]) * c[i]; break;
            case Op::Step: for (int i = 0; i < n; ++i) d[i] = b[i] < a[i] ? 0.0 : 1.0; break;
            case Op::Smoothstep:
                for (int i = 0; i < n; ++i) d[i] = fnSmoothstep(a[i], b[i], c[i]);
                break;
            case Op::Fract: for (int i = 0; i < n; ++i) d[i] = a[i] - std::floor(a[i]); break;
            case Op::Comma: break;  // never emitted
            case Op::Call: {
                const double* p[7];
                for (int k = 0; k < in.arity; ++k) p[k] = reg(in.args[k]);
                using F0 = double (*)();
                using F1 = double (*)(double);
                using F2 = double (*)(double, double);
                using F3 = double (*)(double, double, double);
                using F4 = double (*)(double, double, double, double);
                using F5 = double (*)(double, double, double, double, double);
                using F6 = double (*)(double, double, double, double, double, double);
                using F7 = double (*)(double, double, double, double, double, double, double);
                switch (in.arity) {
                    case 0: { auto f = (F0)in.fn; for (int i = 0; i < n; ++i) d[i] = f(); break; }
                    case 1: { auto f = (F1)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i]); break; }
                    case 2: { auto f = (F2)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i]); break; }
                    case 3: { auto f = (F3)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i], p[2][i]); break; }
                    case 4: { auto f = (F4)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i], p[2][i], p[3][i]); break; }
                    case 5: { auto f = (F5)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i], p[2][i], p[3][i], p[4][i]); break; }
                    case 6: { auto f = (F6)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i], p[2][i], p[3][i], p[4][i], p[5][i]); break; }
                    case 7: { auto f = (F7)in.fn; for (int i = 0; i < n; ++i) d[i] = f(p[0][i], p[1][i], p[2][i], p[3][i], p[4][i], p[5][i], p[6][i]); break; }
                    default: std::fill_n(d, n, NAN);
                }
                break;
            }
        }
    }
    return reg(result_);
}

const char* const kGlsl = R"GLSL(
const float kInf = uintBitsToFloat(0x7F800000u);
// By the bits: drivers may compile x != x and isnan() assuming floats are never NaN.
bool c_isnan(float v) { return (floatBitsToUint(v) & 0x7FFFFFFFu) > 0x7F800000u; }
bool c_finite(float v) { return (floatBitsToUint(v) & 0x7F800000u) != 0x7F800000u; }
bool c_signbit(float v) { return floatBitsToUint(v) >= 0x80000000u; }
// Comparisons false with a NaN (and != true), as in C.
bool c_lt(float a, float b) { return !c_isnan(a) && !c_isnan(b) && a < b; }
bool c_le(float a, float b) { return !c_isnan(a) && !c_isnan(b) && a <= b; }
bool c_gt(float a, float b) { return !c_isnan(a) && !c_isnan(b) && a > b; }
bool c_ge(float a, float b) { return !c_isnan(a) && !c_isnan(b) && a >= b; }
bool c_eq(float a, float b) { return !c_isnan(a) && !c_isnan(b) && a == b; }
bool c_nz(float a) { return c_isnan(a) || a != 0.0; }  // true for logic (NaN is true in C)
// GPU division is approximate (6 / 3 may give 1.9999999, and floor() of it 1): one correction
// step makes the quotient correctly rounded, as the CPU's is.
float c_div(float a, float b) {
    if (b == 0.0) {
        if (a == 0.0 || c_isnan(a)) return kNaN;
        return c_signbit(a) != c_signbit(b) ? -kInf : kInf;
    }
    float q = a / b;
    if (!c_finite(q) || !c_finite(b)) return q;
    precise float r = fma(-q, b, a);
    return q + r / b;
}
float c_sqrt(float a) { return a < 0.0 ? kNaN : sqrt(a); }
float c_ln(float a) { return a < 0.0 ? kNaN : a == 0.0 ? -kInf : log(a); }
float c_log10(float a) { return a < 0.0 ? kNaN : a == 0.0 ? -kInf : log(a) * 0.4342944819; }
float c_asin(float a) { return abs(a) > 1.0 ? kNaN : asin(a); }
float c_acos(float a) { return abs(a) > 1.0 ? kNaN : acos(a); }
float c_atan2(float y, float x) {
    if (x == 0.0 && y == 0.0) return c_signbit(x) ? (c_signbit(y) ? -3.14159265 : 3.14159265) : y;
    return atan(y, x);
}
float c_tanh(float a) { return abs(a) > 10.0 ? sign(a) : tanh(a); }
float c_fmod(float a, float b) { return b == 0.0 ? kNaN : a - b * trunc(c_div(a, b)); }
float c_pow(float a, float b) {
    if (b == 0.0 || a == 1.0) return 1.0;
    if (b == 1.0) return a;
    if (b == 2.0) return a * a;
    if (a == 0.0) return b > 0.0 ? 0.0 : kInf;
    if (a < 0.0) {
        if (b != floor(b)) return kNaN;
        float r = pow(-a, b);
        return mod(b, 2.0) == 1.0 ? -r : r;
    }
    return pow(a, b);
}
float c_smoothstep(float e0, float e1, float x) {
    if (c_le(e1, e0)) return c_lt(x, e0) ? 0.0 : 1.0;
    float t = c_div(x - e0, e1 - e0);
    t = c_lt(t, 0.0) ? 0.0 : (c_gt(t, 1.0) ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}
)GLSL";

bool Program::glsl(std::string& code, std::string& result, const char* const vars[kVarCount], const std::string& prefix) const {
    if (!ok_) return false;
    const OpTable& table = opTable();
    std::vector<std::string> names(size_t(std::max(1, regs_)));
    for (int v = 0; v < kVarCount; ++v)
        if (varReg_[v] >= 0) names[size_t(varReg_[v])] = vars[v];
    for (const auto& [r, value] : consts_) {
        if (std::isnan(value)) names[size_t(r)] = "kNaN";
        else if (std::isinf(value)) names[size_t(r)] = value > 0 ? "kInf" : "(-kInf)";
        else {
            char b[40];
            std::snprintf(b, sizeof b, "%.9g", value);
            std::string s = b;
            if (s.find_first_of(".e") == std::string::npos) s += ".0";
            names[size_t(r)] = "(" + s + ")";
        }
    }
    code.clear();
    for (const Instr& in : code_) {
        const std::string a = in.arity > 0 ? names[size_t(in.args[0])] : "", b = in.arity > 1 ? names[size_t(in.args[1])] : "",
                          c = in.arity > 2 ? names[size_t(in.args[2])] : "";
        std::string e;
        switch (in.op) {
            case Op::Add: e = a + " + " + b; break;
            case Op::Sub: e = a + " - " + b; break;
            case Op::Mul: e = a + " * " + b; break;
            case Op::Div: e = "c_div(" + a + ", " + b + ")"; break;
            case Op::Neg: e = "-" + a; break;
            case Op::Lt: e = "float(c_lt(" + a + ", " + b + "))"; break;
            case Op::Le: e = "float(c_le(" + a + ", " + b + "))"; break;
            case Op::Gt: e = "float(c_gt(" + a + ", " + b + "))"; break;
            case Op::Ge: e = "float(c_ge(" + a + ", " + b + "))"; break;
            case Op::Eq: e = "float(c_eq(" + a + ", " + b + "))"; break;
            case Op::Ne: e = "float(!c_eq(" + a + ", " + b + "))"; break;
            case Op::And: e = "float(c_nz(" + a + ") && c_nz(" + b + "))"; break;
            case Op::Or: e = "float(c_nz(" + a + ") || c_nz(" + b + "))"; break;
            case Op::Not: e = "float(!c_nz(" + a + "))"; break;
            case Op::NotNot: e = "float(c_nz(" + a + "))"; break;
            case Op::NegNot: e = "-float(!c_nz(" + a + "))"; break;
            case Op::NegNotNot: e = "-float(c_nz(" + a + "))"; break;
            case Op::Abs: e = "abs(" + a + ")"; break;
            case Op::Floor: e = "floor(" + a + ")"; break;
            case Op::Ceil: e = "ceil(" + a + ")"; break;
            case Op::Sqrt: e = "c_sqrt(" + a + ")"; break;
            case Op::Min: e = "c_lt(" + a + ", " + b + ") ? " + a + " : " + b; break;
            case Op::Max: e = "c_gt(" + a + ", " + b + ") ? " + a + " : " + b; break;
            case Op::Clamp:
                e = "c_lt(" + a + ", " + b + ") ? " + b + " : (c_gt(" + a + ", " + c + ") ? " + c + " : " + a + ")";
                break;
            case Op::Mix: e = a + " + (" + b + " - " + a + ") * " + c; break;
            case Op::Step: e = "c_lt(" + b + ", " + a + ") ? 0.0 : 1.0"; break;
            case Op::Smoothstep: e = "c_smoothstep(" + a + ", " + b + ", " + c + ")"; break;
            case Op::Fract: e = a + " - floor(" + a + ")"; break;
            case Op::Comma: break;
            case Op::Call: {
                auto f = table.glsl.find(in.fn);
                if (f == table.glsl.end()) return false;
                e = f->second + "(";
                for (int k = 0; k < in.arity; ++k) e += (k ? ", " : "") + names[size_t(in.args[k])];
                e += ")";
                break;
            }
        }
        const std::string d = prefix + std::to_string(in.dst);
        names[size_t(in.dst)] = d;
        code += "    float " + d + " = " + e + ";\n";
    }
    result = names[size_t(result_)];
    return true;
}

double interpret(const std::string& src, const double vars[kVarCount]) {
    double slots[kVarCount];
    std::copy(vars, vars + kVarCount, slots);
    te_expr* e = teCompile(src, slots, nullptr);
    const double r = e ? te_eval(e) : NAN;
    te_free(e);
    return r;
}

}  // namespace exprvm

namespace {

using exprvm::kSpan;
using exprvm::Program;

void validate(const std::string& label, const Program& p, const std::string& src) {
    if (!p.ok())
        throw std::runtime_error(label + ": syntax error at character " + std::to_string(p.errorPos()) + " in \"" + src + "\"");
}

// Shared driver: runs every program over spans of a row, then calls
// emit(results, firstPixelIndex, count) with one result array per program. Coordinates (x y u v
// w h) are of the full image, so a region (see RoiWindow) computes the same as the whole image.
template <typename Emit>
void runExpressions(const Node& node, const EvalContext& ctx, const std::vector<Value>& in, int in1Pin, int in2Pin,
                    const std::vector<const Program*>& progs, int w, int h, Emit&& emit) {
    const PixelFrame fr = frameOf(ctx, w, h);
    ImagePtr img = toImage(in[0], w, h);
    ChannelPtr c1 = channelOr(in[in1Pin], 0.0f), c2 = channelOr(in[in2Pin], 0.0f);
    ChannelSampler s1 = paramSampler(node, in1Pin, c1, w, h), s2 = paramSampler(node, in2Pin, c2, w, h);
    ImageSampler si{img.get(), w, h};
    parallelForChunks(h, [&](int y0, int y1) {
        std::vector<exprvm::Workspace> ws(progs.size());
        for (size_t k = 0; k < progs.size(); ++k) {
            progs[k]->bind(ws[k]);
            // Sizes are the same for every pixel.
            if (double* v = progs[k]->var(ws[k], exprvm::W)) std::fill_n(v, kSpan, double(fr.fullW));
            if (double* v = progs[k]->var(ws[k], exprvm::H)) std::fill_n(v, kSpan, double(fr.fullH));
        }
        std::vector<const double*> results(progs.size());
        for (int y = y0; y < y1; ++y)
            for (int x0 = 0; x0 < w; x0 += kSpan) {
                const int n = std::min(kSpan, w - x0);
                for (size_t k = 0; k < progs.size(); ++k) {
                    const Program& p = *progs[k];
                    auto fill = [&](exprvm::Var v, auto&& value) {
                        if (double* d = p.var(ws[k], v))
                            for (int i = 0; i < n; ++i) d[i] = value(x0 + i);
                    };
                    // Without an Image input, r g b stay 0 and a stays 1, as before.
                    fill(exprvm::R, [&](int x) { return img ? double(si(x, y)[0]) : 0.0; });
                    fill(exprvm::G, [&](int x) { return img ? double(si(x, y)[1]) : 0.0; });
                    fill(exprvm::B, [&](int x) { return img ? double(si(x, y)[2]) : 0.0; });
                    fill(exprvm::A, [&](int x) { return img ? double(si(x, y)[3]) : 1.0; });
                    fill(exprvm::In1, [&](int x) { return double(s1(x, y)); });
                    fill(exprvm::In2, [&](int x) { return double(s2(x, y)); });
                    fill(exprvm::X, [&](int x) { return double(x + fr.x0); });
                    fill(exprvm::Y, [&](int) { return double(y + fr.y0); });
                    fill(exprvm::U, [&](int x) { return (x + fr.x0 + 0.5) / fr.fullW; });
                    fill(exprvm::V, [&](int) { return (y + fr.y0 + 0.5) / fr.fullH; });
                    results[k] = p.run(ws[k], n);
                }
                emit(results, size_t(y) * w + x0, n);
            }
    });
}

inline float finite(double r) { return std::isfinite(r) ? float(r) : 0.0f; }

// The expression variables on the GPU, as runExpressions fills them (pins: 0 Image, 1 In1, 2 In2).
const char* const kGlslVars[exprvm::kVarCount] = {"vR", "vG", "vB", "vA", "vIn1", "vIn2", "vX", "vY", "vU", "vV", "vW", "vH"};
const char* const kGlslVarDecls = R"(
    vec4 px = img0(p);
    float vR = px.r, vG = px.g, vB = px.b, vA = has0 ? px.a : 1.0;  // no image: 0 0 0 1
    float vIn1 = par1(p), vIn2 = par2(p);
    float vX = float(p.x + uOrigin.x), vY = float(p.y + uOrigin.y);
    float vU = c_div(vX + 0.5, float(uFull.x)), vV = c_div(vY + 0.5, float(uFull.y));
    float vW = float(uFull.x), vH = float(uFull.y);
)";

bool gpuExpressions(const std::vector<std::string>& sources) {
    std::string code, res;
    for (size_t i = 0; i < sources.size(); ++i)
        if (!Program(sources[i]).glsl(code, res, kGlslVars, "t")) return false;
    return true;
}

// The GPU kernel: each program's statements, then `assign` with the results named in order.
gpu::PointOp expressionOp(const std::vector<std::string>& sources, const std::vector<std::string>& labels,
                          std::string (*assign)(const std::vector<std::string>&, const void*), const void* user) {
    gpu::PointOp op;
    op.functions = exprvm::kGlsl;
    op.body = kGlslVarDecls;
    std::vector<std::string> results;
    for (size_t i = 0; i < sources.size(); ++i) {
        const Program prog(sources[i]);
        validate(labels[i], prog, sources[i]);
        std::string code, res;
        if (!prog.glsl(code, res, kGlslVars, "e" + std::to_string(i) + "_")) throw gpu::Error("GPU: unsupported function");
        op.body += code;
        results.push_back("finiteOr0(" + res + ")");
    }
    op.body += assign(results, user);
    return op;
}

class ExpressionNode : public Node {
public:
    REFRACTORY_NODE({"conv.expression", "Expression", "Converter",
                  {{"Image", PinType::Image}, {"In1", PinType::Channel, 1}, {"In2", PinType::Channel, 2}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Text("Expression", "(r + g + b) / 3"), ParamDesc::FloatFree("In1", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("In2", 0.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const std::string src = paramS(0);
        const Program prog(src);
        validate("Expression", prog, src);
        int w, h;
        resolveSize(in, ctx, w, h);
        auto ch = std::make_shared<Channel>(Channel::makeSized(w, h));
        runExpressions(*this, ctx, in, 1, 2, {&prog}, w, h, [&](const auto& res, size_t i0, int n) {
            for (int i = 0; i < n; ++i) ch->data[i0 + size_t(i)] = finite(res[0][i]);
        });
        out[0] = Value(ChannelPtr(ch));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return gpuExpressions({paramS(0)}); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op = expressionOp({paramS(0)}, {"Expression"},
                                       [](const std::vector<std::string>& r, const void*) { return "    out0 = " + r[0] + ";"; },
                                       nullptr);
        resolveSize(in, ctx, op.w, op.h);
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

class ImageExpressionNode : public Node {
public:
    REFRACTORY_NODE({"conv.image_expression", "Image Expression", "Converter",
                  {{"Image", PinType::Image}, {"In1", PinType::Channel, 3}, {"In2", PinType::Channel, 4}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Text("R", "r"), ParamDesc::Text("G", "g"), ParamDesc::Text("B", "b"),
                   ParamDesc::FloatFree("In1", 0.0f, 0.0f, 1.0f), ParamDesc::FloatFree("In2", 0.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const Program pr(paramS(0)), pg(paramS(1)), pb(paramS(2));
        validate("R", pr, paramS(0));
        validate("G", pg, paramS(1));
        validate("B", pb, paramS(2));
        int w, h;
        resolveSize(in, ctx, w, h);
        auto img = std::make_shared<Image>(w, h);
        ImagePtr src = toImage(in[0], w, h);
        const bool keepAlpha = src && src->w == w && src->h == h;
        runExpressions(*this, ctx, in, 1, 2, {&pr, &pg, &pb}, w, h, [&](const auto& res, size_t i0, int n) {
            for (int i = 0; i < n; ++i) {
                const size_t p = i0 + size_t(i);
                float* d = img->pixel(p);
                d[0] = finite(res[0][i]);
                d[1] = finite(res[1][i]);
                d[2] = finite(res[2][i]);
                d[3] = keepAlpha ? src->pixel(p)[3] : 1.0f;
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override {
        return gpuExpressions({paramS(0), paramS(1), paramS(2)});
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        // Alpha passes through from an image of the output's size (others are opaque, as on the CPU).
        int sw = 0, sh = 0;
        const bool keepAlpha = in[0].size(sw, sh) && sw == w && sh == h;
        gpu::PointOp op = expressionOp(
            {paramS(0), paramS(1), paramS(2)}, {"R", "G", "B"},
            [](const std::vector<std::string>& r, const void* keep) {
                return "    out0 = vec4(" + r[0] + ", " + r[1] + ", " + r[2] + ", " +
                       (*static_cast<const bool*>(keep) ? "px.a" : "1.0") + ");";
            },
            &keepAlpha);
        op.w = w, op.h = h;
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

}  // namespace

void registerExpressionNodes(NodeRegistry& r) {
    r.add<ExpressionNode>();
    r.add<ImageExpressionNode>();
}
