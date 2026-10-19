# Thread-local variables defined in a shared library can be referenced from a
# PIE main through the GOT, where the dynamic linker resolves the TLS offset.
# This is the general-dynamic shape used by the Linux-style wasm32 target.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t/lib.o %t/lib.s
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t/main.o %t/main.s
# RUN: wasm-ld -shared --experimental-pic --shared-memory -o %t/lib.so %t/lib.o
# RUN: wasm-ld -pie --experimental-pic --shared-memory --import-memory --no-entry --unresolved-symbols=import-dynamic --export=get_tls1 -o %t/main.wasm %t/main.o %t/lib.so
# RUN: obj2yaml %t/main.wasm | FileCheck %s --check-prefix=MAIN

# The PIE main must import the TLS variable through the GOT so the loader can
# fill in the per-module TLS offset.

# MAIN:        Needed:
# MAIN-NEXT:     - lib.so
# MAIN:        - Module:          GOT.mem
# MAIN-NEXT:     Field:           tls1
# MAIN-NEXT:     Kind:            GLOBAL
# MAIN-NEXT:     GlobalType:      I32
# MAIN-NEXT:     GlobalMutable:   true

#--- lib.s
.section  .tdata.tls1,"",@
.globl  tls1
.p2align  2
tls1:
  .int32  1
  .size tls1, 4

.section  .custom_section.target_features,"",@
  .int8 3
  .int8 43
  .int8 7
  .ascii  "atomics"
  .int8 43
  .int8 11
  .ascii  "bulk-memory"
  .int8 43
  .int8 15
  .ascii "mutable-globals"

#--- main.s
.globl  get_tls1
get_tls1:
  .functype get_tls1 () -> (i32)
  global.get  tls1@GOT@TLS
  i32.load 0
  end_function

.section  .custom_section.target_features,"",@
  .int8 3
  .int8 43
  .int8 7
  .ascii  "atomics"
  .int8 43
  .int8 11
  .ascii  "bulk-memory"
  .int8 43
  .int8 15
  .ascii "mutable-globals"
