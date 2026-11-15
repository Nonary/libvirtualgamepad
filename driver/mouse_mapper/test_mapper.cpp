// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#include "mapper_core.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
using lvg::mouse_mapper::stream;
using lvg::mouse_mapper::verdict;
using lvg::mouse_transport::report;
static report make(std::uint32_t sequence) { return {2,1,0,0,0,0,0,0,0,sequence}; }
int main() {
  stream s;
  auto r = make(17); r.buttons=0x1f; r.x=-2147483647; r.y=2147483647; r.wheel=-240; r.pan=360;
  CHECK(s.admit(r)==verdict::accepted); CHECK(s.remaining()==3);
  CHECK(s.data()[0].button_flags==0x155); CHECK(s.data()[0].x==r.x); CHECK(s.data()[0].y==r.y);
  CHECK(s.data()[1].button_flags==0x400); CHECK(static_cast<std::int16_t>(s.data()[1].button_data)==-240);
  CHECK(s.data()[2].button_flags==0x800); CHECK(s.data()[2].button_data==360);
  const auto next=make(18);
  CHECK(s.admit(next)==verdict::busy); CHECK(s.consume(0)); CHECK(s.remaining()==3);
  CHECK(s.consume(1)); CHECK(s.remaining()==2); CHECK(s.data()[0].button_flags==0x400);
  CHECK(s.admit(next)==verdict::busy); CHECK(s.consume(1)); CHECK(s.data()[0].button_flags==0x800);
  CHECK(s.consume(1)); CHECK(s.admit(next)==verdict::accepted);
  CHECK(s.remaining()==1); CHECK(s.data()[0].button_flags==0x2aa); CHECK(s.consume(1));
  r=make(19); r.flags=3; r.x=65535; r.y=0;
  CHECK(s.admit(r)==verdict::accepted); CHECK(s.data()[0].flags==3); CHECK(s.consume(1));
  r=make(20); r.x=-5; r.y=9;
  CHECK(s.admit(r)==verdict::accepted); CHECK(s.data()[0].flags==0); CHECK(s.consume(1));
  CHECK(s.admit(r)==verdict::sequence_error); CHECK(s.failed());
  s.reset(); r=make(UINT32_MAX); CHECK(s.admit(r)==verdict::accepted); CHECK(s.empty());
  r=make(1); CHECK(s.admit(r)==verdict::accepted); r=make(65); r.flags=lvg::mouse_transport::k_purge;
  CHECK(s.admit(r)==verdict::accepted); CHECK(s.empty());
  r=make(64); CHECK(s.admit(r)==verdict::sequence_error);
  s.reset(); r=make(1); CHECK(s.admit(r)==verdict::accepted); r=make(0x80000001u); CHECK(s.admit(r)==verdict::sequence_error);
  for (const auto bad : {1u, 0x80000001u, UINT32_MAX}) {
    s.reset();r=make(1);CHECK(s.admit(r)==verdict::accepted);
    r=make(bad);r.flags=lvg::mouse_transport::k_purge;CHECK(s.admit(r)==verdict::sequence_error);
  }
  s.reset(); r=make(1); r.x=1; CHECK(s.admit(r)==verdict::accepted); CHECK(!s.consume(2)); CHECK(s.failed());
  s.reset();r=make(1);CHECK(s.admit(r)==verdict::accepted);r=make(3);
  CHECK(s.admit(r)==verdict::sequence_error); // Unmarked transport loss is terminal.
  s.reset();r=make(1);r.buttons=0x1f;CHECK(s.admit(r)==verdict::accepted);CHECK(s.consume(1));
  r=make(65);r.flags=lvg::mouse_transport::k_purge;CHECK(s.admit(r)==verdict::accepted);
  CHECK(s.remaining()==1);CHECK(s.data()[0].flags==0);CHECK(s.data()[0].button_flags==0x2aa);CHECK(s.consume(1));
  r=make(66);r.x=1;CHECK(s.admit(r)==verdict::accepted);CHECK(s.consume(1));
  s.reset();r=make(UINT32_MAX-1);CHECK(s.admit(r)==verdict::accepted);r=make(2);r.flags=lvg::mouse_transport::k_purge;
  CHECK(s.admit(r)==verdict::accepted);CHECK(s.empty());
  r=make(3);CHECK(s.admit(r)==verdict::accepted);
  for (unsigned field=0; field<15; ++field) {
    s.reset(); r=make(1);
    switch(field) {
      case 0:r.report_id=3;break; case 1:r.version=2;break; case 2:r.flags=1;break;
      case 3:r.buttons=0x20;break;case 4:r.reserved=1;break;case 5:r.sequence=0;break;
      case 6:r.flags=3;r.x=-1;break;case 7:r.flags=3;r.y=65536;break;case 8:r.flags=7;break;
      case 9:r.flags=4;r.buttons=1;break;case 10:r.flags=4;r.x=1;break;case 11:r.flags=4;r.y=-1;break;
      case 12:r.flags=4;r.wheel=120;break;case 13:r.flags=4;r.pan=-120;break;case 14:r.flags=5;break;
    }
    CHECK(s.admit(r)==verdict::invalid); CHECK(s.failed()); CHECK(s.empty());
  }
  // Every possible button-state transition uses the Windows edge bitmap.
  for(unsigned from=0; from<32; ++from) for(unsigned to=0; to<32; ++to) {
    s.reset();r=make(1);r.buttons=static_cast<std::uint8_t>(from);CHECK(s.admit(r)==verdict::accepted);CHECK(s.consume(s.remaining()));
    r=make(2);r.buttons=static_cast<std::uint8_t>(to);CHECK(s.admit(r)==verdict::accepted);
    unsigned expected=0;for(unsigned b=0;b<5;++b) if((from^to)&(1u<<b)) expected|=1u<<(2*b+((to&(1u<<b))?0:1));
    CHECK(expected ? s.remaining()==1 && s.data()[0].button_flags==expected : s.empty());
  }
  std::cout << "mouse mapper conversion/backpressure passed\n";
}
