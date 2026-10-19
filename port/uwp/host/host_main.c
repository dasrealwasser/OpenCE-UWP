/*
HOST_MAIN.C

Entry point of the UWP / Xbox Dev Mode port (halo_uwp.exe).

Reserves the low-4GB Xbox virtual memory window (0x80000000..0x88000000) and
guest image region (0x88000000..0x8c000000) at startup, resolves the UWP
LocalState and package directories, offers an Xbox-controller-friendly
first-run Game Data Setup & XISO Extractor screen if maps/ui.map is not yet
present, loads halo_guest.elf, starts the background GitHub Releases
auto-updater, and enters the guest's main() on a low-4GB stack.
*/

#define SDL_MAIN_HANDLED
#include "host.h"
#include "posix.h"
#include "tomlc17.h"
#include "xiso.h"

#include <windows.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- logging and termination */

static char log_file_path[1024];
static CRITICAL_SECTION log_lock;
static LONG log_lock_initialized = 0;

static void ensure_log_lock(void)
{
	if (InterlockedCompareExchange(&log_lock_initialized, 1, 0) == 0)
		InitializeCriticalSection(&log_lock);
	else
	{
		while (log_lock_initialized < 2)
			SwitchToThread();
		return;
	}
	InterlockedExchange(&log_lock_initialized, 2);
}

void host_log(int priority, const char *text)
{
	char line[2048];

	(void)priority;
	snprintf(line, sizeof(line), "[halo-uwp] %s\n", text ? text : "");
	OutputDebugStringA(line);
	fputs(line, stderr);
	if (log_file_path[0])
	{
		FILE *f;
		ensure_log_lock();
		EnterCriticalSection(&log_lock);
		f = fopen(log_file_path, "ab");
		if (f)
		{
			fputs(line, f);
			fclose(f);
		}
		LeaveCriticalSection(&log_lock);
	}
}

void host_logf(int priority, const char *format, ...)
{
	char message[1536];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(priority, message);
}

void host_fatal(const char *format, ...)
{
	char message[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	host_log(HOST_LOG_ERROR, message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo (OpenCE-UWP)", message, NULL);
	TerminateProcess(GetCurrentProcess(), 1);
	for (;;)
		;
}

void host_abort(const char *reason)
{
	host_fatal("guest abort: %s", reason ? reason : "unknown");
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	TerminateProcess(GetCurrentProcess(), (UINT)code);
	for (;;)
		;
}

static __thread int thread_host_errno;

int host_errno(void)
{
	return thread_host_errno ? thread_host_errno : errno;
}

void host_set_errno(int err)
{
	thread_host_errno = err;
	errno = err;
}

/* ---------- paths */

static char package_root[1024];
static char local_state_root[1024];
static char data_root[1024];
static char save_root[1024];
static char config_file_path[1024];

const char *host_uwp_data_root(void)
{
	return data_root;
}

const char *host_uwp_save_root(void)
{
	return save_root;
}

const char *host_uwp_config_path(void)
{
	return config_file_path;
}

void host_android_path(int which, char *buffer, uint32_t size)
{
	if (!buffer || !size)
		return;
	snprintf(buffer, size, "%s", which ? save_root : data_root);
}

/* ---------- no-op Android touch / gesture imports (UWP uses Xbox Gamepad + Keyboard/Mouse) */

void host_gesture_insets(int *insets)
{
	if (insets)
	{
		insets[0] = 0;
		insets[1] = 0;
		insets[2] = 0;
		insets[3] = 0;
	}
}

void host_touch_read(int *state)
{
	if (state)
		memset(state, 0, 7 * sizeof(int));
}

void host_touch_look_read(float *delta)
{
	if (delta)
		memset(delta, 0, 4 * sizeof(float));
}

void host_touch_rumble(unsigned int low, unsigned int high)
{
	(void)low;
	(void)high;
}

void host_touch_scene(int scene)
{
	(void)scene;
}

void host_touch_bindings(const int *controls)
{
	(void)controls;
}

/* ---------- POSIX process & desktop stubs for the UWP host (port/linux/src/posix.h) */

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

posix_ulong posix_process_id(void)
{
	return (posix_ulong)GetCurrentProcessId();
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	/* UWP declares the halo:// protocol in Package.appxmanifest */
	(void)scheme;
	(void)description;
	return 0;
}

int posix_user_secret(unsigned char *secret, int size)
{
	(void)secret;
	(void)size;
	return 0;
}

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}

/* ---------- directory & file helpers */

static void trim_trailing_slashes(char *path)
{
	size_t len = strlen(path);
	while (len > 1 && (path[len - 1] == '\\' || path[len - 1] == '/'))
	{
		if (len == 3 && path[1] == ':')
			break;
		path[--len] = 0;
	}
}

static void normalize_forward_slashes(char *path)
{
	char *p;
	for (p = path; *p; p++)
	{
		if (*p == '\\')
			*p = '/';
	}
}

static int file_exists_utf8(const char *path)
{
	wchar_t wide[MAX_PATH * 2];
	DWORD attr;

	if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 2))
		return 0;
	attr = GetFileAttributesW(wide);
	return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int directory_has_maps(const char *root)
{
	char probe[1200];

	if (!root || !root[0])
		return 0;
	snprintf(probe, sizeof(probe), "%s/maps/ui.map", root);
	if (file_exists_utf8(probe))
		return 1;
	snprintf(probe, sizeof(probe), "%s\\maps\\ui.map", root);
	return file_exists_utf8(probe);
}

static void init_uwp_paths(void)
{
	char *pref = SDL_GetPrefPath("OpenCE", "Halo");
	const char *base = SDL_GetBasePath();

	if (base && base[0])
	{
		snprintf(package_root, sizeof(package_root), "%s", base);
		trim_trailing_slashes(package_root);
	}
	else
	{
		wchar_t module_path[MAX_PATH * 2];
		DWORD len = GetModuleFileNameW(NULL, module_path, MAX_PATH * 2);
		if (len > 0 && len < MAX_PATH * 2)
		{
			wchar_t *slash = wcsrchr(module_path, L'\\');
			if (slash)
				*slash = 0;
			WideCharToMultiByte(CP_UTF8, 0, module_path, -1, package_root, sizeof(package_root), NULL, NULL);
		}
		else
		{
			strcpy(package_root, ".");
		}
	}

	if (pref && pref[0])
	{
		snprintf(local_state_root, sizeof(local_state_root), "%s", pref);
		SDL_free(pref);
		trim_trailing_slashes(local_state_root);
	}
	else
	{
		const char *local_appdata = getenv("LOCALAPPDATA");
		if (local_appdata && local_appdata[0])
			snprintf(local_state_root, sizeof(local_state_root), "%s", local_appdata);
		else
			snprintf(local_state_root, sizeof(local_state_root), "%s", package_root);
		trim_trailing_slashes(local_state_root);
	}

	CreateDirectoryA(local_state_root, NULL);
	snprintf(log_file_path, sizeof(log_file_path), "%s/halo_uwp_host.log", local_state_root);

	snprintf(data_root, sizeof(data_root), "%s", local_state_root);
	/* Check if a saved data_path.txt points to an external drive or custom folder */
	{
		char saved_ptr_file[1200];
		size_t saved_len = 0;
		char *saved;

		snprintf(saved_ptr_file, sizeof(saved_ptr_file), "%s/data_path.txt", local_state_root);
		saved = (char *)SDL_LoadFile(saved_ptr_file, &saved_len);
		if (saved)
		{
			while (saved_len > 0 && (saved[saved_len - 1] == '\r' || saved[saved_len - 1] == '\n' || saved[saved_len - 1] == ' '))
				saved[--saved_len] = 0;
			if (saved_len > 0 && directory_has_maps(saved))
				snprintf(data_root, sizeof(data_root), "%s", saved);
			SDL_free(saved);
		}
	}
	if (!directory_has_maps(data_root) && directory_has_maps(package_root))
		snprintf(data_root, sizeof(data_root), "%s", package_root);

	normalize_forward_slashes(data_root);
	snprintf(save_root, sizeof(save_root), "%s/save", local_state_root);
	CreateDirectoryA(save_root, NULL);
	normalize_forward_slashes(save_root);
	snprintf(config_file_path, sizeof(config_file_path), "%s/config.toml", data_root);
}

/* ---------- First-run Gamepad & Keyboard Game Data Setup / XISO Extractor UI */

enum candidate_kind
{
	CANDIDATE_MAPS_DIR = 1,
	CANDIDATE_XISO_FILE = 2,
};

struct data_candidate
{
	int kind;
	char path[1024];
	char label[256];
};

#define MAX_CANDIDATES 32

static int has_iso_extension(const char *name)
{
	size_t len = strlen(name);
	if (len > 4 && _stricmp(name + len - 4, ".iso") == 0)
		return 1;
	if (len > 5 && _stricmp(name + len - 5, ".xiso") == 0)
		return 1;
	return 0;
}

static void add_candidate(struct data_candidate *list, int *count, int kind, const char *path, const char *label)
{
	int i;
	if (*count >= MAX_CANDIDATES)
		return;
	for (i = 0; i < *count; i++)
	{
		if (_stricmp(list[i].path, path) == 0)
			return;
	}
	list[*count].kind = kind;
	snprintf(list[*count].path, sizeof(list[*count].path), "%s", path);
	snprintf(list[*count].label, sizeof(list[*count].label), "%s", label);
	(*count)++;
}

static void scan_directory_for_candidates(const char *dir, int depth, struct data_candidate *list, int *count)
{
	char pattern[1200];
	WIN32_FIND_DATAA data;
	HANDLE find;

	if (depth < 0 || *count >= MAX_CANDIDATES)
		return;

	if (directory_has_maps(dir))
	{
		char label[256];
		snprintf(label, sizeof(label), "[Maps Folder] %s/maps", dir);
		add_candidate(list, count, CANDIDATE_MAPS_DIR, dir, label);
	}

	snprintf(pattern, sizeof(pattern), "%s\\*", dir);
	find = FindFirstFileA(pattern, &data);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char child[1200];
		if (!strcmp(data.cFileName, ".") || !strcmp(data.cFileName, ".."))
			continue;
		snprintf(child, sizeof(child), "%s\\%s", dir, data.cFileName);
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (depth > 0 && _stricmp(data.cFileName, "save") != 0 &&
				_stricmp(data.cFileName, "System Volume Information") != 0 &&
				data.cFileName[0] != '$' && data.cFileName[0] != '.')
			{
				scan_directory_for_candidates(child, depth - 1, list, count);
			}
		}
		else if (has_iso_extension(data.cFileName))
		{
			unsigned long long size_mb = (((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow) >> 20;
			char label[256];
			snprintf(label, sizeof(label), "[Xbox Disc ISO - %llu MB] %s", size_mb, child);
			add_candidate(list, count, CANDIDATE_XISO_FILE, child, label);
		}
	} while (FindNextFileA(find, &data) && *count < MAX_CANDIDATES);
	FindClose(find);
}

static void scan_all_sources(struct data_candidate *list, int *count)
{
	char drive_letter;

	*count = 0;
	scan_directory_for_candidates(local_state_root, 2, list, count);
	if (strcmp(package_root, local_state_root) != 0)
		scan_directory_for_candidates(package_root, 1, list, count);

	for (drive_letter = 'D'; drive_letter <= 'Z'; drive_letter++)
	{
		char root[8];
		UINT drive_type;

		snprintf(root, sizeof(root), "%c:", drive_letter);
		drive_type = GetDriveTypeA(root);
		if (drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_FIXED)
			scan_directory_for_candidates(root, 2, list, count);
	}
}

struct uwp_extraction_state
{
	SDL_Mutex *lock;
	char image[1024];
	char destination[1024];
	char file[256];
	unsigned long long done, total;
	int finished, succeeded;
	char error[512];
};

static void uwp_extraction_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	struct uwp_extraction_state *st = context;

	SDL_LockMutex(st->lock);
	snprintf(st->file, sizeof(st->file), "%s", file ? file : "");
	st->done = done;
	st->total = total;
	SDL_UnlockMutex(st->lock);
}

static int SDLCALL uwp_extraction_thread(void *context)
{
	struct uwp_extraction_state *st = context;
	int ok = xiso_extract_maps(st->image, st->destination, uwp_extraction_progress, st, st->error, sizeof(st->error));

	SDL_LockMutex(st->lock);
	st->succeeded = ok != 0;
	st->finished = 1;
	SDL_UnlockMutex(st->lock);
	return 0;
}

static int run_extraction_with_ui(SDL_Renderer *renderer, const char *image_path, const char *dest_dir,
	char *error, size_t error_size)
{
	struct uwp_extraction_state st;
	SDL_Thread *thread;

	memset(&st, 0, sizeof(st));
	st.lock = SDL_CreateMutex();
	snprintf(st.image, sizeof(st.image), "%s", image_path);
	snprintf(st.destination, sizeof(st.destination), "%s", dest_dir);

	thread = SDL_CreateThread(uwp_extraction_thread, "xiso extract", &st);
	if (!thread)
	{
		SDL_DestroyMutex(st.lock);
		snprintf(error, error_size, "Could not start the extraction thread.");
		return 0;
	}

	for (;;)
	{
		SDL_Event event;
		int finished;
		unsigned long long done, total;
		char current_file[256];

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT)
				host_exit(0);
		}

		SDL_LockMutex(st.lock);
		finished = st.finished;
		done = st.done;
		total = st.total;
		snprintf(current_file, sizeof(current_file), "%s", st.file);
		SDL_UnlockMutex(st.lock);

		if (finished)
			break;

		if (renderer)
		{
			char line[320];
			SDL_FRect bg_bar = { 60.0f, 340.0f, 840.0f, 32.0f };
			SDL_FRect fg_bar = bg_bar;
			float fraction = total ? (float)((double)done / (double)total) : 0.0f;

			SDL_SetRenderDrawColor(renderer, 14, 18, 26, 255);
			SDL_RenderClear(renderer);

			SDL_SetRenderDrawColor(renderer, 120, 210, 120, 255);
			SDL_SetRenderScale(renderer, 2.0f, 2.0f);
			SDL_RenderDebugText(renderer, 30.0f, 40.0f, "OpenCE-UWP: Extracting Game Data...");
			SDL_SetRenderScale(renderer, 1.5f, 1.5f);
			SDL_SetRenderDrawColor(renderer, 230, 235, 240, 255);
			SDL_RenderDebugText(renderer, 40.0f, 110.0f, "Extracting maps/ from Xbox disc image:");
			SDL_RenderDebugText(renderer, 40.0f, 135.0f, image_path);
			snprintf(line, sizeof(line), "Copying: %s  (%llu / %llu MB)",
				current_file[0] ? current_file : "...", done >> 20, total >> 20);
			SDL_RenderDebugText(renderer, 40.0f, 185.0f, line);
			SDL_SetRenderScale(renderer, 1.0f, 1.0f);

			SDL_SetRenderDrawColor(renderer, 45, 52, 64, 255);
			SDL_RenderFillRect(renderer, &bg_bar);
			fg_bar.w *= fraction;
			SDL_SetRenderDrawColor(renderer, 88, 176, 96, 255);
			SDL_RenderFillRect(renderer, &fg_bar);

			SDL_RenderPresent(renderer);
		}
		SDL_Delay(16);
	}

	SDL_WaitThread(thread, NULL);
	SDL_DestroyMutex(st.lock);
	if (!st.succeeded)
		snprintf(error, error_size, "%s", st.error[0] ? st.error : "Extraction failed.");
	return st.succeeded;
}

struct file_picker_state
{
	SDL_AtomicInt done;
	char path[1024];
};

static void SDLCALL on_iso_file_picked(void *userdata, const char * const *files, int filter)
{
	struct file_picker_state *st = userdata;
	(void)filter;
	if (files && files[0])
		snprintf(st->path, sizeof(st->path), "%s", files[0]);
	SDL_SetAtomicInt(&st->done, 1);
}

static int pick_iso_with_dialog(SDL_Window *window, char *out_path, size_t out_size)
{
	static const SDL_DialogFileFilter filters[] =
	{
		{ "Xbox disc images", "iso;xiso" },
		{ "All files", "*" },
	};
	struct file_picker_state st;

	memset(&st, 0, sizeof(st));
	SDL_ShowOpenFileDialog(on_iso_file_picked, &st, window, filters, 2, NULL, false);
	while (!SDL_GetAtomicInt(&st.done))
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_EVENT_QUIT)
				host_exit(0);
		}
		SDL_Delay(30);
	}
	if (!st.path[0])
		return 0;
	snprintf(out_path, out_size, "%s", st.path);
	return 1;
}

static void save_selected_data_root(const char *selected_root)
{
	char ptr_file[1200];

	snprintf(data_root, sizeof(data_root), "%s", selected_root);
	normalize_forward_slashes(data_root);
	snprintf(config_file_path, sizeof(config_file_path), "%s/config.toml", data_root);

	snprintf(ptr_file, sizeof(ptr_file), "%s/data_path.txt", local_state_root);
	SDL_SaveFile(ptr_file, data_root, strlen(data_root));
}

static void ensure_game_data_interactive(void)
{
	struct data_candidate candidates[MAX_CANDIDATES];
	int candidate_count = 0;
	int selected = 0;
	char status_message[512] = "";
	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Gamepad *pad = NULL;

	if (directory_has_maps(data_root))
		return;

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS);
	window = SDL_CreateWindow("Halo: Combat Evolved (OpenCE-UWP) - Game Data Setup", 960, 540, SDL_WINDOW_FULLSCREEN);
	if (!window)
		window = SDL_CreateWindow("Halo: Combat Evolved (OpenCE-UWP) - Game Data Setup", 960, 540, 0);
	if (!window)
		host_fatal("Could not open setup window: %s", SDL_GetError());

	renderer = SDL_CreateRenderer(window, NULL);
	if (!renderer)
		renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);

	scan_all_sources(candidates, &candidate_count);
	if (candidate_count == 1 && candidates[0].kind == CANDIDATE_MAPS_DIR)
	{
		save_selected_data_root(candidates[0].path);
		if (renderer)
			SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(window);
		return;
	}

	for (;;)
	{
		SDL_Event event;
		int activate = 0;
		int browse_dialog = 0;
		int rescan = 0;

		if (!pad)
		{
			int pad_count = 0;
			SDL_JoystickID *pads = SDL_GetGamepads(&pad_count);
			if (pads && pad_count > 0)
				pad = SDL_OpenGamepad(pads[0]);
			SDL_free(pads);
		}

		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
			{
				host_exit(0);
			}
			else if (event.type == SDL_EVENT_GAMEPAD_ADDED && !pad)
			{
				pad = SDL_OpenGamepad(event.gdevice.which);
			}
			else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
			{
				if (event.key.scancode == SDL_SCANCODE_UP && selected > 0)
					selected--;
				else if (event.key.scancode == SDL_SCANCODE_DOWN && selected + 1 < candidate_count)
					selected++;
				else if (event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_SPACE)
					activate = 1;
				else if (event.key.scancode == SDL_SCANCODE_F)
					browse_dialog = 1;
				else if (event.key.scancode == SDL_SCANCODE_R)
					rescan = 1;
				else if (event.key.scancode == SDL_SCANCODE_ESCAPE)
					host_exit(0);
			}
			else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
			{
				if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP && selected > 0)
					selected--;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN && selected + 1 < candidate_count)
					selected++;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) /* A button */
					activate = 1;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_WEST) /* X button */
					browse_dialog = 1;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) /* Y button */
					rescan = 1;
				else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) /* B button */
					host_exit(0);
			}
		}

		if (rescan)
		{
			scan_all_sources(candidates, &candidate_count);
			if (selected >= candidate_count)
				selected = candidate_count > 0 ? candidate_count - 1 : 0;
			snprintf(status_message, sizeof(status_message), "Rescanned LocalState and drives D:..Z: (%d found).", candidate_count);
		}

		if (browse_dialog)
		{
			char chosen_iso[1024];
			if (pick_iso_with_dialog(window, chosen_iso, sizeof(chosen_iso)))
			{
				char err[512] = "";
				if (run_extraction_with_ui(renderer, chosen_iso, local_state_root, err, sizeof(err)) &&
					directory_has_maps(local_state_root))
				{
					save_selected_data_root(local_state_root);
					break;
				}
				snprintf(status_message, sizeof(status_message), "Error: %s", err);
			}
		}

		if (activate && candidate_count > 0 && selected >= 0 && selected < candidate_count)
		{
			const struct data_candidate *c = &candidates[selected];
			if (c->kind == CANDIDATE_MAPS_DIR)
			{
				save_selected_data_root(c->path);
				break;
			}
			else if (c->kind == CANDIDATE_XISO_FILE)
			{
				char err[512] = "";
				if (run_extraction_with_ui(renderer, c->path, local_state_root, err, sizeof(err)) &&
					directory_has_maps(local_state_root))
				{
					save_selected_data_root(local_state_root);
					break;
				}
				snprintf(status_message, sizeof(status_message), "Error: %s", err);
			}
		}

		if (renderer)
		{
			int i;
			SDL_SetRenderDrawColor(renderer, 14, 18, 26, 255);
			SDL_RenderClear(renderer);

			SDL_SetRenderScale(renderer, 2.0f, 2.0f);
			SDL_SetRenderDrawColor(renderer, 120, 210, 120, 255);
			SDL_RenderDebugText(renderer, 20.0f, 16.0f, "Halo: Combat Evolved (OpenCE-UWP)");

			SDL_SetRenderScale(renderer, 1.25f, 1.25f);
			SDL_SetRenderDrawColor(renderer, 225, 230, 240, 255);
			SDL_RenderDebugText(renderer, 32.0f, 56.0f,
				"Game data (maps/ folder from PAL Xbox build 01.01.14.2342) is required.");
			SDL_RenderDebugText(renderer, 32.0f, 76.0f,
				"Copy 'maps/' or a Halo '.iso'/'.xiso' onto a USB drive (D:..Z:) or into LocalState:");

			SDL_SetRenderDrawColor(renderer, 255, 215, 110, 255);
			SDL_RenderDebugText(renderer, 32.0f, 98.0f, local_state_root);

			SDL_SetRenderDrawColor(renderer, 180, 205, 235, 255);
			SDL_RenderDebugText(renderer, 32.0f, 126.0f,
				"Controls: [A / Enter] Load or Extract Selected   [X / F] Browse .iso File   [Y / R] Rescan   [B / Esc] Quit");

			if (candidate_count == 0)
			{
				SDL_SetRenderDrawColor(renderer, 240, 150, 150, 255);
				SDL_RenderDebugText(renderer, 32.0f, 175.0f,
					"No 'maps/' folder or '.iso'/'.xiso' disc image detected yet.");
				SDL_RenderDebugText(renderer, 32.0f, 198.0f,
					"Insert a USB drive with your Halo ISO or maps/ folder and press [Y] (or [R]) to rescan,");
				SDL_RenderDebugText(renderer, 32.0f, 221.0f,
					"or press [X] (or [F]) to pick an .iso/.xiso file.");
			}
			else
			{
				for (i = 0; i < candidate_count && i < 10; i++)
				{
					float y = 165.0f + (float)i * 22.0f;
					if (i == selected)
					{
						SDL_FRect row = { 28.0f, y - 2.0f, 710.0f, 18.0f };
						SDL_SetRenderDrawColor(renderer, 42, 92, 58, 255);
						SDL_RenderFillRect(renderer, &row);
						SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
					}
					else
					{
						SDL_SetRenderDrawColor(renderer, 195, 205, 215, 255);
					}
					SDL_RenderDebugText(renderer, 36.0f, y, candidates[i].label);
				}
			}

			if (status_message[0])
			{
				SDL_SetRenderDrawColor(renderer, 255, 190, 120, 255);
				SDL_RenderDebugText(renderer, 32.0f, 400.0f, status_message);
			}

			SDL_SetRenderScale(renderer, 1.0f, 1.0f);
			SDL_RenderPresent(renderer);
		}
		SDL_Delay(16);
	}

	if (pad)
		SDL_CloseGamepad(pad);
	if (renderer)
		SDL_DestroyRenderer(renderer);
	if (window)
		SDL_DestroyWindow(window);
}

/* ---------- guest environment & boot structure */

#define ENVIRONMENT_MAXIMUM 64

struct environment
{
	char *entries[ENVIRONMENT_MAXIMUM];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	size_t length = strlen(name);
	char *entry;
	int index;

	entry = malloc(length + strlen(value) + 2);
	if (!entry)
		return;
	sprintf(entry, "%s=%s", name, value);
	for (index = 0; index < environment->count; index++)
	{
		if (!strncmp(environment->entries[index], name, length) && environment->entries[index][length] == '=')
		{
			free(environment->entries[index]);
			environment->entries[index] = entry;
			return;
		}
	}
	if (environment->count < ENVIRONMENT_MAXIMUM)
		environment->entries[environment->count++] = entry;
	else
		free(entry);
}

static int config_boolean_or(const char *path, const char *name, int otherwise)
{
	toml_result_t result = toml_parse_file_ex(path);
	int value = otherwise;

	if (!result.ok)
		return otherwise;
	{
		toml_datum_t datum = toml_seek(result.toptab, name);
		if (datum.type == TOML_BOOLEAN)
			value = datum.u.boolean ? 1 : 0;
	}
	toml_free(result);
	return value;
}

static void time_zone(char *buffer, size_t size)
{
	TIME_ZONE_INFORMATION tzi;
	DWORD res = GetTimeZoneInformation(&tzi);
	long bias_minutes = tzi.Bias;

	if (res == TIME_ZONE_ID_DAYLIGHT)
		bias_minutes += tzi.DaylightBias;
	else if (res == TIME_ZONE_ID_STANDARD)
		bias_minutes += tzi.StandardBias;
	snprintf(buffer, size, "<L>%s%ld:%02ld",
		bias_minutes < 0 ? "-" : "",
		labs(bias_minutes) / 60,
		labs(bias_minutes) % 60);
}

static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, 3 /* PROT_READ | PROT_WRITE */);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv = (uint32_t *)(memory + sizeof(*boot));
	uint32_t *environ_list = argv + 2;
	char *strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	int index;

	if (!memory)
		host_fatal("cannot allocate the guest's environment in low-4GB memory");
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;
		if (strings + length > memory + size)
			break;
		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = 4096;
	return (uint32_t)(uintptr_t)boot;
}

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *guest_main_trampoline(void *argument)
{
	uint32_t boot = (uint32_t)(uintptr_t)argument;
	host_run_guest_main(boot);
	return NULL;
}

static int host_uwp_main(int argc, char *argv[])
{
	struct environment environment = { { 0 }, 0 };
	char zone[64];
	char path[1200];
	size_t image_size = 0;
	void *image;
	uint32_t boot;

	(void)argc;
	(void)argv;

	init_uwp_paths();
	host_logf(HOST_LOG_INFO, "OpenCE-UWP starting (package: %s, LocalState: %s)", package_root, local_state_root);

	ensure_game_data_interactive();

	environment_set(&environment, "HOME", save_root);
	environment_set(&environment, "HALO_DATA_ROOT", data_root);
	environment_set(&environment, "HALO_SAVE_ROOT", save_root);

	{
		const SDL_DisplayMode *mode;
		char width[16];

		SDL_InitSubSystem(SDL_INIT_VIDEO);
		mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
		if (mode && mode->w > 0 && mode->h > 0)
		{
			int longer = mode->w > mode->h ? mode->w : mode->h;
			int shorter = mode->w > mode->h ? mode->h : mode->w;
			snprintf(width, sizeof(width), "%d", (480 * longer / shorter) & ~1);
			environment_set(&environment, "HALO_DISPLAY_WIDTH", width);
			host_logf(HOST_LOG_INFO, "display %dx%d: rendering %sx480", mode->w, mode->h, width);
		}
		else
		{
			environment_set(&environment, "HALO_DISPLAY_WIDTH", "854");
		}
	}

	time_zone(zone, sizeof(zone));
	environment_set(&environment, "TZ", zone);

	/* Copy brokers.txt from the package to data_root so internet play works out of the box */
	{
		size_t brokers_size = 0;
		void *brokers;

		snprintf(path, sizeof(path), "%s/brokers.txt", package_root);
		brokers = SDL_LoadFile(path, &brokers_size);
		if (!brokers)
			brokers = SDL_LoadFile("brokers.txt", &brokers_size);
		if (brokers)
		{
			snprintf(path, sizeof(path), "%s/brokers.txt", data_root);
			SDL_SaveFile(path, brokers, brokers_size);
			SDL_free(brokers);
		}
	}

	/* Load halo_guest.elf from the package root */
	snprintf(path, sizeof(path), "%s/halo_guest.elf", package_root);
	image = SDL_LoadFile(path, &image_size);
	if (!image)
		image = SDL_LoadFile("halo_guest.elf", &image_size);
	if (!image)
		host_fatal("Cannot read halo_guest.elf from package (%s): %s", path, SDL_GetError());

	if (host_load_image(image, image_size) != 0)
	{
		FILE *report;
		snprintf(path, sizeof(path), "%s/memory_map.txt", local_state_root);
		report = fopen(path, "w");
		host_memory_report_low_mappings(report);
		if (report)
			fclose(report);
		host_fatal("Could not map the guest image at 0x80000000..0x8c000000.\n"
			"See %s/halo_uwp_host.log and memory_map.txt for details.", local_state_root);
	}
	SDL_free(image);

	if (!config_boolean_or(config_file_path, "debug.memory_watch", 1))
	{
		host_memory_watch_use_hashes();
		host_logf(HOST_LOG_INFO, "write tracking: page hashes (debug.memory_watch = false)");
	}
	else
	{
		host_logf(HOST_LOG_INFO, "write tracking: page protection (VEH)");
	}

	host_updater_start(local_state_root);

	boot = make_boot(&environment);
	host_logf(HOST_LOG_INFO, "entering guest main (data=%s, saves=%s)", data_root, save_root);
	host_run_on_guest_stack(guest_main_trampoline, (void *)(uintptr_t)boot, MAIN_STACK_SIZE);
	return 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance, LPSTR command_line, int show_cmd)
{
	(void)instance;
	(void)prev_instance;
	(void)command_line;
	(void)show_cmd;

	host_memory_reserve_early();
	return SDL_RunApp(0, NULL, host_uwp_main, NULL);
}
