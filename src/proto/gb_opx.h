/*
 * gb_opx.h - hand-written ("extra") opcodes that have no registry entry or
 * need custom client/server logic. Values start above any generated opcode.
 *
 * Wire layouts are documented next to the encoder (client) and the decoder
 * (server/gb_server_manual.c); keep both in sync.
 */
#ifndef GB_OPX_H
#define GB_OPX_H

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
	GB_OPX_ElementMax,        /* itype, count, byte off -> max index (i64, -1 = out of range) */
	GB_OPX_END
};

#endif
