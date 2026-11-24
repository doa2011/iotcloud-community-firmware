/* Shared capacity for the complete G3 business-status JSON tree.
 * The full BRY88BBA15G screen status is substantially larger than legacy
 * Daikin variants and must be included in the same snapshot. */
#pragma once
#include <stddef.h>
#define G3_BUSINESS_STATUS_JSON_CAPACITY 10240u
