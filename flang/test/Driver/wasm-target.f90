! The flang driver supports WebAssembly targets: -pthread (and the wasi-threads
! environment) add the target features that shared memory relies on to the fc1
! command, as clang -cc1 gets them, and linking adds the Fortran runtime and
! libm.

! RUN: %flang --target=wasm32-unknown-linux-musl -pthread -c %s -### 2>&1 \
! RUN:   | FileCheck %s --check-prefix=THREADS
! RUN: %flang --target=wasm32-wasip1-threads -c %s -### 2>&1 \
! RUN:   | FileCheck %s --check-prefix=THREADS
! RUN: %flang --target=wasm32-unknown-linux-musl -c %s -### 2>&1 \
! RUN:   | FileCheck %s --check-prefix=NOTHREADS

! THREADS: "-fc1"
! THREADS-SAME: "-target-feature" "+atomics"
! THREADS-SAME: "-target-feature" "+bulk-memory"
! THREADS-SAME: "-target-feature" "+mutable-globals"
! THREADS-SAME: "-target-feature" "+sign-ext"
! NOTHREADS-NOT: "-target-feature" "+atomics"

! RUN: %flang --target=wasm32-unknown-linux-musl %s -### -o /dev/null 2>&1 \
! RUN:   | FileCheck %s --check-prefix=LINK
! LINK: wasm-ld
! LINK-SAME: "-lflang_rt.runtime"
! LINK-SAME: "-lm"

program t
end program
