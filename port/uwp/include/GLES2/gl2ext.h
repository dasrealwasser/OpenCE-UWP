/*
GL2EXT.H

OpenGL ES extension declarations for the UWP guest and host builds.
*/

#ifndef __HALO_UWP_GLES2_GL2EXT_H
#define __HALO_UWP_GLES2_GL2EXT_H

#include <GLES3/gl32.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3

GL_APICALL void GL_APIENTRY glDrawElementsBaseVertexEXT (GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
GL_APICALL void GL_APIENTRY glDrawElementsBaseVertexOES (GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);

#ifdef __cplusplus
}
#endif

#endif /* __HALO_UWP_GLES2_GL2EXT_H */
