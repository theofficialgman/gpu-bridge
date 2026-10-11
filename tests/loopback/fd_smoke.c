/*
 * fd_smoke.c - file descriptors across the bridge: a dma-buf exported from
 * one EGLImage and imported as another (the plane fds travel as tokens over
 * the control socket), and a native fence fd duplicated from one sync object
 * and used to create another. Same source linked native / bridged 64 / 32.
 *
 * Exit 77 (skipped) when the driver lacks the extensions.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...)                                                   \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("  FAIL line %d: %s  ", __LINE__, #cond);    \
			printf("" __VA_ARGS__);                            \
			printf("\n");                                      \
			g_fail++;                                          \
		}                                                          \
	} while (0)

/* pointers the bridge has not necessarily declared in the system headers */
typedef EGLBoolean (*q_fn)(EGLDisplay, EGLImageKHR, int *, int *, EGLuint64KHR *);
typedef EGLBoolean (*e_fn)(EGLDisplay, EGLImageKHR, int *, EGLint *, EGLint *);
typedef EGLint (*dup_fn)(EGLDisplay, EGLSyncKHR);

static int has(const char *list, const char *name)
{
	return list && strstr(list, name) != NULL;
}

static GLuint compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	return s;
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = EGL_NO_DISPLAY;
	EGLint maj, min;

	if (gpd && getenv("FD_PLATFORM") && !strcmp(getenv("FD_PLATFORM"), "device")) {
		/* first hardware device (NVIDIA has no surfaceless platform) */
		PFNEGLQUERYDEVICESEXTPROC qd =
			(void *)eglGetProcAddress("eglQueryDevicesEXT");
		PFNEGLQUERYDEVICESTRINGEXTPROC qs =
			(void *)eglGetProcAddress("eglQueryDeviceStringEXT");
		EGLDeviceEXT devs[8];
		EGLint nd = 0;

		if (qd && qs && qd(8, devs, &nd))
			for (int i = 0; i < nd && d == EGL_NO_DISPLAY; i++) {
				const char *de = qs(devs[i], EGL_EXTENSIONS);

				if (de && strstr(de, "EGL_MESA_device_software"))
					continue;
				d = gpd(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
			}
	} else if (gpd) {
		d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	}

	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) {
		printf("SKIP: no display\n");
		return 77;
	}
	const char *ex = eglQueryString(d, EGL_EXTENSIONS);
	q_fn query = (q_fn)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
	e_fn export = (e_fn)eglGetProcAddress("eglExportDMABUFImageMESA");
	dup_fn dupfd = (dup_fn)eglGetProcAddress("eglDupNativeFenceFDANDROID");
	PFNEGLCREATEIMAGEKHRPROC create_image =
		(void *)eglGetProcAddress("eglCreateImageKHR");
	PFNEGLCREATESYNCKHRPROC create_sync =
		(void *)eglGetProcAddress("eglCreateSyncKHR");
	PFNEGLCLIENTWAITSYNCKHRPROC wait_sync =
		(void *)eglGetProcAddress("eglClientWaitSyncKHR");
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target =
		(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

	printf("client pointer size %zu\n", sizeof(void *));
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
				     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
				     EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	eglChooseConfig(d, ca, &cfg, 1, &n);
	static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
	static const EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	EGLSurface surf = eglCreatePbufferSurface(d, cfg, pa);
	CHECK(eglMakeCurrent(d, surf, surf, ctx), "makeCurrent");

	/* ---- dma-buf export -> import ------------------------------------ */
	if (has(ex, "EGL_MESA_image_dma_buf_export") &&
	    has(ex, "EGL_EXT_image_dma_buf_import") && query && export &&
	    create_image && target) {
		unsigned char px[4 * 4 * 4];
		for (int i = 0; i < 16; i++) {
			px[i * 4 + 0] = 255;
			px[i * 4 + 1] = 128;
			px[i * 4 + 2] = 64;
			px[i * 4 + 3] = 255;
		}
		GLuint tex;
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, px);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		static const EGLint ia[] = { EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE };
		EGLImageKHR img = create_image(d, ctx, EGL_GL_TEXTURE_2D_KHR,
					       (EGLClientBuffer)(uintptr_t)tex, ia);
		CHECK(img != EGL_NO_IMAGE_KHR, "image from texture (0x%x)",
		      eglGetError());
		int fourcc = 0, planes = 0;
		EGLuint64KHR mods[4] = { 0 };
		int fds[4] = { -1, -1, -1, -1 };
		EGLint strides[4] = { 0 }, offs[4] = { 0 };
		int ok = img != EGL_NO_IMAGE_KHR &&
			 query(d, img, &fourcc, &planes, mods) &&
			 export(d, img, fds, strides, offs);
		CHECK(ok, "export (0x%x)", eglGetError());
		if (ok && planes == 1 && fds[0] >= 0) {
			printf("exported fourcc 0x%x, %d plane(s), fd %d, "
			       "stride %d\n", fourcc, planes, fds[0], strides[0]);
			EGLint at[] = {
				EGL_WIDTH, 4, EGL_HEIGHT, 4,
				EGL_LINUX_DRM_FOURCC_EXT, fourcc,
				EGL_DMA_BUF_PLANE0_FD_EXT, fds[0],
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, offs[0],
				EGL_DMA_BUF_PLANE0_PITCH_EXT, strides[0],
				EGL_NONE
			};
			EGLImageKHR imp = create_image(
				d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, at);
			CHECK(imp != EGL_NO_IMAGE_KHR, "dma-buf import (0x%x)",
			      eglGetError());
			if (imp != EGL_NO_IMAGE_KHR) {
				GLuint t2;
				glGenTextures(1, &t2);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, t2);
				target(GL_TEXTURE_2D, (GLeglImageOES)imp);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				GLuint vs = compile(GL_VERTEX_SHADER,
					"attribute vec2 p; varying vec2 uv;"
					"void main(){ gl_Position=vec4(p,0.,1.);"
					" uv=p*.5+.5; }");
				GLuint fs = compile(GL_FRAGMENT_SHADER,
					"precision mediump float; uniform sampler2D t;"
					"varying vec2 uv;"
					"void main(){ gl_FragColor=texture2D(t,uv); }");
				GLuint pr = glCreateProgram();
				glAttachShader(pr, vs);
				glAttachShader(pr, fs);
				glBindAttribLocation(pr, 0, "p");
				glLinkProgram(pr);
				glUseProgram(pr);
				glUniform1i(glGetUniformLocation(pr, "t"), 0);
				static const GLfloat quad[] = { -1, -1, 3, -1, -1, 3 };
				glViewport(0, 0, 16, 16);
				glEnableVertexAttribArray(0);
				glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
				glClearColor(0, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				unsigned char out[4];
				glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
				CHECK(abs(out[0] - 255) <= 2 && abs(out[1] - 128) <= 3 &&
				      abs(out[2] - 64) <= 3,
				      "imported texture samples %d %d %d", out[0],
				      out[1], out[2]);
			}
			close(fds[0]);
		}
	} else {
		printf("skipping dma-buf (extensions not available)\n");
	}

	/* ---- native fence fd ---------------------------------------------- */
	if (has(ex, "EGL_ANDROID_native_fence_sync") && create_sync && dupfd &&
	    wait_sync) {
		static const EGLint sa[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
					     EGL_NO_NATIVE_FENCE_FD_ANDROID,
					     EGL_NONE };
		EGLSyncKHR s1 = create_sync(d, EGL_SYNC_NATIVE_FENCE_ANDROID, sa);
		CHECK(s1 != EGL_NO_SYNC_KHR, "native fence sync (0x%x)",
		      eglGetError());
		glFlush();
		int fd = s1 != EGL_NO_SYNC_KHR ? dupfd(d, s1) : -1;
		printf("native fence fd %d\n", fd);
		CHECK(fd >= 0, "eglDupNativeFenceFDANDROID returned %d", fd);
		if (fd >= 0) {
			EGLint a2[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd,
					EGL_NONE };
			EGLSyncKHR s2 = create_sync(
				d, EGL_SYNC_NATIVE_FENCE_ANDROID, a2);
			CHECK(s2 != EGL_NO_SYNC_KHR, "sync from fd (0x%x)",
			      eglGetError());
			if (s2 != EGL_NO_SYNC_KHR) {
				EGLint r = wait_sync(d, s2,
						     EGL_SYNC_FLUSH_COMMANDS_BIT_KHR,
						     1000000000ll);
				CHECK(r == EGL_CONDITION_SATISFIED_KHR,
				      "wait on imported fence: 0x%x", r);
			}
		}
	} else {
		printf("skipping native fence (extension not available)\n");
	}

	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	printf("%s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
