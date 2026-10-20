# A shared library after --as-needed gets a NEEDED entry only if a regular
# object references a symbol that it defines, as in ELF. --no-as-needed (the
# default) restores the old behavior for the libraries that follow it.

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t.o %s
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %p/Inputs/ret32.s -o %t.ret32.o
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %p/Inputs/ret64.s -o %t.ret64.o
# RUN: wasm-ld -shared -soname=libret32.so -o %t.ret32.so %t.ret32.o
# RUN: wasm-ld -shared -soname=libret64.so -o %t.ret64.so %t.ret64.o

# RUN: wasm-ld -shared -o %t.default.so %t.o %t.ret32.so %t.ret64.so
# RUN: obj2yaml %t.default.so | FileCheck %s --check-prefix=BOTH

# RUN: wasm-ld -shared -o %t.as-needed.so %t.o --as-needed %t.ret32.so %t.ret64.so
# RUN: obj2yaml %t.as-needed.so | FileCheck %s --check-prefix=USED

# RUN: wasm-ld -shared -o %t.positional.so %t.o --as-needed %t.ret32.so --no-as-needed %t.ret64.so
# RUN: obj2yaml %t.positional.so | FileCheck %s --check-prefix=BOTH

# RUN: wasm-ld -shared --unresolved-symbols=import-dynamic -o %t.unused.so %t.o --as-needed %t.ret64.so
# RUN: obj2yaml %t.unused.so | FileCheck %s --check-prefix=NONE

.functype ret32 (f32) -> (i32)

.globl foo
foo:
  .functype foo (f32) -> (i32)
  local.get 0
  call ret32
  end_function

# BOTH:          Needed:
# BOTH-NEXT:       - libret32.so
# BOTH-NEXT:       - libret64.so

# USED:          Needed:
# USED-NEXT:       - libret32.so
# USED-NOT:        - libret64.so

# NONE:          Needed: []
