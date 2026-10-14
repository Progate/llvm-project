; RUN: llc < %s -mtriple=wasm32-wasip1 -stop-after=wasm-fp-exception-flags | FileCheck %s
; RUN: llc < %s -mtriple=wasm32-wasip1 -stop-after=wasm-fp-exception-flags | FileCheck %s --check-prefix=OUT
; RUN: llc < %s -mtriple=wasm32-unknown-unknown -wasm-raise-fp-exceptions -stop-after=wasm-fp-exception-flags | FileCheck %s
; RUN: llc < %s -mtriple=wasm32-unknown-unknown -stop-after=wasm-fp-exception-flags | FileCheck %s --check-prefix=OFF
; RUN: llc < %s -mtriple=wasm32-wasip1 -wasm-raise-fp-exceptions=false -stop-after=wasm-fp-exception-flags | FileCheck %s --check-prefix=OFF
; RUN: llc < %s -mtriple=wasm32-wasip1 -mattr=+simd128 -O2 -filetype=obj -o /dev/null
; RUN: llc < %s -mtriple=wasm32-wasip1 -mattr=+simd128 -O0 -filetype=obj -o /dev/null
; RUN: llc < %s -mtriple=wasm32-wasip1 -mattr=+simd128 -relocation-model=pic -filetype=obj -o /dev/null
; RUN: llc < %s -mtriple=wasm32-wasip1 -mattr=+simd128 -O2 | FileCheck %s --check-prefix=ASM

; Floating-point operations raise the IEEE 754 exception flags in the wasm
; global __fe_status, which the C library defines. The operations of a block
; up to its next call are tested together (CHECK); their full checks are moved
; out to a function (OUT), which the test calls only when it fails.

; OFF-NOT: __fe_status
; CHECK: @__fe_status = external addrspace(1) global i32
; ASM: .globaltype __fe_status, i32

; A finite sum raises at most inexact, so with inexact set nothing more is
; done. A value minus itself is zero exactly when it is finite. Otherwise the
; full check runs: TwoSum decides inexact, and an infinite or NaN error or
; result goes to the C library.
define double @add(double %a, double %b) {
; CHECK-LABEL: @add(
; CHECK: %r = fadd double %a, %b
; CHECK-NEXT: [[D:%.*]] = fsub double %r, %r
; CHECK-NEXT: fcmp oeq double [[D]], 0.000000e+00
; CHECK: [[ST0:%.*]] = load i32, ptr addrspace(1) @__fe_status
; CHECK-NEXT: and i32 [[ST0]], 32
; CHECK: br i1
; CHECK: call void @add.fe_check(double %r, double %a, double %b)
; CHECK: ret double %r
; OUT-LABEL: define internal void @add.fe_check(double %r, double %a, double %b)
; OUT: [[BITS:%.*]] = bitcast double %r to i64
; OUT-NEXT: [[SHIFT:%.*]] = lshr i64 [[BITS]], 52
; OUT-NEXT: [[EXP:%.*]] = and i64 [[SHIFT]], 2047
; OUT-NEXT: [[FINITE:%.*]] = icmp ne i64 [[EXP]], 2047
; OUT-NEXT: br i1 [[FINITE]]
; OUT: [[ST:%.*]] = load i32, ptr addrspace(1) @__fe_status
; OUT-NEXT: [[IX:%.*]] = and i32 [[ST]], 32
; OUT-NEXT: icmp eq i32 [[IX]], 0
; OUT: [[B1:%.*]] = fsub double %r, %a
; OUT-NEXT: [[A1:%.*]] = fsub double %r, [[B1]]
; OUT-NEXT: [[DA:%.*]] = fsub double %a, [[A1]]
; OUT-NEXT: [[DB:%.*]] = fsub double %b, [[B1]]
; OUT-NEXT: [[ERR:%.*]] = fadd double [[DA]], [[DB]]
; OUT: call void @__fe_add_f64(i64 {{%.*}}, i64 {{%.*}})
; OUT: fcmp one double [[ERR]], 0.000000e+00
; OUT: or i32 {{%.*}}, 32
; OUT: store i32 {{%.*}}, ptr addrspace(1) @__fe_status
  %r = fadd double %a, %b
  ret double %r
}

; A product must be normal and above the smallest binade to rule out
; underflow. A float product is exact in double.
define float @mul_f32(float %a, float %b) {
; CHECK-LABEL: @mul_f32(
; CHECK: %r = fmul float %a, %b
; CHECK-NEXT: [[ABS:%.*]] = call float @llvm.fabs.f32(float %r)
; CHECK-NEXT: fcmp oge float [[ABS]], f0x01000000
; CHECK: fsub float %r, %r
; CHECK: call void @mul_f32.fe_check(float %r, float %a, float %b)
; OUT-LABEL: define internal void @mul_f32.fe_check(
; OUT: [[E:%.*]] = and i32 {{%.*}}, 255
; OUT-NEXT: [[E2:%.*]] = sub i32 [[E]], 2
; OUT-NEXT: icmp ult i32 [[E2]], 253
; OUT: [[AD:%.*]] = fpext float %a to double
; OUT-NEXT: [[BD:%.*]] = fpext float %b to double
; OUT-NEXT: [[P:%.*]] = fmul double [[AD]], [[BD]]
; OUT-NEXT: [[RD:%.*]] = fpext float %r to double
; OUT-NEXT: fcmp une double [[P]], [[RD]]
; OUT: call void @__fe_mul_f32(i32 {{%.*}}, i32 {{%.*}})
  %r = fmul float %a, %b
  ret float %r
}

; A double product uses Dekker's product when both operands are within range.
define double @mul_f64(double %a, double %b) {
; OUT-LABEL: define internal void @mul_f64.fe_check(
; OUT: fmul double %a, f0x41A0000002000000
; OUT: call void @__fe_mul_f64(
  %r = fmul double %a, %b
  ret double %r
}

; A finite nonzero dividend over zero raises division by zero inline.
define double @div(double %a, double %b) {
; OUT-LABEL: define internal void @div.fe_check(
; OUT: or i32 {{%.*}}, 4
; OUT: call void @__fe_div_f64(
  %r = fdiv double %a, %b
  ret double %r
}

; A NaN root raises invalid unless the operand was a quiet NaN.
define double @sqrt(double %a) {
; CHECK-LABEL: @sqrt(
; CHECK: %r = call double @llvm.sqrt.f64(double %a)
; CHECK-NEXT: fcmp uno double %r, %r
; OUT-LABEL: define internal void @sqrt.fe_check(
; OUT: [[NAN:%.*]] = fcmp uno double %r, %r
; OUT: or i32 {{%.*}}, 1
; OUT: call void @__fe_sqrt_f64(
  %r = call double @llvm.sqrt.f64(double %a)
  ret double %r
}

; Comparisons are quiet: invalid only for signaling NaNs, looked at only when
; the operands are unordered. A constant operand needs no check.
define i1 @compare(double %a, double %b) {
; CHECK-LABEL: @compare(
; CHECK: %r = fcmp olt double %a, %b
; CHECK-NEXT: fcmp uno double %a, %b
; CHECK-NOT: load
; CHECK: call void @compare.fe_check(double %a, double %b)
; OUT-LABEL: define internal void @compare.fe_check(
; OUT: [[UNO:%.*]] = fcmp uno double %a, %b
; OUT-NEXT: br i1 [[UNO]]
; OUT: sub i64 {{%.*}}, 9218868437227405313
; OUT: icmp ult i64 {{%.*}}, 2251799813685247
; OUT: or i32 {{%.*}}, 1
  %r = fcmp olt double %a, %b
  ret i1 %r
}

define i1 @compare_constant(double %a) {
; CHECK-LABEL: @compare_constant(
; CHECK: %r = fcmp olt double %a, 0.000000e+00
; CHECK-NEXT: [[UNO:%.*]] = fcmp uno double %a, %a
  %r = fcmp olt double %a, 0.0
  ret i1 %r
}

; Narrowing is exact when widening back gives the operand.
define float @trunc(double %a) {
; OUT-LABEL: define internal void @trunc.fe_check(
; OUT: [[W:%.*]] = fpext float %r to double
; OUT-NEXT: fcmp une double [[W]], %a
; OUT: call void @__fe_trunc_f64_f32(i64
  %r = fptrunc double %a to float
  ret float %r
}

; Widening raises invalid for a signaling NaN.
define double @ext(float %a) {
; CHECK-LABEL: @ext(
; CHECK: %r = fpext float %a to double
; CHECK-NEXT: fcmp uno float %a, %a
  %r = fpext float %a to double
  ret double %r
}

; A conversion to an integer raises invalid out of range (NaN included) and
; inexact when it drops a fraction.
define i32 @to_int(double %a) {
; CHECK-LABEL: @to_int(
; CHECK: %r = fptosi double %a to i32
; CHECK-NEXT: fcmp ogt double %a, f0xC1E0000000200000
; CHECK-NEXT: fcmp olt double %a, f0x41E0000000000000
; CHECK: select i1 {{%.*}}, i32 {{%.*}}, i32 1
  %r = fptosi double %a to i32
  ret i32 %r
}

; A float has no room for 2^31 - 1, so nothing lies between -2^31 - 1 and
; -2^31.
define i32 @to_int_sat(float %a) {
; CHECK-LABEL: @to_int_sat(
; CHECK: %r = call i32 @llvm.fptosi.sat.i32.f32(float %a)
; CHECK-NEXT: fcmp oge float %a, f0xCF000000
; CHECK-NEXT: fcmp olt float %a, f0x4F000000
  %r = call i32 @llvm.fptosi.sat.i32.f32(float %a)
  ret i32 %r
}

; A conversion from an integer is inexact when the magnitude has more
; significant bits than the significand. int32 to double never is.
define double @from_int(i64 %x) {
; CHECK-LABEL: @from_int(
; CHECK: %r = sitofp i64 %x to double
; CHECK-NEXT: [[SIGN:%.*]] = ashr i64 %x, 63
; CHECK: lshr i64 {{%.*}}, 53
; CHECK: or i32 {{%.*}}, {{%.*}}
  %r = sitofp i64 %x to double
  ret double %r
}

define double @from_int32(i32 %x) {
; CHECK-LABEL: @from_int32(
; CHECK-NEXT: %r = sitofp i32 %x to double
; CHECK-NEXT: ret double %r
  %r = sitofp i32 %x to double
  ret double %r
}

; rint raises inexact when it changes the value.
define double @rint(double %a) {
; CHECK-LABEL: @rint(
; CHECK: %r = call double @llvm.rint.f64(double %a)
; CHECK-NEXT: fcmp uno double %a, %a
; OUT-LABEL: define internal void @rint.fe_check(
; OUT: fcmp une double %r, %a
  %r = call double @llvm.rint.f64(double %a)
  ret double %r
}

; WebAssembly computes fmuladd as two rounded operations; both are checked.
define double @muladd(double %a, double %b, double %c) {
; CHECK-LABEL: @muladd(
; CHECK-NOT: llvm.fmuladd
; CHECK: [[M:%.*]] = fmul double %a, %b
; CHECK-NEXT: %r = fadd double [[M]], %c
; OUT-LABEL: define internal void @muladd.fe_check(
; OUT: call void @__fe_mul_f64(
; OUT: call void @__fe_add_f64(
  %r = call double @llvm.fmuladd.f64(double %a, double %b, double %c)
  ret double %r
}

; Formats without instructions go to the C library.
define fp128 @add_f128(fp128 %a, fp128 %b) {
; CHECK-LABEL: @add_f128(
; CHECK: %r = fadd fp128 %a, %b
; CHECK-NEXT: [[A:%.*]] = bitcast fp128 %a to i128
; CHECK-NEXT: [[B:%.*]] = bitcast fp128 %b to i128
; CHECK-NEXT: call void @__fe_add_f128(i128 [[A]], i128 [[B]])
  %r = fadd fp128 %a, %b
  ret fp128 %r
}

define half @add_f16(half %a, half %b) {
; CHECK-LABEL: @add_f16(
; CHECK: %r = fadd half %a, %b
; CHECK: zext i16 {{%.*}} to i32
; CHECK: call void @__fe_add_f16(i32 {{%.*}}, i32 {{%.*}})
  %r = fadd half %a, %b
  ret half %r
}

; Vectors are checked a lane at a time only when some lane could raise more
; than inexact or inexact is clear.
define <2 x double> @add_vector(<2 x double> %a, <2 x double> %b) {
; CHECK-LABEL: @add_vector(
; CHECK: %r = fadd <2 x double> %a, %b
; CHECK-NEXT: call <2 x double> @llvm.fabs.v2f64(<2 x double> %r)
; CHECK: call i1 @llvm.vector.reduce.and.v2i1(
; OUT-LABEL: define internal void @add_vector.fe_check(
; OUT: bitcast <2 x double> %r to <2 x i64>
; OUT: extractelement <2 x double> %a, i64 0
; OUT: call void @__fe_add_f64(
; OUT: extractelement <2 x double> %a, i64 1
; OUT: call void @__fe_add_f64(
  %r = fadd <2 x double> %a, %b
  ret <2 x double> %r
}

define <4 x i1> @compare_vector(<4 x float> %a, <4 x float> %b) {
; CHECK-LABEL: @compare_vector(
; CHECK: %r = fcmp olt <4 x float> %a, %b
; CHECK-NEXT: [[UNO:%.*]] = fcmp uno <4 x float> %a, %b
; CHECK-NEXT: call i1 @llvm.vector.reduce.or.v4i1(<4 x i1> [[UNO]])
  %r = fcmp olt <4 x float> %a, %b
  ret <4 x i1> %r
}

; Operations that WebAssembly does exactly raise nothing.
define double @exact(double %a, double %b) {
; CHECK-LABEL: @exact(
; CHECK-NEXT: [[N:%.*]] = fneg double %a
; CHECK-NEXT: [[F:%.*]] = call double @llvm.fabs.f64(double [[N]])
; CHECK-NEXT: %r = call double @llvm.copysign.f64(double [[F]], double %b)
; CHECK-NEXT: ret double %r
  %n = fneg double %a
  %f = call double @llvm.fabs.f64(double %n)
  %r = call double @llvm.copysign.f64(double %f, double %b)
  ret double %r
}

; A call may read or clear the flags (fetestexcept), so the operations before
; it are checked before it, and those after it after. %x is finite when %y is
; (an infinity or NaN operand of an addition makes its result one too), so
; only %y is tested for it.
define double @runs(double %a, double %b) {
; CHECK-LABEL: @runs(
; CHECK: %x = fmul double %a, %b
; CHECK-NEXT: %y = fadd double %x, %a
; CHECK-NEXT: call double @llvm.fabs.f64(double %x)
; CHECK-NOT: fsub double %x, %x
; CHECK: fsub double %y, %y
; CHECK: load i32, ptr addrspace(1) @__fe_status
; CHECK-NOT: call i32 @fetestexcept
; CHECK: call void @runs.fe_check(double %x, double %a, double %b, double %y)
; CHECK: %t = call i32 @fetestexcept(i32 61)
; CHECK: %z = fsub double %y, %b
; CHECK: call void @runs.fe_check.1(double %z, double %y, double %b)
; CHECK: ret double %z
; OUT-LABEL: define internal void @runs.fe_check(
; OUT: call void @__fe_mul_f64(
; OUT: call void @__fe_add_f64(
; OUT-LABEL: define internal void @runs.fe_check.1(
; OUT: call void @__fe_sub_f64(
  %x = fmul double %a, %b
  %y = fadd double %x, %a
  %t = call i32 @fetestexcept(i32 61)
  %z = fsub double %y, %b
  ret double %z
}

declare i32 @fetestexcept(i32)

; The moved-out checks are cold and their own arithmetic is not checked.
; OUT: attributes #{{[0-9]+}} = { cold noinline {{.*}}"wasm-fp-exception-checks" }
