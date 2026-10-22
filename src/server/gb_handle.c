/* SPDX-License-Identifier: MIT */
/*
 * gb_handle.c - opaque handle <-> native pointer tables.
 *
 * The client never sees a native pointer. Handles are 32-bit IDs:
 *   id = (kind << 24) | (index + 1)        (0 is always "no object")
 * Lookups of an unknown/stale id return NULL, never crash.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gb_handle_kinds.h"
#include "gb_server_impl.h"

struct slot {
	const void *ptr; /* NULL = free */
};

struct table {
	struct slot *slots;
	uint32_t cap, count;
	uint32_t *free_list;
	uint32_t nfree;
	/* ptr -> index open-addressed hash, size is power of two */
	uint32_t *hash; /* index+1, 0 = empty, 0xFFFFFFFF = tombstone */
	uint32_t hcap;
};

static struct table g_tables[HK_COUNT];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hptr(const void *p)
{
	uintptr_t x = (uintptr_t)p;
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	return (uint32_t)x;
}

static void hash_rebuild(struct table *t, uint32_t newcap)
{
	free(t->hash);
	t->hash = calloc(newcap, sizeof(uint32_t));
	t->hcap = newcap;
	for (uint32_t i = 0; i < t->cap; i++) {
		if (!t->slots[i].ptr)
			continue;
		uint32_t h = hptr(t->slots[i].ptr) & (newcap - 1);
		while (t->hash[h])
			h = (h + 1) & (newcap - 1);
		t->hash[h] = i + 1;
	}
}

static int64_t hash_find(struct table *t, const void *p)
{
	if (!t->hcap)
		return -1;
	uint32_t h = hptr(p) & (t->hcap - 1);
	while (t->hash[h]) {
		uint32_t v = t->hash[h];
		if (v != 0xFFFFFFFFu && t->slots[v - 1].ptr == p)
			return v - 1;
		h = (h + 1) & (t->hcap - 1);
	}
	return -1;
}

uint32_t gb_p2h(enum gb_handle_kind k, const void *p)
{
	if (!p || k <= HK_NONE || k >= HK_COUNT)
		return 0;
	pthread_mutex_lock(&g_lock);
	struct table *t = &g_tables[k];
	int64_t idx = hash_find(t, p);
	if (idx < 0) {
		if (t->nfree) {
			idx = t->free_list[--t->nfree];
		} else {
			if (t->count == t->cap) {
				uint32_t nc = t->cap ? t->cap * 2 : 64;
				t->slots = realloc(t->slots, nc * sizeof(*t->slots));
				memset(t->slots + t->cap, 0,
				       (nc - t->cap) * sizeof(*t->slots));
				t->cap = nc;
			}
			idx = t->count++;
		}
		t->slots[idx].ptr = p;
		uint32_t need = 128;
		while (need < t->cap * 2)
			need *= 2;
		hash_rebuild(t, need);
	}
	uint32_t id = ((uint32_t)k << 24) | ((uint32_t)idx + 1);
	pthread_mutex_unlock(&g_lock);
	return id;
}

void *gb_h2p(enum gb_handle_kind k, uint32_t id)
{
	if (!id || k <= HK_NONE || k >= HK_COUNT)
		return NULL;
	if ((id >> 24) != (uint32_t)k) /* not even this kind of name */
		return (k != HK_XDPY && k != HK_GLSYNC)
			       ? (void *)(uintptr_t)(0xdead000000000000ull | id)
			       : NULL;
	uint32_t idx = (id & 0xFFFFFF) - 1;
	pthread_mutex_lock(&g_lock);
	struct table *t = &g_tables[k];
	void *p = idx < t->count ? (void *)t->slots[idx].ptr : NULL;
	pthread_mutex_unlock(&g_lock);
	/*
	 * A name the application made up (or one that was destroyed): the
	 * driver reports EGL_BAD_CONFIG / BAD_SURFACE ... for a handle it does
	 * not know, but treats NULL as a different thing (no config, no
	 * surface). Hand it an address no object can have.
	 */
	if (!p && k != HK_XDPY && k != HK_GLSYNC)
		p = (void *)(uintptr_t)(0xdead000000000000ull | id);
	return p;
}

void gb_h_free(enum gb_handle_kind k, uint32_t id)
{
	if (!id || k <= HK_NONE || k >= HK_COUNT || (id >> 24) != (uint32_t)k)
		return;
	uint32_t idx = (id & 0xFFFFFF) - 1;
	pthread_mutex_lock(&g_lock);
	struct table *t = &g_tables[k];
	if (idx < t->count && t->slots[idx].ptr) {
		t->slots[idx].ptr = NULL;
		t->free_list = realloc(t->free_list,
				       (t->nfree + 1) * sizeof(uint32_t));
		t->free_list[t->nfree++] = idx;
		hash_rebuild(t, t->hcap);
	}
	pthread_mutex_unlock(&g_lock);
}
