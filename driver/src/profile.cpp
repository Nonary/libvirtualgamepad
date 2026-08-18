// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "profile.h"

#include <cstring>
#include <iterator>

namespace lvg::driver {
namespace {

// A conventional HID Game Pad collection. It is purposefully generic: no
// vendor/product identifier, proprietary report layout, or branded device
// behavior is implied by this descriptor.
constexpr std::uint8_t k_generic_gamepad_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x20,        //   Usage Maximum (32)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x20,        //   Report Count (32)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x39,        //   Usage (Hat switch)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x07,        //   Logical Maximum (7)
  0x35, 0x00,        //   Physical Minimum (0)
  0x46, 0x3B, 0x01,  //   Physical Maximum (315)
  0x65, 0x14,        //   Unit (English Rotation: degrees)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x42,        //   Input (Data, Variable, Absolute, Null state)
  0x65, 0x00,        //   Unit (None)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x03,        //   Input (Constant, Variable, Absolute)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x09, 0x33,        //   Usage (Rx)
  0x09, 0x34,        //   Usage (Ry)
  0x16, 0x00, 0x80,  //   Logical Minimum (-32768)
  0x26, 0xFF, 0x7F,  //   Logical Maximum (32767)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x04,        //   Report Count (4)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x09, 0x32,        //   Usage (Z)
  0x09, 0x35,        //   Usage (Rz)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x02,        //   Report ID (2)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor-defined 0xFF00)
  0x09, 0x01,        //   Usage (1)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x05,        //   Report Count (5)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0xC0,              // End Collection
};

constexpr profile_definition k_generic_profile {
  profile::generic_hid,
  k_generic_gamepad_descriptor,
  sizeof(k_generic_gamepad_descriptor),
};

std::uint8_t encode_hat(const std::uint32_t buttons) noexcept {
  const bool up = (buttons & button_mask::dpad_up) != 0;
  const bool down = (buttons & button_mask::dpad_down) != 0;
  const bool left = (buttons & button_mask::dpad_left) != 0;
  const bool right = (buttons & button_mask::dpad_right) != 0;

  if ((up && down) || (left && right)) {
    return 8;  // HID null state.
  }
  if (up) {
    return right ? 1 : (left ? 7 : 0);
  }
  if (down) {
    return right ? 3 : (left ? 5 : 4);
  }
  return right ? 2 : (left ? 6 : 8);
}

std::uint32_t map_buttons(const std::uint32_t buttons) noexcept {
  constexpr std::uint32_t k_source_buttons[] = {
    button_mask::south,
    button_mask::east,
    button_mask::west,
    button_mask::north,
    button_mask::left_shoulder,
    button_mask::right_shoulder,
    button_mask::back,
    button_mask::start,
    button_mask::left_stick,
    button_mask::right_stick,
    button_mask::home,
    button_mask::paddle_1,
    button_mask::paddle_2,
    button_mask::paddle_3,
    button_mask::paddle_4,
    button_mask::touchpad,
    button_mask::misc,
  };

  std::uint32_t mapped = 0;
  for (std::size_t index = 0; index < std::size(k_source_buttons); ++index) {
    if ((buttons & k_source_buttons[index]) != 0) {
      mapped |= 1u << index;
    }
  }
  return mapped;
}

}  // namespace

const profile_definition *find_profile(const profile id) noexcept {
  switch (id) {
    case profile::generic_hid:
      return &k_generic_profile;
    case profile::xbox_360:
    case profile::xbox_one:
    case profile::xbox_series:
    case profile::dualshock_4:
    case profile::dualsense:
    case profile::switch_pro:
      return nullptr;
  }
  return nullptr;
}

generic_input_report encode_generic_input(const input_state_request &input) noexcept {
  return {
    k_generic_input_report_id,
    map_buttons(input.buttons),
    encode_hat(input.buttons),
    input.left_x,
    input.left_y,
    input.right_x,
    input.right_y,
    input.left_trigger,
    input.right_trigger,
  };
}

feedback_event encode_generic_feedback(
  const std::uint32_t controller_id,
  const generic_output_report &output) noexcept {
  feedback_event event {};
  event.header.size = sizeof(event);
  event.header.version = k_protocol_version;
  event.controller_id = controller_id;
  event.type = feedback_type::generic_rumble_rgb;
  const generic_rumble_rgb_feedback feedback {
    static_cast<std::uint16_t>(static_cast<std::uint16_t>(output.low_frequency_rumble) << 8u),
    static_cast<std::uint16_t>(static_cast<std::uint16_t>(output.high_frequency_rumble) << 8u),
    output.red,
    output.green,
    output.blue,
    0,
  };
  event.payload_size = sizeof(feedback);
  std::memcpy(event.payload, &feedback, sizeof(feedback));
  return event;
}

}  // namespace lvg::driver
