/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../screen_event_log.h"
#include <cassert>
#include <cstring>
int main() {
  g3diag::Ring events;
  assert(events.size()==0);
  events.add(100, g3diag::BOOT,g3diag::INFO);
  events.add(101, g3diag::UART_LOSS,g3diag::ERROR,2);
  events.add(102, g3diag::UART_LOSS,g3diag::ERROR,2);
  assert(events.size()==2);
  assert(events.at(0).repeat==2);
  for(unsigned i=0;i<g3diag::CAPACITY+24;i++)events.add(1000+i*31000,g3diag::SNAPSHOT_LOST,g3diag::ERROR,i);
  assert(events.size()==g3diag::CAPACITY);
  assert(events.overwritten()==26);
  assert(events.at(0).argument==g3diag::CAPACITY+23);
  const auto capture=g3diag::capture(events);
  assert(sizeof(capture)==528);
  assert(g3diag::valid(capture));
  assert(capture.count==32);
  assert(capture.entries[0].argument==g3diag::CAPACITY+23);
  auto damaged=capture;
  damaged.entries[1].argument^=0x1234u;
  assert(!g3diag::valid(damaged));
  damaged=capture;
  damaged.count=33;
  assert(!g3diag::valid(damaged));
  // Boot counters always use uint32 monotonic uptime; wrap is supported.
  g3diag::Ring wrap;
  wrap.add(0xfffffff0u,g3diag::UART_LOSS,g3diag::WARN,1);
  wrap.add(0x10u,g3diag::UART_LOSS,g3diag::WARN,1);
  assert(wrap.size()==1 && wrap.at(0).repeat==2);
  return 0;
}
