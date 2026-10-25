// REQUIRES: lld, webassembly-registered-target, shell

// On wasm32 Linux, fork unwinds and rewinds the stack with Binaryen's
// asyncify. After linking an executable that calls fork, the driver runs
// `wasm-opt --asyncify` on it; other programs are left as wasm-ld wrote them.

// RUN: rm -rf %t && mkdir -p %t/bin %t/empty %t/sysroot/usr/lib/wasm32-linux-musl
// RUN: printf '#!/bin/sh\necho "$@" > %t/wasm-opt.args\n' > %t/bin/wasm-opt
// RUN: chmod +x %t/bin/wasm-opt
// RUN: printf 'fork\nqsort\n' > %t/sysroot/usr/lib/wasm32-linux-musl/libc.asyncify-onlylist

// A PIE executable that imports fork from libc.so. The asyncify imports are
// its imports of the functions libc.so lists as rewindable (not puts), and the
// asyncify state is exported for libc.so to import.
// RUN: %clang --target=wasm32-unknown-linux-musl --sysroot=%t/sysroot -B%t/bin \
// RUN:   -nostdlib -Wl,--unresolved-symbols=import-dynamic -O2 -DFORK %s \
// RUN:   -o %t/fork.wasm
// RUN: FileCheck --check-prefix=PIE %s < %t/wasm-opt.args
// PIE: {{.*}}fork.wasm --asyncify --pass-arg=asyncify-imports@env.{{(fork,env.qsort|qsort,env.fork)}} --pass-arg=asyncify-export-globals -O2 -o {{.*}}fork.wasm

// Without libc's list, every call into libc.so may unwind.
// RUN: %clang --target=wasm32-unknown-linux-musl --sysroot=%t/empty -B%t/bin \
// RUN:   -nostdlib -Wl,--unresolved-symbols=import-dynamic -DFORK %s \
// RUN:   -o %t/nolist.wasm
// RUN: FileCheck --check-prefix=NOLIST %s < %t/wasm-opt.args
// NOLIST: {{.*}}nolist.wasm --asyncify --pass-arg=asyncify-imports@env.* --pass-arg=asyncify-export-globals -o {{.*}}nolist.wasm

// A program that does not call fork is left alone.
// RUN: rm -f %t/wasm-opt.args
// RUN: %clang --target=wasm32-unknown-linux-musl --sysroot=%t/sysroot -B%t/bin \
// RUN:   -nostdlib -Wl,--unresolved-symbols=import-dynamic -O2 %s -o %t/plain.wasm
// RUN: not test -e %t/wasm-opt.args

// A static executable calls fork if it has _Fork (from -lbrowseros-fork); the
// unwinding import is the system call.
// RUN: %clang --target=wasm32-unknown-linux-musl --sysroot=%t/sysroot -B%t/bin \
// RUN:   -nostdlib -static -O1 -DSTATIC %s -o %t/static.wasm
// RUN: FileCheck --check-prefix=STATIC %s < %t/wasm-opt.args
// STATIC: {{.*}}static.wasm --asyncify --pass-arg=asyncify-imports@browser_os_linux.syscall -O1 -o {{.*}}static.wasm

// Without wasm-opt the link still succeeds, and the driver says why fork will
// fail.
// RUN: env PATH=%t/empty %clang --target=wasm32-unknown-linux-musl \
// RUN:   --sysroot=%t/sysroot -nostdlib -Wl,--unresolved-symbols=import-dynamic \
// RUN:   -DFORK %s -o %t/nowasmopt.wasm 2>&1 | FileCheck --check-prefix=MISSING %s
// MISSING: warning: '{{.*}}nowasmopt.wasm' calls fork, but 'wasm-opt' was not found to apply asyncify to it; fork will fail with ENOSYS [-Wwasm-fork-asyncify]

// Whether wasm-opt runs depends on the linked output, so -### shows only the
// link. -O does not run wasm-opt as the optimizer on this target.
// RUN: %clang -### --target=wasm32-unknown-linux-musl --sysroot=%t/sysroot \
// RUN:   -B%t/bin -O2 %s 2>&1 | FileCheck --check-prefix=DRYRUN %s
// DRYRUN: wasm-ld
// DRYRUN-NOT: wasm-opt

#ifdef STATIC
volatile int forked;
__attribute__((noinline)) int _Fork(void) { return forked = 1; }
void _start(void) { _Fork(); }
#else
int fork(void);
void qsort(void *, unsigned long, unsigned long,
           int (*)(const void *, const void *));
int puts(const char *);
void _start(void) {
#ifdef FORK
  fork();
#endif
  qsort(0, 0, 0, 0);
  puts("");
}
#endif
