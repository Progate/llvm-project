# The exception tag `__c_longjmp` that setjmp/longjmp lowered onto Wasm
# exception handling throws is provided by the dynamic loader, so PIC links
# (-pie and -shared) import it from the environment, while a static link still
# requires a definition (libc.a has one).

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-linux-musl -mattr=+exception-handling -o %t.o %s
# RUN: wasm-ld --experimental-pic -pie --no-entry --export=jump -o %t.pie.wasm %t.o
# RUN: obj2yaml %t.pie.wasm | FileCheck %s
# RUN: wasm-ld --experimental-pic -shared -o %t.so %t.o
# RUN: obj2yaml %t.so | FileCheck %s
# RUN: not wasm-ld --no-entry --export=jump -o %t.static.wasm %t.o 2>&1 | FileCheck %s --check-prefix=STATIC

.tagtype __c_longjmp i32

.globl jump
jump:
  .functype jump (i32) -> ()
  local.get 0
  throw __c_longjmp
  end_function

# CHECK:        - Type:            IMPORT
# CHECK:              Field:           __c_longjmp
# CHECK-NEXT:       Kind:            TAG

# STATIC: error: {{.*}}: undefined symbol: __c_longjmp
