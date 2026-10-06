// RUN: fir-opt --target-rewrite="target=wasm32-unknown-wasip1" %s | FileCheck %s
// RUN: fir-opt --target-rewrite="target=x86_64-unknown-linux-gnu" %s | FileCheck %s

// A CHARACTER dummy argument of a BIND(C) procedure that is not passed by
// descriptor has length one, and C receives only its address: no hidden
// length argument is appended, neither in the callee nor at the call site.

// CHECK-LABEL: func.func @bindc_callee(
// CHECK-SAME: %[[ARG0:.*]]: !fir.ref<!fir.char<1,?>>) attributes
// CHECK: %[[LEN:.*]] = fir.undefined
// CHECK: fir.emboxchar %[[ARG0]], %[[LEN]]
func.func @bindc_callee(%arg0: !fir.boxchar<1>) attributes {fir.bindc_name = "bindc_callee", fir.proc_attrs = #fir.proc_attrs<bind_c>} {
  %0:2 = fir.unboxchar %arg0 : (!fir.boxchar<1>) -> (!fir.ref<!fir.char<1,?>>, index)
  return
}

// CHECK-LABEL: func.func private @bindc_external(!fir.ref<!fir.char<1,?>>)
func.func private @bindc_external(!fir.boxchar<1>) attributes {fir.bindc_name = "bindc_external", fir.proc_attrs = #fir.proc_attrs<bind_c>}

// CHECK-LABEL: func.func @caller(
// CHECK: %[[ADDR:.*]]:2 = fir.unboxchar
// CHECK: fir.call @bindc_external(%[[ADDR]]#0) : (!fir.ref<!fir.char<1,?>>) -> ()
// CHECK: fir.call @not_bindc(%{{.*}}, %{{.*}}) : (!fir.ref<!fir.char<1,?>>, i{{32|64}}) -> ()
// CHECK: fir.address_of(@bindc_external) : (!fir.ref<!fir.char<1,?>>) -> ()
func.func @caller(%arg0: !fir.boxchar<1>) {
  fir.call @bindc_external(%arg0) proc_attrs<bind_c> : (!fir.boxchar<1>) -> ()
  fir.call @not_bindc(%arg0) : (!fir.boxchar<1>) -> ()
  %0 = fir.address_of(@bindc_external) : (!fir.boxchar<1>) -> ()
  return
}

func.func private @not_bindc(!fir.boxchar<1>)
