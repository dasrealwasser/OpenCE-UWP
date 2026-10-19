/*
HOST_UPDATER.C

In-app GitHub Releases auto-updater for the UWP / Xbox Dev Mode port.

When a release build made by GitHub Actions starts (HALO_BUILD_NUMBER > 0)
and update.auto in config.toml is true (the default), a background thread
queries GitHub's latest release for HALO_UPDATE_REPOSITORY over HTTPS using
WinHTTP. If a newer build-<number> tag is published, host_updater_poll_ui()
asks the player whether to download the new .msix package into
<LocalState>\updates\, showing progress and instructions for installing it
on Windows or via the Xbox Dev Mode Device Portal.
*/

#include "host.h"
#include "tomlc17.h"

#include <windows.h>
#include <winhttp.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HALO_BUILD_NUMBER
#define HALO_BUILD_NUMBER 0
#endif

#ifndef HALO_BUILD_FLAVOR
#define HALO_BUILD_FLAVOR "release"
#endif

#ifndef HALO_UPDATE_REPOSITORY
#define HALO_UPDATE_REPOSITORY "dasrealwasser/OpenCE-UWP"
#endif

#define UPDATE_ASSET "halo-uwp-" HALO_BUILD_FLAVOR ".msix"
#define UPDATE_USER_AGENT L"OpenCE-UWP-Updater/1.0"
#define TIMEOUT_MILLISECONDS 20000

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

enum
{
	_updater_idle = 0,
	_updater_checking,
	_updater_available,
	_updater_handled,
};

static SDL_AtomicInt updater_state;
static long updater_latest_build;
static char updater_local_state[1024];

typedef void (*download_progress_proc)(void *context, unsigned long long received, unsigned long long total);

static int wide_from_utf8(const char *text, wchar_t *wide, int size)
{
	return MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, size) > 0;
}

static int https_download_to_file(const char *url, const char *path, download_progress_proc progress, void *context,
	char *error, size_t error_size)
{
	wchar_t wide_url[2048], wide_path[MAX_PATH * 2], host[256], url_path[2048];
	URL_COMPONENTS components;
	HINTERNET session = NULL, connection = NULL, request = NULL;
	HANDLE file = INVALID_HANDLE_VALUE;
	DWORD status = 0, status_size = sizeof(status);
	DWORD length = 0, length_size = sizeof(length);
	DWORD protocols;
	unsigned long long received = 0;
	int succeeded = 0;

	if (!wide_from_utf8(url, wide_url, 2048) || !wide_from_utf8(path, wide_path, MAX_PATH * 2))
	{
		snprintf(error, error_size, "bad address or path");
		return 0;
	}
	memset(&components, 0, sizeof(components));
	components.dwStructSize = sizeof(components);
	components.lpszHostName = host;
	components.dwHostNameLength = 256;
	components.lpszUrlPath = url_path;
	components.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wide_url, 0, 0, &components) || components.nScheme != INTERNET_SCHEME_HTTPS)
	{
		snprintf(error, error_size, "not an https:// address: %s", url);
		return 0;
	}
	session = WinHttpOpen(UPDATE_USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session)
	{
		snprintf(error, error_size, "could not start WinHTTP (error %lu)", (unsigned long)GetLastError());
		goto done;
	}
	protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
	if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
	{
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
	}
	WinHttpSetTimeouts(session, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS, TIMEOUT_MILLISECONDS,
		TIMEOUT_MILLISECONDS);
	connection = WinHttpConnect(session, host, components.nPort, 0);
	if (connection)
	{
		request = WinHttpOpenRequest(connection, L"GET", url_path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	}
	if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
		!WinHttpReceiveResponse(request, NULL))
	{
		snprintf(error, error_size, "could not reach GitHub (error %lu)", (unsigned long)GetLastError());
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX) || status != 200)
	{
		snprintf(error, error_size, "server returned HTTP %lu", (unsigned long)status);
		goto done;
	}
	if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size, WINHTTP_NO_HEADER_INDEX))
	{
		length = 0;
	}
	file = CreateFileW(wide_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		snprintf(error, error_size, "could not create %s (error %lu)", path, (unsigned long)GetLastError());
		goto done;
	}
	for (;;)
	{
		char buffer[16384];
		DWORD count = 0, written = 0;

		if (!WinHttpReadData(request, buffer, sizeof(buffer), &count))
		{
			snprintf(error, error_size, "download interrupted (error %lu)", (unsigned long)GetLastError());
			goto done;
		}
		if (!count)
			break;
		if (!WriteFile(file, buffer, count, &written, NULL) || written != count)
		{
			snprintf(error, error_size, "could not write download (error %lu)", (unsigned long)GetLastError());
			goto done;
		}
		received += count;
		if (progress)
			progress(context, received, length);
	}
	if (length && received != length)
	{
		snprintf(error, error_size, "download truncated (%llu of %lu bytes)", received, (unsigned long)length);
		goto done;
	}
	succeeded = 1;

done:
	if (file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(file);
		if (!succeeded)
			DeleteFileW(wide_path);
	}
	if (request)
		WinHttpCloseHandle(request);
	if (connection)
		WinHttpCloseHandle(connection);
	if (session)
		WinHttpCloseHandle(session);
	return succeeded;
}

static int config_auto_update_enabled(const char *config_path)
{
	toml_result_t result;
	int enabled = 1;

	if (!config_path || !config_path[0])
		return 1;
	result = toml_parse_file_ex(config_path);
	if (!result.ok)
		return 1;
	{
		toml_datum_t datum = toml_seek(result.toptab, "update.auto");
		if (datum.type == TOML_BOOLEAN)
			enabled = datum.u.boolean ? 1 : 0;
	}
	toml_free(result);
	return enabled;
}

static void disable_auto_update_in_config(const char *config_path)
{
	size_t size = 0;
	char *text;
	char *section;

	if (!config_path || !config_path[0])
		return;
	text = (char *)SDL_LoadFile(config_path, &size);
	if (!text)
		return;
	section = strstr(text, "[update]");
	if (section)
	{
		char *key = strstr(section, "auto = true");
		if (key)
		{
			size_t prefix_len = (size_t)(key - text);
			size_t rest_len = size - prefix_len - strlen("auto = true");
			char *updated = malloc(size + 8);
			if (updated)
			{
				memcpy(updated, text, prefix_len);
				memcpy(updated + prefix_len, "auto = false", strlen("auto = false"));
				memcpy(updated + prefix_len + strlen("auto = false"), key + strlen("auto = true"), rest_len);
				SDL_SaveFile(config_path, updated, prefix_len + strlen("auto = false") + rest_len);
				free(updated);
			}
		}
	}
	SDL_free(text);
}

static long query_latest_release_build(void)
{
	char json_path[1200];
	char error[512] = "";
	size_t size = 0;
	char *text;
	const char *tag;
	long build = 0;

	snprintf(json_path, sizeof(json_path), "%s\\update-check.json", updater_local_state);
	if (!https_download_to_file("https://api.github.com/repos/" HALO_UPDATE_REPOSITORY "/releases/latest",
		json_path, NULL, NULL, error, sizeof(error)))
	{
		host_logf(HOST_LOG_INFO, "updater: could not check for updates: %s", error);
		return 0;
	}
	text = (char *)SDL_LoadFile(json_path, &size);
	DeleteFileA(json_path);
	if (!text)
		return 0;
	tag = strstr(text, "\"tag_name\"");
	if (tag)
	{
		tag = strchr(tag + 10, '"');
		if (tag && !strncmp(tag, "\"build-", 7))
			build = strtol(tag + 7, NULL, 10);
	}
	SDL_free(text);
	return build;
}

static int SDLCALL updater_check_thread(void *context)
{
	long latest = query_latest_release_build();

	(void)context;
	if (latest > HALO_BUILD_NUMBER)
	{
		host_logf(HOST_LOG_INFO, "updater: build %ld is available (current is %d)", latest, HALO_BUILD_NUMBER);
		updater_latest_build = latest;
		SDL_SetAtomicInt(&updater_state, _updater_available);
	}
	else
	{
		if (latest)
			host_logf(HOST_LOG_INFO, "updater: current build %d is up to date", HALO_BUILD_NUMBER);
		SDL_SetAtomicInt(&updater_state, _updater_handled);
	}
	return 0;
}

struct uwp_download_state
{
	SDL_Mutex *lock;
	char url[512];
	char destination[1200];
	unsigned long long received, total;
	int finished, succeeded;
	char error[512];
};

static void uwp_download_progress(void *context, unsigned long long received, unsigned long long total)
{
	struct uwp_download_state *state = context;

	SDL_LockMutex(state->lock);
	state->received = received;
	state->total = total;
	SDL_UnlockMutex(state->lock);
}

static int SDLCALL uwp_download_thread(void *context)
{
	struct uwp_download_state *state = context;
	char error[512] = "";
	int succeeded = https_download_to_file(state->url, state->destination, uwp_download_progress, state,
		error, sizeof(error));

	SDL_LockMutex(state->lock);
	state->succeeded = succeeded;
	snprintf(state->error, sizeof(state->error), "%s", error);
	state->finished = 1;
	SDL_UnlockMutex(state->lock);
	return 0;
}

static void download_and_offer_msix(void)
{
	struct uwp_download_state state;
	char updates_dir[1100];
	SDL_Thread *thread;

	snprintf(updates_dir, sizeof(updates_dir), "%s\\updates", updater_local_state);
	CreateDirectoryA(updates_dir, NULL);

	memset(&state, 0, sizeof(state));
	state.lock = SDL_CreateMutex();
	snprintf(state.url, sizeof(state.url),
		"https://github.com/" HALO_UPDATE_REPOSITORY "/releases/download/build-%ld/" UPDATE_ASSET,
		updater_latest_build);
	snprintf(state.destination, sizeof(state.destination),
		"%s\\halo-uwp-build-%ld.msix", updates_dir, updater_latest_build);

	thread = SDL_CreateThread(uwp_download_thread, "uwp update download", &state);
	if (!thread)
	{
		SDL_DestroyMutex(state.lock);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo Update", "Could not start the update download.", NULL);
		return;
	}

	for (;;)
	{
		SDL_Event event;
		int finished;

		while (SDL_PollEvent(&event))
		{
		}
		SDL_LockMutex(state.lock);
		finished = state.finished;
		SDL_UnlockMutex(state.lock);
		if (finished)
			break;
		SDL_Delay(25);
	}
	SDL_WaitThread(thread, NULL);
	SDL_DestroyMutex(state.lock);

	if (state.succeeded)
	{
		char message[1400];
		char file_url[1300];

		snprintf(file_url, sizeof(file_url), "file:///%s", state.destination);
		{
			char *p;
			for (p = file_url; *p; p++)
			{
				if (*p == '\\')
					*p = '/';
			}
		}
		SDL_OpenURL(file_url);
		snprintf(message, sizeof(message),
			"Update build-%ld has been downloaded to:\n%s\n\n"
			"- On Windows 10/11: Open the .msix file to install the update.\n"
			"- On Xbox Dev Mode: Open Xbox Device Portal in your browser, download or select the new .msix from "
			"File Explorer > LocalAppData > OpenCE-UWP > LocalState > updates (or from GitHub Releases), and update the package.",
			updater_latest_build, state.destination);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Halo: Update Downloaded", message, NULL);
	}
	else
	{
		char message[800];

		snprintf(message, sizeof(message), "The update download failed:\n\n%s", state.error);
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Halo: Update Failed", message, NULL);
	}
}

void host_updater_start(const char *local_state_dir)
{
	SDL_Thread *thread;

	if (!local_state_dir || !local_state_dir[0])
		return;
	snprintf(updater_local_state, sizeof(updater_local_state), "%s", local_state_dir);
	if (HALO_BUILD_NUMBER <= 0 || !config_auto_update_enabled(host_uwp_config_path()))
		return;
	SDL_SetAtomicInt(&updater_state, _updater_checking);
	thread = SDL_CreateThread(updater_check_thread, "uwp update check", NULL);
	if (thread)
		SDL_DetachThread(thread);
	else
		SDL_SetAtomicInt(&updater_state, _updater_handled);
}

void host_updater_poll_ui(void)
{
	static const SDL_MessageBoxButtonData buttons[] =
	{
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
		{ 0, 2, "Do not ask again" },
	};
	char message[512];
	int answer = 0;

	if (SDL_GetAtomicInt(&updater_state) != _updater_available)
		return;
	SDL_SetAtomicInt(&updater_state, _updater_handled);

	snprintf(message, sizeof(message),
		"A new version of OpenCE-UWP is available (build %ld; this is build %d).\n\n"
		"Would you like to download the new .msix package now?",
		updater_latest_build, HALO_BUILD_NUMBER);
	{
		SDL_MessageBoxData dialog = {
			SDL_MESSAGEBOX_INFORMATION,
			NULL,
			"Halo: New Version Available",
			message,
			3,
			buttons,
			NULL
		};
		if (!SDL_ShowMessageBox(&dialog, &answer))
			answer = 0;
	}
	if (answer == 1)
	{
		download_and_offer_msix();
	}
	else if (answer == 2)
	{
		disable_auto_update_in_config(host_uwp_config_path());
	}
}
