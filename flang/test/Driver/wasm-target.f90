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

! On wasm32 Linux the Fortran runtime lives in the multiarch dir next to libc
! (the shared library for -lflang_rt.runtime, the archive for -static), and
! -static-libflangrt names the archive there, picking the threads variant with
! -pthread.

! RUN: rm -rf %t && mkdir -p %t/usr/lib/wasm32-linux-musl %t/usr/lib/wasm32-linux-musl-threads
! RUN: touch %t/usr/lib/wasm32-linux-musl/libflang_rt.runtime.a %t/usr/lib/wasm32-linux-musl-threads/libflang_rt.runtime.a
! RUN: %flang --target=wasm32-unknown-linux-musl --sysroot=%t -static-libflangrt %s -### -o /dev/null 2>&1 \
! RUN:   | FileCheck %s --check-prefix=STATIC-RT -DSYSROOT=%t
! RUN: %flang --target=wasm32-unknown-linux-musl --sysroot=%t -pthread -static-libflangrt %s -### -o /dev/null 2>&1 \
! RUN:   | FileCheck %s --check-prefix=STATIC-RT-THREADS -DSYSROOT=%t
! STATIC-RT: wasm-ld
! STATIC-RT-SAME: "[[SYSROOT]]/usr/lib/wasm32-linux-musl/libflang_rt.runtime.a"
! STATIC-RT-NOT: "-lflang_rt.runtime"
! STATIC-RT-THREADS: wasm-ld
! STATIC-RT-THREADS-SAME: "[[SYSROOT]]/usr/lib/wasm32-linux-musl-threads/libflang_rt.runtime.a"

program t
end program
