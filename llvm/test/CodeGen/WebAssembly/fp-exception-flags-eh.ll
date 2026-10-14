; RUN: llc < %s -mtriple=wasm32-wasip1 -wasm-enable-eh -exception-model=wasm -mattr=+exception-handling -stop-after=wasm-fp-exception-flags | FileCheck %s
; RUN: llc < %s -mtriple=wasm32-wasip1 -wasm-enable-eh -exception-model=wasm -mattr=+exception-handling -filetype=obj -o /dev/null

; A helper call in a catch funclet carries the funclet bundle, as any call
; there must.
define void @in_catch(double %a, double %b, ptr %out) personality ptr @__gxx_wasm_personality_v0 {
; CHECK-LABEL: @in_catch(
; CHECK: [[PAD:%.*]] = catchpad within
; CHECK: %m = fmul double %a, %b
; CHECK: call void @__fe_mul_f64(i64 {{%.*}}, i64 {{%.*}}) [ "funclet"(token [[PAD]]) ]
entry:
  invoke void @may_throw() to label %done unwind label %dispatch
dispatch:
  %cs = catchswitch within none [label %catch] unwind to caller
catch:
  %pad = catchpad within %cs [ptr null]
  %m = fmul double %a, %b
  store double %m, ptr %out
  catchret from %pad to label %done
done:
  ret void
}

declare void @may_throw()
declare i32 @__gxx_wasm_personality_v0(...)
