// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "libvirtualgamepad/keyboard_mouse.h"

namespace lvg::driver {
// Pure producer core. The caller serializes it under its state lock. It owns no
// WDF objects; it must outlive every retained VHF report. This is intentionally
// connected by the native UMDF transport; Windows runtime qualification and
// release packaging remain separate from these portable contracts.
class keyboard_mouse_endpoint {
 public:
  using owner_token = std::uintptr_t; // Internal identity; never sent over ABI.
  static constexpr std::size_t queue_capacity = 64;
  enum class result { accepted, unchanged, invalid, not_owner, not_ready, busy, unsupported };
  enum class state { closed, active, closing, drained, faulted };
  struct report {
    std::array<std::uint8_t, 30> bytes {};
    std::uint8_t length {};
    std::uint8_t id {};
  };

  result open(owner_token owner, std::uint32_t verified_absolute_generation = 0) noexcept;
  result submit(owner_token owner, const km::keyboard_request &request) noexcept;
  result submit(owner_token owner, const km::consumer_request &request) noexcept;
  result submit(owner_token owner, const km::button_request &request) noexcept;
  result submit(owner_token owner, const km::relative_request &request) noexcept;
  result submit(owner_token owner, const km::absolute_request &request) noexcept;
  // Discard queued input, then append neutral keyboard/buttons, preserving the
  // in-flight report until readiness. Remain closing until both are consumed.
  result close(owner_token owner) noexcept;
  // VHF's next-read readiness retires the prior retained report and grants one
  // submission. Only call from the real readiness boundary, including first read.
  void ready_for_report() noexcept;
  [[nodiscard]] const report *take_report() noexcept;
  // An uncertain transport/VHF failure never grants a new credit or permits a
  // SendInput replay. The owner must synchronously delete the HID child first.
  void submission_failed() noexcept;
  void reset_after_device_deleted() noexcept;
  [[nodiscard]] state lifecycle() const noexcept { return state_; }
  [[nodiscard]] std::size_t pending() const noexcept { return count_; }
  [[nodiscard]] std::uint32_t absolute_generation() const noexcept { return absolute_generation_; }

 private:
  result admission(owner_token owner) const noexcept;
  void enqueue(const report &value) noexcept;
  report keyboard_report() const noexcept;
  report consumer_report() const noexcept;
  report relative_report(std::int16_t x = 0, std::int16_t y = 0, std::int8_t wheel = 0, std::int8_t pan = 0) const noexcept;
  owner_token owner_ {};
  state state_ {state::closed};
  std::uint32_t absolute_generation_ {};
  std::array<std::uint8_t, km::k_keyboard_bytes> keys_ {};
  std::uint8_t buttons_ {};
  std::uint8_t consumer_buttons_ {};
  std::array<report, queue_capacity> queue_ {};
  std::size_t head_ {}, count_ {};
  report retained_ {};
  bool credit_ {};
};

// Three top-level collections: keyboard, mouse (relative and absolute reports
// each carry button state), and consumer controls. No other VID/PID is borrowed.
extern const std::uint8_t keyboard_mouse_descriptor[];
extern const std::size_t keyboard_mouse_descriptor_size;
} // namespace lvg::driver
