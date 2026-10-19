/*
HOST_THREAD.C

Threads that execute ILP32 guest code on 64-bit UWP (Windows x64 / Xbox Dev Mode).

Guest ILP32 (x32) code stores stack addresses in 32-bit pointers, so every
thread running guest code needs its stack allocated below 4 GB (via
host_low_map). Before switching %rsp to the low-4GB guest stack, we update
the current thread's Windows TEB stack limits (NtCurrentTeb()->NtTib.StackBase
and StackLimit, just as Windows Fibers SwitchToFiber does) so that Windows x64
SEH, WinRT, Direct3D 12, and SDL3 stack unwinding recognize the low-4GB stack
as the thread's valid stack. When the function returns, %rsp and the TEB stack
limits are restored to the OS thread stack and the low-4GB stack is unmapped.
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "host.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_SIZE 0x4000
#define GUEST_PROT_NONE 0
#define GUEST_PROT_READ 1
#define GUEST_PROT_WRITE 2

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
	return guest_tp;
}

void host_set_tp(uint32_t thread)
{
	guest_tp = thread;
}

/* ---------- low-4GB stack allocation and execution */

static void *stack_allocate(size_t size, void **mapping, size_t *mapping_size)
{
	size_t total = ((size + 0xffff) & ~(size_t)0xffff) + GUARD_SIZE;
	void *base = host_low_map(total, GUEST_PROT_READ | GUEST_PROT_WRITE);

	if (!base)
		return NULL;
	/* guard pages at the bottom of the stack */
	host_guest_mprotect((uint64_t)(uintptr_t)base, GUARD_SIZE, GUEST_PROT_NONE);
	*mapping = base;
	*mapping_size = total;
	return (char *)base + GUARD_SIZE;
}

static int on_guest_stack(void)
{
	uint64_t sp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
	return sp < 0x100000000ULL;
}

void *host_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size)
{
	void *mapping = NULL;
	size_t mapping_size = 0;
	void *stack_low;
	void *stack_high;
	PNT_TIB tib;
	void *old_base;
	void *old_limit;
	void *result = NULL;

	if (on_guest_stack())
		return function(argument);

	stack_low = stack_allocate(stack_size, &mapping, &mapping_size);
	if (!stack_low)
		host_fatal("could not allocate a %zu KB stack below 4 GB", stack_size / 1024);
	stack_high = (char *)mapping + mapping_size;

	tib = (PNT_TIB)NtCurrentTeb();
	old_base = tib->StackBase;
	old_limit = tib->StackLimit;
	tib->StackBase = stack_high;
	tib->StackLimit = stack_low;

	__asm__ volatile(
		"movq %%rsp, %%rbx\n\t"
		"movq %3, %%rsp\n\t"
		"subq $32, %%rsp\n\t"
		"movq %2, %%rcx\n\t"
		"callq *%1\n\t"
		"movq %%rbx, %%rsp\n\t"
		: "=a"(result)
		: "r"(function), "r"(argument), "r"((uintptr_t)stack_high & ~(uintptr_t)15)
		: "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11",
		  "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "memory", "cc"
	);

	tib->StackBase = old_base;
	tib->StackLimit = old_limit;
	host_low_unmap(mapping, mapping_size);
	return result;
}

/* ---------- calling into the x86-64 ILP32 (sysv_abi) guest */

typedef __attribute__((sysv_abi)) uint32_t (*guest_function)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef __attribute__((sysv_abi)) void (*guest_start_function)(uint32_t);

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	if (!on_guest_stack())
		host_fatal("guest code called on a thread without a low-4GB guest stack");
	if (!guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
	return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

void host_run_guest_main(uint32_t boot)
{
	((guest_start_function)(uintptr_t)host_image.header->start)(boot);
	host_fatal("the guest returned from __guest_start");
}

/* ---------- guest threads */

struct thread_start
{
	void *(*function)(void *);
	void *argument;
	size_t stack_size;
};

static DWORD WINAPI thread_main(LPVOID context)
{
	struct thread_start start = *(struct thread_start *)context;

	free(context);
	host_debug_thread_started();
	host_run_on_guest_stack(start.function, start.argument, start.stack_size);
	if (guest_tp)
		((guest_function)(uintptr_t)host_image.header->thread_detach)(0, 0, 0, 0);
	host_debug_thread_exited();
	guest_tp = 0;
	return 0;
}

int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size)
{
	struct thread_start *start = calloc(1, sizeof(*start));
	HANDLE handle;

	if (!start)
		return ENOMEM;
	start->function = function;
	start->argument = argument;
	start->stack_size = stack_size < (1024 * 1024) ? (1024 * 1024) : stack_size;

	handle = CreateThread(NULL, 0, thread_main, start, 0, NULL);
	if (!handle)
	{
		free(start);
		return EAGAIN;
	}
	CloseHandle(handle);
	return 0;
}

static void *guest_thread_main(void *guest_thread)
{
	host_call_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread, 0, 0, 0);
	return NULL;
}

int host_thread_create(uint32_t guest_thread, uint32_t stack_size)
{
	return host_native_thread_create(guest_thread_main, (void *)(uintptr_t)guest_thread, stack_size);
}

void host_debug_thread_started(void)
{
}

void host_debug_thread_exited(void)
{
}

void host_debug_start_sampler(const char *setting)
{
	(void)setting;
}
