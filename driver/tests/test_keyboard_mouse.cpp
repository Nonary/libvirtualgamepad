// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#include "keyboard_mouse.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
using E = lvg::driver::keyboard_mouse_endpoint;
using R = E::result;
namespace km = lvg::km;
int checks {}, failures {};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr, "%d: %s\n", __LINE__, #x); } } while (false)
constexpr E::owner_token owner = 42;
km::keyboard_request key(std::uint16_t usage, bool pressed = true) {
  auto value = km::request<km::keyboard_request>(); value.usage = usage; value.pressed = pressed; return value;
}
km::button_request button(std::uint8_t number, bool pressed = true) {
  auto value = km::request<km::button_request>(); value.button = number; value.pressed = pressed; return value;
}
km::relative_request motion(std::int16_t x, std::int16_t y = 0, std::int8_t wheel = 0, std::int8_t pan = 0) {
  auto value = km::request<km::relative_request>(); value.x = x; value.y = y; value.wheel = wheel; value.pan = pan; return value;
}
E::report drain(E &endpoint) {
  endpoint.ready_for_report();
  const auto *value = endpoint.take_report();
  CHECK(value != nullptr);
  return value ? *value : E::report {};
}
bool down(const E::report &report, std::uint16_t usage) { return (report.bytes[1 + usage / 8] & (1u << (usage % 8))) != 0; }
std::int16_t signed16(const E::report &report, std::size_t offset) {
  return static_cast<std::int16_t>(report.bytes[offset] | (report.bytes[offset + 1] << 8));
}
void test_descriptor() {
  // Parse the real descriptor's short items; check bit lengths, absolute vs
  // relative semantics and signed ranges, rather than searching source text.
  std::array<unsigned, 5> bits {};
  unsigned report = 0, size = 0, count = 0, page = 0, usage = 0, depth = 0, top = 0;
  int minimum = 0, maximum = 0;
  bool saw_xy = false, saw_wheel = false, saw_pan = false, saw_absolute = false;
  const auto *bytes = lvg::driver::keyboard_mouse_descriptor;
  const auto length = lvg::driver::keyboard_mouse_descriptor_size;
  for (std::size_t cursor = 0; cursor < length;) {
    const auto prefix = bytes[cursor++];
    const unsigned length_code = prefix & 3;
    const unsigned n = length_code == 3 ? 4 : length_code;
    CHECK(cursor + n <= length);
    if (cursor + n > length) break;
    std::uint32_t value = 0;
    for (unsigned i = 0; i < n; ++i) value |= std::uint32_t(bytes[cursor++]) << (8 * i);
    const unsigned tag = prefix & 0xFC;
    if (tag == 0x04) page = value;
    if (tag == 0x08) usage = value;
    if (tag == 0x14) minimum = n == 1 ? static_cast<std::int8_t>(value) : n == 2 ? static_cast<std::int16_t>(value) : static_cast<int>(value);
    if (tag == 0x24) maximum = minimum < 0 ? (n == 1 ? static_cast<std::int8_t>(value) : n == 2 ? static_cast<std::int16_t>(value) : static_cast<int>(value)) : static_cast<int>(value);
    if (tag == 0x74) size = value;
    if (tag == 0x94) count = value;
    if (tag == 0x84) report = value;
    if (tag == 0xA0) { if (depth++ == 0) ++top; }
    if (tag == 0xC0) { CHECK(depth > 0); --depth; }
    if (tag == 0x80) {
      CHECK(report > 0 && report < bits.size());
      if (report < bits.size()) bits[report] += size * count;
      if (report == 1) { CHECK(page == 7); CHECK(size == 1 && count == 232); CHECK(value == 2); }
      if (report == 2 && page == 1 && size == 16) { CHECK(count == 2 && minimum == -32768 && maximum == 32767 && value == 6); saw_xy = true; }
      if (report == 2 && page == 1 && usage == 0x38) { CHECK(size == 8 && minimum == -127 && maximum == 127 && value == 6); saw_wheel = true; }
      if (report == 2 && page == 0xC) { CHECK(usage == 0x238 && size == 8 && value == 6); saw_pan = true; }
      if (report == 3 && page == 9 && value == 2) CHECK(size == 1 && count == 5);
      if (report == 3 && page == 1 && value == 2) { CHECK(size == 16 && count == 2 && minimum == 0 && maximum == 65535); saw_absolute = true; }
      if (report == 4 && value == 2) CHECK(page == 0x0c && size == 1 && count == 7);
    }
  }
  CHECK(depth == 0 && top == 3);
  CHECK(bits[1] == 232 && bits[2] == 56 && bits[3] == 40 && bits[4] == 8);
  CHECK(saw_xy && saw_wheel && saw_pan && saw_absolute);
}
void test_keyboard_order_and_retained_storage() {
  E endpoint;
  CHECK(endpoint.open(owner) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::unchanged);
  CHECK(endpoint.take_report() == nullptr);
  auto first = drain(endpoint);
  CHECK(first.length == 30 && first.id == 1 && down(first, 4));
  const auto *retained = endpoint.take_report();
  CHECK(retained == nullptr);
  // A key release and a different press must remain separate and cannot replace
  // the storage returned to VHF before the next ready callback.
  CHECK(endpoint.submit(owner, key(4, false)) == R::accepted);
  CHECK(endpoint.submit(owner, key(5)) == R::accepted);
  auto second = drain(endpoint);
  CHECK(!down(second, 4) && !down(second, 5));
  auto third = drain(endpoint);
  CHECK(!down(third, 4) && down(third, 5));
  for (std::uint16_t usage = 6; usage < 20; ++usage) CHECK(endpoint.submit(owner, key(usage)) == R::accepted);
  CHECK(endpoint.submit(owner, key(0xE0)) == R::accepted);
  CHECK(endpoint.submit(owner, key(0xE4)) == R::accepted);
  E::report last {};
  while (endpoint.pending()) last = drain(endpoint);
  for (std::uint16_t usage = 5; usage < 20; ++usage) CHECK(down(last, usage));
  CHECK(down(last, 0xE0) && down(last, 0xE4));
}
void test_mouse_and_absolute() {
  E endpoint; CHECK(endpoint.open(owner, 11) == R::accepted);
  CHECK(endpoint.submit(owner, button(1)) == R::accepted);
  CHECK(endpoint.submit(owner, motion(17, -33, 1, -2)) == R::accepted);
  auto absolute = km::request<km::absolute_request>(); absolute.x = 65535; absolute.y = 32768; absolute.target_generation = 10;
  CHECK(endpoint.submit(owner, absolute) == R::not_ready);
  absolute.target_generation = 11;
  CHECK(endpoint.submit(owner, absolute) == R::accepted);
  CHECK(endpoint.submit(owner, motion(3)) == R::accepted);
  CHECK(endpoint.submit(owner, button(1, false)) == R::accepted);
  auto press = drain(endpoint); CHECK(press.bytes[1] == 1 && signed16(press, 2) == 0);
  auto first = drain(endpoint); CHECK(first.id == 2 && first.length == 8 && first.bytes[1] == 1);
  CHECK(signed16(first, 2) == 17 && signed16(first, 4) == -33 && first.bytes[6] == 1 && first.bytes[7] == 254);
  auto report = drain(endpoint); CHECK(report.id == 3 && report.length == 6 && report.bytes[1] == 1);
  CHECK(report.bytes[2] == 255 && report.bytes[3] == 255 && report.bytes[4] == 0 && report.bytes[5] == 128);
  auto second = drain(endpoint); CHECK(signed16(second, 2) == 3 && second.bytes[1] == 1);
  CHECK(drain(endpoint).bytes[1] == 0);
  CHECK(endpoint.submit(owner, motion(-32768, 32767)) == R::accepted);
  auto limits = drain(endpoint); CHECK(signed16(limits, 2) == -32768 && signed16(limits, 4) == 32767);
  E unbound; CHECK(unbound.open(owner) == R::accepted);
  CHECK(unbound.submit(owner, absolute) == R::unsupported);
}
void test_backpressure_is_atomic() {
  E endpoint; CHECK(endpoint.open(owner) == R::accepted);
  for (std::size_t i = 0; i < E::queue_capacity; ++i) CHECK(endpoint.submit(owner, motion(1)) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::busy);
  CHECK(endpoint.submit(owner, button(1)) == R::busy);
  CHECK(endpoint.pending() == E::queue_capacity);
  CHECK(signed16(drain(endpoint), 2) == 1);
  // The rejected press must not have changed held state, so retry is accepted.
  CHECK(endpoint.submit(owner, key(4)) == R::accepted);
  CHECK(signed16(drain(endpoint), 2) == 1);
  CHECK(endpoint.submit(owner, button(1)) == R::accepted);
  std::size_t movements = 2;
  while (endpoint.pending()) {
    const auto report = drain(endpoint);
    if (report.id == 1) CHECK(down(report, 4));
    else if (signed16(report, 2) == 1) ++movements;
    else CHECK(report.bytes[1] == 1);
  }
  CHECK(movements == E::queue_capacity);
}
void test_consumer_input() {
  E endpoint; CHECK(endpoint.open(owner) == R::accepted);
  auto media = km::request<km::consumer_request>(); media.usage = 0x00e9; media.pressed = 1;
  CHECK(endpoint.submit(owner, media) == R::accepted);
  CHECK(endpoint.submit(owner, media) == R::unchanged);
  auto up = drain(endpoint); CHECK(up.id == 4 && up.length == 2 && up.bytes[1] == 2);
  media.usage = 0x00e2; CHECK(endpoint.submit(owner, media) == R::accepted);
  auto both = drain(endpoint); CHECK(both.bytes[1] == 3);
  media.usage = 0x00e9; media.pressed = 0; CHECK(endpoint.submit(owner, media) == R::accepted);
  CHECK(drain(endpoint).bytes[1] == 1);
  media.usage = 0xffff; CHECK(endpoint.submit(owner, media) == R::invalid);
  CHECK(endpoint.close(owner) == R::accepted);
  CHECK(drain(endpoint).id == 1); CHECK(drain(endpoint).id == 2);
  auto neutral = drain(endpoint); CHECK(neutral.id == 4 && neutral.bytes[1] == 0);
  endpoint.ready_for_report(); CHECK(endpoint.lifecycle() == E::state::drained);
}
void test_cleanup_and_fault() {
  E endpoint; CHECK(endpoint.open(owner) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::accepted);
  endpoint.ready_for_report(); const auto *retained = endpoint.take_report(); CHECK(retained != nullptr);
  const auto copy = *retained;
  CHECK(endpoint.submit(owner, button(1)) == R::accepted);
  CHECK(endpoint.close(owner + 1) == R::not_owner);
  CHECK(endpoint.close(owner) == R::accepted);
  CHECK(endpoint.close(owner) == R::unchanged);
  CHECK(std::memcmp(retained, &copy, sizeof(copy)) == 0);
  CHECK(endpoint.pending() == 3);
  CHECK(endpoint.submit(owner, key(5)) == R::not_ready);
  const auto neutral_key = drain(endpoint);
  CHECK(neutral_key.id == 1);
  for (std::size_t i = 1; i < neutral_key.length; ++i) CHECK(neutral_key.bytes[i] == 0);
  const auto neutral_mouse = drain(endpoint);
  CHECK(neutral_mouse.id == 2 && neutral_mouse.bytes[1] == 0);
  const auto neutral_consumer = drain(endpoint);
  CHECK(neutral_consumer.id == 4 && neutral_consumer.bytes[1] == 0);
  CHECK(endpoint.lifecycle() == E::state::closing);
  endpoint.ready_for_report();
  CHECK(endpoint.lifecycle() == E::state::drained);
  CHECK(endpoint.open(owner + 1) == R::busy);
  endpoint.reset_after_device_deleted();
  CHECK(endpoint.open(owner + 1) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::not_owner);
  CHECK(endpoint.submit(owner + 1, key(5)) == R::accepted);
  endpoint.ready_for_report(); retained = endpoint.take_report(); CHECK(retained != nullptr);
  const auto held = *retained;
  endpoint.submission_failed();
  CHECK(std::memcmp(retained, &held, sizeof(held)) == 0);
  CHECK(endpoint.lifecycle() == E::state::faulted);
  endpoint.ready_for_report(); CHECK(endpoint.take_report() == nullptr);
  CHECK(endpoint.open(owner) == R::busy);
}
void test_request_validation() {
  E endpoint; CHECK(endpoint.open(0) == R::invalid); CHECK(endpoint.open(owner) == R::accepted);
  CHECK(endpoint.submit(owner + 1, key(4)) == R::not_owner);
  for (auto usage : {0, 1, 2, 3, 0xE8, 0xFFFF}) CHECK(endpoint.submit(owner, key(usage)) == R::invalid);
  auto bad = key(4); bad.prefix.size = 1; CHECK(endpoint.submit(owner, bad) == R::invalid);
  bad = key(4); bad.prefix.version++; CHECK(endpoint.submit(owner, bad) == R::invalid);
  bad = key(4); bad.prefix.reserved = 1; CHECK(endpoint.submit(owner, bad) == R::invalid);
  bad = key(4); bad.reserved = 1; CHECK(endpoint.submit(owner, bad) == R::invalid);
  bad = key(4); bad.pressed = 2; CHECK(endpoint.submit(owner, bad) == R::invalid);
  CHECK(endpoint.submit(owner, button(0)) == R::invalid);
  CHECK(endpoint.submit(owner, button(6)) == R::invalid);
  auto move = motion(1); move.wheel = -128; CHECK(endpoint.submit(owner, move) == R::invalid);
  CHECK(endpoint.pending() == 0);
}
}
int main() {
  test_descriptor(); test_keyboard_order_and_retained_storage(); test_mouse_and_absolute(); test_consumer_input();
  test_backpressure_is_atomic(); test_cleanup_and_fault(); test_request_validation();
  std::printf("Keyboard/mouse producer core: %d checks, %d failures\n", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
