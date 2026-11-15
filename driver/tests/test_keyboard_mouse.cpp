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
bool acknowledge(E &endpoint, std::uint32_t sequence) {
  return endpoint.reserve_mouse_ack(sequence) && endpoint.commit_mouse_ack(sequence);
}
E::report drain(E &endpoint) {
  if (endpoint.awaiting_mouse_ack()) CHECK(acknowledge(endpoint, endpoint.awaiting_mouse_ack()));
  endpoint.ready_for_report();
  const auto *value = endpoint.take_report();
  CHECK(value != nullptr);
  return value ? *value : E::report {};
}
bool down(const E::report &report, std::uint16_t usage) { return (report.bytes[1 + usage / 8] & (1u << (usage % 8))) != 0; }
lvg::mouse_transport::report mouse(const E::report &source) {
  lvg::mouse_transport::report value {};
  CHECK(source.id == lvg::mouse_transport::k_report_id);
  CHECK(source.length == sizeof(value));
  std::memcpy(&value, source.bytes.data(), sizeof(value));
  CHECK(lvg::mouse_transport::valid(value));
  return value;
}
void test_descriptor() {
  std::array<unsigned, 6> bits {};
  unsigned ack_bits = 0;
  unsigned report = 0, size = 0, count = 0, page = 0, usage = 0, depth = 0, top = 0;
  unsigned mouse_collections = 0, vendor_collections = 0;
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
    if (tag == 0x74) size = value;
    if (tag == 0x94) count = value;
    if (tag == 0x84) report = value;
    if (tag == 0xA0 && depth++ == 0) {
      ++top;
      if (page == 1 && usage == 2) ++mouse_collections;
      if (page == lvg::mouse_transport::k_usage_page && usage == lvg::mouse_transport::k_usage) ++vendor_collections;
    }
    if (tag == 0xC0) { CHECK(depth > 0); --depth; }
    if (tag == 0x90) { CHECK(report == 5 && page == lvg::mouse_transport::k_usage_page && value == 2); ack_bits += size * count; }
    if (tag == 0x80) {
      CHECK(report > 0 && report < bits.size());
      if (report < bits.size()) bits[report] += size * count;
      if (report == 1) CHECK(page == 7 && size == 1 && count == 232 && value == 2);
      if (report == 2) CHECK(page == lvg::mouse_transport::k_usage_page && size == 8 && count == 21 && value == 2);
      if (report == 4 && value == 2) CHECK(page == 0x0c && size == 1 && count == 7);
    }
  }
  CHECK(depth == 0 && top == 3 && mouse_collections == 0 && vendor_collections == 1);
  CHECK(bits[1] == 232 && bits[2] == 168 && bits[3] == 0 && bits[4] == 8 && ack_bits == 56);
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
  CHECK(endpoint.submit(owner, button(1)) == R::unchanged);
  CHECK(endpoint.submit(owner, motion(0)) == R::unchanged);
  CHECK(endpoint.submit(owner, motion(17, -33, 1, -2)) == R::accepted);
  auto absolute = km::request<km::absolute_request>(); absolute.x = 65535; absolute.y = 32768; absolute.target_generation = 10;
  CHECK(endpoint.submit(owner, absolute) == R::not_ready);
  absolute.target_generation = 11;
  CHECK(endpoint.submit(owner, absolute) == R::accepted);
  CHECK(endpoint.submit(owner, motion(3)) == R::accepted);
  CHECK(endpoint.submit(owner, button(1, false)) == R::accepted);
  auto press = mouse(drain(endpoint)); CHECK(press.buttons == 1 && press.x == 0 && press.sequence == 1 && press.flags == 0);
  auto first = mouse(drain(endpoint)); CHECK(first.buttons == 1 && first.sequence == 2);
  CHECK(first.x == 17 && first.y == -33 && first.wheel == 120 && first.pan == -240);
  auto report = mouse(drain(endpoint)); CHECK(report.flags == lvg::mouse_transport::k_absolute_flags && report.sequence == 3);
  CHECK(report.x == 65535 && report.y == 32768 && report.buttons == 1);
  auto second = mouse(drain(endpoint)); CHECK(second.x == 3 && second.buttons == 1 && second.sequence == 4);
  auto release = mouse(drain(endpoint)); CHECK(release.buttons == 0 && release.sequence == 5);
  CHECK(endpoint.submit(owner, motion(-32768, 32767, -127, 127)) == R::accepted);
  auto limits = mouse(drain(endpoint)); CHECK(limits.x == -32768 && limits.y == 32767);
  CHECK(limits.wheel == -15240 && limits.pan == 15240);
  CHECK(lvg::mouse_transport::next_sequence(UINT32_MAX) == 1);
  CHECK(lvg::mouse_transport::next_sequence(0) == 1);
  auto invalid = limits; invalid.flags = 1; CHECK(!lvg::mouse_transport::valid(invalid));
  invalid = limits; invalid.flags = 3; CHECK(!lvg::mouse_transport::valid(invalid));
  invalid = limits; invalid.buttons = 0x20; CHECK(!lvg::mouse_transport::valid(invalid));
  invalid = limits; invalid.sequence = 0; CHECK(!lvg::mouse_transport::valid(invalid));
  lvg::mouse_transport::report purge {2,1,lvg::mouse_transport::k_purge,0,0,0,0,0,0,7};
  CHECK(lvg::mouse_transport::valid(purge));
  for (unsigned field = 0; field < 6; ++field) {
    auto malformed = purge;
    if (field == 0) malformed.buttons = 1;
    if (field == 1) malformed.x = 1;
    if (field == 2) malformed.y = -1;
    if (field == 3) malformed.wheel = 120;
    if (field == 4) malformed.pan = -120;
    if (field == 5) malformed.flags |= lvg::mouse_transport::k_absolute;
    CHECK(!lvg::mouse_transport::valid(malformed));
  }
  E unbound; CHECK(unbound.open(owner) == R::accepted);
  CHECK(unbound.submit(owner, absolute) == R::unsupported);
}
void test_backpressure_is_atomic() {
  E endpoint; CHECK(endpoint.open(owner, 11) == R::accepted);
  for (std::size_t i = 0; i < E::queue_capacity; ++i) CHECK(endpoint.submit(owner, motion(1)) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::busy);
  CHECK(endpoint.submit(owner, button(1)) == R::busy);
  auto absolute = km::request<km::absolute_request>(); absolute.target_generation = 11;
  CHECK(endpoint.submit(owner, absolute) == R::busy);
  CHECK(endpoint.pending() == E::queue_capacity);
  CHECK(mouse(drain(endpoint)).x == 1);
  // The rejected press must not have changed held state, so retry is accepted.
  CHECK(endpoint.submit(owner, key(4)) == R::accepted);
  CHECK(mouse(drain(endpoint)).x == 1);
  CHECK(endpoint.submit(owner, button(1)) == R::accepted);
  std::size_t movements = 2;
  while (endpoint.pending()) {
    const auto report = drain(endpoint);
    if (report.id == 1) CHECK(down(report, 4));
    else if (mouse(report).x == 1) ++movements;
    else { auto value = mouse(report); CHECK(value.buttons == 1 && value.sequence == 65); }
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
  CHECK(endpoint.submit(owner, motion(7)) == R::accepted);
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
  CHECK(mouse(neutral_mouse).buttons == 0 && mouse(neutral_mouse).sequence == 3 &&
        mouse(neutral_mouse).flags == lvg::mouse_transport::k_purge);
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
void test_class_ack_gate() {
  E endpoint; CHECK(endpoint.open(owner, 11) == R::accepted);
  CHECK(endpoint.submit(owner, button(1)) == R::accepted);
  CHECK(endpoint.submit(owner, key(4)) == R::accepted);
  CHECK(endpoint.submit(owner, motion(7)) == R::accepted);
  endpoint.ready_for_report();
  CHECK(mouse(*endpoint.take_report()).sequence == 1);
  CHECK(endpoint.awaiting_mouse_ack() == 1);
  CHECK(!endpoint.commit_mouse_ack(1));
  endpoint.ready_for_report();
  CHECK(endpoint.take_report()->id == 1); // Keyboard may replace retained bytes.
  endpoint.ready_for_report();
  CHECK(endpoint.take_report() == nullptr); // Readiness cannot open mouse gate.
  CHECK(!acknowledge(endpoint, 0) && !acknowledge(endpoint, 2));
  CHECK(endpoint.awaiting_mouse_ack() == 1);
  CHECK(endpoint.reserve_mouse_ack(1));
  CHECK(!endpoint.reserve_mouse_ack(1) && !endpoint.reserve_mouse_ack(2));
  endpoint.ready_for_report();CHECK(endpoint.take_report() == nullptr);
  CHECK(!endpoint.commit_mouse_ack(2));
  CHECK(endpoint.awaiting_mouse_ack() == 1);
  CHECK(endpoint.commit_mouse_ack(1));
  CHECK(!acknowledge(endpoint, 1)); // Duplicate is never a second grant.
  CHECK(mouse(*endpoint.take_report()).sequence == 2);
  CHECK(endpoint.submit(owner, motion(9)) == R::accepted);
  CHECK(endpoint.close(owner) == R::accepted); // Purge motion 3, preserve ACK 2.
  CHECK(endpoint.awaiting_mouse_ack() == 2);
  endpoint.ready_for_report(); CHECK(endpoint.take_report()->id == 1);
  endpoint.ready_for_report(); CHECK(endpoint.take_report() == nullptr);
  CHECK(acknowledge(endpoint, 2));
  const auto neutral = mouse(*endpoint.take_report());
  CHECK(neutral.buttons == 0 && neutral.sequence == 4 && neutral.flags == lvg::mouse_transport::k_purge);
  endpoint.ready_for_report(); CHECK(endpoint.take_report()->id == 4);
  endpoint.ready_for_report(); CHECK(endpoint.lifecycle() == E::state::closing);
  CHECK(!acknowledge(endpoint, 2));
  CHECK(acknowledge(endpoint, 4));
  CHECK(endpoint.lifecycle() == E::state::drained);
  endpoint.reset_after_device_deleted();
  CHECK(!acknowledge(endpoint, 4));
  CHECK(endpoint.open(owner) == R::accepted);
  CHECK(endpoint.submit(owner, motion(1)) == R::accepted);
  endpoint.ready_for_report(); CHECK(mouse(*endpoint.take_report()).sequence == 1);
  endpoint.submission_failed(); CHECK(!acknowledge(endpoint, 1));
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
  test_backpressure_is_atomic(); test_cleanup_and_fault(); test_class_ack_gate(); test_request_validation();
  std::printf("Keyboard/mouse producer core: %d checks, %d failures\n", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
