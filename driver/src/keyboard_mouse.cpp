// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#include "keyboard_mouse.h"

namespace lvg::driver {
const std::uint8_t keyboard_mouse_descriptor[] {
  0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,km::k_keyboard_report_id,
  0x05,0x07, 0x19,0x00, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
  0x75,0x01, 0x95,0xE8, 0x81,0x02, 0xC0,
  0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,km::k_relative_report_id,
  0x09,0x01, 0xA1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x05,
  0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x05, 0x81,0x02,
  0x75,0x03, 0x95,0x01, 0x81,0x03,
  0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x00,0x80,
  0x26,0xFF,0x7F, 0x75,0x10, 0x95,0x02, 0x81,0x06,
  0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
  0x05,0x0C, 0x0A,0x38,0x02, 0x81,0x06, 0xC0,0xC0,
  0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,km::k_absolute_report_id,
  0x09,0x01, 0xA1,0x00, 0x09,0x30, 0x09,0x31,
  0x15,0x00, 0x27,0xFF,0xFF,0x00,0x00,
  0x75,0x10, 0x95,0x02, 0x81,0x02, 0xC0,0xC0
};
const std::size_t keyboard_mouse_descriptor_size = sizeof(keyboard_mouse_descriptor);

namespace {
void put16(std::uint8_t *to, std::uint16_t value) noexcept {
  to[0] = static_cast<std::uint8_t>(value);
  to[1] = static_cast<std::uint8_t>(value >> 8);
}
}
using endpoint = keyboard_mouse_endpoint;
endpoint::result endpoint::admission(owner_token owner) const noexcept {
  if (state_ != state::active) return result::not_ready;
  if (owner == 0 || owner != owner_) return result::not_owner;
  return result::accepted;
}
endpoint::result endpoint::open(owner_token owner, std::uint32_t generation) noexcept {
  if (owner == 0) return result::invalid;
  if (state_ != state::closed) return result::busy;
  reset_after_device_deleted();
  owner_ = owner;
  absolute_generation_ = generation;
  state_ = state::active;
  return result::accepted;
}
void endpoint::enqueue(const report &value) noexcept {
  queue_[(head_ + count_) % queue_capacity] = value;
  ++count_;
}
endpoint::report endpoint::keyboard_report() const noexcept {
  report value {};
  value.id = value.bytes[0] = km::k_keyboard_report_id;
  value.length = 30;
  for (std::size_t i = 0; i < keys_.size(); ++i) value.bytes[i + 1] = keys_[i];
  return value;
}
endpoint::report endpoint::relative_report(std::int16_t x, std::int16_t y, std::int8_t wheel, std::int8_t pan) const noexcept {
  report value {};
  value.id = value.bytes[0] = km::k_relative_report_id;
  value.length = 8;
  value.bytes[1] = buttons_;
  put16(&value.bytes[2], static_cast<std::uint16_t>(x));
  put16(&value.bytes[4], static_cast<std::uint16_t>(y));
  value.bytes[6] = static_cast<std::uint8_t>(wheel);
  value.bytes[7] = static_cast<std::uint8_t>(pan);
  return value;
}
endpoint::result endpoint::submit(owner_token owner, const km::keyboard_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request) || request.reserved != 0 || request.pressed > 1 || request.usage < 4 || request.usage > 0xE7) return result::invalid;
  const auto index = request.usage / 8;
  const auto bit = static_cast<std::uint8_t>(1u << (request.usage % 8));
  if (((keys_[index] & bit) != 0) == (request.pressed != 0)) return result::unchanged;
  if (count_ == queue_capacity) return result::busy;
  if (request.pressed) keys_[index] |= bit;
  else keys_[index] &= static_cast<std::uint8_t>(~bit);
  enqueue(keyboard_report());
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::button_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request) || request.reserved != 0 || request.pressed > 1 || request.button < 1 || request.button > 5) return result::invalid;
  const auto bit = static_cast<std::uint8_t>(1u << (request.button - 1));
  if (((buttons_ & bit) != 0) == (request.pressed != 0)) return result::unchanged;
  if (count_ == queue_capacity) return result::busy;
  if (request.pressed) buttons_ |= bit;
  else buttons_ &= static_cast<std::uint8_t>(~bit);
  enqueue(relative_report());
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::relative_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request) || request.reserved != 0 || request.wheel == -128 || request.pan == -128) return result::invalid;
  if (request.x == 0 && request.y == 0 && request.wheel == 0 && request.pan == 0) return result::unchanged;
  if (count_ == queue_capacity) return result::busy;
  enqueue(relative_report(request.x, request.y, request.wheel, request.pan));
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::absolute_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request)) return result::invalid;
  if (absolute_generation_ == 0) return result::unsupported;
  if (request.target_generation != absolute_generation_) return result::not_ready;
  if (count_ == queue_capacity) return result::busy;
  report value {};
  value.id = value.bytes[0] = km::k_absolute_report_id;
  value.length = 5;
  put16(&value.bytes[1], request.x);
  put16(&value.bytes[3], request.y);
  enqueue(value);
  return result::accepted;
}
endpoint::result endpoint::close(owner_token owner) noexcept {
  if (owner == 0 || owner != owner_) return result::not_owner;
  if (state_ == state::closing) return result::unchanged;
  if (state_ != state::active) return result::not_ready;
  state_ = state::closing;
  head_ = count_ = 0;
  keys_ = {};
  buttons_ = 0;
  enqueue(keyboard_report());
  enqueue(relative_report());
  return result::accepted;
}
void endpoint::ready_for_report() noexcept {
  if (state_ != state::active && state_ != state::closing) return;
  credit_ = true;
  if (state_ == state::closing && count_ == 0) {
    // Both neutral reports have been retired. Do not reuse the underlying HID
    // child: the WDF owner must delete it and then reset this core explicitly.
    state_ = state::drained;
    credit_ = false;
  }
}
const endpoint::report *endpoint::take_report() noexcept {
  if (!credit_ || count_ == 0 || (state_ != state::active && state_ != state::closing)) return nullptr;
  retained_ = queue_[head_];
  head_ = (head_ + 1) % queue_capacity;
  --count_;
  credit_ = false;
  return &retained_;
}
void endpoint::submission_failed() noexcept {
  if (state_ == state::closed) return;
  state_ = state::faulted;
  credit_ = false;
  // Preserve retained_ until synchronous VhfDelete returns.
  head_ = count_ = 0;
}
void endpoint::reset_after_device_deleted() noexcept {
  owner_ = 0;
  state_ = state::closed;
  absolute_generation_ = 0;
  keys_ = {};
  buttons_ = 0;
  head_ = count_ = 0;
  retained_ = {};
  credit_ = false;
}
} // namespace lvg::driver
