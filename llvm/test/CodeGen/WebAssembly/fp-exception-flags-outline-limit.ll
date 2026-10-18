; RUN: llc < %s -mtriple=wasm32-wasip1 -stop-after=wasm-fp-exception-flags | FileCheck %s --check-prefix=OUT
; RUN: llc < %s -mtriple=wasm32-wasip1 -wasm-fp-exception-max-outlined-params=2 -stop-after=wasm-fp-exception-flags | FileCheck %s --check-prefix=LIMIT
; RUN: llc < %s -mtriple=wasm32-wasip1 -wasm-fp-exception-max-outlined-params=2 -filetype=obj -o /dev/null

; The full checks of a run are moved out to a function that takes the values
; live into them as parameters (OUT). The WebAssembly JS API limits a function
; to 1000 parameters, so when moving them out would take more than
; -wasm-fp-exception-max-outlined-params, they stay in place (LIMIT). The run
; below has three values live into its full checks: %r, %a and %b.

define double @add(double %a, double %b) {
; OUT-LABEL: @add(
; OUT: call void @add.fe_check(double %r, double %a, double %b)
; OUT: define internal void @add.fe_check(double %r, double %a, double %b)
; LIMIT-LABEL: @add(
; LIMIT: %r = fadd double %a, %b
; LIMIT-NOT: call void @add.fe_check
; LIMIT: load i32, ptr addrspace(1) @__fe_status
; LIMIT: ret double %r
; LIMIT-NOT: define internal void @add.fe_check
  %r = fadd double %a, %b
  ret double %r
}
