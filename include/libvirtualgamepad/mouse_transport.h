// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

// Private HID wire contract between the VHF producer and the owned mouse
// mapper. No Windows headers, allocation, or STL objects: shared with KMDF.
namespace lvg::mouse_transport {
inline constexpr std::uint16_t k_usage_page = 0xff53;
inline constexpr std::uint16_t k_usage = 1;
inline constexpr std::uint8_t k_report_id = 2;
inline constexpr std::uint8_t k_ack_report_id = 5;
inline constexpr std::uint8_t k_version = 1;
inline constexpr std::uint16_t k_hardware_revision = 0x0100;
inline constexpr std::uint16_t k_absolute = 1u << 0;
inline constexpr std::uint16_t k_virtual_desktop = 1u << 1;
inline constexpr std::uint16_t k_absolute_flags = k_absolute | k_virtual_desktop;
// Only normal close may mark a neutral report this way. It explicitly
// accounts for discarded queued reports and permits a forward sequence gap.
inline constexpr std::uint16_t k_purge = 1u << 2;
inline constexpr std::uint8_t k_button_mask = 0x1f;
inline constexpr std::int16_t k_wheel_delta = 120;

#pragma pack(push, 1)
struct report {
  std::uint8_t report_id;
  std::uint8_t version;
  std::uint16_t flags;
  std::uint8_t buttons;
  std::uint8_t reserved;
  std::int16_t wheel; // Windows wheel units, not detents; positive up.
  std::int16_t pan; // Windows wheel units; positive right.
  std::int32_t x; // Raw relative counts, or normalized absolute 0..65535.
  std::int32_t y;
  std::uint32_t sequence; // Admission order, not a desktop-delivery acknowledgment.
};
// Sent only by the exclusively opened kernel mapper after every packet from
// this input report was accepted by MouseClassServiceCallback. This is class
// queue flow control, never desktop/application delivery evidence.
struct ack_report {
  std::uint8_t report_id;
  std::uint8_t version;
  std::uint16_t reserved;
  std::uint32_t sequence;
};
#pragma pack(pop)
static_assert(sizeof(ack_report) == 8 && offsetof(ack_report, sequence) == 4);
static_assert(sizeof(report) == 22);
static_assert(offsetof(report, flags) == 2 && offsetof(report, buttons) == 4);
static_assert(offsetof(report, wheel) == 6 && offsetof(report, x) == 10);
static_assert(offsetof(report, sequence) == 18);

constexpr std::uint32_t next_sequence(std::uint32_t value) noexcept {
  return value == UINT32_MAX ? 1 : value + 1;
}
constexpr bool valid(const report &value) noexcept {
  return value.report_id == k_report_id && value.version == k_version &&
    value.reserved == 0 && (value.buttons & ~k_button_mask) == 0 &&
    value.sequence != 0 && (value.flags == 0 ||
      (value.flags == k_absolute_flags && value.x >= 0 && value.x <= 65535 &&
       value.y >= 0 && value.y <= 65535) ||
      (value.flags == k_purge && value.buttons == 0 && value.x == 0 && value.y == 0 &&
       value.wheel == 0 && value.pan == 0));
}
constexpr bool valid(const ack_report &value) noexcept {
  return value.report_id == k_ack_report_id && value.version == k_version &&
    value.reserved == 0 && value.sequence != 0;
}
} // namespace lvg::mouse_transport
