// guard.cpp - fault guard: run a native call so that a crash inside it becomes an error instead of killing the server.
// Windows: our own SEH frame around the call (thunks.cpp). Exceptions the called code handles itself are untouched.
// Linux: SIGSEGV/SIGBUS/SIGILL/SIGFPE handler + sigsetjmp; signals outside a guarded call go to the previous handler.
#include "codex.h"
#include <cstdio>
#ifdef _WIN32
#include <excpt.h>
#else
#include <csignal>
#include <setjmp.h>
#include <ucontext.h>
#endif

// a real (non-inlined) copy routine so it can be called through the guard
extern "C" __attribute__((noinline, used)) void codex_copy(void *dst, const void *src, size_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	const volatile uint8_t *s = (const volatile uint8_t *)src;
	for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

bool guarded_copy(void *dst, const void *src, size_t n)
{
	uint32_t w[3] = {(uint32_t)(uintptr_t)dst, (uint32_t)(uintptr_t)src, (uint32_t)n};
	CallCtx c;
	memset(&c, 0, sizeof c);
	c.fn = (void *)&codex_copy; c.nwords = 3; c.words = w;
	return guarded_call(&c);
}

#ifdef _WIN32
// ---------------------------------------------------------------------------------------------------------------
extern "C" void codex_raw_call_seh(CallCtx *c);
extern "C" void codex_seh_recover();

extern "C" __attribute__((used)) EXCEPTION_DISPOSITION __cdecl codex_seh_handler(EXCEPTION_RECORD *er, void *frame, CONTEXT *ctx, void *)
{
	if (er->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
	DWORD code = er->ExceptionCode;
	bool error = (code & 0xC0000000u) == 0xC0000000u || code == 0xE06D7363u /* MSVC C++ throw nobody caught */;
	if (!error) return ExceptionContinueSearch;          // breakpoints, debug strings, thread names...
	CallCtx *c = *(CallCtx **)((char *)frame + 8);
	c->faulted = 1;
	c->fault_code = code;
	c->fault_ip = (uint32_t)(uintptr_t)er->ExceptionAddress;
	c->fault_addr = (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) ? (uint32_t)er->ExceptionInformation[1] : 0;
	ctx->Esp = c->rec_esp;
	ctx->Ebp = c->rec_ebp;
	ctx->Ebx = (DWORD)(uintptr_t)c;
	ctx->Eip = (DWORD)(uintptr_t)&codex_seh_recover;
	return ExceptionContinueExecution;
}

bool guard_install() { return true; }
void guard_remove() {}

// after a caught stack overflow the stack's guard page is gone; without it the next overflow would kill the process
// instead of raising an exception. Put it back on the lowest committed page (what the CRT's _resetstkoflw does).
static void reset_stack_guard()
{
	volatile char probe = 0;
	MEMORY_BASIC_INFORMATION mbi;
	if (!VirtualQuery((const void *)&probe, &mbi, sizeof mbi)) return;
	uint8_t *p = (uint8_t *)mbi.AllocationBase, *sp = (uint8_t *)&probe;
	while (p < sp) {
		if (!VirtualQuery(p, &mbi, sizeof mbi)) return;
		if (mbi.State == MEM_COMMIT) break;
		p = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
	}
	if (p + 3 * 4096 >= sp) return;                     // not enough room left below us: leave it to the OS
	if (mbi.Protect & PAGE_GUARD) return;                // still there
	DWORD old;
	VirtualProtect(p, 4096, PAGE_READWRITE | PAGE_GUARD, &old);
}

bool guarded_call(CallCtx *c)
{
	c->faulted = 0;
	codex_raw_call_seh(c);
	if (c->faulted && c->fault_code == EXCEPTION_STACK_OVERFLOW) reset_stack_guard();
	return !c->faulted;
}

std::string fault_text(const CallCtx &c)
{
	char b[256];
	const char *what = "exception";
	switch (c.fault_code) {
	case EXCEPTION_ACCESS_VIOLATION: what = "access violation"; break;
	case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
	case EXCEPTION_PRIV_INSTRUCTION: what = "privileged instruction"; break;
	case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer divide by zero"; break;
	case EXCEPTION_STACK_OVERFLOW: what = "stack overflow"; break;
	case 0xE06D7363u: what = "unhandled C++ exception"; break;
	}
	Module *m = mod_of(c.fault_ip);
	if (c.fault_code == EXCEPTION_ACCESS_VIOLATION)
		snprintf(b, sizeof b, "%s at %08x (%s+%x) touching %08x", what, (unsigned)c.fault_ip, m ? m->name.c_str() : "?", m ? (unsigned)(c.fault_ip - m->base) : 0, (unsigned)c.fault_addr);
	else
		snprintf(b, sizeof b, "%s (0x%08x) at %08x (%s+%x)", what, (unsigned)c.fault_code, (unsigned)c.fault_ip, m ? m->name.c_str() : "?", m ? (unsigned)(c.fault_ip - m->base) : 0);
	return b;
}

#else
// ---------------------------------------------------------------------------------------------------------------
static __thread sigjmp_buf *t_jb = nullptr;
static __thread CallCtx *t_ctx = nullptr;
static __thread uint32_t t_fault_esp = 0;
static struct sigaction s_old[32];
static bool s_installed = false;
static const int SIGS[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};

static void on_fault(int sig, siginfo_t *si, void *uc)
{
	if (t_jb && t_ctx) {
		CallCtx *c = t_ctx;
		c->faulted = 1;
		c->fault_code = (uint32_t)sig;
		c->fault_addr = (uint32_t)(uintptr_t)(si ? si->si_addr : 0);
		c->fault_ip = uc ? (uint32_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_EIP] : 0;
		t_fault_esp = uc ? (uint32_t)((ucontext_t *)uc)->uc_mcontext.gregs[REG_ESP] : 0;
		siglongjmp(*t_jb, 1);
	}
	// not ours: behave exactly as if we were not here
	struct sigaction &o = s_old[sig];
	if (o.sa_flags & SA_SIGINFO) { if (o.sa_sigaction) { o.sa_sigaction(sig, si, uc); return; } }
	else if (o.sa_handler == SIG_IGN) return;
	else if (o.sa_handler != SIG_DFL && o.sa_handler) { o.sa_handler(sig); return; }
	signal(sig, SIG_DFL);
	raise(sig);
}

bool guard_install()
{
	if (s_installed) return true;
	// an alternate signal stack, so a stack overflow inside a guarded call can still be caught (the handler cannot
	// run on the exhausted stack). Only if this thread has none yet - an existing one (the engine's) is kept.
	{
		stack_t cur;
		if (sigaltstack(nullptr, &cur) == 0 && (cur.ss_flags & SS_DISABLE)) {
			static char alt[128 * 1024] __attribute__((aligned(16)));
			stack_t st;
			memset(&st, 0, sizeof st);
			st.ss_sp = alt; st.ss_size = sizeof alt; st.ss_flags = 0;
			sigaltstack(&st, nullptr);
		}
	}
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;    // NODEFER: leaving the handler with longjmp must not keep the signal blocked
	sigemptyset(&sa.sa_mask);
	for (int s : SIGS) sigaction(s, &sa, &s_old[s]);
	s_installed = true;
	return true;
}

void guard_remove()
{
	if (!s_installed) return;
	for (int s : SIGS) {
		struct sigaction cur;
		sigaction(s, nullptr, &cur);
		if (cur.sa_sigaction == on_fault) sigaction(s, &s_old[s], nullptr);   // only if nobody replaced us since
	}
	s_installed = false;
}

bool guarded_call(CallCtx *c)
{
	if (!s_installed) guard_install();
	sigjmp_buf jb;
	sigjmp_buf *prev_jb = t_jb;
	CallCtx *prev_ctx = t_ctx;
	c->faulted = 0;
	__asm__ volatile("fnstenv %0\n\tfldenv %0" : "=m"(*(char(*)[28])c->fpu_env));
	if (sigsetjmp(jb, 0) == 0) {
		t_jb = &jb; t_ctx = c;
		codex_raw_call(c);
		t_jb = prev_jb; t_ctx = prev_ctx;
		return true;
	}
	t_jb = prev_jb; t_ctx = prev_ctx;
	__asm__ volatile("fldenv %0" : : "m"(*(char(*)[28])c->fpu_env));
	return false;
}

std::string fault_text(const CallCtx &c)
{
	char b[256];
	const char *what = c.fault_code == SIGSEGV ? "SIGSEGV (bad memory access)" : c.fault_code == SIGBUS ? "SIGBUS" :
	                   c.fault_code == SIGILL ? "SIGILL (illegal instruction)" : c.fault_code == SIGFPE ? "SIGFPE (divide by zero)" : "signal";
	if (c.fault_code == SIGSEGV && t_fault_esp && c.fault_addr + 65536 >= t_fault_esp && c.fault_addr <= t_fault_esp + 65536) what = "SIGSEGV (stack overflow - endless recursion?)";
	Module *m = mod_of(c.fault_ip);
	snprintf(b, sizeof b, "%s at %08x (%s+%x) touching %08x", what, (unsigned)c.fault_ip, m ? m->name.c_str() : "?",
	         m ? (unsigned)(c.fault_ip - m->base) : 0, (unsigned)c.fault_addr);
	return b;
}
#endif
