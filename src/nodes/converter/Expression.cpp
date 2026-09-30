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
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <tuple>

extern "C" {
#include <tinyexpr.h>
}

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
// emit(results, firstPixelIndex, count) with one result array per program.
template <typename Emit>
void runExpressions(const Node& node, const std::vector<Value>& in, int in1Pin, int in2Pin,
                    const std::vector<const Program*>& progs, int w, int h, Emit&& emit) {
    ImagePtr img = toImage(in[0], w, h);
    ChannelPtr c1 = channelOr(in[in1Pin], 0.0f), c2 = channelOr(in[in2Pin], 0.0f);
    ChannelSampler s1 = paramSampler(node, in1Pin, c1, w, h), s2 = paramSampler(node, in2Pin, c2, w, h);
    ImageSampler si{img.get(), w, h};
    parallelForChunks(h, [&](int y0, int y1) {
        std::vector<exprvm::Workspace> ws(progs.size());
        for (size_t k = 0; k < progs.size(); ++k) {
            progs[k]->bind(ws[k]);
            // Sizes are the same for every pixel.
            if (double* v = progs[k]->var(ws[k], exprvm::W)) std::fill_n(v, kSpan, double(w));
            if (double* v = progs[k]->var(ws[k], exprvm::H)) std::fill_n(v, kSpan, double(h));
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
                    fill(exprvm::X, [&](int x) { return double(x); });
                    fill(exprvm::Y, [&](int) { return double(y); });
                    fill(exprvm::U, [&](int x) { return (x + 0.5) / w; });
                    fill(exprvm::V, [&](int) { return (y + 0.5) / h; });
                    results[k] = p.run(ws[k], n);
                }
                emit(results, size_t(y) * w + x0, n);
            }
    });
}

inline float finite(double r) { return std::isfinite(r) ? float(r) : 0.0f; }

class ExpressionNode : public Node {
public:
    NODELAB_NODE({"conv.expression", "Expression", "Converter",
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
        runExpressions(*this, in, 1, 2, {&prog}, w, h, [&](const auto& res, size_t i0, int n) {
            for (int i = 0; i < n; ++i) ch->data[i0 + size_t(i)] = finite(res[0][i]);
        });
        out[0] = Value(ChannelPtr(ch));
    }
};

class ImageExpressionNode : public Node {
public:
    NODELAB_NODE({"conv.image_expression", "Image Expression", "Converter",
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
        runExpressions(*this, in, 1, 2, {&pr, &pg, &pb}, w, h, [&](const auto& res, size_t i0, int n) {
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
};

}  // namespace

void registerExpressionNodes(NodeRegistry& r) {
    r.add<ExpressionNode>();
    r.add<ImageExpressionNode>();
}
