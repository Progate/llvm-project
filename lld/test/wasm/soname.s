# A shared library records its -soname in dylink.0, and modules linked against
# it name it in NEEDED by that soname rather than by the file it was read from,
# as DT_SONAME and DT_NEEDED do in ELF.

# RUN: rm -rf %t && mkdir %t
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t/main.o %s
# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %p/Inputs/ret32.s -o %t/ret32.o

# RUN: wasm-ld -shared --experimental-pic -soname libret32.so.1 -o %t/libret32.so.1.2.3 %t/ret32.o
# RUN: obj2yaml %t/libret32.so.1.2.3 | FileCheck %s --check-prefix=LIB

# LIB:      Name:            dylink.0
# LIB:        SoName:          libret32.so.1
# LIB-NEXT:   - Type:            TYPE

# RUN: wasm-ld -shared --experimental-pic -o %t/main.so %t/main.o %t/libret32.so.1.2.3
# RUN: obj2yaml %t/main.so | FileCheck %s --check-prefix=NEEDED

# NEEDED:      Name:            dylink.0
# NEEDED:        Needed:
# NEEDED-NEXT:     - libret32.so.1
# NEEDED-NOT:    SoName:
# NEEDED:      - Type:            TYPE

# The same library reached through two file names is one library at runtime,
# so it is needed only once.

# RUN: cp %t/libret32.so.1.2.3 %t/libret32.so
# RUN: wasm-ld -shared --experimental-pic -o %t/twice.so %t/main.o %t/libret32.so %t/libret32.so.1.2.3
# RUN: obj2yaml %t/twice.so | FileCheck %s --check-prefix=NEEDED

# Without -soname a library has no soname, and is needed by its file name.

# RUN: wasm-ld -shared --experimental-pic -o %t/libplain.so %t/ret32.o
# RUN: obj2yaml %t/libplain.so | FileCheck %s --check-prefix=PLAIN
# RUN: wasm-ld -shared --experimental-pic -o %t/main-plain.so %t/main.o %t/libplain.so
# RUN: obj2yaml %t/main-plain.so | FileCheck %s --check-prefix=PLAIN-NEEDED

# PLAIN:      Name:            dylink.0
# PLAIN-NOT:    SoName:
# PLAIN:      - Type:            TYPE

# PLAIN-NEEDED:      Needed:
# PLAIN-NEEDED-NEXT:   - libplain.so

.globl foo

.functype ret32 (f32) -> (i32)

foo:
  .functype foo (f32) -> (i32)
  local.get 0
  call ret32
  end_function
