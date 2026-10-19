/*
HOST_LOADER.C

Loads the static x86-64 ILP32 (x32) guest ELF image (build/uwp/halo_guest.elf)
into the reserved low-4GB address range at 0x88000000 and populates its
__host_import_table with 64-bit host wrapper addresses.

Supports both ELFCLASS32 (x86_64-linux-gnux32) and ELFCLASS64 (x86_64) static
executables (e_machine == EM_X86_64 == 62).
*/

#include "host.h"

#include <string.h>

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS32 1
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_W 2
#define PF_X 1

typedef struct
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct
{
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
} Elf32_Phdr;

typedef struct
{
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
} Elf64_Phdr;

struct guest_segment
{
	uint32_t type;
	uint32_t flags;
	uint64_t offset;
	uint64_t vaddr;
	uint64_t filesz;
	uint64_t memsz;
};

struct host_guest_image host_image;

static void missing_gl_import(void)
{
	host_fatal("the guest called an OpenGL ES function that was not loaded");
}

static int fill_imports(const struct halo_guest_header *header)
{
	uint64_t *table = (uint64_t *)(uintptr_t)header->import_table;
	const char *name = (const char *)(uintptr_t)header->import_names;
	uint32_t count = *(const uint32_t *)(uintptr_t)header->import_count;
	uint32_t index;
	int missing = 0;

	for (index = 0; index < count; index++)
	{
		void *target = host_resolve_import(name);

		if (!target && !strncmp(name, "hostgl_", 7))
		{
			target = host_gl_resolve_wrapper(name);
			if (!target)
				target = (void *)missing_gl_import;
		}
		if (!target)
		{
			host_logf(HOST_LOG_ERROR, "unresolved guest import: %s", name);
			missing++;
		}
		table[index] = (uint64_t)(uintptr_t)target;
		name += strlen(name) + 1;
	}
	return missing ? -1 : 0;
}

int host_load_image(const void *elf, size_t size)
{
	const unsigned char *ident = (const unsigned char *)elf;
	struct guest_segment segments[32];
	uint16_t phnum = 0, index;
	uint64_t low = ~0ULL, high = 0;
	const struct halo_guest_header *guest_header;

	if (size < sizeof(Elf32_Ehdr) ||
		ident[0] != ELFMAG0 || ident[1] != ELFMAG1 || ident[2] != ELFMAG2 || ident[3] != ELFMAG3 ||
		ident[5] != ELFDATA2LSB)
	{
		host_logf(HOST_LOG_ERROR, "halo_guest.elf is not a valid little-endian ELF image");
		return -1;
	}

	if (ident[4] == ELFCLASS32)
	{
		const Elf32_Ehdr *eh = (const Elf32_Ehdr *)elf;
		const Elf32_Phdr *ph;

		if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64 ||
			eh->e_phentsize != sizeof(Elf32_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 32 ||
			(uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf32_Phdr) > size)
		{
			host_logf(HOST_LOG_ERROR, "halo_guest.elf is not an ELF32 x86-64 static executable");
			return -1;
		}
		ph = (const Elf32_Phdr *)((const char *)elf + eh->e_phoff);
		phnum = eh->e_phnum;
		for (index = 0; index < phnum; index++)
		{
			segments[index].type = ph[index].p_type;
			segments[index].flags = ph[index].p_flags;
			segments[index].offset = ph[index].p_offset;
			segments[index].vaddr = ph[index].p_vaddr;
			segments[index].filesz = ph[index].p_filesz;
			segments[index].memsz = ph[index].p_memsz;
		}
	}
	else if (ident[4] == ELFCLASS64)
	{
		const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
		const Elf64_Phdr *ph;

		if (size < sizeof(Elf64_Ehdr) || eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64 ||
			eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 || eh->e_phnum > 32 ||
			eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size)
		{
			host_logf(HOST_LOG_ERROR, "halo_guest.elf is not an ELF64 x86-64 static executable");
			return -1;
		}
		ph = (const Elf64_Phdr *)((const char *)elf + eh->e_phoff);
		phnum = eh->e_phnum;
		for (index = 0; index < phnum; index++)
		{
			segments[index].type = ph[index].p_type;
			segments[index].flags = ph[index].p_flags;
			segments[index].offset = ph[index].p_offset;
			segments[index].vaddr = ph[index].p_vaddr;
			segments[index].filesz = ph[index].p_filesz;
			segments[index].memsz = ph[index].p_memsz;
		}
	}
	else
	{
		host_logf(HOST_LOG_ERROR, "halo_guest.elf has unknown ELF class %u", ident[4]);
		return -1;
	}

	for (index = 0; index < phnum; index++)
	{
		if (segments[index].type != PT_LOAD || segments[index].memsz == 0)
			continue;
		if (segments[index].offset + segments[index].filesz > size ||
			segments[index].filesz > segments[index].memsz)
		{
			host_logf(HOST_LOG_ERROR, "corrupt segment %u in halo_guest.elf", index);
			return -1;
		}
		if (segments[index].vaddr < low)
			low = segments[index].vaddr;
		if (segments[index].vaddr + segments[index].memsz > high)
			high = segments[index].vaddr + segments[index].memsz;
	}
	if (low != HALO_GUEST_IMAGE_BASE || high <= low || high > 0x100000000ULL)
	{
		host_logf(HOST_LOG_ERROR, "guest image span 0x%llx..0x%llx does not start at 0x%x",
			(unsigned long long)low, (unsigned long long)high, HALO_GUEST_IMAGE_BASE);
		return -1;
	}
	if (host_memory_initialize((uint32_t)low, (uint32_t)(high - low)) != 0)
		return -1;

	for (index = 0; index < phnum; index++)
	{
		if (segments[index].type != PT_LOAD || segments[index].memsz == 0)
			continue;
		memcpy((void *)(uintptr_t)segments[index].vaddr,
			(const char *)elf + segments[index].offset,
			(size_t)segments[index].filesz);
	}

	for (index = 0; index < phnum; index++)
	{
		uintptr_t seg_start, seg_end;

		if (segments[index].type != PT_LOAD || segments[index].memsz == 0)
			continue;
		if (segments[index].flags & PF_W)
			continue;
		seg_start = (uintptr_t)segments[index].vaddr & ~(uintptr_t)0xfff;
		seg_end = ((uintptr_t)(segments[index].vaddr + segments[index].memsz) + 0xfff) & ~(uintptr_t)0xfff;
		if (host_image_protect((void *)seg_start, seg_end - seg_start,
			(segments[index].flags & PF_X) != 0, 0) != 0)
		{
			return -1;
		}
	}

	guest_header = (const struct halo_guest_header *)(uintptr_t)low;
	if (guest_header->magic != HALO_GUEST_MAGIC || guest_header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		host_logf(HOST_LOG_ERROR, "guest image header mismatch (magic=0x%x abi=%u)",
			guest_header->magic, guest_header->abi_version);
		return -1;
	}
	if (fill_imports(guest_header) != 0)
		return -1;

	host_image.header = guest_header;
	host_image.base = (uint32_t)low;
	host_image.end = guest_header->image_end;
	host_logf(HOST_LOG_INFO, "loaded UWP x86-64 ILP32 guest image at 0x%08x..0x%08x (%u KB)",
		host_image.base, host_image.end, (host_image.end - host_image.base) / 1024);
	return 0;
}
