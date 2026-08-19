// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// The control-device ABI shared by Vibeshine and the UMDF VHF source driver.
// Keep every request fixed-size and METHOD_BUFFERED. Do not add pointers,
// handles, strings, or compiler-sized fields to this wire contract.

#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>
#include <winioctl.h>

namespace lvg {

inline constexpr std::uint16_t k_protocol_version = 1;
inline constexpr std::uint32_t k_max_controllers = 16;

// This is the source device's private control interface, never the HID child
// interface. Both the Vibeshine client and driver compile this same value.
inline constexpr GUID k_device_interface_guid {
  0x27debbf5,
  0x1d1e,
  0x4e9c,
  {0x90, 0x6d, 0xd1, 0x04, 0xb1, 0x41, 0x8b, 0x2b}
};

inline constexpr wchar_t k_root_hardware_id[] = L"ROOT\\VIBESHINEVIRTUALGAMEPAD";

enum class profile : std::uint16_t {
  generic_hid = 1,
  xbox_360 = 2,
  xbox_one = 3,
  xbox_series = 4,
  dualshock_4 = 5,
  dualsense = 6,
  switch_pro = 7,
  // A generic HID game pad that also publishes the DirectInput Physical
  // Interface Device report set, so DirectInput applications see a
  // force-feedback capable device instead of a rumble-less pad.
  generic_pid = 8,
};

enum button_mask : std::uint32_t {
  // These values are deliberately identical to Vibeshine's existing Windows
  // platform contract. Keeping the normalized ABI stable makes the adapter a
  // field copy rather than a brittle semantic remapping.
  dpad_up = 0x000001u,
  dpad_down = 0x000002u,
  dpad_left = 0x000004u,
  dpad_right = 0x000008u,
  start = 0x000010u,
  back = 0x000020u,
  left_stick = 0x000040u,
  right_stick = 0x000080u,
  left_shoulder = 0x000100u,
  right_shoulder = 0x000200u,
  home = 0x000400u,
  south = 0x001000u,
  east = 0x002000u,
  west = 0x004000u,
  north = 0x008000u,
  paddle_1 = 0x010000u,
  paddle_2 = 0x020000u,
  paddle_3 = 0x040000u,
  paddle_4 = 0x080000u,
  touchpad = 0x100000u,
  misc = 0x200000u,
};

enum class feedback_type : std::uint16_t {
  none = 0,
  generic_rumble_rgb = 1,
  raw_output_report = 2,
  // Rumble without a light. The payload is still generic_rumble_rgb_feedback,
  // but its colour channels carry no meaning: a DirectInput PID effect says
  // nothing about an LED, and reporting one would make a client turn a real
  // controller's light off every time a game started a force.
  generic_rumble = 3,
  // All four Xbox actuators in one event. The driver keeps a single pending
  // feedback slot per controller, so body and trigger rumble have to travel
  // together or one of them would be dropped by coalescing.
  xbox_rumble = 4,
};

using profile_mask_t = std::uint32_t;

[[nodiscard]] constexpr profile_mask_t profile_bit(const profile value) noexcept {
  const auto index = static_cast<std::uint16_t>(value);
  return index == 0 || index > 32 ? 0u : (1u << (index - 1));
}

enum feature_mask : std::uint32_t {
  feature_input_state = 1u << 0,
  feature_feedback = 1u << 1,
  feature_touch = 1u << 2,
  feature_motion = 1u << 3,
  feature_battery = 1u << 4,
  feature_hid_feature_reports = 1u << 5,
};

enum class raw_hid_operation : std::uint8_t {
  write_report = 1,
  set_feature = 2,
  get_feature = 3,
};

#pragma pack(push, 1)

struct request_header {
  std::uint32_t size;
  std::uint16_t version;
  std::uint16_t reserved;
};

struct create_controller_request {
  request_header header;
  std::uint32_t controller_id;
  profile requested_profile;
  std::uint16_t reserved;
};

struct query_info_request {
  request_header header;
};

struct query_info_response {
  request_header header;
  std::uint16_t minimum_protocol_version;
  std::uint16_t maximum_protocol_version;
  profile_mask_t available_profiles;
  std::uint32_t available_features;
  std::uint32_t maximum_controllers;
  std::uint32_t reserved;
};

struct controller_id_request {
  request_header header;
  std::uint32_t controller_id;
};

struct input_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint32_t buttons;
  std::int16_t left_x;
  std::int16_t left_y;
  std::int16_t right_x;
  std::int16_t right_y;
  std::uint8_t left_trigger;
  std::uint8_t right_trigger;
  std::uint16_t reserved;
};

// Coordinates and pressure are normalized to 0..65535 by the client adapter.
// contact_index is a profile-independent logical contact, not a platform
// pointer identifier.
struct touch_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t contact_index;
  std::uint8_t event_type;
  std::uint16_t x;
  std::uint16_t y;
  std::uint16_t pressure;
  std::uint16_t reserved;
};

// Motion uses signed milli-units: m/s^2 * 1000 for accelerometers and degrees
// per second * 1000 for gyroscopes. The profile implementation owns any
// device-specific scaling and calibration.
struct motion_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t motion_type;
  std::uint8_t reserved0[3];
  std::int32_t x_milli;
  std::int32_t y_milli;
  std::int32_t z_milli;
};

struct battery_state_request {
  request_header header;
  std::uint32_t controller_id;
  std::uint8_t percent;
  std::uint8_t flags;
  std::uint16_t reserved;
};

struct generic_rumble_rgb_feedback {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
  std::uint8_t reserved;
};

struct xbox_rumble_feedback {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
  std::uint16_t left_trigger;
  std::uint16_t right_trigger;
};

struct raw_hid_report_feedback {
  raw_hid_operation operation;
  std::uint8_t report_id;
  std::uint8_t data_size;
  std::uint8_t reserved;
  std::uint8_t data[12];
};

struct feedback_event {
  request_header header;
  std::uint32_t controller_id;
  feedback_type type;
  std::uint16_t payload_size;
  std::uint8_t payload[16];
};

#pragma pack(pop)

static_assert(sizeof(request_header) == 8);
static_assert(sizeof(create_controller_request) == 16);
static_assert(sizeof(query_info_request) == 8);
static_assert(sizeof(query_info_response) == 28);
static_assert(sizeof(controller_id_request) == 12);
static_assert(sizeof(input_state_request) == 28);
static_assert(sizeof(touch_state_request) == 22);
static_assert(sizeof(motion_state_request) == 28);
static_assert(sizeof(battery_state_request) == 16);
static_assert(sizeof(generic_rumble_rgb_feedback) == 8);
static_assert(sizeof(xbox_rumble_feedback) == 8);
static_assert(sizeof(raw_hid_report_feedback) == 16);
static_assert(sizeof(feedback_event) == 32);

inline constexpr DWORD ioctl_query_info =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_create_controller =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_destroy_controller =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_input_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_poll_feedback =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_touch_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_motion_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);
inline constexpr DWORD ioctl_submit_battery_state =
  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA);

template<class request_t>
[[nodiscard]] inline bool valid_request(const request_t *request, const std::size_t bytes) {
  return request != nullptr && bytes == sizeof(request_t) &&
         request->header.size == sizeof(request_t) &&
         request->header.version == k_protocol_version &&
         request->header.reserved == 0;
}

}  // namespace lvg
