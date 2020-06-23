#include "CodeGen_WebAssembly.h"

#include "ConciseCasts.h"
#include "IRMatch.h"
#include "IROperator.h"
#include "LLVM_Headers.h"
#include "Util.h"

#include <sstream>

namespace Halide {
namespace Internal {

using namespace Halide::ConciseCasts;
using namespace llvm;
using std::string;
using std::vector;

/*
    TODO:
        - wasm only supports an i8x16 shuffle directly; we should sniff our Shuffle
          nodes for (eg) i16x8 and synthesize the right thing
*/

CodeGen_WebAssembly::CodeGen_WebAssembly(Target t)
    : CodeGen_Posix(t) {
#if !defined(WITH_WEBASSEMBLY)
    user_error << "llvm build not configured with WebAssembly target enabled.\n";
#endif
    user_assert(llvm_WebAssembly_enabled) << "llvm build not configured with WebAssembly target enabled.\n";
    user_assert(target.bits == 32) << "Only wasm32 is supported.";
}

void CodeGen_WebAssembly::visit(const Cast *op) {
    {
        Halide::Type src = op->value.type();
        Halide::Type dst = op->type;
        if (upgrade_type_for_arithmetic(src) != src ||
            upgrade_type_for_arithmetic(dst) != dst) {
            // Handle casts to and from types for which we don't have native support.
            CodeGen_Posix::visit(op);
            return;
        }
    }

    if (!op->type.is_vector()) {
        // We only have peephole optimizations for vectors in here.
        CodeGen_Posix::visit(op);
        return;
    }

    vector<Expr> matches;

    struct Pattern {
        Target::Feature feature;
        bool wide_op;
        Type type;
        int min_lanes;
        string intrin;
        Expr pattern;
    };

    static Pattern patterns[] = {
        {Target::WasmSimd128, true, Int(8, 16), 0, "llvm.sadd.sat.v16i8", i8_sat(wild_i16x_ + wild_i16x_)},
        {Target::WasmSimd128, true, UInt(8, 16), 0, "llvm.uadd.sat.v16i8", u8_sat(wild_u16x_ + wild_u16x_)},
        {Target::WasmSimd128, true, Int(16, 8), 0, "llvm.sadd.sat.v8i16", i16_sat(wild_i32x_ + wild_i32x_)},
        {Target::WasmSimd128, true, UInt(16, 8), 0, "llvm.uadd.sat.v8i16", u16_sat(wild_u32x_ + wild_u32x_)},
        // N.B. Saturating subtracts are expressed by widening to a *signed* type
        {Target::WasmSimd128, true, Int(8, 16), 0, "llvm.wasm.sub.saturate.signed.v16i8", i8_sat(wild_i16x_ - wild_i16x_)},
        {Target::WasmSimd128, true, UInt(8, 16), 0, "llvm.wasm.sub.saturate.unsigned.v16i8", u8_sat(wild_i16x_ - wild_i16x_)},
        {Target::WasmSimd128, true, Int(16, 8), 0, "llvm.wasm.sub.saturate.signed.v8i16", i16_sat(wild_i32x_ - wild_i32x_)},
        {Target::WasmSimd128, true, UInt(16, 8), 0, "llvm.wasm.sub.saturate.unsigned.v8i16", u16_sat(wild_i32x_ - wild_i32x_)},

        {Target::WasmSimd128, true, UInt(8, 16), 0, "llvm.wasm.avgr.unsigned.v16i8", u8(((wild_u16x_ + wild_u16x_) + 1) / 2)},
        {Target::WasmSimd128, true, UInt(8, 16), 0, "llvm.wasm.avgr.unsigned.v16i8", u8(((wild_u16x_ + wild_u16x_) + 1) >> 1)},
        {Target::WasmSimd128, true, UInt(16, 8), 0, "llvm.wasm.avgr.unsigned.v8i16", u16(((wild_u32x_ + wild_u32x_) + 1) / 2)},
        {Target::WasmSimd128, true, UInt(16, 8), 0, "llvm.wasm.avgr.unsigned.v8i16", u16(((wild_u32x_ + wild_u32x_) + 1) >> 1)},

    };

    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        const Pattern &pattern = patterns[i];

        if (!target.has_feature(pattern.feature)) {
            continue;
        }

        if (op->type.lanes() < pattern.min_lanes) {
            continue;
        }

        if (expr_match(pattern.pattern, op, matches)) {
            bool match = true;
            if (pattern.wide_op) {
                // Try to narrow the matches to the target type.
                for (size_t i = 0; i < matches.size(); i++) {
                    matches[i] = lossless_cast(op->type, matches[i]);
                    if (!matches[i].defined()) {
                        match = false;
                    }
                }
            }
            if (match) {
                value = call_intrin(op->type, pattern.type.lanes(), pattern.intrin, matches);
                return;
            }
        }
    }

    CodeGen_Posix::visit(op);
}

void CodeGen_WebAssembly::visit(const Select *op) {
//     Expr cond = op->condition;
//     Expr true_value = op->true_value;
//     Expr false_value = op->false_value;
//     internal_assert(true_value.type() == false_value.type());

//     // For wasm, we want to use v128.bitselect for vector types;
//     // to achieve that, we need the condition to be an int of the same bit-width
//     // as the value types, with values of either all-ones or all-zeroes.
//     const int bits = true_value.type().bits();
//     const int lanes = true_value.type().lanes();
// debug(0)<<"cond1 "<<cond<<"\n";
//     if (cond.type().is_bool() && lanes > 1) {
//         if (cond.type().is_scalar()) {
// debug(0)<<"cond2 "<<cond<<"\n";
//             cond = Broadcast::make(cond, lanes);
//         }
// debug(0)<<"cond3 "<<cond<<"\n";
//         cond = -cast(Int(bits, lanes), cond);
// debug(0)<<"cond4 "<<cond<<"\n";
//         Value *cmp = codegen(cond);
//         Value *a = codegen(true_value);
//         Value *b = codegen(false_value);
//         value = builder->CreateSelect(cmp, a, b);
//     } else
    {
        CodeGen_Posix::visit(op);
    }
}

string CodeGen_WebAssembly::mcpu() const {
    return "";
}

string CodeGen_WebAssembly::mattrs() const {
    std::ostringstream s;
    string sep;

    if (target.has_feature(Target::WasmSignExt)) {
        s << sep << "+sign-ext";
        sep = ",";
    }

    if (target.has_feature(Target::WasmSimd128)) {
        s << sep << "+simd128";
        sep = ",";
    }

    // TODO: Emscripten doesn't seem to be able to validate wasm that contains this yet.
    // We could only generate for JIT mode (where we know we can enable it), but that
    // would mean the execution model for JIT vs AOT could be slightly different,
    // so leave it out entirely until we can do it uniformly.
    // if (target.has_feature(Target::JIT)) {
    //    s << sep << "+nontrapping-fptoint";
    //    sep = ",";
    // }
    // TODO: this is "sat_float_to_int" in WABT

    user_assert(target.os == Target::WebAssemblyRuntime)
        << "wasmrt is the only supported 'os' for WebAssembly at this time.";

    return s.str();
}

bool CodeGen_WebAssembly::use_soft_float_abi() const {
    return false;
}

bool CodeGen_WebAssembly::use_pic() const {
    return false;
}

int CodeGen_WebAssembly::native_vector_bits() const {
    return 128;
}

}  // namespace Internal
}  // namespace Halide
