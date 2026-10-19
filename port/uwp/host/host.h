/*
HOST.H

Internals of the UWP port's 64-bit host executable (halo_uwp.exe).
See port/uwp/README.md for the overall design and
port/android/include/halo_android_abi.h for the ILP32 guest contract.
*/

#ifndef __HALO_UWP_HOST_H
#define __HALO_UWP_HOST_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "halo_android_abi.h"

/* ---------- logging */

void host_logf(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define HOST_LOG_INFO 4
#define HOST_LOG_WARN 5
#define HOST_LOG_ERROR 6

void host_exit(int code) __attribute__((noreturn));
int host_errno(void);
void host_set_errno(int err);

/* logs, shows the message to the player and terminates */
void host_fatal(const char *format, ...) __attribute__((format(printf, 1, 2), noreturn));

/* ---------- guest memory (host_memory.c) */

void host_memory_reserve_early(void);
int host_memory_initialize(uint32_t image_base, uint32_t image_size);
int host_memory_fixed_unavailable(void);
void host_memory_report_low_mappings(FILE *file);
void host_memory_watch_use_hashes(void);
void *host_low_map(size_t size, int protection);
void host_low_unmap(void *address, size_t size);
int host_low_owns(uintptr_t address, size_t size);
long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset);
long host_guest_munmap(uint64_t address, uint64_t size);
long host_guest_mprotect(uint64_t address, uint64_t size, int protection);

/* changes protection of an address range inside the guest image (used by host_loader.c) */
int host_image_protect(void *address, size_t size, int executable, int writable);

/* ---------- the guest image (host_loader.c) */

struct host_guest_image
{
	const struct halo_guest_header *header;
	uint32_t base, end;
};

extern struct host_guest_image host_image;

int host_load_image(const void *elf, size_t size);

/* ---------- entering guest code (host_thread.c) */

uint32_t host_call_guest(uint32_t function, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
int host_native_thread_create(void *(*function)(void *), void *argument, size_t stack_size);
void host_run_guest_main(uint32_t boot) __attribute__((noreturn));

/* runs function(argument) on a newly allocated low-4GB guest stack on the current thread */
void *host_run_on_guest_stack(void *(*function)(void *), void *argument, size_t stack_size);

/* ---------- import table (generated host_table.c & host_gl_table.c) */

void *host_resolve_import(const char *name);
void *host_gl_resolve_wrapper(const char *name);

/* ---------- SDL / GL (host_sdl.c, host_gl.c) */

void *host_gl_resolve(const char *name);

/* ---------- UWP paths & updater (host_main.c, host_updater.c) */

const char *host_uwp_data_root(void);
const char *host_uwp_save_root(void);
const char *host_uwp_config_path(void);
void host_updater_start(const char *local_state_dir);
void host_updater_poll_ui(void);

#endif /* __HALO_UWP_HOST_H */
