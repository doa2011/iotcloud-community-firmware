#include "display_policy.h"
#include <string.h>
static bool page_ok(unsigned p) { return p < SCENE_PAGES || p == DISPLAY_KEEP_PAGE; }
void display_policy_default(display_policy *p) {
  memset(p, 0, sizeof(*p));
  p->revision = 1;
  p->external_minutes = 2;
  p->battery_minutes = 1;
  p->ceiling = 100;
  p->idle_page = DISPLAY_KEEP_PAGE;
}
bool display_policy_valid(const display_policy *p) {
  if (!p || !p->revision || p->count > DISPLAY_PLANS || p->page_count > SCENE_PAGES ||
      p->external_minutes > 1440 || p->battery_minutes > 1440 ||
      p->ceiling > 100 || !p->ceiling || p->idle_brightness > 100 ||
      !page_ok(p->idle_page)) return false;
  for (unsigned i = 0; i < p->count; i++) {
    const display_plan *r = &p->plans[i];
    if (r->days > 127 || (r->enabled && !r->days) || r->start >= 1440 ||
        r->end >= 1440 || r->start == r->end || r->brightness > 100 ||
        !page_ok(r->page) || !scene_utf8(r->name, sizeof(r->name))) return false;
  }
  for (unsigned i = 0; i < p->page_count; i++) {
    const display_page_override *r = &p->page_overrides[i];
    if (!r->page_id || r->external_minutes > 1440 ||
        r->battery_minutes > 1440 || r->idle_brightness > 100) return false;
    for (unsigned j = 0; j < i; j++)
      if (p->page_overrides[j].page_id == r->page_id) return false;
  }
  return true;
}
bool display_policy_encode(const display_policy *p, uint8_t *out, size_t n) {
  if (!out || (n != DISPLAY_POLICY_BYTES && n != DISPLAY_POLICY_V1_BYTES) || !display_policy_valid(p)) return false;
  memset(out, 0, n);
  out[0] = SCREEN_MSG_POLICY; out[1] = n == DISPLAY_POLICY_BYTES ? 2 : 1;
  scene_put32(out + 2, p->revision);
  out[6] = p->external_minutes; out[7] = p->external_minutes >> 8;
  out[8] = p->battery_minutes; out[9] = p->battery_minutes >> 8;
  out[10] = p->ceiling; out[11] = p->idle_brightness;
  out[12] = p->idle_page; out[13] = p->count;
  for (unsigned i = 0; i < p->count; i++) {
    const display_plan *r = &p->plans[i]; uint8_t *q = out + 16 + i * 44;
    q[0] = r->enabled; q[1] = r->days; q[2] = r->brightness; q[3] = r->page;
    q[4] = r->start; q[5] = r->start >> 8; q[6] = r->end; q[7] = r->end >> 8;
    memcpy(q + 8, r->name, 32);
  }
  if (n == DISPLAY_POLICY_BYTES) {
    scene_put32(out + DISPLAY_POLICY_V1_BYTES, p->home_page_id);
    out[DISPLAY_POLICY_V1_BYTES + 4] = p->page_count;
    for (unsigned i = 0; i < p->page_count; i++) {
      const display_page_override *r = &p->page_overrides[i];
      uint8_t *q = out + DISPLAY_POLICY_V1_BYTES + 8 + i * 12;
      scene_put32(q, r->page_id);
      q[4] = r->external_minutes; q[5] = r->external_minutes >> 8;
      q[6] = r->battery_minutes; q[7] = r->battery_minutes >> 8;
      q[8] = r->idle_brightness;
    }
  }
  return true;
}
bool display_policy_decode(display_policy *p, const uint8_t *in, size_t n) {
  if (!p || !in || n < 16 || in[0] != SCREEN_MSG_POLICY ||
      !((n == DISPLAY_POLICY_V1_BYTES && in[1] == 1) ||
        (n == DISPLAY_POLICY_BYTES && in[1] == 2)) || in[13] > DISPLAY_PLANS || in[14] || in[15]) return false;
  display_policy next = {0};
  next.revision = scene_u32(in + 2);
  next.external_minutes = in[6] | in[7] << 8;
  next.battery_minutes = in[8] | in[9] << 8;
  next.ceiling = in[10]; next.idle_brightness = in[11];
  next.idle_page = in[12]; next.count = in[13];
  for (unsigned i = 0; i < next.count; i++) {
    display_plan *r = &next.plans[i]; const uint8_t *q = in + 16 + i * 44;
    if (q[0] > 1 || q[40] || q[41] || q[42] || q[43]) return false;
    r->enabled = q[0]; r->days = q[1]; r->brightness = q[2]; r->page = q[3];
    r->start = q[4] | q[5] << 8; r->end = q[6] | q[7] << 8;
    memcpy(r->name, q + 8, 32);
  }
  if (in[1] == 2) {
    const uint8_t *x = in + DISPLAY_POLICY_V1_BYTES;
    next.home_page_id = scene_u32(x); next.page_count = x[4];
    if (next.page_count > SCENE_PAGES || x[5] || x[6] || x[7]) return false;
    for (unsigned i = 0; i < next.page_count; i++) {
      const uint8_t *q = x + 8 + i * 12;
      display_page_override *r = &next.page_overrides[i];
      if (q[9] || q[10] || q[11]) return false;
      r->page_id = scene_u32(q);
      r->external_minutes = q[4] | q[5] << 8;
      r->battery_minutes = q[6] | q[7] << 8;
      r->idle_brightness = q[8];
    }
  }
  if (!display_policy_valid(&next)) return false;
  *p = next; return true;
}
