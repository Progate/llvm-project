// RUN: %clang_cc1 -triple wasm32-unknown-linux-musl -o - -emit-llvm %s | FileCheck %s
// RUN: %clang_cc1 -triple wasm32 -o - -emit-llvm %s | FileCheck -check-prefix=NOLINUX %s

// On Linux, the no-argument main also gets the __main_argc_argv entry that
// crt1 references strongly, as ELF's crt1 references main.

int main(void) {
  return 0;
}

// CHECK-LABEL: define i32 @main()
// CHECK-LABEL: define hidden i32 @__main_argc_argv(i32 %0, ptr %1)
// CHECK-NEXT: entry:
// CHECK-NEXT: %2 = call i32 @main()
// CHECK-NEXT: ret i32 %2

// NOLINUX-NOT: __main_argc_argv
