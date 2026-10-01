#pragma once
#include <string>
#include <utility>
#include <vector>

class NodeRegistry;
void registerExpressionNodes(NodeRegistry& r);

// Span bytecode for the Expression nodes (exposed for tests).
namespace exprvm {

constexpr int kSpan = 256;  // pixels per instruction

enum Var { R, G, B, A, In1, In2, X, Y, U, V, W, H, kVarCount };

enum class Op {
    Add, Sub, Mul, Div, Neg, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Not, NotNot, NegNot, NegNotNot, Comma,
    Abs, Floor, Ceil, Sqrt, Min, Max, Clamp, Mix, Step, Smoothstep, Fract,
    Call,  // any other function, called per value through its pointer
};

struct Instr {
    Op op = Op::Call;
    const void* fn = nullptr;
    int arity = 0;
    int dst = 0;
    int args[7] = {};
};

// Registers for one thread running a Program: kSpan doubles per register.
struct Workspace {
    std::vector<double> regs;
};

class Program {
public:
    explicit Program(const std::string& src);

    bool ok() const { return ok_; }
    int errorPos() const { return errPos_; }
    bool uses(Var v) const;
    int instructionCount() const { return int(code_.size()); }

    // Sizes the workspace and loads the constants (once per thread).
    void bind(Workspace& ws) const;
    // The register to fill with variable v's values before run(), or null if unused.
    double* var(Workspace& ws, Var v) const;
    // Runs n <= kSpan values; returns the result array (valid until the next run).
    const double* run(Workspace& ws, int n) const;

    // The program as GLSL statements (float registers named prefix + number; variable v read as
    // vars[v]) leaving its value in `result`; false if it calls a function GLSL lacks (fac, ncr,
    // npr). The helpers it calls are in kGlsl.
    bool glsl(std::string& code, std::string& result, const char* const vars[kVarCount], const std::string& prefix) const;

private:
    bool ok_ = false;
    int errPos_ = 0;
    int regs_ = 0;
    int result_ = 0;
    int varReg_[kVarCount] = {};
    std::vector<std::pair<int, double>> consts_;
    std::vector<Instr> code_;
};

// GLSL helpers with C's results where GLSL leaves them undefined (pow and logs of negatives,
// division by zero, fmod...), so the GPU computes what the CPU does, in float precision.
extern const char* const kGlsl;

// Reference: tinyexpr's own evaluation, for tests.
double interpret(const std::string& src, const double vars[kVarCount]);

}  // namespace exprvm
