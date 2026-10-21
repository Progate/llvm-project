# A self-relative pointer (R_WASM_MEMORY_ADDR_LOCREL_I32, `target - .`) to data
# is already correct at link time in PIC links: the target and the location both
# move by __memory_base, so it is resolved to the plain difference and needs no
# runtime relocation. Without this, -pie rejected it as an invalid runtime
# relocation in the data section.

# RUN: llvm-mc -filetype=obj -triple=wasm32-unknown-unknown -o %t.o %s
# RUN: wasm-ld --experimental-pic -pie --export=rel_data --export=rel_func -o %t.wasm %t.o
# RUN: obj2yaml %t.wasm | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn %t.wasm | FileCheck %s --check-prefix=RELOCS

.globl _start
_start:
  .functype _start () -> ()
  end_function

.globl func
func:
  .functype func () -> ()
  end_function

.section .data.target,"",@
.globl target
target:
  .int32 42
  .size target, 4

.section .data.rel,"",@
.globl rel_data
rel_data:
  .int32 target - rel_data
  .size rel_data, 4
.globl rel_func
rel_func:
  .int32 func
  .size rel_func, 4

# `target` is at 0 and `rel_data` at 4, so the stored offset is -4. The table
# index of `func` is only known at load time and stays 0 in the image.
# CHECK:        - Type:            DATA
# CHECK:              Content:         2A000000FCFFFFFF00000000

# Only `rel_func` (offset 8) is relocated at load time.
# RELOCS:      <__wasm_apply_data_relocs>:
# RELOCS-EMPTY:
# RELOCS-NEXT: {{.*}}i32.const 8
# RELOCS-NEXT: {{.*}}global.get 0
# RELOCS-NEXT: {{.*}}i32.add
# RELOCS-NEXT: {{.*}}global.get 1
# RELOCS-NEXT: {{.*}}i32.const 0
# RELOCS-NEXT: {{.*}}i32.add
# RELOCS-NEXT: {{.*}}i32.store 0
# RELOCS-NEXT: {{.*}}end
