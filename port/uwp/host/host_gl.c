/*
HOST_GL.C

OpenGL ES 3.2 helper functions for the UWP guest over mesa-uwp
(libGLESv2.dll + libEGL.dll -> libgallium_wgl.dll on Direct3D 12).
*/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "host.h"

#include <GLES3/gl32.h>
#include <SDL3/SDL.h>
#include <string.h>

void *host_gl_resolve(const char *name)
{
	static HMODULE gles_lib;
	static HMODULE egl_lib;
	static void *(GL_APIENTRY *egl_get_proc)(const char *);
	void *fn = NULL;

	if (!gles_lib)
	{
		gles_lib = LoadPackagedLibrary(L"libGLESv2.dll", 0);
		if (!gles_lib)
			gles_lib = LoadLibraryW(L"libGLESv2.dll");
	}
	if (!egl_lib)
	{
		egl_lib = LoadPackagedLibrary(L"libEGL.dll", 0);
		if (!egl_lib)
			egl_lib = LoadLibraryW(L"libEGL.dll");
		if (egl_lib)
			egl_get_proc = (void *(GL_APIENTRY *)(const char *))GetProcAddress(egl_lib, "eglGetProcAddress");
	}
	if (gles_lib)
		fn = (void *)GetProcAddress(gles_lib, name);
	if (!fn && egl_get_proc)
		fn = egl_get_proc(name);
	if (!fn)
		fn = (void *)SDL_GL_GetProcAddress(name);
	return fn;
}

typedef const GLubyte *(GL_APIENTRY *pfn_glGetString)(GLenum);
typedef const GLubyte *(GL_APIENTRY *pfn_glGetStringi)(GLenum, GLuint);
typedef void (GL_APIENTRY *pfn_glGetIntegerv)(GLenum, GLint *);
typedef void (GL_APIENTRY *pfn_glBindBuffer)(GLenum, GLuint);
typedef void *(GL_APIENTRY *pfn_glMapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef GLboolean (GL_APIENTRY *pfn_glUnmapBuffer)(GLenum);
typedef void (GL_APIENTRY *pfn_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef GLsync (GL_APIENTRY *pfn_glFenceSync)(GLenum, GLbitfield);
typedef void (GL_APIENTRY *pfn_glDeleteSync)(GLsync);
typedef GLenum (GL_APIENTRY *pfn_glClientWaitSync)(GLsync, GLbitfield, GLuint64);

static pfn_glGetString fn_glGetString;
static pfn_glGetStringi fn_glGetStringi;
static pfn_glGetIntegerv fn_glGetIntegerv;
static pfn_glBindBuffer fn_glBindBuffer;
static pfn_glMapBufferRange fn_glMapBufferRange;
static pfn_glUnmapBuffer fn_glUnmapBuffer;
static pfn_glBufferSubData fn_glBufferSubData;
static pfn_glFenceSync fn_glFenceSync;
static pfn_glDeleteSync fn_glDeleteSync;
static pfn_glClientWaitSync fn_glClientWaitSync;

static void ensure_helper_procs(void)
{
	if (!fn_glGetString)
	{
		fn_glGetString = (pfn_glGetString)host_gl_resolve("glGetString");
		fn_glGetStringi = (pfn_glGetStringi)host_gl_resolve("glGetStringi");
		fn_glGetIntegerv = (pfn_glGetIntegerv)host_gl_resolve("glGetIntegerv");
		fn_glBindBuffer = (pfn_glBindBuffer)host_gl_resolve("glBindBuffer");
		fn_glMapBufferRange = (pfn_glMapBufferRange)host_gl_resolve("glMapBufferRange");
		fn_glUnmapBuffer = (pfn_glUnmapBuffer)host_gl_resolve("glUnmapBuffer");
		fn_glBufferSubData = (pfn_glBufferSubData)host_gl_resolve("glBufferSubData");
		fn_glFenceSync = (pfn_glFenceSync)host_gl_resolve("glFenceSync");
		fn_glDeleteSync = (pfn_glDeleteSync)host_gl_resolve("glDeleteSync");
		fn_glClientWaitSync = (pfn_glClientWaitSync)host_gl_resolve("glClientWaitSync");
	}
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = NULL;

	ensure_helper_procs();
	if (index >= 0 && fn_glGetStringi)
		text = fn_glGetStringi(name, (GLuint)index);
	else if (index < 0 && fn_glGetString)
		text = fn_glGetString(name);
	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	ensure_helper_procs();
	if (!fn_glGetIntegerv || !fn_glGetStringi)
		return 0;
	fn_glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *ext = (const char *)fn_glGetStringi(GL_EXTENSIONS, (GLuint)index);
		if (ext && !strcmp(ext, name))
			return 1;
	}
	return 0;
}

void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	const void *mapping = NULL;

	ensure_helper_procs();
	if (!fn_glBindBuffer || !fn_glMapBufferRange || !fn_glUnmapBuffer)
	{
		memset(data, 0, size);
		return;
	}
	fn_glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = fn_glMapBufferRange(GL_COPY_READ_BUFFER, offset, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		fn_glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	fn_glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

#define FRAME_FENCE_SLOTS 8
static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	ensure_helper_procs();
	if (slot >= FRAME_FENCE_SLOTS || !fn_glFenceSync || !fn_glDeleteSync)
		return;
	if (frame_fences[slot])
		fn_glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = fn_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	ensure_helper_procs();
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot] || !fn_glClientWaitSync || !fn_glDeleteSync)
		return;
	fn_glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
	fn_glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping = NULL;

	ensure_helper_procs();
	if (fn_glMapBufferRange && fn_glUnmapBuffer)
		mapping = fn_glMapBufferRange(target, offset, size, GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if (!mapping)
	{
		if (fn_glBufferSubData)
			fn_glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	fn_glUnmapBuffer(target);
}
