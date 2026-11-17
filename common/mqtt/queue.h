/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Zero means unavailable. Capture under the producer namespace lock, then
 * enqueue with that generation so a delayed producer cannot cross sessions. */
uint32_t ph_mqtt_send_generation(void);
int ph_mqtt_enqueue_send_generation(const char *, const void *, size_t, unsigned, bool, uint32_t);

#include "../wire_buffer.h"
/* On success the queue owns wire; rejection leaves caller ownership intact. */
int ph_mqtt_enqueue_buf_generation(const char *,ph_buf *,unsigned,bool,uint32_t);
