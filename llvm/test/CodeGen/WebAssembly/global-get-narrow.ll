; RUN: llc < %s -mtriple=wasm32-unknown-unknown -O2 | FileCheck %s

; A load of a wasm global is never narrowed: an extending load would read
; linear memory at the global's symbol instead of the global.

@g = external addrspace(1) global i32

define i1 @test_bit(i32 %x) {
; CHECK-LABEL: test_bit:
; CHECK: global.get g
; CHECK-NOT: load8
; CHECK-NOT: load16
  %v = load i32, ptr addrspace(1) @g
  %b = and i32 %v, 32
  %c = icmp eq i32 %b, 0
  ret i1 %c
}

define i32 @low_byte() {
; CHECK-LABEL: low_byte:
; CHECK: global.get g
; CHECK-NOT: load8
  %v = load i32, ptr addrspace(1) @g
  %b = and i32 %v, 255
  ret i32 %b
}

define void @set_bit() {
; CHECK-LABEL: set_bit:
; CHECK: global.get g
; CHECK: global.set g
; CHECK-NOT: store8
  %v = load i32, ptr addrspace(1) @g
  %o = or i32 %v, 32
  store i32 %o, ptr addrspace(1) @g
  ret void
}
