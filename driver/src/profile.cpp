// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "profile.h"

#include "dualsense.h"
#include "dualshock4.h"
#include "switch_pro.h"
#include "xbox_one.h"
#include "xbox_series.h"

#include <cstring>
#include <iterator>

namespace lvg::driver {
namespace {

// Xbox Series. This is the profile that reaches XInput: Windows attaches its
// inbox xinputhid.sys filter by hardware ID, so a VHF child carrying a matching
// ID becomes an XInput device without any bus driver. The identity travels with
// the real report shape, trigger resolution, hat encoding, and rumble payload -
// it is not a relabelled generic pad.
[[nodiscard]] const profile_definition &xbox_series_profile() noexcept {
  static const profile_definition definition = [] {
    profile_definition value {};
    value.id = profile::xbox_series;
    value.report_descriptor = xbox_series_descriptor(&value.report_descriptor_size);
    value.vendor_id = k_xbox_vendor_id;
    value.product_id = k_xbox_series_product_id;
    value.version_number = k_xbox_series_version;
    value.force_feedback = false;
    value.hardware_ids = xbox_series_hardware_ids(&value.hardware_ids_bytes);
    return value;
  }();
  return definition;
}

// Xbox One. Reaches XInput the same way the Series pad does, and in fact
// matches xinputhid.inf on its own product ID rather than through the generic
// GIP entry. Offered alongside Series because a handful of titles and anti-cheat
// layers recognise controller generations by product ID.
[[nodiscard]] const profile_definition &xbox_one_profile() noexcept {
  static const profile_definition definition = [] {
    profile_definition value {};
    value.id = profile::xbox_one;
    value.report_descriptor = xbox_one_descriptor(&value.report_descriptor_size);
    value.vendor_id = k_xbox_vendor_id;
    value.product_id = k_xbox_one_product_id;
    value.version_number = k_xbox_one_version;
    value.force_feedback = false;
    value.hardware_ids = xbox_one_hardware_ids(&value.hardware_ids_bytes);
    return value;
  }();
  return definition;
}

// The PlayStation profiles are HID-native: Windows, SDL, and Steam speak to a
// real DualShock 4 or DualSense as a HID device, so the identity travels with
// the device's report shape, touchpad, motion, battery, lightbar, and the
// feature reports its host-side initialization reads.
[[nodiscard]] const profile_definition &dualshock4_profile() noexcept {
  static const profile_definition definition = [] {
    profile_definition value {};
    value.id = profile::dualshock_4;
    value.report_descriptor = ds4_descriptor(&value.report_descriptor_size);
    value.vendor_id = k_ds4_vendor_id;
    value.product_id = k_ds4_product_id;
    value.version_number = k_ds4_version;
    return value;
  }();
  return definition;
}

[[nodiscard]] const profile_definition &dualsense_profile() noexcept {
  static const profile_definition definition = [] {
    profile_definition value {};
    value.id = profile::dualsense;
    value.report_descriptor = ds5_descriptor(&value.report_descriptor_size);
    value.vendor_id = k_ds5_vendor_id;
    value.product_id = k_ds5_product_id;
    value.version_number = k_ds5_version;
    return value;
  }();
  return definition;
}

[[nodiscard]] const profile_definition &switch_pro_profile() noexcept {
  static const profile_definition definition = [] {
    profile_definition value {};
    value.id = profile::switch_pro;
    value.report_descriptor = switch_descriptor(&value.report_descriptor_size);
    value.vendor_id = k_switch_vendor_id;
    value.product_id = k_switch_product_id;
    value.version_number = k_switch_version;
    return value;
  }();
  return definition;
}

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
    case profile::generic_pid:
      // These protocol values remain reserved for compatibility, but neither
      // profile may ship until this project has an accepted public VID/PID.
      return nullptr;
    case profile::xbox_series:
      return &xbox_series_profile();
    // A real Xbox 360 pad is an XUSB device on a USB bus. That needs a bus
    // child, which VHF cannot create, so no descriptor makes this profile
    // honest. It returns nullptr explicitly: grouping it with a neighbouring
    // case would hand the caller a different vendor's controller under an Xbox
    // name.
    case profile::xbox_360:
      return nullptr;
    case profile::xbox_one:
      return &xbox_one_profile();
    case profile::dualshock_4:
      return &dualshock4_profile();
    case profile::dualsense:
      return &dualsense_profile();
    case profile::switch_pro:
      return &switch_pro_profile();
  }
  return nullptr;
}

profile_mask_t available_profiles() noexcept {
  constexpr profile k_candidates[] = {
    profile::generic_hid,
    profile::generic_pid,
    profile::xbox_360,
    profile::xbox_one,
    profile::xbox_series,
    profile::dualshock_4,
    profile::dualsense,
    profile::switch_pro,
  };

  profile_mask_t mask = 0;
  for (const profile candidate : k_candidates) {
    if (find_profile(candidate) != nullptr) {
      mask |= profile_bit(candidate);
    }
  }
  return mask;
}

// HID Generic Desktop Y and Ry are positive-down; the protocol's are positive-up.
// Every other profile converts this itself, and the generic profiles used to be
// the exception, which left the conversion to the client and double-inverted the
// vertical axes once other profiles existed. INT16_MIN has no positive
// counterpart, so it saturates rather than wrapping back to itself.
[[nodiscard]] std::int16_t to_hid_vertical_axis(const std::int16_t value) noexcept {
  return value == -32768 ? 32767 : static_cast<std::int16_t>(-value);
}

generic_input_report encode_generic_input(const input_state_request &input) noexcept {
  return {
    k_generic_input_report_id,
    map_buttons(input.buttons),
    encode_hat(input.buttons),
    input.left_x,
    to_hid_vertical_axis(input.left_y),
    input.right_x,
    to_hid_vertical_axis(input.right_y),
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
