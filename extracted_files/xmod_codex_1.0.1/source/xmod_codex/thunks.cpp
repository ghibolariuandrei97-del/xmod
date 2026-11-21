// thunks.cpp - the x86-32 assembly that makes generic calls and hooks possible (same code for Linux GCC/zig and
// MinGW; Intel syntax inside top-level asm). Offsets used here are the CallCtx / HookRet layouts from codex.h.
#include "codex.h"
#include <cstddef>

static_assert(sizeof(void *) == 4, "Codex is x86-32 only (HLDS is 32-bit)");
static_assert(offsetof(CallCtx, fn) == 0 && offsetof(CallCtx, nwords) == 4 && offsetof(CallCtx, words) == 8, "CallCtx layout");
static_assert(offsetof(CallCtx, ecx) == 12 && offsetof(CallCtx, edx) == 16 && offsetof(CallCtx, fpu) == 20, "CallCtx layout");
static_assert(offsetof(CallCtx, r_eax) == 24 && offsetof(CallCtx, r_edx) == 28 && offsetof(CallCtx, r_st0) == 32, "CallCtx layout");
static_assert(offsetof(CallCtx, rec_esp) == 40 && offsetof(CallCtx, rec_ebp) == 44 && offsetof(CallCtx, fpu_env) == 64, "CallCtx layout");
static_assert(offsetof(HookRet, eax) == 0 && offsetof(HookRet, edx) == 4 && offsetof(HookRet, st0) == 8, "HookRet layout");
static_assert(offsetof(HookRet, fpu) == 16 && offsetof(HookRet, pop) == 20 && sizeof(HookRet) == 24, "HookRet layout");

#ifdef _WIN32
#define SYM(x) "_" #x
#define HIDDEN(x) ""
#else
#define SYM(x) #x
#define HIDDEN(x) ".hidden " #x "\n"
#endif

// push the argument words, set ecx/edx, call, restore esp from esi (works for every convention, even with a wrong
// argument count), store eax/edx and - only when the function returns a float - st(0). Expects ebx = CallCtx*.
#define CALL_BODY(L)                                                                     \
	"    mov esi, esp\n"                                                                 \
	"    mov ecx, [ebx+4]\n"                                                             \
	"    mov edx, [ebx+8]\n"                                                             \
	"    mov eax, ecx\n"                                                                 \
	"    shl eax, 2\n"                                                                   \
	"    mov edi, esp\n"                                                                 \
	"    sub edi, eax\n"                                                                 \
	"    and edi, 15\n"                                                                  \
	"    sub esp, edi\n"                       /* esp is 16-byte aligned at the call */  \
	"1" L ":\n"                                                                          \
	"    test ecx, ecx\n"                                                                \
	"    jz 2" L "f\n"                                                                   \
	"    dec ecx\n"                                                                      \
	"    push dword ptr [edx+ecx*4]\n"                                                   \
	"    jmp 1" L "b\n"                                                                  \
	"2" L ":\n"                                                                          \
	"    mov ecx, [ebx+12]\n"                                                            \
	"    mov edx, [ebx+16]\n"                                                            \
	"    call dword ptr [ebx]\n"                                                         \
	"    mov esp, esi\n"                                                                 \
	"    mov [ebx+24], eax\n"                                                            \
	"    mov [ebx+28], edx\n"                                                            \
	"    cmp dword ptr [ebx+20], 0\n"                                                    \
	"    je 3" L "f\n"                                                                   \
	"    fstp qword ptr [ebx+32]\n"                                                      \
	"3" L ":\n"

__asm__(
	".intel_syntax noprefix\n"
	".text\n"
	".globl " SYM(codex_raw_call) "\n"
	HIDDEN(codex_raw_call)
	SYM(codex_raw_call) ":\n"
	"    push ebp\n"
	"    mov ebp, esp\n"
	"    push ebx\n"
	"    push esi\n"
	"    push edi\n"
	"    mov ebx, [ebp+8]\n"
	CALL_BODY("0")
	"    pop edi\n"
	"    pop esi\n"
	"    pop ebx\n"
	"    pop ebp\n"
	"    ret\n"

	// ---------------------------------------------------------------------------------------------------------
	// common hook entry. The per-hook stub did: mov eax, Hook* ; jmp codex_hook_entry.
	// Stack: [esp] = return address into the caller, [esp+4...] = stack arguments; ecx/edx may hold arguments.
	// Calls codex_hook_dispatch(Hook*, uint32_t *stack_args, ecx, edx, HookRet*), then returns to the caller with
	// the requested eax/edx/st(0) and pops HookRet.pop bytes (callee-cleanup conventions).
	".globl " SYM(codex_hook_entry) "\n"
	HIDDEN(codex_hook_entry)
	SYM(codex_hook_entry) ":\n"
	"    push ebp\n"
	"    mov ebp, esp\n"
	"    push ebx\n"
	"    push esi\n"
	"    push edi\n"
	"    sub esp, 24\n"
	"    lea esi, [ebp-36]\n"                  // HookRet
	"    and esp, -16\n"
	"    sub esp, 12\n"                        // 12 + 5 pushes = 32: aligned at the call
	"    push esi\n"
	"    push edx\n"
	"    push ecx\n"
	"    lea edi, [ebp+8]\n"
	"    push edi\n"
	"    push eax\n"
	"    call " SYM(codex_hook_dispatch) "\n"
	"    mov ecx, [esi+20]\n"                  // bytes to pop
	"    add ecx, ebp\n"
	"    mov edi, [ebp+4]\n"                   // move the return address and the saved ebp up by 'pop' bytes
	"    mov [ecx+4], edi\n"
	"    mov edi, [ebp]\n"
	"    mov [ecx], edi\n"
	"    mov eax, [esi]\n"
	"    mov edx, [esi+4]\n"
	"    cmp dword ptr [esi+16], 0\n"
	"    je 9f\n"
	"    fld qword ptr [esi+8]\n"
	"9:\n"
	"    mov ebx, [ebp-4]\n"
	"    mov edi, [ebp-12]\n"
	"    mov esi, [ebp-8]\n"
	"    mov esp, ecx\n"
	"    pop ebp\n"
	"    ret\n"

#ifdef _WIN32
	// ---------------------------------------------------------------------------------------------------------
	// guarded call (Windows): registers its own SEH frame so that an exception the called code does not handle
	// itself ends up in codex_seh_handler, which resumes at codex_seh_recover with our registers.
	// SEH record on the stack: [esp] next, [esp+4] handler, [esp+8] CallCtx*.
	".globl " SYM(codex_raw_call_seh) "\n"
	SYM(codex_raw_call_seh) ":\n"
	"    push ebp\n"
	"    mov ebp, esp\n"
	"    push ebx\n"
	"    push esi\n"
	"    push edi\n"
	"    mov ebx, [ebp+8]\n"
	"    fnstenv [ebx+64]\n"
	"    fldenv [ebx+64]\n"                    // fnstenv masks x87 exceptions: put the environment back at once
	"    push ebx\n"
	"    push offset " SYM(codex_seh_handler) "\n"
	"    push dword ptr fs:[0]\n"
	"    mov dword ptr fs:[0], esp\n"
	"    mov [ebx+40], esp\n"
	"    mov [ebx+44], ebp\n"
	"    mov dword ptr [ebx+48], 0\n"
	CALL_BODY("1")
	"    jmp 4f\n"
	".globl " SYM(codex_seh_recover) "\n"
	SYM(codex_seh_recover) ":\n"              // esp = SEH record, ebp = our frame, ebx = CallCtx* (set by the handler)
	"    fldenv [ebx+64]\n"                    // drop whatever the faulting code left on the x87 stack
	"4:\n"
	"    mov eax, [esp]\n"
	"    mov dword ptr fs:[0], eax\n"          // unlink our record (and any stale record of the dead frames)
	"    add esp, 12\n"
	"    pop edi\n"
	"    pop esi\n"
	"    pop ebx\n"
	"    pop ebp\n"
	"    ret\n"
#endif
	".att_syntax prefix\n");
