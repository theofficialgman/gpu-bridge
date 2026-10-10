/*
 * gb_carr.h - slot numbering for vertex-array state, shared by the client's
 * shadow state and the DrawClient wire message.
 *
 * Fixed-function (compat profile) arrays come first, then one slot per
 * texture-coordinate unit, then generic vertex attributes.
 */
#ifndef GB_CARR_H
#define GB_CARR_H

#define GB_MAX_TEXUNITS 16
#define GB_MAX_GENERIC 32

enum gb_carr_slot {
	CA_VERTEX = 0,
	CA_NORMAL,
	CA_COLOR,
	CA_SECONDARY,
	CA_FOGCOORD,
	CA_EDGEFLAG,
	CA_INDEX,
	CA_TEX0,
	CA_GENERIC0 = CA_TEX0 + GB_MAX_TEXUNITS,
	CA_COUNT = CA_GENERIC0 + GB_MAX_GENERIC
};

/* DrawClient per-array flags */
#define GB_CA_NORMALIZED (1u << 0)
#define GB_CA_INTEGER (1u << 1)
#define GB_CA_LONG (1u << 2)

/* Buffer-binding kinds the generated encoders ask the client about. */
enum { GB_BUF_PACK = 1, GB_BUF_UNPACK = 2 };
/* Pixel store source for size rules. */
enum { GB_PM_UNPACK = 0, GB_PM_PACK = 1 };

#endif
