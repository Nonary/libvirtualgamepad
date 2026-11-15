// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
// Kernel consumers use only built-in ABI types and compiler layout checks.
// Pulling the user-mode MSVC STL after WDK CRT headers mixes incompatible CRTs.
#if !defined(_KERNEL_MODE)
#include <cstddef>
#include <cstdint>
#endif

// Private HID wire contract between the VHF producer and the owned mouse
// mapper. No Windows headers, allocation, or STL objects: shared with KMDF.
namespace lvg::mouse_transport {
// These built-in types have the same underlying types in the supported
// Windows and Linux ABIs. Kernel builds must not mix the WDK CRT with the
// toolchain's newer user-mode <cstdint>/<cstddef> implementation.
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using int16_t = short;
using uint32_t = unsigned int;
using int32_t = int;
static_assert(sizeof(uint8_t) == 1 && sizeof(uint16_t) == 2 && sizeof(int16_t) == 2);
static_assert(sizeof(uint32_t) == 4 && sizeof(int32_t) == 4);
inline constexpr uint16_t k_usage_page = 0xff53;
inline constexpr uint16_t k_usage = 1;
inline constexpr uint8_t k_report_id = 2;
inline constexpr uint8_t k_ack_report_id = 5;
inline constexpr uint8_t k_version = 1;
inline constexpr uint16_t k_hardware_revision = 0x0100;
inline constexpr uint16_t k_absolute = 1u << 0;
inline constexpr uint16_t k_virtual_desktop = 1u << 1;
inline constexpr uint16_t k_absolute_flags = k_absolute | k_virtual_desktop;
// Only normal close may mark a neutral report this way. It explicitly
// accounts for discarded queued reports and permits a forward sequence gap.
inline constexpr uint16_t k_purge = 1u << 2;
inline constexpr uint8_t k_button_mask = 0x1f;
inline constexpr int16_t k_wheel_delta = 120;

#pragma pack(push, 1)
struct report {
  uint8_t report_id;
  uint8_t version;
  uint16_t flags;
  uint8_t buttons;
  uint8_t reserved;
  int16_t wheel; // Windows wheel units, not detents; positive up.
  int16_t pan; // Windows wheel units; positive right.
  int32_t x; // Raw relative counts, or normalized absolute 0..65535.
  int32_t y;
  uint32_t sequence; // Admission order, not a desktop-delivery acknowledgment.
};
// Sent only by the exclusively opened kernel mapper after every packet from
// this input report was accepted by MouseClassServiceCallback. This is class
// queue flow control, never desktop/application delivery evidence.
struct ack_report {
  uint8_t report_id;
  uint8_t version;
  uint16_t reserved;
  uint32_t sequence;
};
#pragma pack(pop)
static_assert(sizeof(ack_report) == 8 && __builtin_offsetof(ack_report, sequence) == 4);
static_assert(sizeof(report) == 22);
static_assert(__builtin_offsetof(report, flags) == 2 && __builtin_offsetof(report, buttons) == 4);
static_assert(__builtin_offsetof(report, wheel) == 6 && __builtin_offsetof(report, x) == 10);
static_assert(__builtin_offsetof(report, sequence) == 18);

constexpr uint32_t next_sequence(uint32_t value) noexcept {
  return value == ~uint32_t{0} ? 1 : value + 1;
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
