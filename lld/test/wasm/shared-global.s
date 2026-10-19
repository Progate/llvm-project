# A mutable global exported by a shared object is a real WebAssembly global
# shared between modules.  The main module must import it directly via
# `env.<name>` rather than reading its address through a GOT.mem entry.
# Immutable global exports still represent data addresses and are imported
# via GOT.mem.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-emscripten -o %t/side.o %t/side.s
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-emscripten -o %t/main.o %t/main.s
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-emscripten -o %t/mismatch.o %t/mismatch.s
# RUN: wasm-ld -shared -o %t/side.so %t/side.o
# RUN: wasm-ld -pie -o %t/main.wasm %t/main.o %t/side.so
# RUN: obj2yaml %t/main.wasm | FileCheck %s
# RUN: llvm-objdump --disassemble-symbols=_start --no-show-raw-insn --no-leading-addr %t/main.wasm | FileCheck %s --check-prefix=DIS

# A global whose type does not match the shared object's export is an error.
# RUN: not wasm-ld -pie -o %t/mismatch.wasm %t/mismatch.o %t/side.so 2>&1 | FileCheck %s --check-prefix=MISMATCH

# MISMATCH:      error: Global type mismatch: shared_global
# MISMATCH-NEXT: >>> defined as var f64 in {{.*}}mismatch.o
# MISMATCH-NEXT: >>> defined as var i32 in {{.*}}side.so

#--- side.s
  .globl shared_global
  .globaltype shared_global, i32
shared_global:

  .section .data.my_data,"",@
  .globl my_data
my_data:
  .int32 42
  .size my_data, 4

#--- main.s
  .globaltype shared_global, i32

  .globl _start
_start:
  .functype _start () -> (i32)
  global.get shared_global
  global.get my_data@GOT
  i32.add
  end_function

#--- mismatch.s
  .globaltype shared_global, f64

  .globl _start
_start:
  .functype _start () -> ()
  global.get shared_global
  drop
  end_function

# CHECK:      - Type:            IMPORT
# CHECK:        Imports:
# CHECK:          - Module:          env
# CHECK:            Field:           __memory_base
# CHECK:            Kind:            GLOBAL
# CHECK:            GlobalType:      I32
# CHECK:            GlobalMutable:   false
# CHECK:          - Module:          env
# CHECK:            Field:           __table_base
# CHECK:            Kind:            GLOBAL
# CHECK:            GlobalType:      I32
# CHECK:            GlobalMutable:   false
# CHECK:          - Module:          env
# CHECK-NEXT:       Field:           shared_global
# CHECK-NEXT:       Kind:            GLOBAL
# CHECK-NEXT:       GlobalType:      I32
# CHECK-NEXT:       GlobalMutable:   true
# CHECK-NEXT:     - Module:          GOT.mem
# CHECK-NEXT:       Field:           my_data
# CHECK-NEXT:       Kind:            GLOBAL
# CHECK-NEXT:       GlobalType:      I32
# CHECK-NEXT:       GlobalMutable:   true

# The first global.get reads shared_global directly via its import index (2),
# the second reads my_data through the GOT.mem entry (3).
# DIS:      <_start>:
# DIS:       global.get 2
# DIS-NEXT:  global.get 3
# DIS-NEXT:  i32.add
