# --start-group / --end-group (and -( / -)) are accepted and ignored, as in
# ELF lld: archives are searched regardless of their order, so a member of an
# earlier archive can still satisfy a reference from a later one.

# RUN: rm -rf %t && split-file %s %t
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %t/main.s -o %t/main.o
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %t/a.s -o %t/a.o
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %t/b.s -o %t/b.o
# RUN: llvm-ar rcs %t/liba.a %t/a.o
# RUN: llvm-ar rcs %t/libb.a %t/b.o
# RUN: wasm-ld %t/main.o --start-group %t/libb.a %t/liba.a --end-group -o %t/out.wasm
# RUN: obj2yaml %t/out.wasm | FileCheck %s
# RUN: wasm-ld %t/main.o -\( %t/libb.a %t/liba.a -\) -o %t/out2.wasm
# RUN: obj2yaml %t/out2.wasm | FileCheck %s

# CHECK:       Name:            name
# CHECK-NEXT:  FunctionNames:
# CHECK-NEXT:    - Index:           0
# CHECK-NEXT:      Name:            _start
# CHECK-NEXT:    - Index:           1
# CHECK-NEXT:      Name:            from_b
# CHECK-NEXT:    - Index:           2
# CHECK-NEXT:      Name:            from_a

#--- main.s
.functype from_a () -> ()
.globl _start
_start:
  .functype _start () -> ()
  call from_a
  end_function

#--- a.s
.functype from_b () -> ()
.globl from_a
from_a:
  .functype from_a () -> ()
  call from_b
  end_function

#--- b.s
.globl from_b
from_b:
  .functype from_b () -> ()
  end_function
