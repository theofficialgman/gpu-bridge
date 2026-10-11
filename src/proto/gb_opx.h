/*
 * gb_opx.h - hand-written ("extra") opcodes that have no registry entry or
 * need custom client/server logic. Values start above any generated opcode.
 *
 * Wire layouts are documented next to the encoder (client) and the decoder
 * (server/gb_server_manual.c); keep both in sync.
 */
#ifndef GB_OPX_H
#define GB_OPX_H

/* Server -> client upcalls (carried in the response ring, flagged
 * GB_CMD_F_UPCALL). Non-blocking ones are queued by driver callbacks and
 * flushed by the worker thread; blocking ones (WANT_REPLY) are only issued
 * while the client is waiting for a synchronous reply. */
enum gb_up {
	GB_UP_BASE = 0x20000,
	GB_UP_DebugMessage,   /* source,type,id,severity,len, message */
	GB_UP_BlobSet,        /* keysize, valuesize, key, value */
	GB_UP_BlobGet,        /* keysize, valuemax, key -> n, value (blocking) */
	GB_UP_END
};

enum gb_opx {
	GB_OPX_BASE = 0x10000,
	GB_OPX_GetDisplay,        /* kind, inst, str      -> handle */
	GB_OPX_QueryString,       /* dpy, name            -> str   */
	GB_OPX_GetString,         /* name, index          -> str   */
	GB_OPX_ShaderSource,      /* shader, count, blob  (async)  */
	GB_OPX_DrawClient,        /* client-memory vertex arrays/indices (async) */
	GB_OPX_CreateWindowSurface,/* dpy, cfg, xid, attribs -> handle */
	GB_OPX_OpenXDisplay,      /* str                  -> handle */
	GB_OPX_GLXString,         /* kind, dpy, screen, name -> str */
	GB_OPX_GLXFBConfigs,      /* kind, dpy, screen, attribs -> n, ids[] */
	GB_OPX_BufferRead,        /* mode, target|buffer, off64, len64 -> bytes */
	GB_OPX_EglDevice,         /* sub, a, b -> device enumeration / queries */
	GB_OPX_DeviceString,      /* device, name -> str */
	GB_OPX_SelectBuffer,      /* kind, size, type (async) */
	GB_OPX_RenderMode,        /* mode, prev -> ret, newmode, words[] */
	GB_OPX_Ping,              /* -> reply: flushes pending upcalls */
	GB_OPX_DebugCallback,     /* enable (1) / disable (0) debug upcalls */
	GB_OPX_DebugStr,          /* sub, a, b, c, length, string */
	GB_OPX_BlobFuncs,         /* dpy, enable: EGL_ANDROID_blob_cache */
	GB_OPX_ElementMax,        /* itype, count, byte off -> max index (i64, -1 = out of range) */
	GB_OPX_END
};

#endif
