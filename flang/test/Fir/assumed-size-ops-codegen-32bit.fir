// RUN: fir-opt --fir-to-llvm-ir="target=wasm32-unknown-wasip1" %s | FileCheck %s

// On a target with 32-bit pointers, `index` is lowered to i32, and so is the
// -1 that marks the extent of an assumed-size dimension.

module attributes {llvm.data_layout = "e-m:e-p:32:32-p10:8:8-p20:8:8-i64:64-i128:128-n32:64-S128-ni:1:10:20"} {

// CHECK-LABEL: @assumed_size_extent(
// CHECK: %[[CNEG1:.*]] = llvm.mlir.constant(-1 : {{.*}}) : i32
// CHECK: llvm.return %[[CNEG1]] : i32
func.func @assumed_size_extent() -> index {
  %e = fir.assumed_size_extent : index
  return %e : index
}

}
