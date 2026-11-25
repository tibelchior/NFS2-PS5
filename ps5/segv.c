#define _GNU_SOURCE
#include <signal.h>
#include <ucontext.h>
#include <execinfo.h>
#include <stdio.h>
#include <unistd.h>
static void h(int s, siginfo_t *si, void *u) {
    ucontext_t *c = u; void *bt[40]; char b[160];
    int n = snprintf(b, sizeof b, "SEGV addr=%p rip=%llx rax=%llx rdi=%llx rsi=%llx\n", si->si_addr,
        (unsigned long long)c->uc_mcontext.gregs[REG_RIP], (unsigned long long)c->uc_mcontext.gregs[REG_RAX],
        (unsigned long long)c->uc_mcontext.gregs[REG_RDI], (unsigned long long)c->uc_mcontext.gregs[REG_RSI]);
    write(2, b, n);
    { unsigned long long *sp = (unsigned long long *)c->uc_mcontext.gregs[REG_RSP]; void *w[24]; int m = 0; for (int i = 0; i < 24; i++) w[m++] = (void *)sp[i]; write(2, "stack words:\n", 13); backtrace_symbols_fd(w, m, 2); }
    int k = backtrace(bt, 40); backtrace_symbols_fd(bt, k, 2); _exit(139);
}
__attribute__((constructor)) static void inst(void) { struct sigaction a = {0}; a.sa_sigaction = h; a.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &a, 0); }
