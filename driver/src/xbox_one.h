// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// Xbox One-shaped HID gamepad.
//
// The same filter route as the Series pad, with one difference that matters for
// how reliably it attaches: xinputhid.inf lists PID_02EA outright, while the
// Series pad's PID_0B12 is absent and reaches the filter only through the
// generic GIP software ID. So this profile matches on its own identity and
// keeps the generic ID as a second entry rather than depending on it.
//
// An Xbox One pad has no Share button. That is the whole difference in the
// report: the Consumer Record usage the Series descriptor carries is gone, and
// the report is a byte shorter. Everything else - unsigned 16-bit sticks,
// 10-bit triggers, the 1-based hat, the fifteen buttons with the vendor's
// gaps, and the four-actuator rumble - is shared, and a test asserts the two
// descriptors stay identical outside that one block.

#pragma once

#include <cstddef>
#include <cstdint>

#include "libvirtualgamepad/protocol.h"
#include "xbox_series.h"

namespace lvg::driver {

inline constexpr std::uint8_t k_xbox_one_input_report_id = k_xbox_series_input_report_id;
inline constexpr std::uint8_t k_xbox_one_output_report_id = k_xbox_series_output_report_id;

// The Xbox One S controller. Listed directly in xinputhid.inf.
inline constexpr std::uint16_t k_xbox_one_product_id = 0x02EA;
inline constexpr std::uint16_t k_xbox_one_version = 0x0408;

#pragma pack(push, 1)

// The Series report without its trailing Share byte.
struct xbox_one_input_report {
  std::uint8_t report_id;
  std::uint16_t left_x;
  std::uint16_t left_y;
  std::uint16_t right_x;
  std::uint16_t right_y;
  std::uint16_t left_trigger;
  std::uint16_t right_trigger;
  std::uint8_t hat;
  std::uint16_t buttons;
};

#pragma pack(pop)

static_assert(sizeof(xbox_one_input_report) == 16);
static_assert(sizeof(xbox_one_input_report) + 1 == sizeof(xbox_series_input_report));

// Every shared field has to sit where the Series report puts it, because the
// encoder below builds a Series report and keeps this prefix of it.
static_assert(offsetof(xbox_one_input_report, left_x) == offsetof(xbox_series_input_report, left_x));
static_assert(offsetof(xbox_one_input_report, left_y) == offsetof(xbox_series_input_report, left_y));
static_assert(offsetof(xbox_one_input_report, right_x) == offsetof(xbox_series_input_report, right_x));
static_assert(offsetof(xbox_one_input_report, right_y) == offsetof(xbox_series_input_report, right_y));
static_assert(offsetof(xbox_one_input_report, left_trigger) ==
              offsetof(xbox_series_input_report, left_trigger));
static_assert(offsetof(xbox_one_input_report, right_trigger) ==
              offsetof(xbox_series_input_report, right_trigger));
static_assert(offsetof(xbox_one_input_report, hat) == offsetof(xbox_series_input_report, hat));
static_assert(offsetof(xbox_one_input_report, buttons) ==
              offsetof(xbox_series_input_report, buttons));

// An Xbox One pad's rumble report is the Series one, so the same type and the
// same decoder serve both.
using xbox_one_output_report = xbox_series_output_report;

[[nodiscard]] const std::uint8_t *xbox_one_descriptor(std::size_t *size) noexcept;

[[nodiscard]] const wchar_t *xbox_one_hardware_ids(std::size_t *bytes) noexcept;

[[nodiscard]] xbox_one_input_report encode_xbox_one_input(
  const input_state_request &input) noexcept;

}  // namespace lvg::driver
