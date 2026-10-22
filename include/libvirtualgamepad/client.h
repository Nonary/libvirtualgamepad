// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include "libvirtualgamepad/protocol.h"
#include "libvirtualgamepad/keyboard_mouse_control.h"

namespace lvg {

// Opens the driver's private WDF device interface, never a HID child path.
// A connection owns the controller slots it creates. Closing it releases those
// slots in the driver even if the caller did not explicitly destroy them.
//
// A controller is deleted when the machine sleeps, hibernates, or shuts down.
// Its next submit or poll returns ERROR_DEVICE_REMOVED; destroy_controller()
// then create_controller() on the same slot can restore it after resume.
// Creation may return ERROR_BUSY until PnP removes the previous child; callers
// should retry with their own bounded backoff, not block a suspend callback.
class client final {
 public:
  client() noexcept = default;
  ~client() noexcept;

  client(const client &) = delete;
  client &operator=(const client &) = delete;

  client(client &&other) noexcept;
  client &operator=(client &&other) noexcept;

  // Finds the first present Vibeshine VHF source-device interface and opens it
  // with the access required by the protocol IOCTLs. Returns ERROR_SUCCESS or
  // a Win32 error code.
  [[nodiscard]] DWORD connect() noexcept;
  void close() noexcept;

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] profile_mask_t available_profiles() const noexcept;
  [[nodiscard]] std::uint32_t available_features() const noexcept;
  [[nodiscard]] std::uint32_t maximum_controllers() const noexcept;

  [[nodiscard]] DWORD create_controller(
    std::uint32_t controller_id,
    profile requested_profile) noexcept;
  [[nodiscard]] DWORD destroy_controller(std::uint32_t controller_id) noexcept;
  [[nodiscard]] DWORD submit_input_state(const input_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_touch_state(const touch_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_motion_state(const motion_state_request &input) noexcept;
  [[nodiscard]] DWORD submit_battery_state(const battery_state_request &input) noexcept;

  // Returns ERROR_NO_MORE_ITEMS when the driver's bounded feedback slot has
  // no fresh output. It does not block the streaming path.
  [[nodiscard]] DWORD poll_feedback(
    std::uint32_t controller_id,
    feedback_event *event) noexcept;

  // Optional source-stage extension on this same owned control handle. No
  // protocol2/released capability or native identity qualification is implied.
  // The client (including connect/close/move) requires external serialization.
  [[nodiscard]] DWORD query_keyboard_mouse(km::query_response *response) noexcept;
  [[nodiscard]] DWORD open_keyboard_mouse() noexcept;
  // ERROR_BUSY means neutral drain is incomplete; retry boundedly. Success
  // means the child was synchronously removed after both neutrals retired.
  [[nodiscard]] DWORD close_keyboard_mouse() noexcept;
  // Force-remove only this owned KM child without closing controller slots on
  // the shared handle. Use after uncertain input/close timeout; no neutral proof.
  [[nodiscard]] DWORD abort_keyboard_mouse() noexcept;
  // ERROR_BUSY rejects before mutation. Any other transport failure after
  // acceptance is uncertain; never replay through SendInput before teardown.
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::keyboard_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::button_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::relative_request &input) noexcept;
  [[nodiscard]] DWORD submit_keyboard_mouse(const km::absolute_request &input) noexcept;

 private:
  [[nodiscard]] DWORD issue_keyboard_mouse_submission(
    DWORD control_code, void *input, DWORD input_size) noexcept;
  [[nodiscard]] DWORD query_info() noexcept;
  [[nodiscard]] DWORD issue(
    DWORD control_code,
    void *input,
    DWORD input_size,
    void *output,
    DWORD output_size,
    DWORD *bytes_returned = nullptr) noexcept;

  HANDLE handle_ {INVALID_HANDLE_VALUE};
  query_info_response info_ {};
};

}  // namespace lvg
