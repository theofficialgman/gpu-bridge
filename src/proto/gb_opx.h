/*
 * gb_opx.h - hand-written ("extra") opcodes that have no registry entry or
 * need custom client/server logic. Values start above any generated opcode.
 *
 * Wire layouts are documented next to the encoder in client/gb_client_gl.c
 * and the decoder in server/gb_server_manual.c; keep both in sync.
 */
#ifndef GB_OPX_H
#define GB_OPX_H

enum gb_opx {
	GB_OPX_BASE = 0x10000,
	GB_OPX_GetDisplay,        /* kind, str            -> handle */
	GB_OPX_QueryString,       /* dpy, name            -> str   */
	GB_OPX_GetString,         /* name, index          -> str   */
	GB_OPX_ShaderSource,      /* shader, count, blob  (async)  */
	GB_OPX_VertexAttribPtrOff,/* idx,size,type,norm,stride,off64 (async) */
	GB_OPX_DrawClient,        /* mode, kind, ..., attribs + data (async) */
	GB_OPX_DrawElementsOff,   /* mode, count, type, off64 (async) */
	GB_OPX_CreateWindowSurface,/* dpy, cfg, xid, attribs -> handle */
	GB_OPX_END
};

#endif
