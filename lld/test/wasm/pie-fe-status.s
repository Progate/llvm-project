# The FP exception flags global `__fe_status` is provided by the dynamic loader,
# so PIC links (-pie and -shared) import it from the environment, while a
# static link still requires a definition (libc.a has one).

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-linux-musl -o %t.o %s
# RUN: wasm-ld --experimental-pic -pie --no-entry --export=fp -o %t.pie.wasm %t.o
# RUN: obj2yaml %t.pie.wasm | FileCheck %s
# RUN: wasm-ld --experimental-pic -shared -o %t.so %t.o
# RUN: obj2yaml %t.so | FileCheck %s
# RUN: not wasm-ld --no-entry --export=fp -o %t.static.wasm %t.o 2>&1 | FileCheck %s --check-prefix=STATIC

.globaltype __fe_status, i32

.globl fp
fp:
  .functype fp () -> (i32)
  global.get __fe_status
  end_function

# CHECK:        - Type:            IMPORT
# CHECK:              Field:           __fe_status
# CHECK-NEXT:       Kind:            GLOBAL
# CHECK-NEXT:       GlobalType:      I32
# CHECK-NEXT:       GlobalMutable:   true

# STATIC: error: {{.*}}: undefined symbol: __fe_status
