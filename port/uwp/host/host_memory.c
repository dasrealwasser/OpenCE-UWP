/*
HOST_MEMORY.C

Guest address space management and memory write tracking on 64-bit UWP
(Windows 10/11 and Xbox One / Xbox Series X|S Dev Mode).

Reserves the fixed guest ranges below 4 GB:
- 0x80000000..0x88000000: the Xbox contiguous memory window (128 MB)
- 0x88000000..0x8c000000: the guest ELF image (64 MB)
And manages on-demand pools below 4 GB for guest heap (musl malloc),
guest thread stacks, and anonymous mmap allocations.
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memoryapi.h>

#include "host.h"
#include "../../android/host/host_watch_hash.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE_BYTES 0x1000ULL
#define ALLOC_GRANULARITY 0x10000ULL
#define LOW_POOL_SIZE (64ULL * 1024 * 1024)
#define LOW_POOL_PAGES (LOW_POOL_SIZE / PAGE_SIZE_BYTES)
#define MAXIMUM_POOLS 24

/* POSIX mmap/mprotect flags passed by the guest's musl or xbox_memory.c */
#define GUEST_PROT_NONE 0
#define GUEST_PROT_READ 1
#define GUEST_PROT_WRITE 2
#define GUEST_PROT_EXEC 4

#define GUEST_MAP_SHARED 0x01
#define GUEST_MAP_PRIVATE 0x02
#define GUEST_MAP_FIXED 0x10
#define GUEST_MAP_ANONYMOUS 0x20
#define GUEST_MAP_NORESERVE 0x4000
#define GUEST_MAP_FIXED_NOREPLACE 0x100000

struct low_pool
{
	uintptr_t base;
	uint64_t used[LOW_POOL_PAGES / 64];
};

static SRWLOCK pool_lock = SRWLOCK_INIT;
static struct low_pool pools[MAXIMUM_POOLS];
static size_t pool_count;
static uintptr_t next_pool_hint = 0x10000000ULL;

static int fixed_reserved;
static int fixed_failed;
static uint32_t committed_image_size;

static PVOID VirtualAlloc_wrapper(PVOID base, SIZE_T size, ULONG type, ULONG protect)
{
	PVOID p = VirtualAllocFromApp(base, size, type, protect);
	if (!p)
		p = VirtualAlloc(base, size, type, protect);
	return p;
}

static BOOL VirtualProtect_wrapper(PVOID base, SIZE_T size, ULONG new_protect, PULONG old_protect)
{
	if (VirtualProtectFromApp(base, size, new_protect, old_protect))
		return TRUE;
	return VirtualProtect(base, size, new_protect, old_protect);
}

static ULONG win32_protect_from_guest(int protection)
{
	if ((protection & GUEST_PROT_EXEC) && (protection & GUEST_PROT_WRITE))
		return PAGE_EXECUTE_READWRITE;
	if (protection & GUEST_PROT_EXEC)
		return PAGE_EXECUTE_READ;
	if (protection & GUEST_PROT_WRITE)
		return PAGE_READWRITE;
	if (protection & GUEST_PROT_READ)
		return PAGE_READONLY;
	return PAGE_NOACCESS;
}

void host_memory_reserve_early(void)
{
	PVOID xbox_win;
	PVOID img_win;

	if (fixed_reserved || fixed_failed)
		return;

	xbox_win = VirtualAlloc_wrapper(
		(PVOID)(uintptr_t)HALO_GUEST_XBOX_MEMORY_BASE,
		HALO_GUEST_XBOX_MEMORY_SIZE,
		MEM_RESERVE,
		PAGE_NOACCESS);
	if (xbox_win != (PVOID)(uintptr_t)HALO_GUEST_XBOX_MEMORY_BASE)
	{
		if (xbox_win)
			VirtualFree(xbox_win, 0, MEM_RELEASE);
		fixed_failed = 1;
		return;
	}

	img_win = VirtualAlloc_wrapper(
		(PVOID)(uintptr_t)HALO_GUEST_IMAGE_BASE,
		HALO_GUEST_IMAGE_MAX_SIZE,
		MEM_RESERVE,
		PAGE_NOACCESS);
	if (img_win != (PVOID)(uintptr_t)HALO_GUEST_IMAGE_BASE)
	{
		if (img_win)
			VirtualFree(img_win, 0, MEM_RELEASE);
		VirtualFree(xbox_win, 0, MEM_RELEASE);
		fixed_failed = 1;
		return;
	}

	fixed_reserved = 1;
}

int host_memory_fixed_unavailable(void)
{
	host_memory_reserve_early();
	return fixed_failed;
}

void host_memory_report_low_mappings(FILE *file)
{
	uintptr_t addr = 0x10000;
	MEMORY_BASIC_INFORMATION mbi;

	host_logf(HOST_LOG_ERROR, "virtual memory mappings below 4 GB:");
	if (file)
		fprintf(file, "virtual memory mappings below 4 GB:\n");
	while (addr < 0x100000000ULL && VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
	{
		uintptr_t base = (uintptr_t)mbi.BaseAddress;
		uintptr_t end = base + mbi.RegionSize;
		if (mbi.State != MEM_FREE)
		{
			host_logf(HOST_LOG_ERROR, "  %08llx-%08llx state=%lx protect=%lx type=%lx",
				(unsigned long long)base, (unsigned long long)end,
				(unsigned long)mbi.State, (unsigned long)mbi.Protect, (unsigned long)mbi.Type);
			if (file)
			{
				fprintf(file, "  %08llx-%08llx state=%lx protect=%lx type=%lx\n",
					(unsigned long long)base, (unsigned long long)end,
					(unsigned long)mbi.State, (unsigned long)mbi.Protect, (unsigned long)mbi.Type);
			}
		}
		if (end <= addr)
			break;
		addr = end;
	}
}

int host_memory_initialize(uint32_t image_base, uint32_t image_size)
{
	uint32_t aligned_size = (image_size + 0xfffU) & ~0xfffU;
	PVOID committed;

	if (image_base != HALO_GUEST_IMAGE_BASE || image_size == 0 || aligned_size > HALO_GUEST_IMAGE_MAX_SIZE)
	{
		host_logf(HOST_LOG_ERROR, "guest image range 0x%08x..+0x%x is outside the reserved slot",
			image_base, image_size);
		return -1;
	}

	host_memory_reserve_early();
	if (fixed_failed)
	{
		host_logf(HOST_LOG_ERROR, "could not reserve fixed guest address range 0x80000000..0x8c000000");
		host_memory_report_low_mappings(NULL);
		return -1;
	}

	committed = VirtualAlloc_wrapper(
		(PVOID)(uintptr_t)image_base,
		aligned_size,
		MEM_COMMIT,
		PAGE_READWRITE);
	if (committed != (PVOID)(uintptr_t)image_base)
	{
		host_logf(HOST_LOG_ERROR, "VirtualAlloc MEM_COMMIT for guest image at 0x%08x failed (err=%lu)",
			image_base, GetLastError());
		return -1;
	}

	committed_image_size = aligned_size;
	return 0;
}

int host_image_protect(void *address, size_t size, int executable, int writable)
{
	ULONG protect = PAGE_READONLY;
	ULONG old_protect = 0;

	if (executable && writable)
		protect = PAGE_EXECUTE_READWRITE;
	else if (executable)
		protect = PAGE_EXECUTE_READ;
	else if (writable)
		protect = PAGE_READWRITE;

	if (!VirtualProtect_wrapper(address, size, protect, &old_protect))
	{
		host_logf(HOST_LOG_ERROR, "VirtualProtectFromApp(%p, 0x%zx, 0x%lx) failed (err=%lu)",
			address, size, (unsigned long)protect, GetLastError());
		return -1;
	}
	if (executable)
		FlushInstructionCache(GetCurrentProcess(), address, size);
	return 0;
}

/* ---------- low-4GB page pool */

static int pool_test(const struct low_pool *pool, size_t page)
{
	return (pool->used[page >> 6] >> (page & 63)) & 1;
}

static void pool_set(struct low_pool *pool, size_t first, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
	{
		size_t p = first + i;
		pool->used[p >> 6] |= 1ULL << (p & 63);
	}
}

static void pool_clear(struct low_pool *pool, size_t first, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
	{
		size_t p = first + i;
		pool->used[p >> 6] &= ~(1ULL << (p & 63));
	}
}

static struct low_pool *pool_grow(void)
{
	uintptr_t addr;

	if (pool_count >= MAXIMUM_POOLS)
		return NULL;
	for (addr = next_pool_hint; addr + LOW_POOL_SIZE <= 0xfe000000ULL; addr += 0x01000000ULL)
	{
		PVOID mapping;
		if (addr + LOW_POOL_SIZE > HALO_GUEST_XBOX_MEMORY_BASE &&
			addr < HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_MAX_SIZE)
		{
			addr = HALO_GUEST_IMAGE_BASE + HALO_GUEST_IMAGE_MAX_SIZE - 0x01000000ULL;
			continue;
		}
		mapping = VirtualAlloc_wrapper((PVOID)addr, LOW_POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS);
		if (mapping == (PVOID)addr)
		{
			struct low_pool *pool = &pools[pool_count++];
			memset(pool, 0, sizeof(*pool));
			pool->base = addr;
			next_pool_hint = addr + LOW_POOL_SIZE;
			return pool;
		}
		if (mapping)
			VirtualFree(mapping, 0, MEM_RELEASE);
	}
	return NULL;
}

static void *pool_take(struct low_pool *pool, size_t pages, int protection)
{
	size_t run = 0, start = 0, page;

	for (page = 0; page < LOW_POOL_PAGES; page++)
	{
		if (pool->used[page >> 6] == ~0ULL && run == 0)
		{
			page = (page | 63);
			continue;
		}
		if (pool_test(pool, page))
		{
			run = 0;
			continue;
		}
		if (run == 0)
			start = page;
		if (++run == pages)
		{
			void *address = (void *)(pool->base + start * PAGE_SIZE_BYTES);
			ULONG win_prot = win32_protect_from_guest(protection);
			if (win_prot != PAGE_NOACCESS)
			{
				if (!VirtualAlloc_wrapper(address, pages * PAGE_SIZE_BYTES, MEM_COMMIT, win_prot))
					return NULL;
			}
			pool_set(pool, start, pages);
			return address;
		}
	}
	return NULL;
}

void *host_low_map(size_t size, int protection)
{
	size_t pages = (size + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	void *address = NULL;
	size_t i;

	if (pages == 0 || pages > LOW_POOL_PAGES)
		return NULL;
	AcquireSRWLockExclusive(&pool_lock);
	for (i = 0; i < pool_count && !address; i++)
		address = pool_take(&pools[i], pages, protection);
	if (!address)
	{
		struct low_pool *pool = pool_grow();
		if (pool)
			address = pool_take(pool, pages, protection);
	}
	ReleaseSRWLockExclusive(&pool_lock);
	return address;
}

void host_low_unmap(void *address, size_t size)
{
	uintptr_t start = (uintptr_t)address & ~(PAGE_SIZE_BYTES - 1);
	size_t pages = ((uintptr_t)address + size - start + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES;
	size_t i;

	if (!pages)
		return;
	AcquireSRWLockExclusive(&pool_lock);
	for (i = 0; i < pool_count; i++)
	{
		struct low_pool *pool = &pools[i];
		if (start >= pool->base && start + pages * PAGE_SIZE_BYTES <= pool->base + LOW_POOL_SIZE)
		{
			VirtualFree((PVOID)start, pages * PAGE_SIZE_BYTES, MEM_DECOMMIT);
			pool_clear(pool, (start - pool->base) / PAGE_SIZE_BYTES, pages);
			break;
		}
	}
	ReleaseSRWLockExclusive(&pool_lock);
}

int host_low_owns(uintptr_t address, size_t size)
{
	uint64_t end = (uint64_t)address + size;
	size_t i;
	int owns = 0;

	if (end < address)
		return 0;
	if (address >= HALO_GUEST_XBOX_MEMORY_BASE &&
		end <= (uint64_t)HALO_GUEST_XBOX_MEMORY_BASE + HALO_GUEST_XBOX_MEMORY_SIZE)
	{
		return 1;
	}
	if (committed_image_size && address >= HALO_GUEST_IMAGE_BASE &&
		end <= (uint64_t)HALO_GUEST_IMAGE_BASE + committed_image_size)
	{
		return 1;
	}
	AcquireSRWLockExclusive(&pool_lock);
	for (i = 0; i < pool_count; i++)
	{
		if (address >= pools[i].base && end <= pools[i].base + LOW_POOL_SIZE)
		{
			owns = 1;
			break;
		}
	}
	ReleaseSRWLockExclusive(&pool_lock);
	return owns;
}

long host_guest_mmap(uint64_t address, uint64_t size, int protection, int flags, int fd, int64_t offset)
{
	void *result;
	size_t aligned_size = (size_t)((size + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1));

	(void)fd;
	(void)offset;
	if (size == 0 || size >= 0x80000000ULL)
		return -EINVAL;
	if (!(flags & GUEST_MAP_ANONYMOUS))
		return -ENOTSUP;

	if (flags & (GUEST_MAP_FIXED | GUEST_MAP_FIXED_NOREPLACE))
	{
		if ((flags & GUEST_MAP_FIXED_NOREPLACE) || !host_low_owns((uintptr_t)address, aligned_size))
			return -ENOMEM;
		if (protection == GUEST_PROT_NONE)
		{
			VirtualFree((PVOID)(uintptr_t)address, aligned_size, MEM_DECOMMIT);
			return (long)address;
		}
		/* Decommit first so re-mapping a range yields freshly zeroed pages, matching POSIX MAP_FIXED */
		VirtualFree((PVOID)(uintptr_t)address, aligned_size, MEM_DECOMMIT);
		result = VirtualAlloc_wrapper(
			(PVOID)(uintptr_t)address,
			aligned_size,
			MEM_COMMIT,
			win32_protect_from_guest(protection));
		return result ? (long)(uintptr_t)result : -ENOMEM;
	}

	result = host_low_map(aligned_size, protection);
	if (!result)
		return -ENOMEM;
	return (long)(uintptr_t)result;
}

long host_guest_munmap(uint64_t address, uint64_t size)
{
	if (!host_low_owns((uintptr_t)address, (size_t)size))
		return -EINVAL;
	host_low_unmap((void *)(uintptr_t)address, (size_t)size);
	return 0;
}

long host_guest_mprotect(uint64_t address, uint64_t size, int protection)
{
	uintptr_t start = (uintptr_t)address & ~(PAGE_SIZE_BYTES - 1);
	size_t aligned_size = (size_t)(((uintptr_t)address + size - start + PAGE_SIZE_BYTES - 1) & ~(PAGE_SIZE_BYTES - 1));
	ULONG old_protect = 0;

	if (!host_low_owns(start, aligned_size))
		return -ENOMEM;
	if (protection == GUEST_PROT_NONE)
	{
		VirtualFree((PVOID)start, aligned_size, MEM_DECOMMIT);
		return 0;
	}
	if (!VirtualAlloc_wrapper((PVOID)start, aligned_size, MEM_COMMIT, win32_protect_from_guest(protection)))
		return -ENOMEM;
	if (!VirtualProtect_wrapper((PVOID)start, aligned_size, win32_protect_from_guest(protection), &old_protect))
		return -EACCES;
	return 0;
}

/* ---------- memory write tracking (vectored exception handler + page hash fallback) */

#define WATCH_BASE HALO_GUEST_XBOX_MEMORY_BASE
#define WATCH_SIZE HALO_GUEST_XBOX_MEMORY_SIZE
#define WATCH_PAGE_SHIFT 12
#define WATCH_PAGE_SIZE (1UL << WATCH_PAGE_SHIFT)
#define WATCH_PAGE_COUNT (WATCH_SIZE >> WATCH_PAGE_SHIFT)

static SRWLOCK watch_lock = SRWLOCK_INIT;
static volatile LONG watch_generations[WATCH_PAGE_COUNT];
static unsigned char watch_protected[WATCH_PAGE_COUNT];
static volatile LONG watch_serial_counter = 1;
static PVOID watch_veh_handle;
static int watch_initialized;
static int watch_use_hashes;
static struct host_watch_hash watch_hashes;

void host_memory_watch_use_hashes(void)
{
	watch_use_hashes = 1;
}

static int watch_range(uint32_t address, uint32_t size, uint32_t *first_page, uint32_t *last_page)
{
	uint64_t start, end;

	if (!size || address < WATCH_BASE)
		return 0;
	start = address - WATCH_BASE;
	end = start + size;
	if (end > WATCH_SIZE)
		return 0;
	*first_page = (uint32_t)(start >> WATCH_PAGE_SHIFT);
	*last_page = (uint32_t)((end - 1) >> WATCH_PAGE_SHIFT);
	return 1;
}

static LONG WINAPI watch_exception_handler(PEXCEPTION_POINTERS info)
{
	PEXCEPTION_RECORD rec = info->ExceptionRecord;
	uintptr_t fault_addr;
	uint32_t page;
	ULONG old_protect = 0;

	if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
		return EXCEPTION_CONTINUE_SEARCH;
	/* ExceptionInformation[0] == 1 means write access violation */
	if (rec->ExceptionInformation[0] != 1)
		return EXCEPTION_CONTINUE_SEARCH;

	fault_addr = (uintptr_t)rec->ExceptionInformation[1];
	if (fault_addr < WATCH_BASE || fault_addr >= WATCH_BASE + WATCH_SIZE)
		return EXCEPTION_CONTINUE_SEARCH;

	page = (uint32_t)((fault_addr - WATCH_BASE) >> WATCH_PAGE_SHIFT);
	AcquireSRWLockExclusive(&watch_lock);
	if (!watch_protected[page])
	{
		ReleaseSRWLockExclusive(&watch_lock);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (!VirtualProtect_wrapper(
		(PVOID)(uintptr_t)(WATCH_BASE + ((uintptr_t)page << WATCH_PAGE_SHIFT)),
		WATCH_PAGE_SIZE,
		PAGE_READWRITE,
		&old_protect))
	{
		ReleaseSRWLockExclusive(&watch_lock);
		return EXCEPTION_CONTINUE_SEARCH;
	}
	watch_protected[page] = 0;
	InterlockedIncrement(&watch_generations[page]);
	InterlockedIncrement(&watch_serial_counter);
	ReleaseSRWLockExclusive(&watch_lock);
	return EXCEPTION_CONTINUE_EXECUTION;
}

static void watch_switch_to_hashes(const char *reason)
{
	uint32_t page;
	ULONG old_protect = 0;

	for (page = 0; page < WATCH_PAGE_COUNT; page++)
	{
		if (watch_protected[page])
		{
			VirtualProtect_wrapper(
				(PVOID)(uintptr_t)(WATCH_BASE + ((uintptr_t)page << WATCH_PAGE_SHIFT)),
				WATCH_PAGE_SIZE,
				PAGE_READWRITE,
				&old_protect);
			watch_protected[page] = 0;
		}
	}
	host_watch_hash_initialize(&watch_hashes);
	watch_use_hashes = 1;
	host_logf(HOST_LOG_INFO, "write tracking: page hashes (%s)", reason);
}

void host_memory_watch_initialize(void)
{
	AcquireSRWLockExclusive(&watch_lock);
	if (!watch_initialized)
	{
		if (watch_use_hashes)
		{
			host_watch_hash_initialize(&watch_hashes);
			host_logf(HOST_LOG_INFO, "write tracking: page hashes (debug.memory_watch = false)");
		}
		else
		{
			watch_veh_handle = AddVectoredExceptionHandler(1, watch_exception_handler);
			if (!watch_veh_handle)
				watch_switch_to_hashes("AddVectoredExceptionHandler failed");
			else
				host_logf(HOST_LOG_INFO, "write tracking: page protection (VEH)");
		}
		watch_initialized = 1;
	}
	ReleaseSRWLockExclusive(&watch_lock);
}

void host_memory_watch_protect(uint32_t address, uint32_t size)
{
	uint32_t first, last, page, run_start;

	if (!watch_initialized)
		host_memory_watch_initialize();
	if (!watch_range(address, size, &first, &last))
		return;
	AcquireSRWLockExclusive(&watch_lock);
	if (watch_use_hashes)
	{
		host_watch_hash_protect(&watch_hashes, (const void *)(uintptr_t)WATCH_BASE, first, last);
		ReleaseSRWLockExclusive(&watch_lock);
		return;
	}
	page = first;
	while (page <= last)
	{
		if (watch_protected[page])
		{
			page++;
			continue;
		}
		run_start = page;
		while (page <= last && !watch_protected[page])
		{
			watch_protected[page] = 1;
			page++;
		}
		{
			ULONG old_protect = 0;
			if (!VirtualProtect_wrapper(
				(PVOID)(uintptr_t)(WATCH_BASE + ((uintptr_t)run_start << WATCH_PAGE_SHIFT)),
				(size_t)(page - run_start) << WATCH_PAGE_SHIFT,
				PAGE_READONLY,
				&old_protect))
			{
				while (run_start < page)
					watch_protected[run_start++] = 0;
				watch_switch_to_hashes("VirtualProtect PAGE_READONLY failed");
				host_watch_hash_protect(&watch_hashes, (const void *)(uintptr_t)WATCH_BASE, first, last);
				break;
			}
		}
	}
	ReleaseSRWLockExclusive(&watch_lock);
}

uint32_t host_memory_watch_generation(uint32_t address, uint32_t size)
{
	uint32_t first, last, page;
	uint32_t total = 0;

	if (!watch_range(address, size, &first, &last))
		return 0;
	if (watch_use_hashes)
	{
		AcquireSRWLockExclusive(&watch_lock);
		total = host_watch_hash_generation(&watch_hashes, first, last);
		ReleaseSRWLockExclusive(&watch_lock);
		return total;
	}
	for (page = first; page <= last; page++)
		total += (uint32_t)watch_generations[page];
	return total;
}

uint32_t host_memory_watch_serial(void)
{
	if (watch_use_hashes)
		return watch_hashes.serial;
	return (uint32_t)watch_serial_counter;
}

void host_memory_watch_prepare_write(uint32_t address, uint32_t size)
{
	uint32_t first, last, page, run_start;
	int changed = 0;

	if (!watch_initialized || !watch_range(address, size, &first, &last))
		return;
	AcquireSRWLockExclusive(&watch_lock);
	if (watch_use_hashes)
	{
		host_watch_hash_prepare_write(&watch_hashes, first, last);
		ReleaseSRWLockExclusive(&watch_lock);
		return;
	}
	page = first;
	while (page <= last)
	{
		if (!watch_protected[page])
		{
			page++;
			continue;
		}
		run_start = page;
		while (page <= last && watch_protected[page])
		{
			watch_protected[page] = 0;
			InterlockedIncrement(&watch_generations[page]);
			page++;
		}
		{
			ULONG old_protect = 0;
			VirtualProtect_wrapper(
				(PVOID)(uintptr_t)(WATCH_BASE + ((uintptr_t)run_start << WATCH_PAGE_SHIFT)),
				(size_t)(page - run_start) << WATCH_PAGE_SHIFT,
				PAGE_READWRITE,
				&old_protect);
		}
		changed = 1;
	}
	if (changed)
		InterlockedIncrement(&watch_serial_counter);
	ReleaseSRWLockExclusive(&watch_lock);
}

void host_memory_watch_forget(uint32_t address, uint32_t size)
{
	host_memory_watch_prepare_write(address, size);
}

void host_memory_watch_begin_frame(void)
{
	if (!watch_use_hashes)
		return;
	AcquireSRWLockExclusive(&watch_lock);
	host_watch_hash_scan(&watch_hashes, (const void *)(uintptr_t)WATCH_BASE);
	ReleaseSRWLockExclusive(&watch_lock);
}
