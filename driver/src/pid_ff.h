// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// DirectInput Physical Interface Device (PID) force feedback for the generic
// profile. Windows only reports DIDC_FORCEFEEDBACK for a HID device that
// publishes the PID report set, so a plain vendor-defined output report never
// reaches a DirectInput application. Everything here is pure logic so it can be
// exercised without the WDK.
//
// Report layouts and the descriptor must agree bit for bit. `pid_report_bits()`
// exists so a test can prove that rather than trusting review.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lvg::driver {

// Report IDs. 1 and 2 stay with the gamepad collection; PID owns the rest.
inline constexpr std::uint8_t k_pid_state_report_id = 3;
inline constexpr std::uint8_t k_pid_set_effect_report_id = 0x11;
inline constexpr std::uint8_t k_pid_set_envelope_report_id = 0x12;
inline constexpr std::uint8_t k_pid_set_condition_report_id = 0x13;
inline constexpr std::uint8_t k_pid_set_periodic_report_id = 0x14;
inline constexpr std::uint8_t k_pid_set_constant_force_report_id = 0x15;
inline constexpr std::uint8_t k_pid_set_ramp_force_report_id = 0x16;
inline constexpr std::uint8_t k_pid_effect_operation_report_id = 0x17;
inline constexpr std::uint8_t k_pid_block_free_report_id = 0x18;
inline constexpr std::uint8_t k_pid_device_control_report_id = 0x19;
inline constexpr std::uint8_t k_pid_device_gain_report_id = 0x1A;
inline constexpr std::uint8_t k_pid_create_new_effect_report_id = 0x1B;
inline constexpr std::uint8_t k_pid_block_load_report_id = 0x1C;
inline constexpr std::uint8_t k_pid_pool_report_id = 0x1D;

// DirectInput normalizes force magnitudes to +/-10000 (DI_FFNOMINALMAX).
inline constexpr std::int16_t k_pid_nominal_max = 10000;

// Effect block indices are 1-based; index 0 means "no effect".
inline constexpr std::uint8_t k_pid_max_effects = 16;

// Effect types, as ordinals in the descriptor's Effect Type array.
enum class pid_effect_type : std::uint8_t {
  none = 0,
  constant_force = 1,
  ramp = 2,
  square = 3,
  sine = 4,
  triangle = 5,
  sawtooth_up = 6,
  sawtooth_down = 7,
  spring = 8,
  damper = 9,
  inertia = 10,
  friction = 11,
  custom_force_data = 12,
};

enum class pid_effect_operation : std::uint8_t {
  start = 1,
  start_solo = 2,
  stop = 3,
};

enum class pid_device_control : std::uint8_t {
  enable_actuators = 1,
  disable_actuators = 2,
  stop_all_effects = 3,
  device_reset = 4,
  device_pause = 5,
  device_continue = 6,
};

enum class pid_block_load_status : std::uint8_t {
  success = 1,
  full = 2,
  error = 3,
};

#pragma pack(push, 1)

struct pid_state_report {
  std::uint8_t report_id;
  // bit 0 device paused, 1 actuators enabled, 2 safety switch, 3 actuator
  // override switch, 4 actuator power, bits 5-7 padding.
  std::uint8_t flags;
  // bit 0 effect playing, bits 1-7 effect block index.
  std::uint8_t playing_and_index;
};

struct pid_set_effect_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint8_t effect_type;
  std::uint16_t duration_ms;
  std::uint16_t trigger_repeat_interval_ms;
  std::uint16_t sample_period_ms;
  std::uint8_t gain;
  std::uint8_t trigger_button;
  // bit 0 X enable, 1 Y enable, 2 direction enable, bits 3-7 padding.
  std::uint8_t axes_and_direction_enable;
  std::uint8_t direction_x;
  std::uint8_t direction_y;
  std::uint16_t start_delay_ms;
};

struct pid_set_envelope_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint16_t attack_level;
  std::uint16_t fade_level;
  std::uint16_t attack_time_ms;
  std::uint16_t fade_time_ms;
};

struct pid_set_condition_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint8_t parameter_block_offset;
  std::int16_t cp_offset;
  std::int16_t positive_coefficient;
  std::int16_t negative_coefficient;
  std::uint16_t positive_saturation;
  std::uint16_t negative_saturation;
  std::uint16_t dead_band;
};

struct pid_set_periodic_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint16_t magnitude;
  std::int16_t offset;
  std::uint16_t phase;
  std::uint16_t period_ms;
};

struct pid_set_constant_force_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::int16_t magnitude;
};

struct pid_set_ramp_force_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::int16_t ramp_start;
  std::int16_t ramp_end;
};

struct pid_effect_operation_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint8_t operation;
  std::uint8_t loop_count;
};

struct pid_block_free_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
};

struct pid_device_control_report {
  std::uint8_t report_id;
  std::uint8_t control;
};

struct pid_device_gain_report {
  std::uint8_t report_id;
  std::uint8_t device_gain;
};

struct pid_create_new_effect_report {
  std::uint8_t report_id;
  std::uint8_t effect_type;
  std::uint16_t byte_count;
};

struct pid_block_load_report {
  std::uint8_t report_id;
  std::uint8_t effect_block_index;
  std::uint8_t block_load_status;
  std::uint16_t ram_pool_available;
};

struct pid_pool_report {
  std::uint8_t report_id;
  std::uint16_t ram_pool_size;
  std::uint8_t simultaneous_effects_max;
  // bit 0 device managed pool, bit 1 shared parameter blocks, bits 2-7 padding.
  std::uint8_t pool_flags;
};

#pragma pack(pop)

static_assert(sizeof(pid_state_report) == 3);
static_assert(sizeof(pid_set_effect_report) == 16);
static_assert(sizeof(pid_set_envelope_report) == 10);
static_assert(sizeof(pid_set_condition_report) == 15);
static_assert(sizeof(pid_set_periodic_report) == 10);
static_assert(sizeof(pid_set_constant_force_report) == 4);
static_assert(sizeof(pid_set_ramp_force_report) == 6);
static_assert(sizeof(pid_effect_operation_report) == 4);
static_assert(sizeof(pid_block_free_report) == 2);
static_assert(sizeof(pid_device_control_report) == 2);
static_assert(sizeof(pid_device_gain_report) == 2);
static_assert(sizeof(pid_create_new_effect_report) == 4);
static_assert(sizeof(pid_block_load_report) == 5);
static_assert(sizeof(pid_pool_report) == 5);

// The PID force-feedback gamepad report descriptor.
[[nodiscard]] const std::uint8_t *pid_gamepad_descriptor(std::size_t *size) noexcept;

// The rumble a host application is currently asking for, normalized the same
// way the vendor-defined output report already is.
struct pid_rumble_t {
  std::uint16_t low_frequency;
  std::uint16_t high_frequency;
};

// One host-allocated effect block.
struct pid_effect_t {
  bool allocated;
  bool playing;
  pid_effect_type type;
  std::uint16_t duration_ms;  // 0 means infinite.
  std::uint16_t start_delay_ms;
  std::uint8_t gain;
  std::uint8_t loop_count;
  std::int16_t constant_magnitude;
  std::uint16_t periodic_magnitude;
  std::int16_t ramp_start;
  std::int16_t ramp_end;
  std::uint16_t envelope_attack_level;
  std::uint16_t envelope_fade_level;
  std::uint16_t envelope_attack_ms;
  std::uint16_t envelope_fade_ms;
  // Milliseconds of playback elapsed since the last start, including delay.
  std::uint32_t elapsed_ms;
};

// Tracks the host's effect blocks and reduces them to a single rumble value.
//
// This deliberately models only what a rumble actuator can reproduce: constant
// force, ramps, and the magnitude envelope of periodic effects. Condition
// effects (spring, damper, inertia, friction) describe forces that depend on
// stick position and have no meaning for a rumble motor, so they are accepted
// and ignored rather than turned into misleading vibration.
class pid_engine {
 public:
  void reset() noexcept;

  // Feature report handling. `create_new_effect` allocates a block; the host
  // then reads `block_load` to learn which index it received.
  [[nodiscard]] bool create_new_effect(const pid_create_new_effect_report &request) noexcept;
  [[nodiscard]] pid_block_load_report block_load() const noexcept;
  [[nodiscard]] pid_pool_report pool() const noexcept;
  [[nodiscard]] pid_state_report state() const noexcept;

  // Output report handling. Each returns true when the report was understood.
  [[nodiscard]] bool set_effect(const pid_set_effect_report &report) noexcept;
  [[nodiscard]] bool set_envelope(const pid_set_envelope_report &report) noexcept;
  [[nodiscard]] bool set_condition(const pid_set_condition_report &report) noexcept;
  [[nodiscard]] bool set_periodic(const pid_set_periodic_report &report) noexcept;
  [[nodiscard]] bool set_constant_force(const pid_set_constant_force_report &report) noexcept;
  [[nodiscard]] bool set_ramp_force(const pid_set_ramp_force_report &report) noexcept;
  [[nodiscard]] bool effect_operation(const pid_effect_operation_report &report) noexcept;
  [[nodiscard]] bool block_free(const pid_block_free_report &report) noexcept;
  [[nodiscard]] bool device_control(const pid_device_control_report &report) noexcept;
  [[nodiscard]] bool device_gain(const pid_device_gain_report &report) noexcept;

  // Advances playback. Finite effects stop on their own so a host that never
  // sends an explicit stop cannot leave the motors running.
  void advance(std::uint32_t elapsed_ms) noexcept;

  // Returns true while any effect still needs the clock.
  [[nodiscard]] bool needs_tick() const noexcept;

  [[nodiscard]] pid_rumble_t rumble() const noexcept;

 private:
  [[nodiscard]] pid_effect_t *effect(std::uint8_t block_index) noexcept;
  [[nodiscard]] const pid_effect_t *effect(std::uint8_t block_index) const noexcept;
  void stop_all() noexcept;

  pid_effect_t effects_[k_pid_max_effects] {};
  std::uint8_t last_allocated_ {0};
  std::uint8_t last_load_status_ {static_cast<std::uint8_t>(pid_block_load_status::success)};
  std::uint8_t device_gain_ {255};
  bool actuators_enabled_ {true};
  bool paused_ {false};
};

// Total bit count each report occupies in the descriptor, for the layout test.
struct pid_report_bits_t {
  std::uint8_t report_id;
  std::size_t input_bits;
  std::size_t output_bits;
  std::size_t feature_bits;
};

[[nodiscard]] const pid_report_bits_t *pid_report_bits(std::size_t *count) noexcept;

}  // namespace lvg::driver
