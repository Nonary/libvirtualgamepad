// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

#include "libvirtualgamepad/protocol.h"

namespace lvg::driver {

// HID report buffers include their report-ID byte. HID_XFER_PACKET::reportId
// must agree with that byte; VHF/HID consumers use both fields.
inline constexpr std::uint8_t k_generic_input_report_id = 1;
inline constexpr std::uint8_t k_generic_output_report_id = 2;

#pragma pack(push, 1)

struct generic_input_report {
  std::uint8_t report_id;
  std::uint32_t buttons;
  std::uint8_t hat_and_padding;
  std::int16_t left_x;
  std::int16_t left_y;
  std::int16_t right_x;
  std::int16_t right_y;
  std::uint8_t left_trigger;
  std::uint8_t right_trigger;
};

struct generic_output_report {
  std::uint8_t report_id;
  std::uint8_t low_frequency_rumble;
  std::uint8_t high_frequency_rumble;
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

#pragma pack(pop)

static_assert(sizeof(generic_input_report) == 16);
static_assert(sizeof(generic_output_report) == 6);

struct profile_definition {
  profile id;
  const std::uint8_t *report_descriptor;
  std::size_t report_descriptor_size;
  // VHF leaves these zero unless the driver supplies them, which leaves the HID
  // child with no identity for Windows PnP or for applications that match on
  // one. They are never a real vendor's identifiers: a descriptor is not
  // relabelled as somebody else's product by changing a VID/PID.
  std::uint16_t vendor_id;
  std::uint16_t product_id;
  std::uint16_t version_number;
  // True when the profile publishes the DirectInput PID report set and its
  // output and feature reports must be routed to the force-feedback engine.
  bool force_feedback;
  // Optional REG_MULTI_SZ hardware ID list for the VHF child. Windows attaches
  // xinputhid.sys by hardware ID, so this is what puts a profile on the XInput
  // path. Null leaves VHF to synthesize the child's IDs.
  const wchar_t *hardware_ids;
  std::size_t hardware_ids_bytes;
};

// Returns nullptr until a profile has passed descriptor, output, and
// compatibility validation. This intentionally prevents product-name or
// VID/PID-only emulation from becoming a supported controller type.
[[nodiscard]] const profile_definition *find_profile(profile id) noexcept;

[[nodiscard]] generic_input_report encode_generic_input(
  const input_state_request &input) noexcept;

[[nodiscard]] feedback_event encode_generic_feedback(
  std::uint32_t controller_id,
  const generic_output_report &output) noexcept;

}  // namespace lvg::driver
