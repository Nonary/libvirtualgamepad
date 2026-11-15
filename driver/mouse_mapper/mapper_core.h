// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include <libvirtualgamepad/mouse_transport.h>

namespace lvg::mouse_mapper {
// Values are the public ntddmou.h MOUSE_INPUT_DATA contract. Keeping this
// allocation-free core independent of WDK headers lets tests execute the exact
// conversion and backpressure state used by the kernel driver.
struct packet {
  mouse_transport::uint16_t flags;
  mouse_transport::uint16_t button_flags;
  mouse_transport::uint16_t button_data;
  mouse_transport::int32_t x;
  mouse_transport::int32_t y;
};
enum class verdict { accepted, busy, invalid, sequence_error, faulted };
class stream {
public:
  void reset() noexcept { *this = stream{}; }
  void fault() noexcept { failed_ = true; }
  bool failed() const noexcept { return failed_; }
  bool empty() const noexcept { return first_ == count_; }
  unsigned remaining() const noexcept { return count_ - first_; }
  const packet *data() const noexcept { return packets_ + first_; }
  // An impossible MouseClass consumption count is terminal, never a clamp
  // that could silently drop input or permit the next report to overtake it.
  bool consume(unsigned count) noexcept {
    if (count > remaining()) { fault(); return false; }
    first_ += count;
    return true;
  }
  verdict admit(const mouse_transport::report &report) noexcept {
    if (failed_) return verdict::faulted;
    if (!empty()) return verdict::busy;
    if (!mouse_transport::valid(report)) {
      fault(); return verdict::invalid;
    }
    // The first report establishes the epoch: the mapper may start after its
    // VHF producer or resume independently. Normal reports are contiguous,
    // including UINT32_MAX -> 1. Only an explicit, validated neutral close
    // marker explains a forward gap caused by purging queued mouse input.
    // Duplicate/backward/ambiguous half-range order is always terminal.
    const auto delta = static_cast<mouse_transport::uint32_t>(report.sequence - sequence_);
    if (sequence_ && (delta == 0 || delta >= 0x80000000u ||
        (report.sequence != mouse_transport::next_sequence(sequence_) && report.flags != mouse_transport::k_purge))) {
      fault(); return verdict::sequence_error;
    }
    first_ = count_ = 0;
    mouse_transport::uint16_t edges = 0;
    for (unsigned bit = 0; bit != 5; ++bit) {
      const unsigned mask = 1u << bit;
      if ((buttons_ ^ report.buttons) & mask)
        edges |= static_cast<mouse_transport::uint16_t>(1u << (bit * 2 + ((report.buttons & mask) ? 0 : 1)));
    }
    const bool absolute = report.flags == mouse_transport::k_absolute_flags;
    if (edges || report.x || report.y || absolute)
      packets_[count_++] = {static_cast<mouse_transport::uint16_t>(absolute ? 0x0003 : 0), edges, 0, report.x, report.y};
    // ButtonData is shared by wheel and horizontal-wheel flags. Emit separate
    // packets, after motion/button edges, to preserve both signed magnitudes.
    if (report.wheel)
      packets_[count_++] = {0, 0x0400, static_cast<mouse_transport::uint16_t>(report.wheel), 0, 0};
    if (report.pan)
      packets_[count_++] = {0, 0x0800, static_cast<mouse_transport::uint16_t>(report.pan), 0, 0};
    buttons_ = report.buttons;
    sequence_ = report.sequence;
    return verdict::accepted;
  }
private:
  packet packets_[3]{};
  unsigned first_{};
  unsigned count_{};
  mouse_transport::uint32_t sequence_{};
  mouse_transport::uint8_t buttons_{};
  bool failed_{};
};
} // namespace lvg::mouse_mapper
