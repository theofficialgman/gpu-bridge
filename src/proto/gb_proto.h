/*
 * gb_proto.h - wire-level definitions shared by the arm32/i386 client and the
 * arm64/x86_64 server.
 *
 * Invariant I-2 (ARCHITECTURE.md): nothing in this file may depend on the
 * pointer width of the compiling ABI. Only fixed-width integer types are used,
 * and every layout is checked with static asserts that must hold identically
 * for -m32 and -m64.
 */
#ifndef GB_PROTO_H
#define GB_PROTO_H

#include <stddef.h>
#include <stdint.h>

#define GB_PROTO_VERSION 1u

/* Ring-level record header: the first 8 bytes of every record in a ring. */
struct gb_rec_hdr {
	uint32_t opcode;
	uint32_t size; /* total record bytes incl. this header; multiple of 8 */
};

/* Command header: every real (non-PAD) record starts with this. */
struct gb_cmd_hdr {
	uint32_t opcode;
	uint32_t size;
	uint32_t seq;   /* per-thread, monotonically increasing */
	uint32_t flags; /* GB_CMD_F_* */
};

#define GB_OP_PAD 0xFFFFFFFFu /* ring wrap filler; only gb_rec_hdr is valid */

#define GB_CMD_F_WANT_REPLY (1u << 0)
#define GB_CMD_F_HAS_FDS    (1u << 1)
#define GB_CMD_F_BARRIER    (1u << 2)

/* Every record is padded to this alignment. */
#define GB_REC_ALIGN 8u

#define GB_ALIGN_UP(x, a) (((x) + ((a) - 1u)) & ~((a) - 1u))

_Static_assert(sizeof(struct gb_rec_hdr) == 8, "gb_rec_hdr layout");
_Static_assert(sizeof(struct gb_cmd_hdr) == 16, "gb_cmd_hdr layout");
_Static_assert(offsetof(struct gb_cmd_hdr, seq) == 8, "gb_cmd_hdr.seq");
_Static_assert(offsetof(struct gb_cmd_hdr, flags) == 12, "gb_cmd_hdr.flags");

#endif /* GB_PROTO_H */
