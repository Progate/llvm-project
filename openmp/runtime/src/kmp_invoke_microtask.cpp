#include "kmp.h"

#if !(KMP_ARCH_X86 || KMP_ARCH_X86_64 || KMP_MIC || KMP_ARCH_AARCH64 ||        \
      KMP_ARCH_PPC64 || KMP_ARCH_RISCV64 || KMP_ARCH_LOONGARCH64 ||            \
      KMP_ARCH_ARM || KMP_ARCH_VE || KMP_ARCH_S390X || KMP_ARCH_PPC_XCOFF ||   \
      KMP_ARCH_AARCH64_32)

// Because WebAssembly will use `call_indirect` to invoke the microtask and
// WebAssembly indirect calls check that the called signature is a precise
// match, we need to cast each microtask function pointer back from `void *` to
// its original type.
typedef void (*microtask_t0)(int *, int *);
typedef void (*microtask_t1)(int *, int *, void *);
typedef void (*microtask_t2)(int *, int *, void *, void *);
typedef void (*microtask_t3)(int *, int *, void *, void *, void *);
typedef void (*microtask_t4)(int *, int *, void *, void *, void *, void *);
typedef void (*microtask_t5)(int *, int *, void *, void *, void *, void *,
                             void *);
typedef void (*microtask_t6)(int *, int *, void *, void *, void *, void *,
                             void *, void *);
typedef void (*microtask_t7)(int *, int *, void *, void *, void *, void *,
                             void *, void *, void *);
typedef void (*microtask_t8)(int *, int *, void *, void *, void *, void *,
                             void *, void *, void *, void *);
typedef void (*microtask_t9)(int *, int *, void *, void *, void *, void *,
                             void *, void *, void *, void *, void *);
typedef void (*microtask_t10)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *);
typedef void (*microtask_t11)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *,
                              void *);
typedef void (*microtask_t12)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *,
                              void *, void *);
typedef void (*microtask_t13)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *,
                              void *, void *, void *);
typedef void (*microtask_t14)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *,
                              void *, void *, void *, void *);
typedef void (*microtask_t15)(int *, int *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *, void *,
                              void *, void *, void *, void *, void *);

#if KMP_ARCH_WASM
// Clang passes every variable a parallel region captures as a separate
// argument of __kmpc_fork_call, so the outlined function can take many more
// arguments than the switch below covers (a loop over 18 arrays already needs
// 19). Every argument is a pointer or a pointer-sized value, so the type of a
// microtask is fixed by its argument count: build one caller per count up to
// KMP_WASM_MICROTASK_MAX_ARGS and index them by argc.
#define KMP_WASM_MICROTASK_MAX_ARGS 255

namespace {
typedef void (*kmp_microtask_caller_t)(microtask_t, int *, int *, void **);

template <size_t> using kmp_microtask_arg_t = void *;
template <size_t... I> struct kmp_index_list {};
template <size_t N, size_t... I>
struct kmp_make_index_list : kmp_make_index_list<N - 1, N - 1, I...> {};
template <size_t... I> struct kmp_make_index_list<0, I...> {
  typedef kmp_index_list<I...> type;
};

template <size_t... I>
void __kmp_call_microtask(microtask_t pkfn, int *gtid, int *tid,
                          void **p_argv) {
  ((void (*)(int *, int *, kmp_microtask_arg_t<I>...))pkfn)(gtid, tid,
                                                            p_argv[I]...);
}

template <size_t... I>
constexpr kmp_microtask_caller_t __kmp_microtask_caller(kmp_index_list<I...>) {
  return &__kmp_call_microtask<I...>;
}

template <typename> struct kmp_microtask_callers;
template <size_t... N> struct kmp_microtask_callers<kmp_index_list<N...>> {
  static constexpr kmp_microtask_caller_t table[] = {
      __kmp_microtask_caller(typename kmp_make_index_list<N>::type())...};
};
} // namespace

int __kmp_invoke_microtask(microtask_t pkfn, int gtid, int tid, int argc,
                           void *p_argv[]
#if OMPT_SUPPORT
                           ,
                           void **exit_frame_ptr
#endif
) {
#if OMPT_SUPPORT
  *exit_frame_ptr = OMPT_GET_FRAME_ADDRESS(0);
#endif
  typedef kmp_microtask_callers<
      kmp_make_index_list<KMP_WASM_MICROTASK_MAX_ARGS + 1>::type>
      callers;

  if (argc < 0 || argc > KMP_WASM_MICROTASK_MAX_ARGS) {
    fprintf(stderr, "Too many args to microtask: %d!\n", argc);
    fflush(stderr);
    exit(-1);
  }
  callers::table[argc](pkfn, &gtid, &tid, p_argv);
  return 1;
}
#else
// we really only need the case with 1 argument, because CLANG always build
// a struct of pointers to shared variables referenced in the outlined function
int __kmp_invoke_microtask(microtask_t pkfn, int gtid, int tid, int argc,
                           void *p_argv[]
#if OMPT_SUPPORT
                           ,
                           void **exit_frame_ptr
#endif
) {
#if OMPT_SUPPORT
  *exit_frame_ptr = OMPT_GET_FRAME_ADDRESS(0);
#endif

  switch (argc) {
  default:
    fprintf(stderr, "Too many args to microtask: %d!\n", argc);
    fflush(stderr);
    exit(-1);
  case 0:
    (*(microtask_t0)pkfn)(&gtid, &tid);
    break;
  case 1:
    (*(microtask_t1)pkfn)(&gtid, &tid, p_argv[0]);
    break;
  case 2:
    (*(microtask_t2)pkfn)(&gtid, &tid, p_argv[0], p_argv[1]);
    break;
  case 3:
    (*(microtask_t3)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2]);
    break;
  case 4:
    (*(microtask_t4)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3]);
    break;
  case 5:
    (*(microtask_t5)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3], p_argv[4]);
    break;
  case 6:
    (*(microtask_t6)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3], p_argv[4], p_argv[5]);
    break;
  case 7:
    (*(microtask_t7)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3], p_argv[4], p_argv[5], p_argv[6]);
    break;
  case 8:
    (*(microtask_t8)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                          p_argv[7]);
    break;
  case 9:
    (*(microtask_t9)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                          p_argv[3], p_argv[4], p_argv[5], p_argv[6], p_argv[7],
                          p_argv[8]);
    break;
  case 10:
    (*(microtask_t10)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9]);
    break;
  case 11:
    (*(microtask_t11)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9], p_argv[10]);
    break;
  case 12:
    (*(microtask_t12)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9], p_argv[10],
                           p_argv[11]);
    break;
  case 13:
    (*(microtask_t13)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9], p_argv[10],
                           p_argv[11], p_argv[12]);
    break;
  case 14:
    (*(microtask_t14)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9], p_argv[10],
                           p_argv[11], p_argv[12], p_argv[13]);
    break;
  case 15:
    (*(microtask_t15)pkfn)(&gtid, &tid, p_argv[0], p_argv[1], p_argv[2],
                           p_argv[3], p_argv[4], p_argv[5], p_argv[6],
                           p_argv[7], p_argv[8], p_argv[9], p_argv[10],
                           p_argv[11], p_argv[12], p_argv[13], p_argv[14]);
    break;
  }

  return 1;
}
#endif // KMP_ARCH_WASM

#endif
