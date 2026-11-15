// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#include "keyboard_mouse.h"

namespace lvg::driver {
const std::uint8_t keyboard_mouse_descriptor[] {
  0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,km::k_keyboard_report_id,
  0x05,0x07, 0x19,0x00, 0x29,0xE7, 0x15,0x00, 0x25,0x01,
  0x75,0x01, 0x95,0xE8, 0x81,0x02, 0xC0,
  // One private transport collection; the owned mapper creates the mouse
  // class stream, including relative/absolute mode and button edges.
  0x06,0x53,0xFF, 0x09,0x01, 0xA1,0x01,
  0x85,mouse_transport::k_report_id,
  0x15,0x00, 0x26,0xFF,0x00,
  0x75,0x08, 0x95,sizeof(mouse_transport::report) - 1,
  0x19,0x01, 0x29,sizeof(mouse_transport::report) - 1,
  0x81,0x02,
  0x85,mouse_transport::k_ack_report_id,
  0x95,sizeof(mouse_transport::ack_report) - 1,
  0x19,0x01, 0x29,sizeof(mouse_transport::ack_report) - 1,
  0x91,0x02, 0xC0,
  // Consumer controls: one bit for each advertised mute/volume/playback key.
  0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,km::k_consumer_report_id,
  0x09,0xE2, 0x09,0xE9, 0x09,0xEA, 0x09,0xCD,
  0x09,0xB5, 0x09,0xB6, 0x09,0xB7,
  0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x07, 0x81,0x02,
  0x75,0x01, 0x95,0x01, 0x81,0x03, 0xC0
};
const std::size_t keyboard_mouse_descriptor_size = sizeof(keyboard_mouse_descriptor);

namespace {
void put16(std::uint8_t *to, std::uint16_t value) noexcept {
  to[0] = static_cast<std::uint8_t>(value);
  to[1] = static_cast<std::uint8_t>(value >> 8);
}
void put32(std::uint8_t *to, std::uint32_t value) noexcept {
  for (unsigned i = 0; i < 4; ++i) to[i] = static_cast<std::uint8_t>(value >> (8 * i));
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
endpoint::report endpoint::consumer_report() const noexcept {
  report value {};
  value.id = value.bytes[0] = km::k_consumer_report_id;
  value.bytes[1] = consumer_buttons_;
  value.length = 2;
  return value;
}
endpoint::report endpoint::mouse_report(std::int32_t x, std::int32_t y,
    std::int16_t wheel, std::int16_t pan, std::uint16_t flags) noexcept {
  report value {};
  value.id = value.bytes[0] = mouse_transport::k_report_id;
  value.length = sizeof(mouse_transport::report);
  value.bytes[1] = mouse_transport::k_version;
  put16(&value.bytes[2], flags);
  value.bytes[4] = buttons_;
  put16(&value.bytes[6], static_cast<std::uint16_t>(wheel));
  put16(&value.bytes[8], static_cast<std::uint16_t>(pan));
  put32(&value.bytes[10], static_cast<std::uint32_t>(x));
  put32(&value.bytes[14], static_cast<std::uint32_t>(y));
  mouse_sequence_ = mouse_transport::next_sequence(mouse_sequence_);
  put32(&value.bytes[18], mouse_sequence_);
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
  enqueue(mouse_report());
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::consumer_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request) || request.reserved != 0 || request.pressed > 1) return result::invalid;
  std::uint8_t bit = 0;
  switch (request.usage) {
    case 0x00e2: bit = 1u << 0; break; case 0x00e9: bit = 1u << 1; break;
    case 0x00ea: bit = 1u << 2; break; case 0x00cd: bit = 1u << 3; break;
    case 0x00b5: bit = 1u << 4; break; case 0x00b6: bit = 1u << 5; break;
    case 0x00b7: bit = 1u << 6; break; default: return result::invalid;
  }
  if (((consumer_buttons_ & bit) != 0) == (request.pressed != 0)) return result::unchanged;
  if (count_ == queue_capacity) return result::busy;
  if (request.pressed) consumer_buttons_ |= bit;
  else consumer_buttons_ &= static_cast<std::uint8_t>(~bit);
  enqueue(consumer_report());
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::relative_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request) || request.reserved != 0 || request.wheel == -128 || request.pan == -128) return result::invalid;
  if (request.x == 0 && request.y == 0 && request.wheel == 0 && request.pan == 0) return result::unchanged;
  if (count_ == queue_capacity) return result::busy;
  enqueue(mouse_report(request.x, request.y,
    static_cast<std::int16_t>(request.wheel * mouse_transport::k_wheel_delta),
    static_cast<std::int16_t>(request.pan * mouse_transport::k_wheel_delta)));
  return result::accepted;
}
endpoint::result endpoint::submit(owner_token owner, const km::absolute_request &request) noexcept {
  const auto admitted = admission(owner);
  if (admitted != result::accepted) return admitted;
  if (!km::valid(request)) return result::invalid;
  if (absolute_generation_ == 0) return result::unsupported;
  if (request.target_generation != absolute_generation_) return result::not_ready;
  if (count_ == queue_capacity) return result::busy;
  enqueue(mouse_report(request.x, request.y, 0, 0, mouse_transport::k_absolute_flags));
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
  consumer_buttons_ = 0;
  enqueue(keyboard_report());
  enqueue(mouse_report(0, 0, 0, 0, mouse_transport::k_purge));
  enqueue(consumer_report());
  return result::accepted;
}
void endpoint::ready_for_report() noexcept {
  if (state_ != state::active && state_ != state::closing) return;
  credit_ = true;
  if (state_ == state::closing && count_ == 0 && awaiting_mouse_ack_ == 0) {
    // All neutral reports have been retired. Do not reuse the underlying HID
    // child: the WDF owner must delete it and then reset this core explicitly.
    state_ = state::drained;
    credit_ = false;
  }
}
const endpoint::report *endpoint::take_report() noexcept {
  if (!credit_ || count_ == 0 || (state_ != state::active && state_ != state::closing)) return nullptr;
  if (queue_[head_].id == mouse_transport::k_report_id && awaiting_mouse_ack_ != 0) return nullptr;
  retained_ = queue_[head_];
  if (retained_.id == mouse_transport::k_report_id) {
    awaiting_mouse_ack_ = 0;
    for (unsigned i = 0; i < 4; ++i)
      awaiting_mouse_ack_ |= std::uint32_t(retained_.bytes[18 + i]) << (8 * i);
  }
  head_ = (head_ + 1) % queue_capacity;
  --count_;
  credit_ = false;
  return &retained_;
}
bool endpoint::reserve_mouse_ack(std::uint32_t sequence) noexcept {
  if ((state_ != state::active && state_ != state::closing) ||
      sequence == 0 || sequence != awaiting_mouse_ack_ || reserved_mouse_ack_ != 0) return false;
  reserved_mouse_ack_ = sequence;
  return true;
}
bool endpoint::commit_mouse_ack(std::uint32_t sequence) noexcept {
  if ((state_ != state::active && state_ != state::closing) || sequence == 0 ||
      sequence != awaiting_mouse_ack_ || sequence != reserved_mouse_ack_) return false;
  reserved_mouse_ack_ = 0;
  awaiting_mouse_ack_ = 0;
  if (state_ == state::closing && count_ == 0 && credit_) {
    state_ = state::drained;
    credit_ = false;
  }
  return true;
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
  consumer_buttons_ = 0;
  mouse_sequence_ = 0;
  awaiting_mouse_ack_ = 0;
  reserved_mouse_ack_ = 0;
  head_ = count_ = 0;
  retained_ = {};
  credit_ = false;
}
} // namespace lvg::driver
