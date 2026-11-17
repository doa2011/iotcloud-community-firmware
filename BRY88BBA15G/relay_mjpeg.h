/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "protocol/media_wire.h"
typedef struct {
  uint8_t *image;
  void *context;
  bool (*want)(void *);
  bool (*store)(void *, size_t, uint8_t);
  void (*frame)(void *, size_t);
  unsigned status, error, state, part_state, chunk_state;
  bool chunked, multipart, jpeg, capture, length_known, ended;
  uint32_t content_length, received, chunk_left, header_bytes,
      part_header_bytes;
  unsigned line_used, last;
  unsigned chunk_used;
  char chunk_line[64];
  char line[512], boundary[76], challenge[512];
} relay_mjpeg_parser;
void relay_mjpeg_init(relay_mjpeg_parser *);
void relay_mjpeg_feed(relay_mjpeg_parser *, const uint8_t *, size_t);
