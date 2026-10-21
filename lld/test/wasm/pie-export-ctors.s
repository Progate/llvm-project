# A dynamic loader runs a module's constructors by calling the exported
# __wasm_call_ctors after __wasm_apply_data_relocs, as it does for shared
# libraries, so -pie exports it too. A non-PIC executable keeps it hidden: the
# start function there is _start, which calls it.

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t.o %s
# RUN: wasm-ld --experimental-pic -pie -o %t.pie.wasm %t.o
# RUN: obj2yaml %t.pie.wasm | FileCheck %s
# RUN: wasm-ld -o %t.static.wasm %t.o
# RUN: obj2yaml %t.static.wasm | FileCheck %s --check-prefix=STATIC

.globl _start
_start:
  .functype _start () -> ()
  end_function

.globl ctor
ctor:
  .functype ctor () -> ()
  end_function

.section .init_array,"",@
.p2align 2
.int32 ctor

# CHECK:        - Type:            EXPORT
# CHECK:            - Name:            __wasm_call_ctors
# CHECK-NEXT:         Kind:            FUNCTION

# STATIC:       - Type:            EXPORT
# STATIC-NOT:       Name:            __wasm_call_ctors
# STATIC:       - Type:            CODE
