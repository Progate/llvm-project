# As in ELF, an executable exports the symbols that the shared libraries it
# links against refer to, so that the dynamic linker can bind those
# references to the executable's definitions (an executable's replacement
# operator new, for example). Symbols that no library refers to stay private.

# RUN: split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %t/lib.s -o %t/lib.o
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %t/main.s -o %t/main.o
# RUN: wasm-ld -shared --experimental-pic --unresolved-symbols=import-dynamic -o %t/lib.so %t/lib.o
# RUN: wasm-ld -pie --experimental-pic -o %t/main.wasm %t/main.o %t/lib.so
# RUN: obj2yaml %t/main.wasm | FileCheck %s

# CHECK:       - Type:            EXPORT
# CHECK-NEXT:    Exports:
# CHECK-NEXT:      - Name:            memory
# CHECK-NEXT:        Kind:            MEMORY
# CHECK-NEXT:        Index:           0
# CHECK-NEXT:      - Name:            __wasm_call_ctors
# CHECK-NEXT:        Kind:            FUNCTION
# CHECK-NEXT:        Index:           1
# CHECK-NEXT:      - Name:            _start
# CHECK-NEXT:        Kind:            FUNCTION
# CHECK-NEXT:        Index:           2
# CHECK-NEXT:      - Name:            hook
# CHECK-NEXT:        Kind:            FUNCTION
# CHECK-NEXT:        Index:           4
# CHECK-NEXT:      - Name:            counter
# CHECK-NEXT:        Kind:            GLOBAL
# CHECK-NEXT:        Index:           2
# CHECK-NEXT:  - Type:            CODE

#--- lib.s
.functype hook () -> ()

.globl call_hook
call_hook:
  .functype call_hook () -> (i32)
  call hook
  global.get counter@GOT
  end_function

#--- main.s
.functype call_hook () -> (i32)
.functype unreferenced () -> ()

.globl _start
_start:
  .functype _start () -> ()
  call call_hook
  drop
  call unreferenced
  end_function

.globl hook
hook:
  .functype hook () -> ()
  end_function

.globl unreferenced
unreferenced:
  .functype unreferenced () -> ()
  end_function

.section .data.counter,"",@
.globl counter
counter:
  .int32 0
  .size counter, 4
