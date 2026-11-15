// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>

// Read-only readiness contract. This is a PnP/class connection handshake, not
// an acknowledgment that an application or desktop consumed a mouse event.
namespace lvg::mouse_mapper {
struct guid {
  std::uint32_t data1;
  std::uint16_t data2;
  std::uint16_t data3;
  std::uint8_t data4[8];
};
inline constexpr guid k_interface_guid {0x3e51d443, 0xd3c3, 0x40fd, {0xb1, 0x5f, 0x8f, 0x3e, 0x67, 0x0a, 0xcf, 0x2a}};
inline constexpr guid k_container_guid {0x5d43cc5c, 0x318a, 0x46f4, {0x8a, 0x8f, 0x2d, 0x1d, 0x3c, 0x99, 0x80, 0x21}};
inline constexpr std::uint16_t k_status_version = 1;
// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_READ_ACCESS).
inline constexpr std::uint32_t k_ioctl_status = 0x00226404;
inline constexpr std::uint32_t k_class_connected = 1u << 0;
inline constexpr std::uint32_t k_d0 = 1u << 1;
// The HID reader is initialized and running; an outstanding read may be
// temporarily replaced by a retained class packet while Mouclass is busy.
inline constexpr std::uint32_t k_hid_reader_active = 1u << 2;
inline constexpr std::uint32_t k_ready = k_class_connected | k_d0 | k_hid_reader_active;
#pragma pack(push, 1)
struct status {
  std::uint32_t size;
  std::uint16_t version;
  std::uint16_t reserved;
  std::uint32_t ready_flags;
  guid container_id;
};
#pragma pack(pop)
static_assert(sizeof(guid) == 16 && sizeof(status) == 28);
static_assert(offsetof(status, container_id) == 12);
constexpr bool same_guid(const guid &a, const guid &b) noexcept {
  if (a.data1 != b.data1 || a.data2 != b.data2 || a.data3 != b.data3) return false;
  for (std::size_t i = 0; i < 8; ++i) if (a.data4[i] != b.data4[i]) return false;
  return true;
}
constexpr bool valid_ready(const status &value, std::size_t received, const guid &container) noexcept {
  return received == sizeof(status) && value.size == sizeof(status) &&
    value.version == k_status_version && value.reserved == 0 &&
    value.ready_flags == k_ready && same_guid(value.container_id, container);
}
} // namespace lvg::mouse_mapper
