# -Bsymbolic-non-weak binds references to non-weak definitions locally and
# leaves weak definitions preemptible, as in ELF: the addresses of weak_func
# and weak_data still come from the GOT, while foo and bar need no GOT entry.
# -Bsymbolic binds both kinds locally.

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown %s -o %t.o
# RUN: wasm-ld -shared -Bsymbolic-non-weak %t.o -o %t.non-weak.so
# RUN: obj2yaml %t.non-weak.so | FileCheck -check-prefix=NON-WEAK %s
# RUN: wasm-ld -shared -Bsymbolic %t.o -o %t.all.so
# RUN: obj2yaml %t.all.so | FileCheck -check-prefix=ALL %s

# NON-WEAK:       - Type:            IMPORT
# NON-WEAK-NOT:       Field:           foo
# NON-WEAK-NOT:       Field:           bar
# NON-WEAK:           - Module:          GOT.func
# NON-WEAK-NEXT:        Field:           weak_func
# NON-WEAK:           - Module:          GOT.mem
# NON-WEAK-NEXT:        Field:           weak_data
# NON-WEAK:       - Type:            FUNCTION

# ALL:       - Type:            IMPORT
# ALL-NOT:       Module:          GOT
# ALL:       - Type:            FUNCTION

.globl foo
foo:
  .functype foo () -> ()
  end_function

.globl weak_func
.weak weak_func
weak_func:
  .functype weak_func () -> ()
  end_function

.globl get_addresses
get_addresses:
  .functype get_addresses () -> ()
  global.get foo@GOT
  drop
  global.get weak_func@GOT
  drop
  global.get bar@GOT
  drop
  global.get weak_data@GOT
  drop
  end_function

.globl bar
.section .data.bar,"",@
bar:
  .int32 1
  .size bar, 4

.globl weak_data
.weak weak_data
.section .data.weak_data,"",@
weak_data:
  .int32 2
  .size weak_data, 4
