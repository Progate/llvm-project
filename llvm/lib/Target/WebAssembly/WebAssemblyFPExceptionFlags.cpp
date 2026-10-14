//===-- WebAssemblyFPExceptionFlags.cpp - Raise IEEE 754 exception flags --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Make floating-point operations raise the IEEE 754 exception flags.
///
/// WebAssembly has no floating-point status register: its instructions never
/// record invalid, division by zero, overflow, underflow or inexact, so
/// fetestexcept() in <fenv.h>, and everything that reports errors through it
/// (NumPy's floating-point error handling, for one), sees nothing. This pass
/// makes the code record them itself. After each floating-point operation it
/// classifies the result and sets the flags that the operation raises on a CPU
/// in the mutable wasm global __fe_status, which the C library defines and
/// reads. The bits are those of <fenv.h> (the x86 values), and the semantics
/// are those of x86-64 with SSE: tininess is detected after rounding,
/// comparisons are quiet (invalid only for signaling NaNs), and a conversion
/// to an integer that does not fit raises invalid. Rounding is always to
/// nearest, the only mode WebAssembly has.
///
/// The common case is cheap. A result that is normal (and above the smallest
/// binade) rules out every exception but inexact, and inexact is usually set
/// already: the check is then a few integer instructions and a global.get.
/// While inexact is clear, an error-free transformation (TwoSum, Dekker's
/// product) tells whether the operation was exact. Results at the edges of the
/// format (zero, subnormal, infinite, NaN), operands at the extremes of the
/// exponent range, and the formats that WebAssembly lacks (half, bfloat,
/// fp128) are passed to helper functions in the C library (__fe_add_f64,
/// __fe_trunc_f64_f32, ...), which compute the exceptions exactly in software
/// and raise them.
///
/// Operations that WebAssembly lowers to library calls (frem, fma, pow, ...)
/// are left alone: the library function raises the flags of the operations it
/// performs, since it is compiled with this pass as well.
///
//===----------------------------------------------------------------------===//

#include "Utils/WasmAddressSpaces.h"
#include "WebAssembly.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/EHPersonalities.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IntrinsicsWebAssembly.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "wasm-fp-exception-flags"

namespace {

// The exception bits of <fenv.h> (arch/wasm32/bits/fenv.h in the C library).
enum : unsigned {
  FEInvalid = 1,
  FEDivByZero = 4,
  FEOverflow = 8,
  FEUnderflow = 16,
  FEInexact = 32,
};

/// The encoding of a scalar floating-point type.
struct Format {
  Type *Ty;
  IntegerType *IntTy; // an integer of the same width
  unsigned Mantissa;  // stored significand bits
  unsigned ExpMax;    // the biased exponent of infinities and NaNs
  StringRef Name;     // the suffix of the helper functions

  /// Whether WebAssembly has instructions for the type.
  bool isNative() const { return Ty->isFloatTy() || Ty->isDoubleTy(); }
  unsigned width() const { return IntTy->getBitWidth(); }
};

std::optional<Format> formatOf(Type *Ty) {
  Ty = Ty->getScalarType();
  LLVMContext &C = Ty->getContext();
  if (Ty->isHalfTy())
    return Format{Ty, Type::getInt16Ty(C), 10, 0x1f, "f16"};
  if (Ty->isBFloatTy())
    return Format{Ty, Type::getInt16Ty(C), 7, 0xff, "bf16"};
  if (Ty->isFloatTy())
    return Format{Ty, Type::getInt32Ty(C), 23, 0xff, "f32"};
  if (Ty->isDoubleTy())
    return Format{Ty, Type::getInt64Ty(C), 52, 0x7ff, "f64"};
  if (Ty->isFP128Ty())
    return Format{Ty, Type::getInt128Ty(C), 112, 0x7fff, "f128"};
  return std::nullopt;
}

/// Intrinsics that raise only invalid, and only for a signaling NaN operand:
/// comparisons and the operations WebAssembly does exactly.
bool raisesOnlyForSignalingNaN(Intrinsic::ID ID) {
  switch (ID) {
  case Intrinsic::ceil:
  case Intrinsic::floor:
  case Intrinsic::trunc:
  case Intrinsic::round:
  case Intrinsic::roundeven:
  case Intrinsic::nearbyint:
  case Intrinsic::minnum:
  case Intrinsic::maxnum:
  case Intrinsic::minimum:
  case Intrinsic::maximum:
  case Intrinsic::minimumnum:
  case Intrinsic::maximumnum:
  case Intrinsic::canonicalize:
  case Intrinsic::wasm_pmin:
  case Intrinsic::wasm_pmax:
    return true;
  default:
    return false;
  }
}

/// Conversions from floating point to integer. The saturating and trapping
/// forms raise the same flags (a trap ends the program before they matter).
bool isFPToInt(Intrinsic::ID ID) {
  switch (ID) {
  case Intrinsic::fptosi_sat:
  case Intrinsic::fptoui_sat:
  case Intrinsic::wasm_trunc_signed:
  case Intrinsic::wasm_trunc_unsigned:
    return true;
  default:
    return false;
  }
}

/// The scalar results of a run that must be finite, summed per type. The sum of
/// finite values minus itself is zero; an infinite or NaN term makes it NaN (an
/// overflowing sum too, which only sends the run to its full checks). One
/// addition a result is cheaper than testing each magnitude.
/// The terms are added as a balanced tree, so that the sum does not lengthen a
/// chain of dependent additions (a dot product's accumulator) in the run.
using Sums = DenseMap<Type *, SmallVector<Value *, 8>>;

/// Marks the functions that hold the moved-out full checks of runs, whose own
/// arithmetic must not be checked.
constexpr const char *OutlinedAttr = "wasm-fp-exception-checks";

class Instrumenter {
  Function &F;
  Module &M;
  LLVMContext &Ctx;
  MDNode *Likely;
  MDNode *Unlikely;
  GlobalVariable *Status = nullptr;
  /// The funclet pad of each instrumented instruction in a funclet, taken
  /// before the blocks are split, for the bundle of the helper calls (a call in
  /// a funclet without it is treated as unreachable).
  DenseMap<Instruction *, Value *> Pads;
  Value *Pad = nullptr;

public:
  explicit Instrumenter(Function &F)
      : F(F), M(*F.getParent()), Ctx(F.getContext()) {
    MDBuilder MDB(Ctx);
    Likely = MDB.createLikelyBranchWeights();
    Unlikely = MDB.createUnlikelyBranchWeights();
  }

  bool run();

private:
  void instrument(Instruction *I, Instruction *IP);
  SmallVector<BasicBlock *, 16> regionBetween(BasicBlock *Head,
                                              BasicBlock *Join);
  Value *settles(Instruction *I, IRBuilder<> &B, bool &NeedsInexact,
                 Sums &Finite);
  Value *quietOperands(ArrayRef<Value *> Operands, const Format &Fm,
                       IRBuilder<> &B);
  SmallVector<Value *, 2> checkedOperands(ArrayRef<Value *> Operands);

  // Building blocks. All of them insert before the given instruction.
  GlobalVariable *status();
  Value *lacks(IRBuilder<> &B, unsigned Flag);
  void raise(IRBuilder<> &B, Value *Flags);
  void raiseIf(Value *Cond, unsigned Flags, Instruction *IP);
  void callHelper(IRBuilder<> &B, const Twine &Name,
                  ArrayRef<std::pair<Value *, Format>> Args);
  Value *bitsOf(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *exponentOf(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *isSignalingNaN(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *isNaN(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *isInf(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *isFinite(IRBuilder<> &B, Value *V);
  Value *isZero(IRBuilder<> &B, Value *V);
  Value *inProductRange(IRBuilder<> &B, Value *V, const Format &Fm);
  Value *lowBitsNonZero(IRBuilder<> &B, Value *Bits, Value *Count);
  Value *twoSumError(IRBuilder<> &B, Value *A, Value *Bv, Value *S);
  Value *productError(IRBuilder<> &B, Value *X, Value *Y, Value *P);
  /// Splits off a block run when Cond holds and returns its terminator.
  Instruction *ifThen(Value *Cond, Instruction *IP, MDNode *Weights);
  std::pair<Instruction *, Instruction *>
  ifThenElse(Value *Cond, Instruction *IP, MDNode *Weights);

  // The operations. The scalar forms take scalar operands; the others
  // dispatch vectors to them.
  void arithmetic(unsigned Opcode, Value *A, Value *Bv, Value *R,
                  const Format &Fm, Instruction *IP);
  void scalarArithmetic(unsigned Opcode, Value *A, Value *Bv, Value *R,
                        const Format &Fm, Instruction *IP);
  void arithmeticInexact(unsigned Opcode, Value *A, Value *Bv, Value *R,
                         const Format &Fm, Instruction *IP);
  void arithmeticSpecial(unsigned Opcode, Value *A, Value *Bv, Value *R,
                         const Format &Fm, Instruction *IP);
  void squareRoot(Value *A, Value *R, const Format &Fm, Instruction *IP);
  void scalarSquareRoot(Value *A, Value *R, const Format &Fm, Instruction *IP);
  void truncation(Value *A, Value *R, const Format &Src, const Format &Dst,
                  Instruction *IP);
  void scalarTruncation(Value *A, Value *R, const Format &Src,
                        const Format &Dst, Instruction *IP);
  void signalingOperands(ArrayRef<Value *> Operands, const Format &Fm,
                         Instruction *IP);
  void roundToIntegral(Value *A, Value *R, const Format &Fm, Instruction *IP);
  void toInteger(Value *A, Type *ResultTy, bool Signed, const Format &Fm,
                 Instruction *IP);
  void fromInteger(Value *X, Value *R, bool Signed, const Format &Fm,
                   Instruction *IP);
  /// Runs Emit on each lane of vector operands, or once on scalars.
  void forEachLane(ArrayRef<Value *> Vs, Instruction *IP,
                   function_ref<void(ArrayRef<Value *>, Instruction *)> Emit);
};

GlobalVariable *Instrumenter::status() {
  if (Status)
    return Status;
  Type *I32 = Type::getInt32Ty(Ctx);
  Status = M.getNamedGlobal("__fe_status");
  if (!Status)
    Status = new GlobalVariable(
        M, I32, /*isConstant=*/false, GlobalValue::ExternalLinkage, nullptr,
        "__fe_status", nullptr, GlobalValue::NotThreadLocal,
        WebAssembly::WASM_ADDRESS_SPACE_VAR);
  if (Status->getValueType() != I32 ||
      Status->getAddressSpace() != WebAssembly::WASM_ADDRESS_SPACE_VAR)
    report_fatal_error("__fe_status must be an i32 wasm global");
  return Status;
}

Value *Instrumenter::lacks(IRBuilder<> &B, unsigned Flag) {
  Value *St = B.CreateLoad(B.getInt32Ty(), status());
  return B.CreateICmpEQ(B.CreateAnd(St, Flag), B.getInt32(0));
}

void Instrumenter::raise(IRBuilder<> &B, Value *Flags) {
  Value *St = B.CreateLoad(B.getInt32Ty(), status());
  B.CreateStore(B.CreateOr(St, Flags), status());
}

void Instrumenter::raiseIf(Value *Cond, unsigned Flags, Instruction *IP) {
  IRBuilder<> B(ifThen(Cond, IP, Unlikely));
  raise(B, B.getInt32(Flags));
}

void Instrumenter::callHelper(IRBuilder<> &B, const Twine &Name,
                              ArrayRef<std::pair<Value *, Format>> Args) {
  // The operands go as their bits: a 16-bit format widened to i32 as C passes
  // uint16_t, the others as the integer of their width.
  SmallVector<Type *, 2> Params;
  SmallVector<Value *, 2> Values;
  for (auto &[V, Fm] : Args) {
    Value *Bits = B.CreateBitCast(V, Fm.IntTy);
    if (Fm.width() < 32)
      Bits = B.CreateZExt(Bits, B.getInt32Ty());
    Params.push_back(Bits->getType());
    Values.push_back(Bits);
  }
  FunctionCallee Helper = M.getOrInsertFunction(
      Name.str(), FunctionType::get(B.getVoidTy(), Params, false));
  if (auto *Fn = dyn_cast<Function>(Helper.getCallee()))
    Fn->addFnAttr(Attribute::NoUnwind);
  SmallVector<OperandBundleDef, 1> Bundles;
  if (Pad)
    Bundles.emplace_back("funclet", Pad);
  B.CreateCall(Helper, Values, Bundles);
}

Value *Instrumenter::bitsOf(IRBuilder<> &B, Value *V, const Format &Fm) {
  return B.CreateBitCast(V, V->getType()->getWithNewType(Fm.IntTy));
}

Value *Instrumenter::exponentOf(IRBuilder<> &B, Value *V, const Format &Fm) {
  Value *Bits = bitsOf(B, V, Fm);
  return B.CreateAnd(B.CreateLShr(Bits, Fm.Mantissa),
                     ConstantInt::get(Bits->getType(), Fm.ExpMax));
}

Value *Instrumenter::isSignalingNaN(IRBuilder<> &B, Value *V,
                                    const Format &Fm) {
  // A NaN with the quiet bit (the top of the significand) clear: the
  // magnitude is above infinity and below the quiet NaNs.
  unsigned W = Fm.width();
  Value *Bits = bitsOf(B, V, Fm);
  Type *Ty = Bits->getType();
  APInt Inf = APInt(W, Fm.ExpMax).shl(Fm.Mantissa);
  APInt Quiet = APInt::getOneBitSet(W, Fm.Mantissa - 1);
  Value *Abs =
      B.CreateAnd(Bits, ConstantInt::get(Ty, APInt::getSignedMaxValue(W)));
  return B.CreateICmpULT(B.CreateSub(Abs, ConstantInt::get(Ty, Inf + 1)),
                         ConstantInt::get(Ty, Quiet - 1));
}

Value *Instrumenter::isNaN(IRBuilder<> &B, Value *V, const Format &Fm) {
  if (Fm.isNative())
    return B.CreateFCmpUNO(V, V);
  unsigned W = Fm.width();
  Value *Bits = bitsOf(B, V, Fm);
  Type *Ty = Bits->getType();
  Value *Abs =
      B.CreateAnd(Bits, ConstantInt::get(Ty, APInt::getSignedMaxValue(W)));
  return B.CreateICmpUGT(
      Abs, ConstantInt::get(Ty, APInt(W, Fm.ExpMax).shl(Fm.Mantissa)));
}

Value *Instrumenter::isInf(IRBuilder<> &B, Value *V, const Format &Fm) {
  unsigned W = Fm.width();
  Value *Bits = bitsOf(B, V, Fm);
  Type *Ty = Bits->getType();
  Value *Abs =
      B.CreateAnd(Bits, ConstantInt::get(Ty, APInt::getSignedMaxValue(W)));
  return B.CreateICmpEQ(
      Abs, ConstantInt::get(Ty, APInt(W, Fm.ExpMax).shl(Fm.Mantissa)));
}

Value *Instrumenter::isFinite(IRBuilder<> &B, Value *V) {
  Value *Abs = B.CreateUnaryIntrinsic(Intrinsic::fabs, V);
  return B.CreateFCmpOLT(Abs, ConstantFP::getInfinity(V->getType()));
}

Value *Instrumenter::isZero(IRBuilder<> &B, Value *V) {
  return B.CreateFCmpOEQ(V, ConstantFP::getZero(V->getType()));
}

Value *Instrumenter::inProductRange(IRBuilder<> &B, Value *V,
                                    const Format &Fm) {
  // Dekker's product is exact when neither the splitting overflows nor a
  // partial product underflows. Magnitudes in [2^-450, 2^451) keep every
  // partial product of two of them above 2^-1004.
  assert(Fm.Ty->isDoubleTy() && "Dekker's product is used only for double");
  Value *E = exponentOf(B, V, Fm);
  Type *Ty = E->getType();
  return B.CreateICmpULT(B.CreateSub(E, ConstantInt::get(Ty, 1023 - 450)),
                         ConstantInt::get(Ty, 901));
}

Value *Instrumenter::lowBitsNonZero(IRBuilder<> &B, Value *Bits, Value *Count) {
  // Whether any of the low Count bits (fewer than the width) is set. fp128's
  // bits are taken as two i64 halves: a variable shift of an i128 is a
  // library call (__ashlti3).
  Type *Ty = Bits->getType();
  if (Ty->getScalarSizeInBits() <= 64) {
    Value *Mask = B.CreateSub(B.CreateShl(ConstantInt::get(Ty, 1), Count),
                              ConstantInt::get(Ty, 1));
    return B.CreateICmpNE(B.CreateAnd(Bits, Mask), ConstantInt::get(Ty, 0));
  }
  Type *I64 = Ty->getWithNewType(B.getInt64Ty());
  auto C64 = [&](uint64_t V) { return ConstantInt::get(I64, V); };
  Value *Lo = B.CreateTrunc(Bits, I64);
  Value *Hi = B.CreateTrunc(B.CreateLShr(Bits, 64), I64);
  Value *N = B.CreateTrunc(Count, I64);
  Value *LoMask = B.CreateSelect(
      B.CreateICmpUGE(N, C64(64)), C64(~0ULL),
      B.CreateSub(B.CreateShl(C64(1), B.CreateAnd(N, C64(63))), C64(1)));
  Value *HiMask = B.CreateSelect(
      B.CreateICmpUGT(N, C64(64)),
      B.CreateSub(
          B.CreateShl(C64(1), B.CreateAnd(B.CreateSub(N, C64(64)), C64(63))),
          C64(1)),
      C64(0));
  Value *Any = B.CreateOr(B.CreateAnd(Lo, LoMask), B.CreateAnd(Hi, HiMask));
  return B.CreateICmpNE(Any, C64(0));
}

Value *Instrumenter::twoSumError(IRBuilder<> &B, Value *A, Value *Bv,
                                 Value *S) {
  // Knuth's TwoSum: S + error == A + Bv exactly, unless an intermediate
  // overflows (the error then comes out infinite or NaN).
  Value *Bv1 = B.CreateFSub(S, A);
  Value *A1 = B.CreateFSub(S, Bv1);
  return B.CreateFAdd(B.CreateFSub(A, A1), B.CreateFSub(Bv, Bv1));
}

Value *Instrumenter::productError(IRBuilder<> &B, Value *X, Value *Y,
                                  Value *P) {
  // Dekker's TwoProduct with Veltkamp's splitting into 26- and 27-bit halves:
  // P + error == X * Y exactly within inProductRange.
  auto Split = [&](Value *V) {
    Value *C = B.CreateFMul(V, ConstantFP::get(V->getType(), 134217729.0));
    Value *Hi = B.CreateFSub(C, B.CreateFSub(C, V));
    return std::make_pair(Hi, B.CreateFSub(V, Hi));
  };
  auto [Xh, Xl] = Split(X);
  auto [Yh, Yl] = Split(Y);
  Value *E = B.CreateFSub(B.CreateFMul(Xh, Yh), P);
  E = B.CreateFAdd(E, B.CreateFMul(Xh, Yl));
  E = B.CreateFAdd(E, B.CreateFMul(Xl, Yh));
  return B.CreateFAdd(E, B.CreateFMul(Xl, Yl));
}

Instruction *Instrumenter::ifThen(Value *Cond, Instruction *IP,
                                  MDNode *Weights) {
  return SplitBlockAndInsertIfThen(Cond, IP->getIterator(),
                                   /*Unreachable=*/false, Weights);
}

std::pair<Instruction *, Instruction *>
Instrumenter::ifThenElse(Value *Cond, Instruction *IP, MDNode *Weights) {
  Instruction *Then, *Else;
  SplitBlockAndInsertIfThenElse(Cond, IP->getIterator(), &Then, &Else, Weights);
  return {Then, Else};
}

void Instrumenter::forEachLane(
    ArrayRef<Value *> Vs, Instruction *IP,
    function_ref<void(ArrayRef<Value *>, Instruction *)> Emit) {
  auto *VT = dyn_cast<FixedVectorType>(Vs[0]->getType());
  if (!VT) {
    Emit(Vs, IP);
    return;
  }
  for (unsigned L = 0, N = VT->getNumElements(); L < N; ++L) {
    IRBuilder<> B(IP);
    SmallVector<Value *, 3> Lanes;
    for (Value *V : Vs)
      Lanes.push_back(B.CreateExtractElement(V, L));
    Emit(Lanes, IP);
  }
}

void Instrumenter::arithmetic(unsigned Opcode, Value *A, Value *Bv, Value *R,
                              const Format &Fm, Instruction *IP) {
  if (isa<FixedVectorType>(R->getType()) && Fm.isNative()) {
    // Check the lanes one by one only when some lane could raise more than
    // inexact or inexact is still clear.
    IRBuilder<> B(IP);
    Value *E = exponentOf(B, R, Fm);
    Type *Ty = E->getType();
    Value *Common =
        Opcode == Instruction::FAdd || Opcode == Instruction::FSub
            ? B.CreateICmpNE(E, ConstantInt::get(Ty, Fm.ExpMax))
            : B.CreateICmpULT(B.CreateSub(E, ConstantInt::get(Ty, 2)),
                              ConstantInt::get(Ty, Fm.ExpMax - 2));
    Value *Done = B.CreateAnd(B.CreateAndReduce(Common),
                              B.CreateNot(lacks(B, FEInexact)));
    IP = ifThen(B.CreateNot(Done), IP, Unlikely);
  }
  forEachLane({A, Bv, R}, IP, [&](ArrayRef<Value *> L, Instruction *At) {
    scalarArithmetic(Opcode, L[0], L[1], L[2], Fm, At);
  });
}

void Instrumenter::scalarArithmetic(unsigned Opcode, Value *A, Value *Bv,
                                    Value *R, const Format &Fm,
                                    Instruction *IP) {
  IRBuilder<> B(IP);
  if (!Fm.isNative()) {
    StringRef Op = Opcode == Instruction::FAdd   ? "add"
                   : Opcode == Instruction::FSub ? "sub"
                   : Opcode == Instruction::FMul ? "mul"
                                                 : "div";
    callHelper(B, "__fe_" + Op + "_" + Fm.Name, {{A, Fm}, {Bv, Fm}});
    return;
  }
  // A result that rules out every exception but inexact. A sum that is
  // neither infinite nor NaN is one: overflow gives infinity when rounding to
  // nearest, and a sum below twice the smallest normal is a multiple of the
  // smallest subnormal that fits in the significand, so it is exact. A product
  // or quotient has to be normal and above the smallest binade, where
  // tininess after rounding would need a closer look.
  Value *E = exponentOf(B, R, Fm);
  Type *Ty = E->getType();
  Value *Common = Opcode == Instruction::FAdd || Opcode == Instruction::FSub
                      ? B.CreateICmpNE(E, ConstantInt::get(Ty, Fm.ExpMax))
                      : B.CreateICmpULT(B.CreateSub(E, ConstantInt::get(Ty, 2)),
                                        ConstantInt::get(Ty, Fm.ExpMax - 2));
  auto [Then, Else] = ifThenElse(Common, IP, Likely);
  arithmeticInexact(Opcode, A, Bv, R, Fm, Then);
  arithmeticSpecial(Opcode, A, Bv, R, Fm, Else);
}

void Instrumenter::arithmeticInexact(unsigned Opcode, Value *A, Value *Bv,
                                     Value *R, const Format &Fm,
                                     Instruction *IP) {
  IRBuilder<> B(IP);
  Instruction *T = ifThen(lacks(B, FEInexact), IP, Unlikely);
  B.SetInsertPoint(T);
  StringRef Op;
  bool IsDouble = Fm.Ty->isDoubleTy();
  Type *DoubleTy = B.getDoubleTy();
  switch (Opcode) {
  case Instruction::FAdd:
  case Instruction::FSub: {
    Op = Opcode == Instruction::FAdd ? "add" : "sub";
    Value *Addend = Opcode == Instruction::FSub ? B.CreateFNeg(Bv) : Bv;
    Value *Err = twoSumError(B, A, Addend, R);
    auto [Then, Else] = ifThenElse(isFinite(B, Err), T, Likely);
    raiseIf(IRBuilder<>(Then).CreateFCmpONE(Err, ConstantFP::getZero(Fm.Ty)),
            FEInexact, Then);
    IRBuilder<> EB(Else);
    callHelper(EB, "__fe_" + Op + "_" + Fm.Name, {{A, Fm}, {Bv, Fm}});
    return;
  }
  case Instruction::FMul:
    if (!IsDouble) {
      // The product of two floats is exact in double.
      Value *P =
          B.CreateFMul(B.CreateFPExt(A, DoubleTy), B.CreateFPExt(Bv, DoubleTy));
      raiseIf(B.CreateFCmpUNE(P, B.CreateFPExt(R, DoubleTy)), FEInexact, T);
      return;
    } else {
      Value *InRange =
          B.CreateAnd(inProductRange(B, A, Fm), inProductRange(B, Bv, Fm));
      auto [Then, Else] = ifThenElse(InRange, T, Likely);
      IRBuilder<> TB(Then);
      Value *Err = productError(TB, A, Bv, R);
      raiseIf(TB.CreateFCmpUNE(Err, ConstantFP::getZero(Fm.Ty)), FEInexact,
              Then);
      IRBuilder<> EB(Else);
      callHelper(EB, "__fe_mul_" + Fm.Name, {{A, Fm}, {Bv, Fm}});
      return;
    }
  case Instruction::FDiv:
    // The quotient is exact exactly when it times the divisor gives the
    // dividend back.
    if (!IsDouble) {
      Value *P =
          B.CreateFMul(B.CreateFPExt(R, DoubleTy), B.CreateFPExt(Bv, DoubleTy));
      raiseIf(B.CreateFCmpUNE(P, B.CreateFPExt(A, DoubleTy)), FEInexact, T);
      return;
    } else {
      Value *InRange =
          B.CreateAnd(inProductRange(B, R, Fm), inProductRange(B, Bv, Fm));
      auto [Then, Else] = ifThenElse(InRange, T, Likely);
      IRBuilder<> TB(Then);
      Value *P = TB.CreateFMul(R, Bv);
      Value *Err = productError(TB, R, Bv, P);
      Value *Inexact =
          TB.CreateOr(TB.CreateFCmpUNE(P, A),
                      TB.CreateFCmpUNE(Err, ConstantFP::getZero(Fm.Ty)));
      raiseIf(Inexact, FEInexact, Then);
      IRBuilder<> EB(Else);
      callHelper(EB, "__fe_div_" + Fm.Name, {{A, Fm}, {Bv, Fm}});
      return;
    }
  default:
    llvm_unreachable("not an arithmetic operation");
  }
}

void Instrumenter::arithmeticSpecial(unsigned Opcode, Value *A, Value *Bv,
                                     Value *R, const Format &Fm,
                                     Instruction *IP) {
  IRBuilder<> B(IP);
  // The results that need no flags: NaN operands propagate quietly unless
  // one of them is signaling, an infinite operand gives an exact infinity,
  // and a zero operand an exact zero.
  Value *Quiet =
      B.CreateAnd(B.CreateFCmpUNO(A, Bv),
                  B.CreateNot(B.CreateOr(isSignalingNaN(B, A, Fm),
                                         isSignalingNaN(B, Bv, Fm))));
  Value *RInf = isInf(B, R, Fm);
  Value *NoFlags = Quiet;
  Value *DivByZero = nullptr;
  StringRef Op;
  switch (Opcode) {
  case Instruction::FAdd:
  case Instruction::FSub:
    Op = Opcode == Instruction::FAdd ? "add" : "sub";
    NoFlags = B.CreateOr(
        NoFlags,
        B.CreateAnd(RInf, B.CreateOr(isInf(B, A, Fm), isInf(B, Bv, Fm))));
    break;
  case Instruction::FMul:
    Op = "mul";
    NoFlags = B.CreateOr(
        NoFlags,
        B.CreateAnd(isZero(B, R), B.CreateOr(isZero(B, A), isZero(B, Bv))));
    NoFlags = B.CreateOr(
        NoFlags,
        B.CreateAnd(RInf, B.CreateOr(isInf(B, A, Fm), isInf(B, Bv, Fm))));
    break;
  case Instruction::FDiv:
    Op = "div";
    NoFlags = B.CreateOr(
        NoFlags,
        B.CreateAnd(isZero(B, R), B.CreateOr(isZero(B, A), isInf(B, Bv, Fm))));
    NoFlags = B.CreateOr(NoFlags, B.CreateAnd(RInf, isInf(B, A, Fm)));
    // A finite nonzero dividend over zero raises division by zero alone.
    DivByZero =
        B.CreateAnd(isZero(B, Bv),
                    B.CreateAnd(B.CreateFCmpONE(A, ConstantFP::getZero(Fm.Ty)),
                                isFinite(B, A)));
    break;
  default:
    llvm_unreachable("not an arithmetic operation");
  }
  Instruction *T = ifThen(B.CreateNot(NoFlags), IP, Unlikely);
  if (DivByZero) {
    auto [Then, Else] = ifThenElse(DivByZero, T, nullptr);
    IRBuilder<> TB(Then);
    raise(TB, TB.getInt32(FEDivByZero));
    T = Else;
  }
  IRBuilder<> TB(T);
  callHelper(TB, "__fe_" + Op + "_" + Fm.Name, {{A, Fm}, {Bv, Fm}});
}

void Instrumenter::squareRoot(Value *A, Value *R, const Format &Fm,
                              Instruction *IP) {
  if (isa<FixedVectorType>(R->getType()) && Fm.isNative()) {
    IRBuilder<> B(IP);
    Value *Done =
        B.CreateAnd(B.CreateNot(B.CreateOrReduce(B.CreateFCmpUNO(R, R))),
                    B.CreateNot(lacks(B, FEInexact)));
    IP = ifThen(B.CreateNot(Done), IP, Unlikely);
  }
  forEachLane({A, R}, IP, [&](ArrayRef<Value *> L, Instruction *At) {
    scalarSquareRoot(L[0], L[1], Fm, At);
  });
}

void Instrumenter::scalarSquareRoot(Value *A, Value *R, const Format &Fm,
                                    Instruction *IP) {
  IRBuilder<> B(IP);
  if (!Fm.isNative()) {
    callHelper(B, "__fe_sqrt_" + Fm.Name, {{A, Fm}});
    return;
  }
  // A square root never overflows or underflows. A NaN result is invalid
  // unless the operand was a quiet NaN; otherwise only inexact remains.
  auto [NaN, Number] = ifThenElse(B.CreateFCmpUNO(R, R), IP, Unlikely);
  {
    IRBuilder<> NB(NaN);
    Value *QuietOperand =
        NB.CreateAnd(isNaN(NB, A, Fm), NB.CreateNot(isSignalingNaN(NB, A, Fm)));
    raiseIf(NB.CreateNot(QuietOperand), FEInvalid, NaN);
  }
  IRBuilder<> NB(Number);
  Value *E = exponentOf(NB, R, Fm);
  Type *Ty = E->getType();
  // Zeros and infinity are their own exact roots.
  Value *Check =
      NB.CreateAnd(lacks(NB, FEInexact),
                   NB.CreateICmpULT(NB.CreateSub(E, ConstantInt::get(Ty, 1)),
                                    ConstantInt::get(Ty, Fm.ExpMax - 1)));
  Instruction *T = ifThen(Check, Number, Unlikely);
  IRBuilder<> TB(T);
  if (Fm.Ty->isFloatTy()) {
    Value *RD = TB.CreateFPExt(R, TB.getDoubleTy());
    raiseIf(TB.CreateFCmpUNE(TB.CreateFMul(RD, RD),
                             TB.CreateFPExt(A, TB.getDoubleTy())),
            FEInexact, T);
    return;
  }
  auto [Then, Else] = ifThenElse(inProductRange(TB, R, Fm), T, Likely);
  IRBuilder<> PB(Then);
  Value *P = PB.CreateFMul(R, R);
  Value *Err = productError(PB, R, R, P);
  raiseIf(PB.CreateOr(PB.CreateFCmpUNE(P, A),
                      PB.CreateFCmpUNE(Err, ConstantFP::getZero(Fm.Ty))),
          FEInexact, Then);
  IRBuilder<> EB(Else);
  callHelper(EB, "__fe_sqrt_" + Fm.Name, {{A, Fm}});
}

void Instrumenter::truncation(Value *A, Value *R, const Format &Src,
                              const Format &Dst, Instruction *IP) {
  bool Native = Src.isNative() && Dst.isNative();
  if (isa<FixedVectorType>(R->getType()) && Native) {
    IRBuilder<> B(IP);
    Value *E = exponentOf(B, R, Dst);
    Type *Ty = E->getType();
    Value *Common = B.CreateICmpULT(B.CreateSub(E, ConstantInt::get(Ty, 2)),
                                    ConstantInt::get(Ty, Dst.ExpMax - 2));
    Value *Done = B.CreateAnd(B.CreateAndReduce(Common),
                              B.CreateNot(lacks(B, FEInexact)));
    IP = ifThen(B.CreateNot(Done), IP, Unlikely);
  }
  forEachLane({A, R}, IP, [&](ArrayRef<Value *> L, Instruction *At) {
    scalarTruncation(L[0], L[1], Src, Dst, At);
  });
}

void Instrumenter::scalarTruncation(Value *A, Value *R, const Format &Src,
                                    const Format &Dst, Instruction *IP) {
  IRBuilder<> B(IP);
  std::string Helper = ("__fe_trunc_" + Src.Name + "_" + Dst.Name).str();
  if (!(Src.isNative() && Dst.isNative())) {
    callHelper(B, Helper, {{A, Src}});
    return;
  }
  Value *E = exponentOf(B, R, Dst);
  Type *Ty = E->getType();
  Value *Common = B.CreateICmpULT(B.CreateSub(E, ConstantInt::get(Ty, 2)),
                                  ConstantInt::get(Ty, Dst.ExpMax - 2));
  auto [Then, Else] = ifThenElse(Common, IP, Likely);
  {
    // Widening back is exact, so it differs from the operand exactly when the
    // narrowing rounded.
    IRBuilder<> TB(Then);
    Value *Differs = TB.CreateFCmpUNE(TB.CreateFPExt(R, Src.Ty), A);
    raiseIf(TB.CreateAnd(lacks(TB, FEInexact), Differs), FEInexact, Then);
  }
  IRBuilder<> EB(Else);
  Value *NoFlags =
      EB.CreateOr(EB.CreateOr(isZero(EB, A), isInf(EB, A, Src)),
                  EB.CreateAnd(isNaN(EB, A, Src),
                               EB.CreateNot(isSignalingNaN(EB, A, Src))));
  Instruction *T = ifThen(EB.CreateNot(NoFlags), Else, Unlikely);
  IRBuilder<> TB(T);
  callHelper(TB, Helper, {{A, Src}});
}

SmallVector<Value *, 2>
Instrumenter::checkedOperands(ArrayRef<Value *> Operands) {
  // Constants that are not signaling NaNs need no check, and in comparisons
  // with a constant they are the common case.
  auto MayBeSignaling = [](Constant *C) {
    if (isa<UndefValue>(C))
      return false;
    if (auto *CFP = dyn_cast<ConstantFP>(C))
      return CFP->getValueAPF().isSignaling();
    auto *VT = dyn_cast<FixedVectorType>(C->getType());
    if (!VT)
      return true;
    for (unsigned L = 0, N = VT->getNumElements(); L < N; ++L) {
      Constant *E = C->getAggregateElement(L);
      if (!E || (!isa<UndefValue>(E) &&
                 (!isa<ConstantFP>(E) ||
                  cast<ConstantFP>(E)->getValueAPF().isSignaling())))
        return true;
    }
    return false;
  };
  SmallVector<Value *, 2> Checked;
  for (Value *V : Operands)
    if (!isa<Constant>(V) || MayBeSignaling(cast<Constant>(V)))
      Checked.push_back(V);
  return Checked;
}

void Instrumenter::signalingOperands(ArrayRef<Value *> Operands,
                                     const Format &Fm, Instruction *IP) {
  IRBuilder<> B(IP);
  SmallVector<Value *, 2> Checked = checkedOperands(Operands);
  if (Checked.empty())
    return;
  auto Reduce = [&](IRBuilder<> &RB, Value *V) {
    return isa<FixedVectorType>(V->getType()) ? RB.CreateOrReduce(V) : V;
  };
  if (Fm.isNative()) {
    // Unordered operands are rare; look at the bits only then.
    Value *Unordered = Checked.size() == 2
                           ? B.CreateFCmpUNO(Checked[0], Checked[1])
                           : B.CreateFCmpUNO(Checked[0], Checked[0]);
    IP = ifThen(Reduce(B, Unordered), IP, Unlikely);
    B.SetInsertPoint(IP);
  }
  Value *Any = nullptr;
  for (Value *V : Checked) {
    Value *S = Reduce(B, isSignalingNaN(B, V, Fm));
    Any = Any ? B.CreateOr(Any, S) : S;
  }
  raiseIf(Any, FEInvalid, IP);
}

void Instrumenter::roundToIntegral(Value *A, Value *R, const Format &Fm,
                                   Instruction *IP) {
  // rint raises inexact when the result differs from the operand.
  IRBuilder<> B(IP);
  auto Reduce = [&](IRBuilder<> &RB, Value *V) {
    return isa<FixedVectorType>(V->getType()) ? RB.CreateOrReduce(V) : V;
  };
  Value *Differs;
  if (Fm.isNative()) {
    Differs = B.CreateFCmpUNE(R, A);
  } else {
    Value *RBits = bitsOf(B, R, Fm);
    Differs =
        B.CreateOr(B.CreateICmpNE(RBits, bitsOf(B, A, Fm)), isNaN(B, A, Fm));
  }
  Instruction *T = ifThen(Reduce(B, Differs), IP, Unlikely);
  IRBuilder<> TB(T);
  Value *Signaling = isSignalingNaN(TB, A, Fm);
  Value *Inexact = TB.CreateAnd(
      TB.CreateNot(isNaN(TB, A, Fm)),
      Fm.isNative() ? TB.CreateFCmpONE(R, A)
                    : TB.CreateICmpNE(bitsOf(TB, R, Fm), bitsOf(TB, A, Fm)));
  Type *FlagTy = Signaling->getType()->getWithNewType(TB.getInt32Ty());
  Value *Flags = TB.CreateOr(
      TB.CreateSelect(Signaling, ConstantInt::get(FlagTy, FEInvalid),
                      ConstantInt::get(FlagTy, 0)),
      TB.CreateSelect(Inexact, ConstantInt::get(FlagTy, FEInexact),
                      ConstantInt::get(FlagTy, 0)));
  if (isa<FixedVectorType>(FlagTy))
    Flags = TB.CreateOrReduce(Flags);
  raise(TB, Flags);
}

void Instrumenter::toInteger(Value *A, Type *ResultTy, bool Signed,
                             const Format &Fm0, Instruction *IP) {
  IRBuilder<> B(IP);
  Format Fm = Fm0;
  // Half and bfloat widen to float exactly, and WebAssembly converts them that
  // way too.
  if (Fm.width() == 16) {
    A = B.CreateFPExt(A, A->getType()->getWithNewType(B.getFloatTy()));
    Fm = *formatOf(B.getFloatTy());
  }
  unsigned N = ResultTy->getScalarSizeInBits();
  const fltSemantics &Sem = Fm.Ty->getFltSemantics();
  // An integer as a value of the format, and whether it converted exactly.
  // Bounds beyond the format's range become infinite.
  auto Convert = [&](const APInt &I) {
    APFloat V(Sem);
    APFloat::opStatus St =
        V.convertFromAPInt(I, /*IsSigned=*/true, APFloat::rmNearestTiesToEven);
    return std::make_pair(ConstantFP::get(A->getType(), V),
                          St == APFloat::opOK);
  };
  // The operand converts when it truncates into [Lo, Hi), that is when it is
  // above Lo - 1 and below Hi. When the format cannot hold Lo - 1, no value
  // lies between it and Lo, and the test is A >= Lo. NaN fails both
  // comparisons. The comparisons use the operand itself rather than its
  // truncation, which would be a library call (truncl) for fp128, and musl's
  // raises inexact.
  APInt Lo = Signed ? APInt::getSignedMinValue(N).sext(N + 2) : APInt(N + 2, 0);
  APInt Hi = APInt::getOneBitSet(N + 2, Signed ? N - 1 : N);
  auto [BelowLo, BelowLoExact] = Convert(Lo - 1);
  Value *AboveLo = BelowLoExact ? B.CreateFCmpOGT(A, BelowLo)
                                : B.CreateFCmpOGE(A, Convert(Lo).first);
  Value *Valid = B.CreateAnd(AboveLo, B.CreateFCmpOLT(A, Convert(Hi).first));
  // A fraction is dropped when the significand has bits below the binary
  // point, whose position follows from the exponent.
  Value *Bits = bitsOf(B, A, Fm);
  Type *IntTy = Bits->getType();
  unsigned W = Fm.width();
  uint64_t Bias = (Fm.ExpMax >> 1);
  Value *E = exponentOf(B, A, Fm);
  Value *AbsBits =
      B.CreateAnd(Bits, ConstantInt::get(IntTy, APInt::getSignedMaxValue(W)));
  // Below one: any nonzero value is all fraction.
  Value *BelowOne = B.CreateICmpULT(E, ConstantInt::get(IntTy, Bias));
  // Below 2^Mantissa: the low (Bias + Mantissa - E) bits are the fraction.
  Value *Fractional =
      B.CreateICmpULT(E, ConstantInt::get(IntTy, Bias + Fm.Mantissa));
  Value *Shift = B.CreateSelect(
      B.CreateAnd(Fractional, B.CreateNot(BelowOne)),
      B.CreateSub(ConstantInt::get(IntTy, Bias + Fm.Mantissa), E),
      ConstantInt::get(IntTy, 0));
  Value *Zero = ConstantInt::get(IntTy, 0);
  Value *Fraction =
      B.CreateSelect(BelowOne, B.CreateICmpNE(AbsBits, Zero),
                     B.CreateAnd(Fractional, lowBitsNonZero(B, Bits, Shift)));
  Value *Raises = B.CreateOr(B.CreateNot(Valid), Fraction);
  Value *Any = isa<FixedVectorType>(Raises->getType())
                   ? B.CreateOrReduce(Raises)
                   : Raises;
  Instruction *Then = ifThen(Any, IP, Unlikely);
  IRBuilder<> TB(Then);
  Type *FlagTy = Valid->getType()->getWithNewType(TB.getInt32Ty());
  // Invalid alone when out of range (as x86's cvttsd2si), inexact when the
  // conversion dropped a fraction.
  Value *Flags = TB.CreateSelect(
      Valid,
      TB.CreateSelect(Fraction, ConstantInt::get(FlagTy, FEInexact),
                      ConstantInt::get(FlagTy, 0)),
      ConstantInt::get(FlagTy, FEInvalid));
  if (isa<FixedVectorType>(FlagTy))
    Flags = TB.CreateOrReduce(Flags);
  raise(TB, Flags);
}

void Instrumenter::fromInteger(Value *X, Value *R, bool Signed,
                               const Format &Fm, Instruction *IP) {
  unsigned N = X->getType()->getScalarSizeInBits();
  unsigned Precision = Fm.Mantissa + 1;
  // The largest magnitudes: 2^(N-1) for signed (INT_MIN), 2^N - 1 for
  // unsigned.
  bool CanBeInexact = (Signed ? N - 1 : N) > Precision;
  APFloat Max(Fm.Ty->getFltSemantics());
  APFloat::opStatus St = Max.convertFromAPInt(
      Signed ? APInt::getSignedMinValue(N) : APInt::getMaxValue(N), Signed,
      APFloat::rmNearestTiesToEven);
  bool CanOverflow = (St & APFloat::opOverflow) || Max.isInfinity();
  if (!CanBeInexact && !CanOverflow)
    return;
  IRBuilder<> B(IP);
  Type *Ty = X->getType();
  Type *FlagTy = Ty->getWithNewType(B.getInt32Ty());
  Value *Zero = ConstantInt::get(FlagTy, 0);
  Value *Flags = Zero;
  if (CanBeInexact) {
    // The magnitude fits the significand when it is below its lowest set bit
    // times 2^Precision. INT_MIN's magnitude wraps to 2^(N-1), which is right
    // as unsigned.
    Value *Abs = X;
    if (Signed) {
      Value *Sign = B.CreateAShr(X, N - 1);
      Abs = B.CreateSub(B.CreateXor(X, Sign), Sign);
    }
    Value *Lowest = B.CreateAnd(Abs, B.CreateNeg(Abs));
    Value *Exact =
        B.CreateOr(B.CreateICmpEQ(Abs, ConstantInt::get(Ty, 0)),
                   B.CreateICmpULT(B.CreateLShr(Abs, Precision), Lowest));
    Flags = B.CreateSelect(Exact, Zero, ConstantInt::get(FlagTy, FEInexact));
  }
  if (CanOverflow)
    Flags = B.CreateOr(
        Flags,
        B.CreateSelect(isInf(B, R, Fm),
                       ConstantInt::get(FlagTy, FEOverflow | FEInexact), Zero));
  if (isa<FixedVectorType>(FlagTy))
    Flags = B.CreateOrReduce(Flags);
  Instruction *T = ifThen(B.CreateICmpNE(Flags, B.getInt32(0)), IP, Unlikely);
  IRBuilder<> TB(T);
  raise(TB, Flags);
}

void Instrumenter::instrument(Instruction *I, Instruction *IP) {
  Pad = Pads.lookup(I);
  if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    arithmetic(BO->getOpcode(), BO->getOperand(0), BO->getOperand(1), BO,
               *formatOf(BO->getType()), IP);
    return;
  }
  if (auto *Cmp = dyn_cast<FCmpInst>(I)) {
    signalingOperands({Cmp->getOperand(0), Cmp->getOperand(1)},
                      *formatOf(Cmp->getOperand(0)->getType()), IP);
    return;
  }
  switch (I->getOpcode()) {
  case Instruction::FPTrunc:
    truncation(I->getOperand(0), I, *formatOf(I->getOperand(0)->getType()),
               *formatOf(I->getType()), IP);
    return;
  case Instruction::FPExt:
    signalingOperands({I->getOperand(0)},
                      *formatOf(I->getOperand(0)->getType()), IP);
    return;
  case Instruction::FPToSI:
  case Instruction::FPToUI:
    toInteger(I->getOperand(0), I->getType(),
              I->getOpcode() == Instruction::FPToSI,
              *formatOf(I->getOperand(0)->getType()), IP);
    return;
  case Instruction::SIToFP:
  case Instruction::UIToFP:
    fromInteger(I->getOperand(0), I, I->getOpcode() == Instruction::SIToFP,
                *formatOf(I->getType()), IP);
    return;
  default:
    break;
  }
  auto *II = cast<IntrinsicInst>(I);
  Intrinsic::ID ID = II->getIntrinsicID();
  // __fp16 conversions: the half value travels as its bits in an i16.
  Format Half = *formatOf(Type::getHalfTy(Ctx));
  if (ID == Intrinsic::convert_to_fp16) {
    Value *A = II->getArgOperand(0);
    truncation(A, II, *formatOf(A->getType()), Half, IP);
  } else if (ID == Intrinsic::convert_from_fp16) {
    signalingOperands({II->getArgOperand(0)}, Half, IP);
  } else if (ID == Intrinsic::sqrt) {
    squareRoot(II->getArgOperand(0), II, *formatOf(II->getType()), IP);
  } else if (ID == Intrinsic::rint) {
    roundToIntegral(II->getArgOperand(0), II, *formatOf(II->getType()), IP);
  } else if (isFPToInt(ID)) {
    Value *A = II->getArgOperand(0);
    bool Signed =
        ID == Intrinsic::fptosi_sat || ID == Intrinsic::wasm_trunc_signed;
    toInteger(A, II->getType(), Signed, *formatOf(A->getType()), IP);
  } else {
    assert(raisesOnlyForSignalingNaN(ID));
    SmallVector<Value *, 2> Ops(II->args());
    signalingOperands(Ops, *formatOf(Ops[0]->getType()), IP);
  }
}

Value *Instrumenter::quietOperands(ArrayRef<Value *> Operands, const Format &Fm,
                                   IRBuilder<> &B) {
  // No operand is a NaN (constants that are not signaling NaNs aside).
  SmallVector<Value *, 2> Checked = checkedOperands(Operands);
  if (Checked.empty())
    return B.getTrue();
  Value *AnyNaN = nullptr;
  if (Fm.isNative()) {
    AnyNaN = Checked.size() == 2 ? B.CreateFCmpUNO(Checked[0], Checked[1])
                                 : B.CreateFCmpUNO(Checked[0], Checked[0]);
  } else {
    for (Value *V : Checked) {
      Value *N = isNaN(B, V, Fm);
      AnyNaN = AnyNaN ? B.CreateOr(AnyNaN, N) : N;
    }
  }
  if (isa<FixedVectorType>(AnyNaN->getType()))
    AnyNaN = B.CreateOrReduce(AnyNaN);
  return B.CreateNot(AnyNaN);
}

/// Whether instrument() can be deferred to the end of a run of operations and
/// skipped there when settles() holds for all of them.
static bool canGroup(Instruction &I) {
  auto Native = [](Type *Ty) { return formatOf(Ty)->isNative(); };
  if (isa<BinaryOperator>(I))
    return Native(I.getType());
  if (isa<FCmpInst>(I) || isa<FPExtInst>(I))
    return true;
  if (isa<FPTruncInst>(I))
    return Native(I.getType()) && Native(I.getOperand(0)->getType());
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  Intrinsic::ID ID = II->getIntrinsicID();
  if (ID == Intrinsic::sqrt || ID == Intrinsic::rint)
    return Native(II->getType());
  return raisesOnlyForSignalingNaN(ID);
}

Value *Instrumenter::settles(Instruction *I, IRBuilder<> &B, bool &NeedsInexact,
                             Sums &Finite) {
  // A condition under which I raises nothing but inexact (NeedsInexact) or
  // nothing at all. It is what the full check tests first, written with
  // floating-point comparisons, which are the cheapest test on WebAssembly.
  // A scalar result that must be finite is added to Finite instead (true is
  // returned), and the run tests the sum once.
  NeedsInexact = false;
  auto All = [&](Value *V) {
    return isa<FixedVectorType>(V->getType()) ? B.CreateAndReduce(V) : V;
  };
  auto IsFinite = [&](Value *R) -> Value * {
    Type *Ty = R->getType();
    if (isa<FixedVectorType>(Ty))
      return All(isFinite(B, R));
    Finite[Ty].push_back(R);
    return B.getTrue();
  };
  // Normal and above the smallest binade.
  auto Common = [&](Value *R) {
    Type *Ty = R->getType();
    const fltSemantics &Sem = Ty->getScalarType()->getFltSemantics();
    APFloat Lo = scalbn(APFloat::getSmallestNormalized(Sem), 1,
                        APFloat::rmNearestTiesToEven);
    Value *Abs = B.CreateUnaryIntrinsic(Intrinsic::fabs, R);
    return B.CreateAnd(All(B.CreateFCmpOGE(Abs, ConstantFP::get(Ty, Lo))),
                       IsFinite(R));
  };
  if (auto *BO = dyn_cast<BinaryOperator>(I)) {
    NeedsInexact = true;
    if (BO->getOpcode() == Instruction::FAdd ||
        BO->getOpcode() == Instruction::FSub)
      return IsFinite(BO);
    return Common(BO);
  }
  if (isa<FCmpInst>(I) || isa<FPExtInst>(I)) {
    SmallVector<Value *, 2> Ops(I->operands());
    return quietOperands(Ops, *formatOf(Ops[0]->getType()), B);
  }
  if (isa<FPTruncInst>(I)) {
    NeedsInexact = true;
    return Common(I);
  }
  auto *II = cast<IntrinsicInst>(I);
  Intrinsic::ID ID = II->getIntrinsicID();
  if (ID == Intrinsic::sqrt || ID == Intrinsic::rint) {
    NeedsInexact = true;
    Value *Of = ID == Intrinsic::sqrt ? II : II->getArgOperand(0);
    return quietOperands({Of}, *formatOf(II->getType()), B);
  }
  SmallVector<Value *, 2> Ops(II->args());
  return quietOperands(Ops, *formatOf(Ops[0]->getType()), B);
}

/// Whether I is an operation this pass instruments.
bool isInstrumented(Instruction &I) {
  auto HasFormat = [](Value *V) { return formatOf(V->getType()).has_value(); };
  switch (I.getOpcode()) {
  case Instruction::FAdd:
  case Instruction::FSub:
  case Instruction::FMul:
  case Instruction::FDiv:
  case Instruction::FPTrunc:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
    return HasFormat(&I);
  case Instruction::FPExt:
  case Instruction::FPToSI:
  case Instruction::FPToUI:
    return HasFormat(I.getOperand(0));
  case Instruction::FCmp: {
    auto Pred = cast<FCmpInst>(I).getPredicate();
    return Pred != FCmpInst::FCMP_FALSE && Pred != FCmpInst::FCMP_TRUE &&
           HasFormat(I.getOperand(0));
  }
  default:
    break;
  }
  auto *II = dyn_cast<IntrinsicInst>(&I);
  if (!II)
    return false;
  Intrinsic::ID ID = II->getIntrinsicID();
  if (ID == Intrinsic::sqrt || ID == Intrinsic::rint ||
      raisesOnlyForSignalingNaN(ID))
    return HasFormat(II);
  if (isFPToInt(ID) || ID == Intrinsic::convert_to_fp16)
    return HasFormat(II->getArgOperand(0));
  if (ID == Intrinsic::convert_from_fp16)
    return true;
  return false;
}

bool Instrumenter::run() {
  SmallVector<Instruction *, 64> Work;
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    // WebAssembly computes fmuladd as a multiplication and an addition, each
    // rounded (it has no fused multiply-add). Split it so that both are
    // checked.
    if (auto *II = dyn_cast<IntrinsicInst>(&I);
        II && II->getIntrinsicID() == Intrinsic::fmuladd) {
      IRBuilder<> B(II);
      B.setFastMathFlags(II->getFastMathFlags());
      Value *Mul = B.CreateFMul(II->getArgOperand(0), II->getArgOperand(1));
      Value *Add = B.CreateFAdd(Mul, II->getArgOperand(2));
      Add->takeName(II);
      II->replaceAllUsesWith(Add);
      II->eraseFromParent();
      for (Value *V : {Mul, Add})
        if (auto *NI = dyn_cast<Instruction>(V); NI && isInstrumented(*NI))
          Work.push_back(NI);
      continue;
    }
    if (isInstrumented(I))
      Work.push_back(&I);
  }
  if (Work.empty())
    return false;
  if (F.hasPersonalityFn() &&
      isScopedEHPersonality(classifyEHPersonality(F.getPersonalityFn()))) {
    DenseMap<BasicBlock *, ColorVector> Colors = colorEHFunclets(F);
    for (Instruction *I : Work) {
      const ColorVector &C = Colors.lookup(I->getParent());
      if (C.size() == 1 && C.front() != &F.getEntryBlock())
        if (auto *FPI = dyn_cast<FuncletPadInst>(C.front()->getFirstNonPHIIt()))
          Pads[I] = FPI;
    }
  }

  // The flags are sticky and only a call can look at them (fetestexcept) or
  // clear them, so the operations of a block up to its next call can be
  // checked together at that point. One test then covers them all in the
  // common case (each raises at most inexact, which is set already), and
  // only otherwise do they get their full checks, one after another.
  struct Run {
    SmallVector<Instruction *, 8> Ops;
    Instruction *End = nullptr;
  };
  SmallVector<Run, 16> Runs;
  SmallVector<SmallVector<BasicBlock *, 16>, 8> Cold;
  SmallVector<Instruction *, 16> Singles;
  SmallPtrSet<Instruction *, 32> Instrumented(llvm::from_range, Work);
  for (BasicBlock &BB : F) {
    Run Current;
    for (Instruction &I : BB) {
      if (I.isTerminator() || (isa<CallBase>(I) && !isa<IntrinsicInst>(I))) {
        if (!Current.Ops.empty()) {
          Current.End = &I;
          Runs.push_back(std::move(Current));
          Current = Run();
        }
        continue;
      }
      if (!Instrumented.contains(&I))
        continue;
      if (canGroup(I))
        Current.Ops.push_back(&I);
      else
        Singles.push_back(&I);
    }
  }

  // The checks come after the operations collected above, so their own
  // arithmetic is not checked.
  for (Instruction *I : Singles)
    instrument(I, I->getNextNode());
  for (Run &R : Runs) {
    IRBuilder<> B(R.End);
    Value *Settled = nullptr;
    bool NeedsInexact = false;
    Sums Finite;
    for (Instruction *I : R.Ops) {
      bool Needs;
      Value *S = settles(I, B, Needs, Finite);
      NeedsInexact |= Needs;
      Settled = Settled ? B.CreateAnd(Settled, S) : S;
    }
    for (auto &[Ty, Terms] : Finite) {
      // Addition, subtraction and multiplication carry an infinity or a NaN
      // operand into their result (an infinity times zero or minus itself is
      // NaN), and so does division in its dividend. A term used that way by
      // another term is finite when that one is, and need not be added.
      SmallPtrSet<Value *, 8> InSum(llvm::from_range, Terms);
      auto Implied = [&](Value *V) {
        for (User *U : V->users()) {
          auto *BO = dyn_cast<BinaryOperator>(U);
          if (!BO || !InSum.contains(BO))
            continue;
          unsigned Opc = BO->getOpcode();
          if (Opc == Instruction::FAdd || Opc == Instruction::FSub ||
              Opc == Instruction::FMul ||
              (Opc == Instruction::FDiv && BO->getOperand(0) == V))
            return true;
        }
        return false;
      };
      llvm::erase_if(Terms, Implied);
      while (Terms.size() > 1) {
        SmallVector<Value *, 8> Next;
        for (unsigned I = 0; I + 1 < Terms.size(); I += 2)
          Next.push_back(B.CreateFAdd(Terms[I], Terms[I + 1]));
        if (Terms.size() % 2)
          Next.push_back(Terms.back());
        Terms = std::move(Next);
      }
      Settled =
          B.CreateAnd(Settled, B.CreateFCmpOEQ(B.CreateFSub(Terms[0], Terms[0]),
                                               ConstantFP::getZero(Ty)));
    }
    if (NeedsInexact)
      Settled = B.CreateAnd(Settled, B.CreateNot(lacks(B, FEInexact)));
    Instruction *Check = ifThen(B.CreateNot(Settled), R.End, Unlikely);
    BasicBlock *Head = Check->getParent(), *Join = R.End->getParent();
    bool InFunclet = false;
    for (Instruction *I : R.Ops) {
      instrument(I, Check);
      InFunclet |= Pads.count(I) != 0;
    }
    // A funclet's blocks cannot be moved out of it
    if (!InFunclet)
      Cold.push_back(regionBetween(Head, Join));
  }

  // Move the full checks of each run out to a function of their own.
  // WebAssembly has structured control flow, so a cold block stays inside the
  // loop that contains it, and the engine compiles it as part of the loop: the
  // hundreds of instructions of a run's full checks then make the loop's own
  // code slower (more values live across it, registers spilled), even when they
  // never run. A call is all that stays in the loop.
  if (!Cold.empty()) {
    CodeExtractorAnalysisCache CEAC(F);
    for (SmallVector<BasicBlock *, 16> &Blocks : Cold) {
      CodeExtractor CE(Blocks, nullptr, false, nullptr, nullptr, nullptr, false,
                       false, nullptr, "fe_check");
      if (Function *Outlined = CE.extractCodeRegion(CEAC)) {
        Outlined->addFnAttr(Attribute::Cold);
        Outlined->addFnAttr(Attribute::NoInline);
        Outlined->addFnAttr(OutlinedAttr);
      }
    }
  }
  return true;
}

SmallVector<BasicBlock *, 16> Instrumenter::regionBetween(BasicBlock *Head,
                                                          BasicBlock *Join) {
  // The blocks reachable from Head without going through Join: the full
  // checks of a run, which all lead back to Join.
  SmallVector<BasicBlock *, 16> Region{Head};
  SmallPtrSet<BasicBlock *, 16> Seen{Head, Join};
  for (unsigned I = 0; I < Region.size(); ++I)
    for (BasicBlock *Succ : successors(Region[I]))
      if (Seen.insert(Succ).second)
        Region.push_back(Succ);
  return Region;
}

class WebAssemblyFPExceptionFlags final : public FunctionPass {
  StringRef getPassName() const override {
    return "WebAssembly FP Exception Flags";
  }

  bool runOnFunction(Function &F) override {
    if (F.hasFnAttribute(OutlinedAttr))
      return false;
    return Instrumenter(F).run();
  }

public:
  static char ID;
  WebAssemblyFPExceptionFlags() : FunctionPass(ID) {}
};

} // end anonymous namespace

char WebAssemblyFPExceptionFlags::ID = 0;
INITIALIZE_PASS(WebAssemblyFPExceptionFlags, DEBUG_TYPE,
                "Make floating-point operations raise IEEE 754 exception flags",
                false, false)

FunctionPass *llvm::createWebAssemblyFPExceptionFlags() {
  return new WebAssemblyFPExceptionFlags();
}
