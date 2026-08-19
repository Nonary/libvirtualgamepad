// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "dualshock4.h"

#include <cstring>

namespace lvg::driver {
namespace {

// A DualShock 4's USB report descriptor: a 64-byte input report on ID 1, a
// 32-byte output report on ID 5, and the vendor feature reports its host-side
// initialization reads. The input report's interior structure is vendor-defined
// on real hardware beyond the sticks, triggers, hat, and buttons, which is why
// the motion and touch bytes are declared as vendor-defined padding rather than
// as Generic Desktop usages.
constexpr std::uint8_t k_ds4_descriptor[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x05,        // Usage (Game Pad)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x09, 0x30,        //   Usage (X)
  0x09, 0x31,        //   Usage (Y)
  0x09, 0x32,        //   Usage (Z)
  0x09, 0x35,        //   Usage (Rz)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x04,        //   Report Count (4)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x09, 0x39,        //   Usage (Hat switch)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x07,        //   Logical Maximum (7)
  0x35, 0x00,        //   Physical Minimum (0)
  0x46, 0x3B, 0x01,  //   Physical Maximum (315)
  0x65, 0x14,        //   Unit (English Rotation: Degrees)
  0x75, 0x04,        //   Report Size (4)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x42,        //   Input (Data, Variable, Absolute, Null state)
  0x65, 0x00,        //   Unit (None)
  0x05, 0x09,        //   Usage Page (Button)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x0E,        //   Usage Maximum (14)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x0E,        //   Report Count (14)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor-defined 0xFF00)
  0x09, 0x20,        //   Usage (0x20)
  0x75, 0x06,        //   Report Size (6)
  0x95, 0x01,        //   Report Count (1)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x3F,        //   Logical Maximum (63)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x05, 0x01,        //   Usage Page (Generic Desktop)
  0x09, 0x33,        //   Usage (Rx)
  0x09, 0x34,        //   Usage (Ry)
  0x15, 0x00,        //   Logical Minimum (0)
  0x26, 0xFF, 0x00,  //   Logical Maximum (255)
  0x75, 0x08,        //   Report Size (8)
  0x95, 0x02,        //   Report Count (2)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x06, 0x00, 0xFF,  //   Usage Page (Vendor-defined 0xFF00)
  0x09, 0x21,        //   Usage (0x21)
  0x95, 0x36,        //   Report Count (54)
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0x85, 0x05,        //   Report ID (5)
  0x09, 0x22,        //   Usage (0x22)
  0x95, 0x1F,        //   Report Count (31)
  0x91, 0x02,        //   Output (Data, Variable, Absolute)
  0x85, 0x02,        //   Report ID (2)
  0x09, 0x24,        //   Usage (0x24)
  0x95, 0x24,        //   Report Count (36)
  0xB1, 0x02,        //   Feature (Data, Variable, Absolute)
  0x85, 0x12,        //   Report ID (18)
  0x09, 0x2E,        //   Usage (0x2E)
  0x95, 0x0F,        //   Report Count (15)
  0xB1, 0x02,        //   Feature (Data, Variable, Absolute)
  0x85, 0xA3,        //   Report ID (163)
  0x09, 0x25,        //   Usage (0x25)
  0x95, 0x30,        //   Report Count (48)
  0xB1, 0x02,        //   Feature (Data, Variable, Absolute)
  0xC0,              // End Collection
};

// Identity calibration: zero bias and a symmetric range that reduces to the
// device's nominal counts-per-unit. Inventing a plausible-looking non-identity
// calibration would silently skew every motion sample a consumer computes.
constexpr std::uint8_t k_ds4_calibration[36] = {
  k_ds4_feature_calibration_id,
  0x00, 0x00,  // gyro pitch bias
  0x00, 0x00,  // gyro yaw bias
  0x00, 0x00,  // gyro roll bias
  0x00, 0x20,  // gyro pitch plus   (+8192)
  0x00, 0xE0,  // gyro pitch minus  (-8192)
  0x00, 0x20,  // gyro yaw plus
  0x00, 0xE0,  // gyro yaw minus
  0x00, 0x20,  // gyro roll plus
  0x00, 0xE0,  // gyro roll minus
  0x00, 0x20,  // gyro speed plus
  0x00, 0x20,  // gyro speed minus
  0x00, 0x20,  // accel x plus      (+8192)
  0x00, 0xE0,  // accel x minus     (-8192)
  0x00, 0x20,  // accel y plus
  0x00, 0xE0,  // accel y minus
  0x00, 0x20,  // accel z plus
  0x00, 0xE0,  // accel z minus
  0x00,        // padding
};

// A locally administered MAC: the second-least-significant bit of the first
// octet marks it as not globally assigned, so it cannot collide with a real
// Sony device's address.
constexpr std::uint8_t k_ds4_mac[6] = {0x02, 0x56, 0x47, 0x50, 0x41, 0x44};

[[nodiscard]] std::uint8_t encode_hat(const std::uint32_t buttons) noexcept {
  const bool up = (buttons & button_mask::dpad_up) != 0;
  const bool down = (buttons & button_mask::dpad_down) != 0;
  const bool left = (buttons & button_mask::dpad_left) != 0;
  const bool right = (buttons & button_mask::dpad_right) != 0;

  if ((up && down) || (left && right)) {
    return 8;  // Released.
  }
  if (up) {
    return right ? 1 : (left ? 7 : 0);
  }
  if (down) {
    return right ? 3 : (left ? 5 : 4);
  }
  return right ? 2 : (left ? 6 : 8);
}

[[nodiscard]] std::uint8_t to_stick(const std::int16_t value) noexcept {
  return static_cast<std::uint8_t>((static_cast<std::int32_t>(value) + 32768) >> 8);
}

// Vibeshine's vertical axes are positive-up; the device's are positive-down.
// Negating around 32768 keeps a centred stick on exactly 128, which games treat
// as the rest position; negating around 32767 lands on 127 and reads as a
// permanent slight tilt. The clamp catches INT16_MIN, whose negation overflows
// the 8-bit range.
[[nodiscard]] std::uint8_t to_stick_inverted(const std::int16_t value) noexcept {
  const std::int32_t scaled = (32768 - static_cast<std::int32_t>(value)) >> 8;
  return static_cast<std::uint8_t>(scaled > 255 ? 255 : (scaled < 0 ? 0 : scaled));
}

[[nodiscard]] std::int16_t clamp_i16(const std::int32_t value) noexcept {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return static_cast<std::int16_t>(value);
}

void pack_touch_point(ds4_touch_point *const point,
                      const bool active,
                      const std::uint8_t tracking_id,
                      const std::uint16_t x,
                      const std::uint16_t y) noexcept {
  // The high bit means "no contact"; the low seven bits are the tracking id.
  point->tracking_id = static_cast<std::uint8_t>((active ? 0x00u : 0x80u) | (tracking_id & 0x7Fu));
  point->coordinates[0] = static_cast<std::uint8_t>(x & 0xFFu);
  point->coordinates[1] =
    static_cast<std::uint8_t>(((x >> 8) & 0x0Fu) | ((y & 0x0Fu) << 4));
  point->coordinates[2] = static_cast<std::uint8_t>((y >> 4) & 0xFFu);
}

// Normalized 0..65535 maps onto the touchpad's own resolution.
[[nodiscard]] std::uint16_t to_touch_x(const std::uint16_t value) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * (k_ds4_touch_width - 1)) / 65535u);
}

[[nodiscard]] std::uint16_t to_touch_y(const std::uint16_t value) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value) * (k_ds4_touch_height - 1)) / 65535u);
}

}  // namespace

void ds4_state::reset() noexcept {
  report_counter = 0;
  timestamp = 0;
  touch_timestamp = 0;
  battery_level = 8;  // Mid scale until the client says otherwise.
  cable_connected = false;
  battery_full = false;
  for (int axis = 0; axis < 3; ++axis) {
    gyro[axis] = 0;
    accel[axis] = 0;
  }
  // A DS4 at rest reads one gravity on Y.
  accel[1] = static_cast<std::int16_t>(k_ds4_accel_counts_per_g);
  next_tracking_id = 1;
  for (std::uint8_t i = 0; i < k_ds4_touch_contacts; ++i) {
    contact_active[i] = false;
    contact_tracking_id[i] = 0;
    contact_x[i] = 0;
    contact_y[i] = 0;
  }
}

const std::uint8_t *ds4_descriptor(std::size_t *const size) noexcept {
  if (size != nullptr) {
    *size = sizeof(k_ds4_descriptor);
  }
  return k_ds4_descriptor;
}

ds4_input_report encode_ds4_input(
  const input_state_request &input,
  ds4_state *const state) noexcept {
  ds4_input_report report {};
  report.report_id = k_ds4_input_report_id;

  report.left_x = to_stick(input.left_x);
  report.left_y = to_stick_inverted(input.left_y);
  report.right_x = to_stick(input.right_x);
  report.right_y = to_stick_inverted(input.right_y);

  std::uint8_t buttons0 = encode_hat(input.buttons);
  if (input.buttons & button_mask::west) {
    buttons0 |= 0x10;  // Square
  }
  if (input.buttons & button_mask::south) {
    buttons0 |= 0x20;  // Cross
  }
  if (input.buttons & button_mask::east) {
    buttons0 |= 0x40;  // Circle
  }
  if (input.buttons & button_mask::north) {
    buttons0 |= 0x80;  // Triangle
  }
  report.buttons0 = buttons0;

  std::uint8_t buttons1 = 0;
  if (input.buttons & button_mask::left_shoulder) {
    buttons1 |= 0x01;
  }
  if (input.buttons & button_mask::right_shoulder) {
    buttons1 |= 0x02;
  }
  // A real DS4 sets the L2/R2 digital bits from the analog travel.
  if (input.left_trigger > 0) {
    buttons1 |= 0x04;
  }
  if (input.right_trigger > 0) {
    buttons1 |= 0x08;
  }
  if (input.buttons & button_mask::back) {
    buttons1 |= 0x10;  // Share
  }
  if (input.buttons & button_mask::start) {
    buttons1 |= 0x20;  // Options
  }
  if (input.buttons & button_mask::left_stick) {
    buttons1 |= 0x40;
  }
  if (input.buttons & button_mask::right_stick) {
    buttons1 |= 0x80;
  }
  report.buttons1 = buttons1;

  std::uint8_t buttons2 = 0;
  if (input.buttons & button_mask::home) {
    buttons2 |= 0x01;  // PS
  }
  if (input.buttons & (button_mask::touchpad | button_mask::misc)) {
    buttons2 |= 0x02;  // Touchpad click
  }
  if (state != nullptr) {
    state->report_counter = static_cast<std::uint8_t>((state->report_counter + 1) & 0x3F);
    buttons2 |= static_cast<std::uint8_t>(state->report_counter << 2);
  }
  report.buttons2 = buttons2;

  report.left_trigger = input.left_trigger;
  report.right_trigger = input.right_trigger;

  if (state != nullptr) {
    // Real hardware advances this every report; consumers use it to order
    // motion samples and to detect a stalled device.
    state->timestamp = static_cast<std::uint16_t>(state->timestamp + 188);
    report.timestamp = state->timestamp;

    report.gyro_x = state->gyro[0];
    report.gyro_y = state->gyro[1];
    report.gyro_z = state->gyro[2];
    report.accel_x = state->accel[0];
    report.accel_y = state->accel[1];
    report.accel_z = state->accel[2];

    report.battery = state->battery_level;
    std::uint8_t status = static_cast<std::uint8_t>(state->battery_level & 0x0F);
    if (state->cable_connected) {
      status |= 0x10;
    }
    if (state->battery_full && state->cable_connected) {
      status = 0x1B;
    }
    report.battery_status = status;

    const bool any_contact = state->contact_active[0] || state->contact_active[1];
    report.touch_packet_count = 1;
    state->touch_timestamp = static_cast<std::uint8_t>(state->touch_timestamp + 1);
    report.touch[0].timestamp = state->touch_timestamp;
    for (std::uint8_t i = 0; i < k_ds4_touch_contacts; ++i) {
      pack_touch_point(
        &report.touch[0].points[i],
        state->contact_active[i],
        state->contact_tracking_id[i],
        to_touch_x(state->contact_x[i]),
        to_touch_y(state->contact_y[i]));
    }
    if (!any_contact) {
      // Still send the packet so a consumer sees the release, but do not claim
      // additional stale packets.
      report.touch_packet_count = 1;
    }
  } else {
    report.battery = 8;
    report.battery_status = 0x08;
    report.accel_y = static_cast<std::int16_t>(k_ds4_accel_counts_per_g);
    for (auto &point : report.touch[0].points) {
      pack_touch_point(&point, false, 0, 0, 0);
    }
  }

  return report;
}

bool apply_ds4_touch(const touch_state_request &touch, ds4_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }

  const auto event = static_cast<touch_event>(touch.event_type);
  if (event == touch_event::cancel_all) {
    for (std::uint8_t i = 0; i < k_ds4_touch_contacts; ++i) {
      state->contact_active[i] = false;
    }
    return true;
  }

  if (touch.contact_index >= k_ds4_touch_contacts) {
    // The touchpad reports two contacts. Silently folding a third onto one of
    // them would produce a phantom jump, so it is dropped instead.
    return false;
  }

  const std::uint8_t index = touch.contact_index;
  switch (event) {
    case touch_event::down:
      if (!state->contact_active[index]) {
        state->contact_tracking_id[index] = state->next_tracking_id;
        state->next_tracking_id = static_cast<std::uint8_t>((state->next_tracking_id + 1) & 0x7F);
        if (state->next_tracking_id == 0) {
          state->next_tracking_id = 1;
        }
      }
      state->contact_active[index] = true;
      state->contact_x[index] = touch.x;
      state->contact_y[index] = touch.y;
      return true;
    case touch_event::move:
    case touch_event::hover:
      if (!state->contact_active[index]) {
        return false;
      }
      state->contact_x[index] = touch.x;
      state->contact_y[index] = touch.y;
      return true;
    case touch_event::up:
    case touch_event::cancel:
      state->contact_active[index] = false;
      return true;
    default:
      return false;
  }
}

bool apply_ds4_motion(const motion_state_request &motion, ds4_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }

  switch (static_cast<motion_kind>(motion.motion_type)) {
    case motion_kind::accelerometer: {
      // Milli-metres per second squared into counts, via gravity.
      const auto convert = [](const std::int32_t milli) {
        return clamp_i16((milli * k_ds4_accel_counts_per_g) / k_milli_g);
      };
      state->accel[0] = convert(motion.x_milli);
      state->accel[1] = convert(motion.y_milli);
      state->accel[2] = convert(motion.z_milli);
      return true;
    }
    case motion_kind::gyroscope: {
      // Milli-degrees per second into counts.
      const auto convert = [](const std::int32_t milli) {
        return clamp_i16((milli * k_ds4_gyro_counts_per_dps) / 1000);
      };
      state->gyro[0] = convert(motion.x_milli);
      state->gyro[1] = convert(motion.y_milli);
      state->gyro[2] = convert(motion.z_milli);
      return true;
    }
    default:
      return false;
  }
}

bool apply_ds4_battery(const battery_state_request &battery, ds4_state *const state) noexcept {
  if (state == nullptr) {
    return false;
  }

  const auto reported = static_cast<lvg::battery_state>(battery.flags);
  state->cable_connected =
    reported == lvg::battery_state::charging || reported == lvg::battery_state::full ||
    reported == lvg::battery_state::not_charging;
  state->battery_full = reported == lvg::battery_state::full;

  if (battery.percent <= 100) {
    // The DS4 reports 0..10 on battery and 0..11 on cable power.
    const std::uint32_t scale = state->cable_connected ? 11u : 10u;
    state->battery_level =
      static_cast<std::uint8_t>((static_cast<std::uint32_t>(battery.percent) * scale + 50u) / 100u);
  }
  return true;
}

bool decode_ds4_output(
  const ds4_output_report &output,
  playstation_output_feedback *const feedback) noexcept {
  if (feedback == nullptr || output.report_id != k_ds4_output_report_id) {
    return false;
  }

  *feedback = {};
  const bool rumble_valid = (output.flags & 0x01) != 0;
  const bool lightbar_valid = (output.flags & 0x02) != 0;

  // 8-bit motors widen to the 16-bit range the rest of the protocol uses.
  feedback->low_frequency =
    rumble_valid ? static_cast<std::uint16_t>(output.left_rumble << 8) : 0;
  feedback->high_frequency =
    rumble_valid ? static_cast<std::uint16_t>(output.right_rumble << 8) : 0;

  if (lightbar_valid) {
    feedback->red = output.red;
    feedback->green = output.green;
    feedback->blue = output.blue;
    feedback->valid |= ps_output_lightbar_valid;
  }
  return true;
}

std::size_t fill_ds4_feature(
  const std::uint8_t report_id,
  std::uint8_t *const buffer,
  const std::size_t capacity) noexcept {
  if (buffer == nullptr) {
    return 0;
  }

  switch (report_id) {
    case k_ds4_feature_calibration_id: {
      if (capacity < sizeof(k_ds4_calibration)) {
        return 0;
      }
      std::memcpy(buffer, k_ds4_calibration, sizeof(k_ds4_calibration));
      return sizeof(k_ds4_calibration);
    }
    case k_ds4_feature_pairing_id: {
      // Report id, the device address, then the host address slot.
      constexpr std::size_t k_size = 16;
      if (capacity < k_size) {
        return 0;
      }
      std::memset(buffer, 0, k_size);
      buffer[0] = k_ds4_feature_pairing_id;
      std::memcpy(buffer + 1, k_ds4_mac, sizeof(k_ds4_mac));
      return k_size;
    }
    case k_ds4_feature_firmware_id: {
      constexpr std::size_t k_size = 49;
      if (capacity < k_size) {
        return 0;
      }
      std::memset(buffer, 0, k_size);
      buffer[0] = k_ds4_feature_firmware_id;
      // A real device returns a build date string here. Consumers only parse
      // the version words that follow, so the string stays empty rather than
      // impersonating a specific factory build.
      buffer[35] = 0x00;
      buffer[36] = 0x01;  // Firmware version
      buffer[41] = 0x00;
      buffer[42] = 0x01;  // Hardware version
      return k_size;
    }
    default:
      return 0;
  }
}

feedback_event encode_playstation_feedback(
  const std::uint32_t controller_id,
  const playstation_output_feedback &feedback) noexcept {
  feedback_event event {};
  event.header.size = sizeof(event);
  event.header.version = k_protocol_version;
  event.controller_id = controller_id;
  event.type = feedback_type::playstation_output;
  event.payload_size = sizeof(feedback);
  std::memcpy(event.payload, &feedback, sizeof(feedback));
  return event;
}

}  // namespace lvg::driver
