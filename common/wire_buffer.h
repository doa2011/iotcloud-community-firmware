/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
/* len is wire length. cap includes one trailing sentinel byte, never payload. */
typedef struct ph_buf {
    char *buf;
    size_t len,cap;
    /* Heap buffers may expose an immutable prefixed view. Fixed/measure
     * writers leave these fields zero and never enter the heap lifecycle. */
    char *storage;
    struct ph_buf *owner;
    unsigned references;
} ph_buf;
/* Override weak defaults for device budget/admission or deterministic OOM tests.
 * Both functions must belong to the same allocator and remain thread-safe. */
void *ph_wire_malloc(size_t);
void ph_wire_free(void *);
ph_buf *ph_buf_new(size_t capacity);
ph_buf *ph_buf_new_headroom(size_t capacity,size_t headroom);
ph_buf *ph_buf_retain(ph_buf *);
bool ph_buf_unique(const ph_buf *);
/* Exclusive prefix lease: the owner and returned view must remain immutable
 * until the view is freed. Returns NULL if the headroom is unavailable. */
ph_buf *ph_buf_prefix(ph_buf *,const void *,size_t);
/* Conservative retained allocation size, including descriptors and headroom. */
size_t ph_buf_owned_bytes(const ph_buf *);
bool ph_buf_reserve(ph_buf *,size_t capacity);
void ph_buf_free(ph_buf *);
