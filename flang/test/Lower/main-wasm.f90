! The C ABI of WebAssembly calls a main that takes arguments by the name
! __main_argc_argv, with exactly (argc, argv).

! RUN: %flang_fc1 -triple wasm32-unknown-wasip1 -emit-hlfir %s -o - | FileCheck %s --check-prefix=WASM
! RUN: %flang_fc1 -triple wasm32-unknown-linux-musl -emit-hlfir %s -o - | FileCheck %s --check-prefix=WASM
! RUN: %flang_fc1 -triple wasm64-unknown-unknown -emit-hlfir %s -o - | FileCheck %s --check-prefix=WASM

! WASM-LABEL: func.func @__main_argc_argv(
! WASM-SAME: %[[ARGC:.*]]: i32, %[[ARGV:.*]]: !llvm.ptr) -> i32
! WASM: %[[ENVP:.*]] = fir.zero_bits !llvm.ptr
! WASM: fir.call @_FortranAProgramStart(%[[ARGC]], %[[ARGV]], %[[ENVP]], %{{.*}})
! WASM-NOT: func.func @main(

program p
end program
