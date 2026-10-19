/*
HOST_SYSCALL.C

System call dispatcher for the UWP x86-64 ILP32 (x32) guest's musl runtime.

Translates the guest's musl syscalls (numbered by arch/arm64_32/bits/syscall.h.in)
to Windows UWP / UCRT operations:
- futexes -> WaitOnAddress / WakeByAddressSingle / WakeByAddressAll
- mmap / munmap / mprotect -> host_guest_mmap / munmap / mprotect (host_memory.c)
- file descriptors -> UCRT _wopen / _read / _write / _close / _lseeki64 and Win32 ReadFile/WriteFile
- clocks and sleep -> QueryPerformanceCounter / GetSystemTimeAsFileTime / Sleep
- getrandom -> BCryptGenRandom
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "host.h"
#include "../../../port/linux/src/posix.h"

/* Syscall numbers from port/android/guest/libc/arch/arm64_32/bits/syscall.h.in */
#define GUEST_SYS_getcwd 17
#define GUEST_SYS_dup 23
#define GUEST_SYS_dup3 24
#define GUEST_SYS_fcntl 25
#define GUEST_SYS_ioctl 29
#define GUEST_SYS_flock 32
#define GUEST_SYS_mkdirat 34
#define GUEST_SYS_unlinkat 35
#define GUEST_SYS_renameat 38
#define GUEST_SYS_statfs 43
#define GUEST_SYS_fstatfs 44
#define GUEST_SYS_truncate 45
#define GUEST_SYS_ftruncate 46
#define GUEST_SYS_faccessat 48
#define GUEST_SYS_chdir 49
#define GUEST_SYS_fchmod 52
#define GUEST_SYS_fchmodat 53
#define GUEST_SYS_openat 56
#define GUEST_SYS_close 57
#define GUEST_SYS_pipe2 59
#define GUEST_SYS_getdents64 61
#define GUEST_SYS_lseek 62
#define GUEST_SYS_read 63
#define GUEST_SYS_write 64
#define GUEST_SYS_readv 65
#define GUEST_SYS_writev 66
#define GUEST_SYS_pread64 67
#define GUEST_SYS_pwrite64 68
#define GUEST_SYS_preadv 69
#define GUEST_SYS_pwritev 70
#define GUEST_SYS_pselect6 72
#define GUEST_SYS_ppoll 73
#define GUEST_SYS_readlinkat 78
#define GUEST_SYS_newfstatat 79
#define GUEST_SYS_fstat 80
#define GUEST_SYS_fsync 82
#define GUEST_SYS_fdatasync 83
#define GUEST_SYS_utimensat 88
#define GUEST_SYS_exit 93
#define GUEST_SYS_exit_group 94
#define GUEST_SYS_waitid 95
#define GUEST_SYS_set_tid_address 96
#define GUEST_SYS_futex 98
#define GUEST_SYS_nanosleep 101
#define GUEST_SYS_getitimer 102
#define GUEST_SYS_setitimer 103
#define GUEST_SYS_timer_create 107
#define GUEST_SYS_timer_gettime 108
#define GUEST_SYS_timer_settime 110
#define GUEST_SYS_clock_gettime 113
#define GUEST_SYS_clock_getres 114
#define GUEST_SYS_clock_nanosleep 115
#define GUEST_SYS_sched_yield 124
#define GUEST_SYS_kill 129
#define GUEST_SYS_tkill 130
#define GUEST_SYS_tgkill 131
#define GUEST_SYS_sigaltstack 132
#define GUEST_SYS_rt_sigaction 134
#define GUEST_SYS_rt_sigprocmask 135
#define GUEST_SYS_rt_sigtimedwait 137
#define GUEST_SYS_times 153
#define GUEST_SYS_uname 160
#define GUEST_SYS_getrlimit 163
#define GUEST_SYS_getrusage 165
#define GUEST_SYS_umask 166
#define GUEST_SYS_gettimeofday 169
#define GUEST_SYS_getpid 172
#define GUEST_SYS_getppid 173
#define GUEST_SYS_getuid 174
#define GUEST_SYS_geteuid 175
#define GUEST_SYS_getgid 176
#define GUEST_SYS_getegid 177
#define GUEST_SYS_gettid 178
#define GUEST_SYS_sysinfo 179
#define GUEST_SYS_sendmsg 211
#define GUEST_SYS_recvmsg 212
#define GUEST_SYS_brk 214
#define GUEST_SYS_munmap 215
#define GUEST_SYS_mremap 216
#define GUEST_SYS_clone 220
#define GUEST_SYS_execve 221
#define GUEST_SYS_mmap 222
#define GUEST_SYS_mprotect 226
#define GUEST_SYS_madvise 233
#define GUEST_SYS_wait4 260
#define GUEST_SYS_prlimit64 261
#define GUEST_SYS_renameat2 276
#define GUEST_SYS_getrandom 278
#define GUEST_SYS_membarrier 283
#define GUEST_SYS_statx 291
#define GUEST_SYS_clone3 435

/* Guest open flags (arch/arm64_32/bits/fcntl.h) */
#define GUEST_O_ACCMODE 3
#define GUEST_O_RDONLY 0
#define GUEST_O_WRONLY 1
#define GUEST_O_RDWR 2
#define GUEST_O_CREAT 0100
#define GUEST_O_EXCL 0200
#define GUEST_O_TRUNC 01000
#define GUEST_O_APPEND 02000
#define GUEST_O_DIRECTORY 040000

/* Guest futex operations */
#define GUEST_FUTEX_WAIT 0
#define GUEST_FUTEX_WAKE 1
#define GUEST_FUTEX_WAIT_BITSET 9
#define GUEST_FUTEX_WAKE_BITSET 10
#define GUEST_FUTEX_CMD_MASK 0x7f

/* Guest errno values (musl generic bits/errno.h) */
#define GUEST_EPERM 1
#define GUEST_ENOENT 2
#define GUEST_EINTR 4
#define GUEST_EIO 5
#define GUEST_EBADF 9
#define GUEST_EAGAIN 11
#define GUEST_ENOMEM 12
#define GUEST_EACCES 13
#define GUEST_EFAULT 14
#define GUEST_EBUSY 16
#define GUEST_EEXIST 17
#define GUEST_EXDEV 18
#define GUEST_ENODEV 19
#define GUEST_ENOTDIR 20
#define GUEST_EISDIR 21
#define GUEST_EINVAL 22
#define GUEST_ENFILE 23
#define GUEST_EMFILE 24
#define GUEST_ENOTTY 25
#define GUEST_ENOSPC 28
#define GUEST_ESPIPE 29
#define GUEST_EROFS 30
#define GUEST_ERANGE 34
#define GUEST_ENOSYS 38
#define GUEST_ENOTEMPTY 39
#define GUEST_ENOTSUP 95
#define GUEST_ETIMEDOUT 110

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

struct guest_kstat
{
	unsigned long long st_dev;
	unsigned long long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned long long st_rdev;
	unsigned long long __pad;
	long long st_size;
	int st_blksize;
	int __pad2;
	long long st_blocks;
	long long st_atime_sec;
	long long st_atime_nsec;
	long long st_mtime_sec;
	long long st_mtime_nsec;
	long long st_ctime_sec;
	long long st_ctime_nsec;
	unsigned int __unused[2];
};

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

static int guest_errno_from_host(int err)
{
	switch (err)
	{
	case 0: return 0;
	case EPERM: return GUEST_EPERM;
	case ENOENT: return GUEST_ENOENT;
	case EINTR: return GUEST_EINTR;
	case EIO: return GUEST_EIO;
	case EBADF: return GUEST_EBADF;
	case EAGAIN: return GUEST_EAGAIN;
	case ENOMEM: return GUEST_ENOMEM;
	case EACCES: return GUEST_EACCES;
	case EFAULT: return GUEST_EFAULT;
	case EBUSY: return GUEST_EBUSY;
	case EEXIST: return GUEST_EEXIST;
	case EXDEV: return GUEST_EXDEV;
	case ENODEV: return GUEST_ENODEV;
	case ENOTDIR: return GUEST_ENOTDIR;
	case EISDIR: return GUEST_EISDIR;
	case EINVAL: return GUEST_EINVAL;
	case ENFILE: return GUEST_ENFILE;
	case EMFILE: return GUEST_EMFILE;
	case ENOTTY: return GUEST_ENOTTY;
	case ENOSPC: return GUEST_ENOSPC;
	case ESPIPE: return GUEST_ESPIPE;
	case EROFS: return GUEST_EROFS;
	case ERANGE: return GUEST_ERANGE;
	case ENOSYS: return GUEST_ENOSYS;
	case ENOTEMPTY: return GUEST_ENOTEMPTY;
	case ETIMEDOUT: return GUEST_ETIMEDOUT;
	default: return GUEST_EIO;
	}
}

static long neg_host_errno(void)
{
	int err = errno;
	return err ? -guest_errno_from_host(err) : -GUEST_EIO;
}

static int widen_utf8_path(const char *path, wchar_t *wide, size_t capacity)
{
	int len;
	size_t i;

	if (!path || !*path)
		return 0;
	len = MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)capacity);
	if (len <= 0)
		len = MultiByteToWideChar(CP_ACP, 0, path, -1, wide, (int)capacity);
	if (len <= 0)
		return 0;
	for (i = 0; wide[i]; i++)
	{
		if (wide[i] == L'/')
			wide[i] = L'\\';
	}
	return 1;
}

/* ---------- standard output and error logging */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static SRWLOCK log_lock = SRWLOCK_INIT;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	AcquireSRWLockExclusive(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_logf(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	ReleaseSRWLockExclusive(&log_lock);
}

static long guest_pread_fd(int fd, void *buffer, size_t count, int64_t offset)
{
	HANDLE handle;
	OVERLAPPED overlapped;
	DWORD bytes_read = 0;

	if (fd < 0)
		return -GUEST_EBADF;
	handle = (HANDLE)_get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE)
		return -GUEST_EBADF;
	if (count > 0x7fffffffU)
		count = 0x7fffffffU;
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = (DWORD)((uint64_t)offset & 0xffffffffULL);
	overlapped.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
	if (!ReadFile(handle, buffer, (DWORD)count, &bytes_read, &overlapped))
	{
		if (GetLastError() == ERROR_HANDLE_EOF)
			return 0;
		return -GUEST_EIO;
	}
	return (long)bytes_read;
}

static long guest_pwrite_fd(int fd, const void *buffer, size_t count, int64_t offset)
{
	HANDLE handle;
	OVERLAPPED overlapped;
	DWORD bytes_written = 0;

	if (fd < 0)
		return -GUEST_EBADF;
	handle = (HANDLE)_get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE)
		return -GUEST_EBADF;
	if (count > 0x7fffffffU)
		count = 0x7fffffffU;
	memset(&overlapped, 0, sizeof(overlapped));
	overlapped.Offset = (DWORD)((uint64_t)offset & 0xffffffffULL);
	overlapped.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
	if (!WriteFile(handle, buffer, (DWORD)count, &bytes_written, &overlapped))
		return -GUEST_EIO;
	return (long)bytes_written;
}

static long guest_writev(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 64)
		return -GUEST_EINVAL;
	if (fd == 1 || fd == 2)
	{
		for (index = 0; index < count; index++)
		{
			const char *base = GUEST(const char *, guest_vector[index].base);
			log_bytes(fd, base, guest_vector[index].length);
			total += (long)guest_vector[index].length;
		}
		return total;
	}
	for (index = 0; index < count; index++)
	{
		const void *base = GUEST(const void *, guest_vector[index].base);
		uint32_t len = guest_vector[index].length;
		long written;

		if (!len)
			continue;
		if (positional)
			written = guest_pwrite_fd(fd, base, len, offset + total);
		else
		{
			int r = _write(fd, base, len);
			written = r < 0 ? neg_host_errno() : r;
		}
		if (written < 0)
			return total > 0 ? total : written;
		total += written;
		if ((uint32_t)written < len)
			break;
	}
	return total;
}

static long guest_readv(int fd, uint64_t vector, int count, int64_t offset, int positional)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 64)
		return -GUEST_EINVAL;
	for (index = 0; index < count; index++)
	{
		void *base = GUEST(void *, guest_vector[index].base);
		uint32_t len = guest_vector[index].length;
		long got;

		if (!len)
			continue;
		if (positional)
			got = guest_pread_fd(fd, base, len, offset + total);
		else
		{
			int r = _read(fd, base, len);
			got = r < 0 ? neg_host_errno() : r;
		}
		if (got < 0)
			return total > 0 ? total : got;
		total += got;
		if ((uint32_t)got < len)
			break;
	}
	return total;
}

/* ---------- time and futex */

static void host_clock_realtime(struct guest_timespec *out)
{
	FILETIME ft;
	uint64_t ticks;
	const uint64_t epoch_diff = 116444736000000000ULL;

	GetSystemTimeAsFileTime(&ft);
	ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	if (ticks >= epoch_diff)
		ticks -= epoch_diff;
	else
		ticks = 0;
	out->seconds = (int32_t)(ticks / 10000000ULL);
	out->nanoseconds = (int32_t)((ticks % 10000000ULL) * 100ULL);
}

static void host_clock_monotonic(struct guest_timespec *out)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER counter;
	uint64_t seconds, remainder;

	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&counter);
	seconds = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
	remainder = (uint64_t)counter.QuadPart % (uint64_t)freq.QuadPart;
	out->seconds = (int32_t)seconds;
	out->nanoseconds = (int32_t)((remainder * 1000000000ULL) / (uint64_t)freq.QuadPart);
}

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout)
{
	int command = operation & GUEST_FUTEX_CMD_MASK;
	volatile uint32_t *target = GUEST(volatile uint32_t *, address);

	if (!address)
		return -GUEST_EFAULT;

	if (command == GUEST_FUTEX_WAIT || command == GUEST_FUTEX_WAIT_BITSET)
	{
		DWORD wait_ms = INFINITE;
		uint32_t expected = value;

		if (timeout)
		{
			const struct guest_timespec *ts = GUEST(const struct guest_timespec *, timeout);
			if (ts->seconds < 0 || ts->nanoseconds < 0 || ts->nanoseconds >= 1000000000)
				return -GUEST_EINVAL;
			if (command == GUEST_FUTEX_WAIT_BITSET)
			{
				struct guest_timespec now;
				int64_t diff_ms;
				host_clock_realtime(&now);
				diff_ms = ((int64_t)ts->seconds - (int64_t)now.seconds) * 1000LL +
					((int64_t)ts->nanoseconds - (int64_t)now.nanoseconds + 999999LL) / 1000000LL;
				wait_ms = diff_ms <= 0 ? 0 : (DWORD)diff_ms;
			}
			else
			{
				uint64_t ms = (uint64_t)ts->seconds * 1000ULL + ((uint64_t)ts->nanoseconds + 999999ULL) / 1000000ULL;
				wait_ms = ms > 0xfffffffeULL ? 0xfffffffeUL : (DWORD)ms;
			}
		}
		if (!WaitOnAddress((volatile VOID *)target, &expected, sizeof(uint32_t), wait_ms))
		{
			if (GetLastError() == ERROR_TIMEOUT)
				return -GUEST_ETIMEDOUT;
			return -GUEST_EAGAIN;
		}
		return 0;
	}
	if (command == GUEST_FUTEX_WAKE || command == GUEST_FUTEX_WAKE_BITSET)
	{
		if (value <= 1)
			WakeByAddressSingle((PVOID)target);
		else
			WakeByAddressAll((PVOID)target);
		return 1;
	}
	return -GUEST_ENOSYS;
}

static void fill_kstat(struct guest_kstat *ks, const struct posix_file_information *info)
{
	memset(ks, 0, sizeof(*ks));
	ks->st_mode = (info->flags & _posix_file_is_directory) ? 0040755 :
		((info->flags & _posix_file_is_read_only) ? 0100444 : 0100644);
	ks->st_nlink = 1;
	ks->st_size = ((long long)info->size_high << 32) | (unsigned long long)info->size_low;
	ks->st_blksize = 4096;
	ks->st_blocks = (ks->st_size + 511) / 512;
	ks->st_atime_sec = info->access_seconds;
	ks->st_atime_nsec = info->access_nanoseconds;
	ks->st_mtime_sec = info->modification_seconds;
	ks->st_mtime_nsec = info->modification_nanoseconds;
	ks->st_ctime_sec = info->creation_seconds;
	ks->st_ctime_nsec = info->creation_nanoseconds;
}

static long guest_openat(const char *path, int guest_flags)
{
	wchar_t wide[1024];
	int win_flags = _O_BINARY;
	int pmode = _S_IREAD | _S_IWRITE;
	int fd = -1;

	if (!widen_utf8_path(path, wide, 1024))
		return -GUEST_ENOENT;

	switch (guest_flags & GUEST_O_ACCMODE)
	{
	case GUEST_O_RDONLY: win_flags |= _O_RDONLY; break;
	case GUEST_O_WRONLY: win_flags |= _O_WRONLY; break;
	case GUEST_O_RDWR:   win_flags |= _O_RDWR; break;
	default: return -GUEST_EINVAL;
	}
	if (guest_flags & GUEST_O_CREAT)  win_flags |= _O_CREAT;
	if (guest_flags & GUEST_O_EXCL)   win_flags |= _O_EXCL;
	if (guest_flags & GUEST_O_TRUNC)  win_flags |= _O_TRUNC;
	if (guest_flags & GUEST_O_APPEND) win_flags |= _O_APPEND;

	if (_wsopen_s(&fd, wide, win_flags, _SH_DENYNO, pmode) != 0 || fd < 0)
		return neg_host_errno();
	return fd;
}

/* ---------- main syscall dispatch */

long long host_syscall(long long number, long long a, long long b, long long c,
	long long d, long long e, long long f)
{
	switch (number)
	{
	case GUEST_SYS_write:
		if (a == 1 || a == 2)
		{
			log_bytes((int)a, GUEST(const char *, b), (size_t)(uint32_t)c);
			return (uint32_t)c;
		}
		{
			int r = _write((int)a, GUEST(const void *, b), (unsigned int)(uint32_t)c);
			return r < 0 ? neg_host_errno() : r;
		}
	case GUEST_SYS_read:
		{
			int r = _read((int)a, GUEST(void *, b), (unsigned int)(uint32_t)c);
			return r < 0 ? neg_host_errno() : r;
		}
	case GUEST_SYS_writev:
		return guest_writev((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_pwritev:
		return guest_writev((int)a, (uint64_t)b, (int)c, d, 1);
	case GUEST_SYS_readv:
		return guest_readv((int)a, (uint64_t)b, (int)c, 0, 0);
	case GUEST_SYS_preadv:
		return guest_readv((int)a, (uint64_t)b, (int)c, d, 1);
	case GUEST_SYS_pread64:
		return guest_pread_fd((int)a, GUEST(void *, b), (size_t)(uint32_t)c, d);
	case GUEST_SYS_pwrite64:
		return guest_pwrite_fd((int)a, GUEST(const void *, b), (size_t)(uint32_t)c, d);

	case GUEST_SYS_openat:
		return guest_openat(GUEST(const char *, b), (int)c);
	case GUEST_SYS_close:
		if (a <= 2)
			return 0;
		return _close((int)a) == 0 ? 0 : neg_host_errno();
	case GUEST_SYS_lseek:
		{
			__int64 pos = _lseeki64((int)a, (__int64)b, (int)c);
			return pos < 0 ? neg_host_errno() : (long long)pos;
		}
	case GUEST_SYS_ftruncate:
		return _chsize_s((int)a, (__int64)b) == 0 ? 0 : neg_host_errno();
	case GUEST_SYS_fsync:
	case GUEST_SYS_fdatasync:
		return _commit((int)a) == 0 ? 0 : neg_host_errno();
	case GUEST_SYS_dup:
		{
			int r = _dup((int)a);
			return r < 0 ? neg_host_errno() : r;
		}
	case GUEST_SYS_dup3:
		{
			int r = _dup2((int)a, (int)b);
			return r < 0 ? neg_host_errno() : (int)b;
		}
	case GUEST_SYS_fcntl:
		return 0;

	case GUEST_SYS_fstat:
		{
			struct posix_file_information info;
			struct guest_kstat *ks = GUEST(struct guest_kstat *, b);
			if (!ks)
				return -GUEST_EFAULT;
			if (posix_fstat((int)a, &info) != 0)
				return -GUEST_EBADF;
			fill_kstat(ks, &info);
			return 0;
		}
	case GUEST_SYS_newfstatat:
		{
			struct posix_file_information info;
			struct guest_kstat *ks = GUEST(struct guest_kstat *, c);
			const char *path = GUEST(const char *, b);
			if (!ks || !path)
				return -GUEST_EFAULT;
			if (posix_stat(path, &info) != 0)
				return -GUEST_ENOENT;
			fill_kstat(ks, &info);
			return 0;
		}
	case GUEST_SYS_faccessat:
		{
			struct posix_file_information info;
			const char *path = GUEST(const char *, b);
			if (!path || posix_stat(path, &info) != 0)
				return -GUEST_ENOENT;
			return 0;
		}
	case GUEST_SYS_mkdirat:
		return posix_make_directory(GUEST(const char *, b)) == 0 ? 0 : neg_host_errno();
	case GUEST_SYS_unlinkat:
		{
			wchar_t wide[1024];
			if (!widen_utf8_path(GUEST(const char *, b), wide, 1024))
				return -GUEST_ENOENT;
			if (c & 0x200) /* AT_REMOVEDIR */
				return RemoveDirectoryW(wide) ? 0 : -GUEST_EACCES;
			return DeleteFileW(wide) ? 0 : -GUEST_ENOENT;
		}
	case GUEST_SYS_renameat:
	case GUEST_SYS_renameat2:
		{
			wchar_t old_w[1024], new_w[1024];
			if (!widen_utf8_path(GUEST(const char *, b), old_w, 1024) ||
				!widen_utf8_path(GUEST(const char *, d), new_w, 1024))
			{
				return -GUEST_ENOENT;
			}
			return MoveFileExW(old_w, new_w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -GUEST_EACCES;
		}
	case GUEST_SYS_getcwd:
		{
			char *buf = GUEST(char *, a);
			size_t size = (size_t)(uint32_t)b;
			const char *root = host_uwp_data_root();
			size_t len = strlen(root);
			if (!buf || size <= len)
				return -GUEST_ERANGE;
			memcpy(buf, root, len + 1);
			return (long long)(uint32_t)(uintptr_t)buf;
		}
	case GUEST_SYS_chdir:
		return _chdir(GUEST(const char *, a)) == 0 ? 0 : -GUEST_ENOENT;
	case GUEST_SYS_readlinkat:
		{
			const char *path = GUEST(const char *, b);
			char *buf = GUEST(char *, c);
			size_t size = (size_t)(uint32_t)d;
			char exe_path[1024];
			size_t len;
			if (!path || strcmp(path, "/proc/self/exe") != 0 || !buf || !size)
				return -GUEST_ENOENT;
			snprintf(exe_path, sizeof(exe_path), "%s/halo_uwp.exe", host_uwp_data_root());
			len = strlen(exe_path);
			if (len > size)
				len = size;
			memcpy(buf, exe_path, len);
			return (long long)len;
		}

	case GUEST_SYS_clock_gettime:
		{
			struct guest_timespec *ts = GUEST(struct guest_timespec *, b);
			if (!ts)
				return -GUEST_EFAULT;
			if ((int)a == 0)
				host_clock_realtime(ts);
			else
				host_clock_monotonic(ts);
			return 0;
		}
	case GUEST_SYS_clock_getres:
		{
			struct guest_timespec *ts = GUEST(struct guest_timespec *, b);
			if (ts)
			{
				ts->seconds = 0;
				ts->nanoseconds = 1000;
			}
			return 0;
		}
	case GUEST_SYS_gettimeofday:
		{
			struct guest_timespec ts;
			struct guest_timespec *tv = GUEST(struct guest_timespec *, a);
			host_clock_realtime(&ts);
			if (tv)
			{
				tv->seconds = ts.seconds;
				tv->nanoseconds = ts.nanoseconds / 1000;
			}
			return 0;
		}
	case GUEST_SYS_nanosleep:
	case GUEST_SYS_clock_nanosleep:
		{
			const struct guest_timespec *req = GUEST(const struct guest_timespec *,
				number == GUEST_SYS_nanosleep ? a : c);
			uint64_t ms;
			if (!req)
				return -GUEST_EFAULT;
			if (number == GUEST_SYS_clock_nanosleep && ((int)b & 1)) /* TIMER_ABSTIME */
			{
				struct guest_timespec now;
				int64_t diff_ns;
				if ((int)a == 0)
					host_clock_realtime(&now);
				else
					host_clock_monotonic(&now);
				diff_ns = ((int64_t)req->seconds - (int64_t)now.seconds) * 1000000000LL +
					((int64_t)req->nanoseconds - (int64_t)now.nanoseconds);
				if (diff_ns > 0)
					Sleep((DWORD)((diff_ns + 999999LL) / 1000000LL));
				return 0;
			}
			ms = (uint64_t)req->seconds * 1000ULL + ((uint64_t)req->nanoseconds + 999999ULL) / 1000000ULL;
			Sleep((DWORD)ms);
			return 0;
		}

	case GUEST_SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d);

	case GUEST_SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case GUEST_SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case GUEST_SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case GUEST_SYS_madvise:
		return 0;
	case GUEST_SYS_mremap:
	case GUEST_SYS_brk:
		return -GUEST_ENOMEM;

	case GUEST_SYS_exit:
	case GUEST_SYS_exit_group:
		host_exit((int)a);

	case GUEST_SYS_getpid:
	case GUEST_SYS_getppid:
		return (long long)GetCurrentProcessId();
	case GUEST_SYS_gettid:
	case GUEST_SYS_set_tid_address:
		return (long long)GetCurrentThreadId();
	case GUEST_SYS_getuid:
	case GUEST_SYS_geteuid:
	case GUEST_SYS_getgid:
	case GUEST_SYS_getegid:
		return 1000;
	case GUEST_SYS_sched_yield:
		SwitchToThread();
		return 0;
	case GUEST_SYS_getrandom:
		if (BCryptGenRandom(NULL, GUEST(PUCHAR, a), (ULONG)(uint32_t)b, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
			return -GUEST_EIO;
		return (uint32_t)b;

	case GUEST_SYS_rt_sigaction:
	case GUEST_SYS_rt_sigprocmask:
	case GUEST_SYS_sigaltstack:
	case GUEST_SYS_membarrier:
	case GUEST_SYS_fchmod:
	case GUEST_SYS_fchmodat:
	case GUEST_SYS_utimensat:
	case GUEST_SYS_umask:
	case GUEST_SYS_flock:
		return 0;

	case GUEST_SYS_ioctl:
		return -GUEST_ENOTTY;

	default:
		return -GUEST_ENOSYS;
	}
}
