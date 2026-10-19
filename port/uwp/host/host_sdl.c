/*
HOST_SDL.C

SDL3 services for the UWP guest (guest/runtime/guest_sdl.c).

Mirrors port/android/host/host_sdl.c, configuring SDL3-uwp to use Mesa's
EGL + OpenGL ES 3.2 implementation (libEGL.dll + libGLESv2.dll ->
libgallium_wgl.dll on Direct3D 12) and checking the UWP auto-updater prompt
between frames.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAXIMUM_HANDLES 64

static SDL_Window *windows[MAXIMUM_HANDLES];
static SDL_GLContext contexts[MAXIMUM_HANDLES];
static SDL_Gamepad *gamepads[MAXIMUM_HANDLES];

/* ---------- guest SDL_Event layout (32-bit ILP32) */

union guest_sdl_event
{
	uint32_t type;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t window_id;
		int32_t data1, data2;
	} window;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t window_id, which;
		int32_t scancode;
		uint32_t key;
		uint16_t mod, raw;
		uint8_t down, repeat;
	} key;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t window_id, which, state;
		float x, y, xrel, yrel;
	} motion;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t window_id, which;
		uint8_t button, down, clicks, padding;
		float x, y;
	} button;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t window_id, which;
		float x, y;
		int32_t direction;
		float mouse_x, mouse_y;
		int32_t integer_x, integer_y;
	} wheel;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint32_t which;
	} gdevice;
	struct
	{
		uint32_t type, reserved;
		uint64_t timestamp;
		uint64_t touch_id, finger_id;
		float x, y, dx, dy, pressure;
		uint32_t window_id;
	} tfinger;
	uint8_t padding[128];
};

/* ---------- initialization, hints, errors, clock */

int host_sdl_init(uint32_t flags)
{
	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, "libEGL.dll");
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, "libGLESv2.dll");
	return SDL_Init(flags) ? 1 : 0;
}

int host_sdl_set_hint(const char *name, const char *value)
{
	return SDL_SetHint(name, value) ? 1 : 0;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	const char *error = SDL_GetError();

	if (!size)
		return;
	strncpy(buffer, error ? error : "", size - 1);
	buffer[size - 1] = 0;
}

long long host_sdl_ticks(void)
{
	return (long long)SDL_GetTicks();
}

long long host_sdl_thread_id(void)
{
	return (long long)SDL_GetCurrentThreadID();
}

/* ---------- window & OpenGL ES context */

uint32_t host_sdl_create_window(const char *title, int width, int height, long long flags)
{
	uint32_t index;

	SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
	SDL_SetHint(SDL_HINT_EGL_LIBRARY, "libEGL.dll");
	SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, "libGLESv2.dll");

	for (index = 1; index < MAXIMUM_HANDLES; index++)
	{
		if (!windows[index])
		{
			/* If the UWP setup screen already created a window, reuse or recreate */
			windows[index] = SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags | SDL_WINDOW_FULLSCREEN);
			if (!windows[index])
				windows[index] = SDL_CreateWindow(title, width, height, (SDL_WindowFlags)flags);
			return windows[index] ? index : 0;
		}
	}
	return 0;
}

void host_sdl_window_size_in_pixels(uint32_t window, int *width, int *height)
{
	*width = 0;
	*height = 0;
	if (window && window < MAXIMUM_HANDLES && windows[window])
		SDL_GetWindowSizeInPixels(windows[window], width, height);
}

int host_sdl_set_relative_mouse(uint32_t window, int enabled)
{
	if (!window || window >= MAXIMUM_HANDLES || !windows[window])
		return 0;
	return SDL_SetWindowRelativeMouseMode(windows[window], enabled != 0) ? 1 : 0;
}

int host_sdl_gl_set_attribute(int attribute, int value)
{
	return SDL_GL_SetAttribute((SDL_GLAttr)attribute, value) ? 1 : 0;
}

uint32_t host_sdl_gl_create_context(uint32_t window)
{
	uint32_t index;

	if (!window || window >= MAXIMUM_HANDLES || !windows[window])
		return 0;
	for (index = 1; index < MAXIMUM_HANDLES; index++)
	{
		if (!contexts[index])
		{
			contexts[index] = SDL_GL_CreateContext(windows[window]);
			if (!contexts[index])
			{
				/* Fall back to GLES 3.0 if 3.2 context creation was refused */
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
				SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
				contexts[index] = SDL_GL_CreateContext(windows[window]);
			}
			return contexts[index] ? index : 0;
		}
	}
	return 0;
}

int host_sdl_gl_make_current(uint32_t window, uint32_t context)
{
	SDL_Window *w = (window && window < MAXIMUM_HANDLES) ? windows[window] : NULL;
	SDL_GLContext c = (context && context < MAXIMUM_HANDLES) ? contexts[context] : NULL;

	return SDL_GL_MakeCurrent(w, c) ? 1 : 0;
}

int host_sdl_gl_set_swap_interval(int interval)
{
	return SDL_GL_SetSwapInterval(interval) ? 1 : 0;
}

int host_sdl_gl_swap_window(uint32_t window)
{
	int ok;
	if (!window || window >= MAXIMUM_HANDLES || !windows[window])
		return 0;
	ok = SDL_GL_SwapWindow(windows[window]) ? 1 : 0;
	host_updater_poll_ui();
	return ok;
}

/* ---------- events */

int host_sdl_poll_event(void *out)
{
	union guest_sdl_event *guest = out;
	SDL_Event host;

	if (!SDL_PollEvent(&host))
		return 0;
	memset(guest, 0, sizeof(*guest));
	guest->type = host.type;
	if (host.type >= SDL_EVENT_WINDOW_FIRST && host.type <= SDL_EVENT_WINDOW_LAST)
	{
		guest->window.timestamp = host.window.timestamp;
		guest->window.window_id = host.window.windowID;
		guest->window.data1 = host.window.data1;
		guest->window.data2 = host.window.data2;
		return 1;
	}
	switch (host.type)
	{
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
		guest->key.timestamp = host.key.timestamp;
		guest->key.window_id = host.key.windowID;
		guest->key.which = host.key.which;
		guest->key.scancode = (int32_t)host.key.scancode;
		guest->key.key = host.key.key;
		guest->key.mod = host.key.mod;
		guest->key.raw = host.key.raw;
		guest->key.down = host.key.down ? 1 : 0;
		guest->key.repeat = host.key.repeat ? 1 : 0;
		break;
	case SDL_EVENT_MOUSE_MOTION:
		guest->motion.timestamp = host.motion.timestamp;
		guest->motion.window_id = host.motion.windowID;
		guest->motion.which = host.motion.which;
		guest->motion.state = host.motion.state;
		guest->motion.x = host.motion.x;
		guest->motion.y = host.motion.y;
		guest->motion.xrel = host.motion.xrel;
		guest->motion.yrel = host.motion.yrel;
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		guest->button.timestamp = host.button.timestamp;
		guest->button.window_id = host.button.windowID;
		guest->button.which = host.button.which;
		guest->button.button = host.button.button;
		guest->button.down = host.button.down ? 1 : 0;
		guest->button.clicks = host.button.clicks;
		guest->button.x = host.button.x;
		guest->button.y = host.button.y;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		guest->wheel.timestamp = host.wheel.timestamp;
		guest->wheel.window_id = host.wheel.windowID;
		guest->wheel.which = host.wheel.which;
		guest->wheel.x = host.wheel.x;
		guest->wheel.y = host.wheel.y;
		guest->wheel.direction = (int32_t)host.wheel.direction;
		guest->wheel.mouse_x = host.wheel.mouse_x;
		guest->wheel.mouse_y = host.wheel.mouse_y;
		guest->wheel.integer_x = host.wheel.integer_x;
		guest->wheel.integer_y = host.wheel.integer_y;
		break;
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
	case SDL_EVENT_GAMEPAD_REMAPPED:
		guest->gdevice.timestamp = host.gdevice.timestamp;
		guest->gdevice.which = host.gdevice.which;
		break;
	case SDL_EVENT_FINGER_DOWN:
	case SDL_EVENT_FINGER_MOTION:
	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		guest->tfinger.timestamp = host.tfinger.timestamp;
		guest->tfinger.touch_id = host.tfinger.touchID;
		guest->tfinger.finger_id = host.tfinger.fingerID;
		guest->tfinger.x = host.tfinger.x;
		guest->tfinger.y = host.tfinger.y;
		guest->tfinger.dx = host.tfinger.dx;
		guest->tfinger.dy = host.tfinger.dy;
		guest->tfinger.pressure = host.tfinger.pressure;
		guest->tfinger.window_id = host.tfinger.windowID;
		break;
	default:
		break;
	}
	return 1;
}

/* ---------- clipboard, key names & message boxes */

int host_sdl_set_clipboard_text(const char *text)
{
	return SDL_SetClipboardText(text) ? 1 : 0;
}

void host_sdl_get_clipboard_text(char *buffer, uint32_t size)
{
	char *text;

	if (!size)
		return;
	buffer[0] = 0;
	text = SDL_GetClipboardText();
	if (text)
	{
		strncpy(buffer, text, size - 1);
		buffer[size - 1] = 0;
		SDL_free(text);
	}
}

void host_sdl_scancode_name(int scancode, char *buffer, uint32_t size)
{
	const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);

	if (!size)
		return;
	snprintf(buffer, size, "%s", name ? name : "");
}

int host_sdl_scancode_from_name(const char *name)
{
	return (int)SDL_GetScancodeFromName(name);
}

int host_sdl_show_toast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration;
	(void)gravity;
	(void)x;
	(void)y;
	host_logf(HOST_LOG_INFO, "toast: %s", message ? message : "");
	return 1;
}

int host_sdl_show_simple_message_box(uint32_t flags, const char *title, const char *message)
{
	return SDL_ShowSimpleMessageBox(flags, title, message, windows[1]) ? 1 : 0;
}

int host_sdl_show_message_box(uint32_t flags, const char *title, const char *message, int default_index,
	const char *button0, const char *button1, const char *button2)
{
	const char *labels[3] = { button0, button1, button2 };
	SDL_MessageBoxButtonData buttons[3];
	SDL_MessageBoxData box;
	int count = 0, index, answer = -1;

	for (index = 0; index < 3 && labels[index]; index++)
	{
		buttons[count].flags = count == default_index ?
			SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0;
		buttons[count].buttonID = count;
		buttons[count].text = labels[index];
		count++;
	}
	memset(&box, 0, sizeof(box));
	box.flags = flags;
	box.window = windows[1];
	box.title = title;
	box.message = message;
	box.numbuttons = count;
	box.buttons = buttons;
	if (!SDL_ShowMessageBox(&box, &answer))
		return -1;
	return answer;
}

/* ---------- gamepads */

static uint32_t handle_for_gamepad(SDL_Gamepad *pad)
{
	uint32_t index;

	if (!pad)
		return 0;
	for (index = 1; index < MAXIMUM_HANDLES; index++)
	{
		if (gamepads[index] == pad)
			return index;
	}
	for (index = 1; index < MAXIMUM_HANDLES; index++)
	{
		if (!gamepads[index])
		{
			gamepads[index] = pad;
			return index;
		}
	}
	return 0;
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = 0, copy;
	SDL_JoystickID *list = SDL_GetGamepads(&count);

	if (!list)
		return 0;
	copy = count < capacity ? count : capacity;
	if (copy > 0 && ids)
		memcpy(ids, list, (size_t)copy * sizeof(uint32_t));
	SDL_free(list);
	return copy;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	return handle_for_gamepad(SDL_OpenGamepad(id));
}

uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	return handle_for_gamepad(SDL_GetGamepadFromID(id));
}

int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	if (!gamepad || gamepad >= MAXIMUM_HANDLES || !gamepads[gamepad])
		return 0;
	return SDL_GetGamepadAxis(gamepads[gamepad], (SDL_GamepadAxis)axis);
}

int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	if (!gamepad || gamepad >= MAXIMUM_HANDLES || !gamepads[gamepad])
		return 0;
	return SDL_GetGamepadButton(gamepads[gamepad], (SDL_GamepadButton)button) ? 1 : 0;
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	if (!gamepad || gamepad >= MAXIMUM_HANDLES || !gamepads[gamepad])
		return SDL_GAMEPAD_TYPE_UNKNOWN;
	return (int)SDL_GetGamepadType(gamepads[gamepad]);
}

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	if (!gamepad || gamepad >= MAXIMUM_HANDLES || !gamepads[gamepad])
		return 0;
	return SDL_RumbleGamepad(gamepads[gamepad], ( Uint16)low, (Uint16)high, milliseconds) ? 1 : 0;
}

void host_sdl_close_gamepad(uint32_t gamepad)
{
	if (!gamepad || gamepad >= MAXIMUM_HANDLES || !gamepads[gamepad])
		return;
	SDL_CloseGamepad(gamepads[gamepad]);
	gamepads[gamepad] = NULL;
}

/* ---------- audio streams */

struct audio_slot
{
	SDL_AudioStream *stream;
	uint32_t guest_callback;
	uint32_t guest_userdata;
	SDL_Mutex *lock;
	SDL_Cond *requested;
	SDL_Cond *completed;
	int pending_additional;
	int pending_total;
	int has_request;
	int stop;
	int worker_started;
};

static struct audio_slot audio_slots[MAXIMUM_HANDLES];

static void *audio_worker(void *context)
{
	struct audio_slot *slot = context;
	uint32_t handle = (uint32_t)(slot - audio_slots);

	SDL_LockMutex(slot->lock);
	while (!slot->stop)
	{
		if (!slot->has_request)
		{
			SDL_WaitCondition(slot->requested, slot->lock);
			continue;
		}
		{
			int additional = slot->pending_additional;
			int total = slot->pending_total;

			SDL_UnlockMutex(slot->lock);
			host_call_guest(slot->guest_callback, slot->guest_userdata, handle,
				(uint32_t)additional, (uint32_t)total);
			SDL_LockMutex(slot->lock);
			slot->has_request = 0;
			SDL_SignalCondition(slot->completed);
		}
	}
	SDL_UnlockMutex(slot->lock);
	return NULL;
}

static void SDLCALL host_audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	struct audio_slot *slot = userdata;

	(void)stream;
	SDL_LockMutex(slot->lock);
	if (!slot->stop && slot->worker_started)
	{
		slot->pending_additional = additional;
		slot->pending_total = total;
		slot->has_request = 1;
		SDL_SignalCondition(slot->requested);
		while (slot->has_request && !slot->stop)
			SDL_WaitCondition(slot->completed, slot->lock);
	}
	SDL_UnlockMutex(slot->lock);
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	uint32_t index;

	for (index = 1; index < MAXIMUM_HANDLES; index++)
	{
		struct audio_slot *slot = &audio_slots[index];

		if (!slot->stream)
		{
			memset(slot, 0, sizeof(*slot));
			if (callback)
			{
				slot->guest_callback = callback;
				slot->guest_userdata = userdata;
				slot->lock = SDL_CreateMutex();
				slot->requested = SDL_CreateCondition();
				slot->completed = SDL_CreateCondition();
				if (!slot->lock || !slot->requested || !slot->completed)
					return 0;
				if (host_native_thread_create(audio_worker, slot, 512 * 1024) != 0)
					return 0;
				slot->worker_started = 1;
			}
			slot->stream = SDL_OpenAudioDeviceStream((SDL_AudioDeviceID)device,
				(const SDL_AudioSpec *)spec,
				callback ? host_audio_callback : NULL,
				callback ? slot : NULL);
			if (!slot->stream)
			{
				if (slot->worker_started)
				{
					SDL_LockMutex(slot->lock);
					slot->stop = 1;
					SDL_SignalCondition(slot->requested);
					SDL_UnlockMutex(slot->lock);
				}
				return 0;
			}
			return index;
		}
	}
	return 0;
}

int host_sdl_put_audio_stream_data(uint32_t stream, const void *data, int length)
{
	if (!stream || stream >= MAXIMUM_HANDLES || !audio_slots[stream].stream)
		return 0;
	return SDL_PutAudioStreamData(audio_slots[stream].stream, data, length) ? 1 : 0;
}

int host_sdl_resume_audio_stream_device(uint32_t stream)
{
	if (!stream || stream >= MAXIMUM_HANDLES || !audio_slots[stream].stream)
		return 0;
	return SDL_ResumeAudioStreamDevice(audio_slots[stream].stream) ? 1 : 0;
}

int host_sdl_get_audio_stream_data(uint32_t stream, void *data, int length)
{
	if (!stream || stream >= MAXIMUM_HANDLES || !audio_slots[stream].stream)
		return -1;
	return SDL_GetAudioStreamData(audio_slots[stream].stream, data, length);
}

int host_sdl_get_audio_stream_available(uint32_t stream)
{
	if (!stream || stream >= MAXIMUM_HANDLES || !audio_slots[stream].stream)
		return 0;
	return SDL_GetAudioStreamAvailable(audio_slots[stream].stream);
}

void host_sdl_destroy_audio_stream(uint32_t stream)
{
	struct audio_slot *slot;

	if (!stream || stream >= MAXIMUM_HANDLES || !audio_slots[stream].stream)
		return;
	slot = &audio_slots[stream];
	if (slot->worker_started)
	{
		SDL_LockMutex(slot->lock);
		slot->stop = 1;
		slot->has_request = 0;
		SDL_SignalCondition(slot->requested);
		SDL_SignalCondition(slot->completed);
		SDL_UnlockMutex(slot->lock);
	}
	SDL_DestroyAudioStream(slot->stream);
	slot->stream = NULL;
}

/* ---------- touch stubs (no touch overlay on Xbox / UWP) */

void host_gesture_insets(int *insets)
{
	if (insets)
		memset(insets, 0, 4 * sizeof(int));
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

void host_touch_rumble(uint32_t low, uint32_t high)
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
