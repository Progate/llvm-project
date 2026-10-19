; RUN: llc < %s -mtriple=wasm32-wasip1 -stop-after=wasm-fp-exception-flags | FileCheck %s

; The finiteness of a run's results is checked with one sum per type. The sums
; come in the order their types are first met in the run, so the same input
; compiles to the same code whatever the addresses of the types.

; CHECK-LABEL: define double @float_first(
; CHECK:      fsub float %x, %x
; CHECK-NEXT: fcmp oeq float
; CHECK:      fsub double %r, %r
; CHECK-NEXT: fcmp oeq double
define double @float_first(float %a, float %b, double %c, double %d) {
  %x = fmul float %a, %b
  %y = fmul double %c, %d
  %e = fpext float %x to double
  %r = fadd double %e, %y
  ret double %r
}

; CHECK-LABEL: define float @double_first(
; CHECK:      fsub double %x, %x
; CHECK-NEXT: fcmp oeq double
; CHECK:      fsub float %r, %r
; CHECK-NEXT: fcmp oeq float
define float @double_first(double %a, double %b, float %c, float %d) {
  %x = fmul double %a, %b
  %y = fmul float %c, %d
  %e = fptrunc double %x to float
  %r = fadd float %e, %y
  ret float %r
}
