// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#include "xbox_one.h"

#include <cstring>

namespace lvg::driver {
namespace {

// The Series descriptor without the Consumer Record block. Keeping the two in
// step by hand would be a standing hazard, so a test asserts that removing that
// one block from the Series descriptor reproduces this array exactly.
constexpr std::uint8_t k_xbox_one_descriptor[] = {
  0x05, 0x01,              // Usage Page (Generic Desktop)
  0x09, 0x05,              // Usage (Game Pad)
  0xA1, 0x01,              // Collection (Application)
  0x85, 0x01,              //   Report ID (1)
  0x09, 0x01,              //   Usage (Pointer)
  0xA1, 0x00,              //   Collection (Physical)
  0x09, 0x30,              //     Usage (X)
  0x09, 0x31,              //     Usage (Y)
  0x15, 0x00,              //     Logical Minimum (0)
  0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
  0x95, 0x02,              //     Report Count (2)
  0x75, 0x10,              //     Report Size (16)
  0x81, 0x02,              //     Input (Data, Variable, Absolute)
  0xC0,                    //   End Collection
  0x09, 0x01,              //   Usage (Pointer)
  0xA1, 0x00,              //   Collection (Physical)
  0x09, 0x32,              //     Usage (Z)
  0x09, 0x35,              //     Usage (Rz)
  0x15, 0x00,              //     Logical Minimum (0)
  0x27, 0xFF, 0xFF, 0x00, 0x00,  // Logical Maximum (65535)
  0x95, 0x02,              //     Report Count (2)
  0x75, 0x10,              //     Report Size (16)
  0x81, 0x02,              //     Input (Data, Variable, Absolute)
  0xC0,                    //   End Collection
  0x05, 0x02,              //   Usage Page (Simulation Controls)
  0x09, 0xC5,              //   Usage (Brake)
  0x15, 0x00,              //   Logical Minimum (0)
  0x26, 0xFF, 0x03,        //   Logical Maximum (1023)
  0x95, 0x01,              //   Report Count (1)
  0x75, 0x0A,              //   Report Size (10)
  0x81, 0x02,              //   Input (Data, Variable, Absolute)
  0x15, 0x00,              //   Logical Minimum (0)
  0x25, 0x00,              //   Logical Maximum (0)
  0x75, 0x06,              //   Report Size (6)
  0x95, 0x01,              //   Report Count (1)
  0x81, 0x03,              //   Input (Constant, Variable, Absolute)
  0x05, 0x02,              //   Usage Page (Simulation Controls)
  0x09, 0xC4,              //   Usage (Accelerator)
  0x15, 0x00,              //   Logical Minimum (0)
  0x26, 0xFF, 0x03,        //   Logical Maximum (1023)
  0x95, 0x01,              //   Report Count (1)
  0x75, 0x0A,              //   Report Size (10)
  0x81, 0x02,              //   Input (Data, Variable, Absolute)
  0x15, 0x00,              //   Logical Minimum (0)
  0x25, 0x00,              //   Logical Maximum (0)
  0x75, 0x06,              //   Report Size (6)
  0x95, 0x01,              //   Report Count (1)
  0x81, 0x03,              //   Input (Constant, Variable, Absolute)
  0x05, 0x01,              //   Usage Page (Generic Desktop)
  0x09, 0x39,              //   Usage (Hat switch)
  0x15, 0x01,              //   Logical Minimum (1)
  0x25, 0x08,              //   Logical Maximum (8)
  0x35, 0x00,              //   Physical Minimum (0)
  0x46, 0x3B, 0x01,        //   Physical Maximum (315)
  0x66, 0x14, 0x00,        //   Unit (English Rotation: Degrees)
  0x75, 0x04,              //   Report Size (4)
  0x95, 0x01,              //   Report Count (1)
  0x81, 0x42,              //   Input (Data, Variable, Absolute, Null state)
  0x75, 0x04,              //   Report Size (4)
  0x95, 0x01,              //   Report Count (1)
  0x15, 0x00,              //   Logical Minimum (0)
  0x25, 0x00,              //   Logical Maximum (0)
  0x35, 0x00,              //   Physical Minimum (0)
  0x45, 0x00,              //   Physical Maximum (0)
  0x65, 0x00,              //   Unit (None)
  0x81, 0x03,              //   Input (Constant, Variable, Absolute)
  0x05, 0x09,              //   Usage Page (Button)
  0x19, 0x01,              //   Usage Minimum (1)
  0x29, 0x0F,              //   Usage Maximum (15)
  0x15, 0x00,              //   Logical Minimum (0)
  0x25, 0x01,              //   Logical Maximum (1)
  0x75, 0x01,              //   Report Size (1)
  0x95, 0x0F,              //   Report Count (15)
  0x81, 0x02,              //   Input (Data, Variable, Absolute)
  0x15, 0x00,              //   Logical Minimum (0)
  0x25, 0x00,              //   Logical Maximum (0)
  0x75, 0x01,              //   Report Size (1)
  0x95, 0x01,              //   Report Count (1)
  0x81, 0x03,              //   Input (Constant, Variable, Absolute)

  // Rumble. Real hardware describes this with Physical Interface Device
  // usages, and four one-byte magnitudes: both trigger motors and both body
  // motors, gated by an actuator-enable mask.
  0x05, 0x0F,              //   Usage Page (Physical Interface Device)
  0x09, 0x21,              //   Usage (Set Effect Report)
  0x85, 0x03,              //   Report ID (3)
  0xA1, 0x02,              //   Collection (Logical)
  0x09, 0x97,              //     Usage (DC Enable Actuators)
  0x15, 0x00,              //     Logical Minimum (0)
  0x25, 0x01,              //     Logical Maximum (1)
  0x75, 0x04,              //     Report Size (4)
  0x95, 0x01,              //     Report Count (1)
  0x91, 0x02,              //     Output (Data, Variable, Absolute)
  0x15, 0x00,              //     Logical Minimum (0)
  0x25, 0x00,              //     Logical Maximum (0)
  0x75, 0x04,              //     Report Size (4)
  0x95, 0x01,              //     Report Count (1)
  0x91, 0x03,              //     Output (Constant, Variable, Absolute)
  0x09, 0x70,              //     Usage (Magnitude)
  0x15, 0x00,              //     Logical Minimum (0)
  0x25, 0x64,              //     Logical Maximum (100)
  0x75, 0x08,              //     Report Size (8)
  0x95, 0x04,              //     Report Count (4)
  0x91, 0x02,              //     Output (Data, Variable, Absolute)
  0x09, 0x50,              //     Usage (Duration)
  0x66, 0x01, 0x10,        //     Unit (SI Linear: Seconds)
  0x55, 0x0E,              //     Unit Exponent (-2)
  0x15, 0x00,              //     Logical Minimum (0)
  0x26, 0xFF, 0x00,        //     Logical Maximum (255)
  0x75, 0x08,              //     Report Size (8)
  0x95, 0x01,              //     Report Count (1)
  0x91, 0x02,              //     Output (Data, Variable, Absolute)
  0x09, 0xA7,              //     Usage (Start Delay)
  0x15, 0x00,              //     Logical Minimum (0)
  0x26, 0xFF, 0x00,        //     Logical Maximum (255)
  0x75, 0x08,              //     Report Size (8)
  0x95, 0x01,              //     Report Count (1)
  0x91, 0x02,              //     Output (Data, Variable, Absolute)
  0x65, 0x00,              //     Unit (None)
  0x55, 0x00,              //     Unit Exponent (0)
  0x09, 0x7C,              //     Usage (Loop Count)
  0x15, 0x00,              //     Logical Minimum (0)
  0x26, 0xFF, 0x00,        //     Logical Maximum (255)
  0x75, 0x08,              //     Report Size (8)
  0x95, 0x01,              //     Report Count (1)
  0x91, 0x02,              //     Output (Data, Variable, Absolute)
  0xC0,                    //   End Collection
  0xC0,                    // End Collection
};

// The Xbox One S entry appears in xinputhid.inf on its own, so unlike the
// Series pad this does not lean on the generic GIP software ID. That ID stays
// second as a fallback.
constexpr wchar_t k_xbox_one_hardware_ids[] =
  L"HID\\VID_045E&PID_02EA&IG_00\0"
  L"HID\\VID_045E&PID_02FF&IG_00\0"
  L"\0";

}  // namespace

const std::uint8_t *xbox_one_descriptor(std::size_t *const size) noexcept {
  if (size != nullptr) {
    *size = sizeof(k_xbox_one_descriptor);
  }
  return k_xbox_one_descriptor;
}

const wchar_t *xbox_one_hardware_ids(std::size_t *const bytes) noexcept {
  if (bytes != nullptr) {
    *bytes = sizeof(k_xbox_one_hardware_ids);
  }
  return k_xbox_one_hardware_ids;
}

xbox_one_input_report encode_xbox_one_input(const input_state_request &input) noexcept {
  // Built as a Series report and truncated. Duplicating the axis, hat and
  // button mapping would let the two drift apart for no gain, and the static
  // assertions in the header hold the shared prefix in place.
  const xbox_series_input_report full = encode_xbox_series_input(input);
  xbox_one_input_report report {};
  std::memcpy(&report, &full, sizeof(report));
  report.report_id = k_xbox_one_input_report_id;
  return report;
}

}  // namespace lvg::driver
