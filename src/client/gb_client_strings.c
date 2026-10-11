/*
 * gb_client_strings.c - glGetString / glGetStringi / glShaderSource, shared
 * by both client libraries. Extension strings are filtered to the entry
 * points the library implements (gb_cl_ext_list).
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>

#include "gb_client_impl.h"

GB_API const GLubyte *glGetString(GLenum name)
{
	if (name == GL_EXTENSIONS) {
		const struct gb_extlist *l = gb_cl_ext_list();
		return l ? (const GLubyte *)l->joined : NULL;
	}
	return (const GLubyte *)gb_cl_fetch_string(GB_OPX_GetString, name,
						   0xFFFFFFFFu);
}

GB_API const GLubyte *glGetStringi(GLenum name, GLuint index)
{
	if (name == GL_EXTENSIONS) {
		const struct gb_extlist *l = gb_cl_ext_list();
		if (!l || index >= (GLuint)l->n) {
			gb_cl_gl_error(GL_INVALID_VALUE);
			return NULL;
		}
		return (const GLubyte *)l->names[index];
	}
	return (const GLubyte *)gb_cl_fetch_string(GB_OPX_GetString, name,
						   index);
}

/* ---- glShaderSource ------------------------------------------------------------------- */

/*
 * A driver validates the shader name before it touches the string array, so a
 * caller may legitimately pass garbage strings with a name that is not a
 * shader (the resulting error is the expected outcome). The strings have to be
 * read to marshal them, so remember which names glCreateShader returned and
 * send an empty source for any other name: the driver raises the error.
 */
static pthread_mutex_t shaders_lock = PTHREAD_MUTEX_INITIALIZER;
static GLuint *shaders;     /* open-addressing set, 0 = empty slot */
static size_t shaders_cap, shaders_n;

static void shader_note(GLuint name)
{
	if (!name)
		return;
	pthread_mutex_lock(&shaders_lock);
	if ((shaders_n + 1) * 2 > shaders_cap) {
		size_t ncap = shaders_cap ? shaders_cap * 2 : 256;
		GLuint *n = calloc(ncap, sizeof(GLuint));
		if (!n) {
			pthread_mutex_unlock(&shaders_lock);
			return;
		}
		for (size_t i = 0; i < shaders_cap; i++) {
			if (!shaders[i])
				continue;
			size_t j = (shaders[i] * 2654435761u) & (ncap - 1);
			while (n[j])
				j = (j + 1) & (ncap - 1);
			n[j] = shaders[i];
		}
		free(shaders);
		shaders = n;
		shaders_cap = ncap;
	}
	size_t j = (name * 2654435761u) & (shaders_cap - 1);
	while (shaders[j] && shaders[j] != name)
		j = (j + 1) & (shaders_cap - 1);
	if (!shaders[j]) {
		shaders[j] = name;
		shaders_n++;
	}
	pthread_mutex_unlock(&shaders_lock);
}

static int shader_known(GLuint name)
{
	int found = 0;

	pthread_mutex_lock(&shaders_lock);
	if (shaders_cap) {
		size_t j = (name * 2654435761u) & (shaders_cap - 1);
		while (shaders[j] && shaders[j] != name)
			j = (j + 1) & (shaders_cap - 1);
		found = shaders[j] == name;
	}
	pthread_mutex_unlock(&shaders_lock);
	return found;
}

GB_API GLuint glCreateShader(GLenum type)
{
	GLuint s = gbx_glCreateShader(type);

	shader_note(s);
	return s;
}

GB_API GLuint glCreateShaderObjectARB(GLenum type)
{
	GLuint s = gbx_glCreateShaderObjectARB(type);

	shader_note(s);
	return s;
}

GB_API void glShaderSource(GLuint shader, GLsizei count,
			   const GLchar *const *string, const GLint *length)
{
	struct gb_cl *cx = gb_cl_get();
	struct gb_out o;
	struct gb_w w;

	if (!cx)
		return;
	if (count < 0 || (count > 0 && !string) || count > 4096) {
		gb_cl_gl_error(count < 0 || !string ? GL_INVALID_VALUE
						    : GL_INVALID_OPERATION);
		return;
	}
	if (count > 0 && !shader_known(shader))
		count = 0; /* the driver rejects the name; strings stay unread */
	uint64_t total = 4ull * count;
	uint32_t *lens = malloc(((size_t)count + 1) * sizeof(uint32_t));
	if (!lens)
		return;
	for (GLsizei i = 0; i < count; i++) {
		size_t l = 0;
		if (string[i])
			l = (length && length[i] >= 0) ? (size_t)length[i]
						       : strlen(string[i]);
		lens[i] = (uint32_t)l;
		total += l;
	}
	if (total > GB_MAX_ARRAY_BYTES) {
		free(lens);
		gb_cl_gl_error(GL_OUT_OF_MEMORY);
		return;
	}
	uint32_t sz = GB_ALIGN_UP(16 + 12, 8u) + (uint32_t)total;
	if (gb_cl_msg_begin(cx, &o, &w, GB_OPX_ShaderSource, sz, 0)) {
		free(lens);
		return;
	}
	gb_w_u32(&w, shader);
	gb_w_u32(&w, (uint32_t)count);
	gb_w_u32(&w, (uint32_t)total);
	uint8_t *blob = gb_w_blob(&w, (uint32_t)total);
	memcpy(blob, lens, 4u * (uint32_t)count);
	size_t off = 4u * (size_t)count;
	for (GLsizei i = 0; i < count; i++) {
		if (lens[i])
			memcpy(blob + off, string[i], lens[i]);
		off += lens[i];
	}
	gb_cl_end(cx, &o);
	free(lens);
}

