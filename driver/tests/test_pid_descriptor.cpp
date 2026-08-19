// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
//
// A malformed HID report descriptor does not fail loudly: VhfCreate still
// succeeds and the HID child simply never starts, which looks like a driver
// bug from the host side. This walks the descriptor the way HIDCLASS does and
// proves that every report's bit layout matches the packed struct the driver
// memcpys into, so that class of error is caught at build time instead of on a
// test machine.
//
// Build and run standalone:
//   g++ -std=c++20 -I driver/src -I include
//     driver/tests/test_pid_descriptor.cpp driver/src/pid_ff.cpp -o pid_test

#include "pid_ff.h"
#include "xbox_series.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(const bool condition, const std::string &what) {
  if (!condition) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

struct report_bits_t {
  std::size_t input = 0;
  std::size_t output = 0;
  std::size_t feature = 0;
};

struct walk_result_t {
  bool well_formed = true;
  std::string error;
  int collection_depth = 0;
  int max_collection_depth = 0;
  std::map<std::uint8_t, report_bits_t> reports;
  std::size_t item_count = 0;
  bool saw_pid_usage_page = false;
  int top_level_collections = 0;
};

// Walks short HID items. The descriptor deliberately contains no long items.
walk_result_t walk(const std::uint8_t *data, const std::size_t size) {
  walk_result_t result;

  std::size_t report_size = 0;
  std::size_t report_count = 0;
  std::uint8_t report_id = 0;
  std::uint32_t usage_page = 0;

  std::size_t offset = 0;
  while (offset < size) {
    const std::uint8_t prefix = data[offset];
    if (prefix == 0xFE) {
      result.well_formed = false;
      result.error = "long items are not supported by this descriptor";
      return result;
    }

    const std::uint8_t tag = static_cast<std::uint8_t>((prefix >> 4) & 0x0F);
    const std::uint8_t type = static_cast<std::uint8_t>((prefix >> 2) & 0x03);
    const std::uint8_t raw_size = static_cast<std::uint8_t>(prefix & 0x03);
    const std::size_t data_size = raw_size == 3 ? 4 : raw_size;

    if (offset + 1 + data_size > size) {
      result.well_formed = false;
      result.error = "item at offset " + std::to_string(offset) + " runs past the end";
      return result;
    }

    std::uint32_t value = 0;
    for (std::size_t i = 0; i < data_size; ++i) {
      value |= static_cast<std::uint32_t>(data[offset + 1 + i]) << (8 * i);
    }

    ++result.item_count;

    if (type == 0) {  // Main
      switch (tag) {
        case 0x8:  // Input
        case 0x9:  // Output
        case 0xB: {  // Feature
          if (report_size == 0 || report_count == 0) {
            result.well_formed = false;
            result.error = "main item at offset " + std::to_string(offset) +
                           " has no report size/count";
            return result;
          }
          if (result.collection_depth == 0) {
            result.well_formed = false;
            result.error = "main item at offset " + std::to_string(offset) +
                           " is outside any collection";
            return result;
          }
          const std::size_t bits = report_size * report_count;
          auto &bucket = result.reports[report_id];
          if (tag == 0x8) {
            bucket.input += bits;
          } else if (tag == 0x9) {
            bucket.output += bits;
          } else {
            bucket.feature += bits;
          }
          break;
        }
        case 0xA:  // Collection
          if (result.collection_depth == 0) {
            ++result.top_level_collections;
          }
          ++result.collection_depth;
          if (result.collection_depth > result.max_collection_depth) {
            result.max_collection_depth = result.collection_depth;
          }
          break;
        case 0xC:  // End Collection
          --result.collection_depth;
          if (result.collection_depth < 0) {
            result.well_formed = false;
            result.error = "unbalanced End Collection at offset " + std::to_string(offset);
            return result;
          }
          break;
        default:
          break;
      }
    } else if (type == 1) {  // Global
      switch (tag) {
        case 0x0:
          usage_page = value;
          if (usage_page == 0x0F) {
            result.saw_pid_usage_page = true;
          }
          break;
        case 0x7:
          report_size = value;
          break;
        case 0x8:
          report_id = static_cast<std::uint8_t>(value);
          break;
        case 0x9:
          report_count = value;
          break;
        case 0xA:
        case 0xB:
          result.well_formed = false;
          result.error = "push/pop is not modelled by this walker";
          return result;
        default:
          break;
      }
    }

    offset += 1 + data_size;
  }

  return result;
}

}  // namespace

int main() {
  using namespace lvg;
  using namespace lvg::driver;

  std::size_t descriptor_size = 0;
  const std::uint8_t *const descriptor = pid_gamepad_descriptor(&descriptor_size);
  check(descriptor != nullptr && descriptor_size > 0, "descriptor is present");

  const walk_result_t parsed = walk(descriptor, descriptor_size);
  check(parsed.well_formed, "descriptor parses: " + parsed.error);
  if (!parsed.well_formed) {
    std::printf("descriptor walk aborted\n");
    return 1;
  }

  check(parsed.collection_depth == 0, "collections balance");
  check(parsed.top_level_collections == 2, "two top-level application collections");
  check(parsed.saw_pid_usage_page, "PID usage page is present");
  std::printf("descriptor: %zu bytes, %zu items, %zu report ids, max depth %d\n",
              descriptor_size, parsed.item_count, parsed.reports.size(),
              parsed.max_collection_depth);

  // Every report the descriptor declares must match the expected bit budget.
  std::size_t expected_count = 0;
  const pid_report_bits_t *const expected = pid_report_bits(&expected_count);
  check(parsed.reports.size() == expected_count, "descriptor declares the expected report count");

  for (std::size_t i = 0; i < expected_count; ++i) {
    const pid_report_bits_t &want = expected[i];
    const auto found = parsed.reports.find(want.report_id);
    const std::string id = std::to_string(want.report_id);
    if (found == parsed.reports.end()) {
      check(false, "report " + id + " exists in the descriptor");
      continue;
    }
    check(found->second.input == want.input_bits,
          "report " + id + " input bits: got " + std::to_string(found->second.input) +
            " want " + std::to_string(want.input_bits));
    check(found->second.output == want.output_bits,
          "report " + id + " output bits: got " + std::to_string(found->second.output) +
            " want " + std::to_string(want.output_bits));
    check(found->second.feature == want.feature_bits,
          "report " + id + " feature bits: got " + std::to_string(found->second.feature) +
            " want " + std::to_string(want.feature_bits));

    const std::size_t total = want.input_bits + want.output_bits + want.feature_bits;
    check(total % 8 == 0, "report " + id + " is a whole number of bytes");
  }

  // Each parsed report must also match the packed struct the driver uses.
  const auto expect_struct = [&](const std::uint8_t report_id,
                                 const std::size_t struct_size,
                                 const char *name) {
    const auto found = parsed.reports.find(report_id);
    if (found == parsed.reports.end()) {
      check(false, std::string(name) + ": report id missing");
      return;
    }
    const std::size_t bits =
      found->second.input + found->second.output + found->second.feature;
    const std::size_t bytes = bits / 8 + 1;  // +1 for the report ID byte.
    check(bytes == struct_size,
          std::string(name) + ": descriptor says " + std::to_string(bytes) +
            " bytes, struct is " + std::to_string(struct_size));
  };

  expect_struct(k_pid_state_report_id, sizeof(pid_state_report), "pid_state_report");
  expect_struct(k_pid_set_effect_report_id, sizeof(pid_set_effect_report), "pid_set_effect_report");
  expect_struct(k_pid_set_envelope_report_id, sizeof(pid_set_envelope_report), "pid_set_envelope_report");
  expect_struct(k_pid_set_condition_report_id, sizeof(pid_set_condition_report), "pid_set_condition_report");
  expect_struct(k_pid_set_periodic_report_id, sizeof(pid_set_periodic_report), "pid_set_periodic_report");
  expect_struct(k_pid_set_constant_force_report_id, sizeof(pid_set_constant_force_report), "pid_set_constant_force_report");
  expect_struct(k_pid_set_ramp_force_report_id, sizeof(pid_set_ramp_force_report), "pid_set_ramp_force_report");
  expect_struct(k_pid_effect_operation_report_id, sizeof(pid_effect_operation_report), "pid_effect_operation_report");
  expect_struct(k_pid_block_free_report_id, sizeof(pid_block_free_report), "pid_block_free_report");
  expect_struct(k_pid_device_control_report_id, sizeof(pid_device_control_report), "pid_device_control_report");
  expect_struct(k_pid_device_gain_report_id, sizeof(pid_device_gain_report), "pid_device_gain_report");
  expect_struct(k_pid_create_new_effect_report_id, sizeof(pid_create_new_effect_report), "pid_create_new_effect_report");
  expect_struct(k_pid_block_load_report_id, sizeof(pid_block_load_report), "pid_block_load_report");
  expect_struct(k_pid_pool_report_id, sizeof(pid_pool_report), "pid_pool_report");

  // ---- Effect engine behavior ----
  {
    pid_engine engine;

    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    check(engine.create_new_effect(create), "constant force block allocates");

    const pid_block_load_report load = engine.block_load();
    check(load.effect_block_index == 1, "first block is index 1");
    check(load.block_load_status == static_cast<std::uint8_t>(pid_block_load_status::success),
          "first block loads successfully");
    check(load.ram_pool_available < engine.pool().ram_pool_size, "pool accounts for the block");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    set_effect.duration_ms = 100;
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "set effect accepted");

    pid_set_constant_force_report constant {};
    constant.report_id = k_pid_set_constant_force_report_id;
    constant.effect_block_index = 1;
    constant.magnitude = k_pid_nominal_max;
    check(engine.set_constant_force(constant), "constant force accepted");

    check(engine.rumble().low_frequency == 0, "idle before start");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "start accepted");

    check(engine.rumble().low_frequency == 65535, "full constant force drives the low motor");
    check(engine.rumble().high_frequency == 0, "constant force leaves the high motor alone");
    check(engine.needs_tick(), "a playing effect needs the clock");

    // A negative magnitude is the same strength in the other direction.
    constant.magnitude = static_cast<std::int16_t>(-k_pid_nominal_max);
    check(engine.set_constant_force(constant), "negative constant force accepted");
    check(engine.rumble().low_frequency == 65535, "direction does not change rumble strength");

    // Finite effects must stop themselves.
    engine.advance(150);
    check(!engine.needs_tick(), "finite effect stops on its own");
    check(engine.rumble().low_frequency == 0, "expired effect leaves the motors off");
  }

  {
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::sine);
    check(engine.create_new_effect(create), "sine block allocates");

    pid_set_periodic_report periodic {};
    periodic.report_id = k_pid_set_periodic_report_id;
    periodic.effect_block_index = 1;
    periodic.magnitude = k_pid_nominal_max / 2;
    check(engine.set_periodic(periodic), "periodic accepted");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::sine);
    set_effect.duration_ms = 0;  // Infinite.
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "infinite periodic accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "periodic start accepted");

    const pid_rumble_t rumble = engine.rumble();
    check(rumble.high_frequency > 32000 && rumble.high_frequency < 33500,
          "half-magnitude sine drives the high motor at about half");
    check(rumble.low_frequency == 0, "periodic leaves the low motor alone");

    engine.advance(100000);
    check(engine.needs_tick(), "infinite effect keeps playing");

    pid_device_control_report stop {};
    stop.report_id = k_pid_device_control_report_id;
    stop.control = static_cast<std::uint8_t>(pid_device_control::stop_all_effects);
    check(engine.device_control(stop), "stop all accepted");
    check(engine.rumble().high_frequency == 0, "stop all silences the motors");
  }

  {
    // Condition effects must be accepted without inventing vibration.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::spring);
    check(engine.create_new_effect(create), "spring block allocates");

    pid_set_condition_report condition {};
    condition.report_id = k_pid_set_condition_report_id;
    condition.effect_block_index = 1;
    condition.positive_coefficient = k_pid_nominal_max;
    condition.negative_coefficient = static_cast<std::int16_t>(-k_pid_nominal_max);
    check(engine.set_condition(condition), "condition accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "spring start accepted");
    check(engine.rumble().low_frequency == 0 && engine.rumble().high_frequency == 0,
          "spring produces no rumble");
  }

  {
    // The pool must refuse gracefully rather than overrun.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    for (int i = 0; i < k_pid_max_effects; ++i) {
      check(engine.create_new_effect(create), "block " + std::to_string(i + 1) + " allocates");
    }
    check(!engine.create_new_effect(create), "the pool refuses one block too many");
    check(engine.block_load().block_load_status ==
            static_cast<std::uint8_t>(pid_block_load_status::full),
          "a refused allocation reports Block Load Full");

    pid_block_free_report free_block {};
    free_block.report_id = k_pid_block_free_report_id;
    free_block.effect_block_index = 1;
    check(engine.block_free(free_block), "freeing a block succeeds");
    check(engine.create_new_effect(create), "a freed block can be reallocated");
  }

  {
    // Device gain scales everything the host asks for.
    pid_engine engine;
    pid_create_new_effect_report create {};
    create.report_id = k_pid_create_new_effect_report_id;
    create.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    check(engine.create_new_effect(create), "gain test block allocates");

    pid_set_effect_report set_effect {};
    set_effect.report_id = k_pid_set_effect_report_id;
    set_effect.effect_block_index = 1;
    set_effect.effect_type = static_cast<std::uint8_t>(pid_effect_type::constant_force);
    set_effect.gain = 255;
    check(engine.set_effect(set_effect), "gain test effect accepted");

    pid_set_constant_force_report constant {};
    constant.report_id = k_pid_set_constant_force_report_id;
    constant.effect_block_index = 1;
    constant.magnitude = k_pid_nominal_max;
    check(engine.set_constant_force(constant), "gain test force accepted");

    pid_effect_operation_report op {};
    op.report_id = k_pid_effect_operation_report_id;
    op.effect_block_index = 1;
    op.operation = static_cast<std::uint8_t>(pid_effect_operation::start);
    check(engine.effect_operation(op), "gain test start accepted");

    pid_device_gain_report gain {};
    gain.report_id = k_pid_device_gain_report_id;
    gain.device_gain = 128;
    check(engine.device_gain(gain), "device gain accepted");

    const std::uint16_t halved = engine.rumble().low_frequency;
    check(halved > 32000 && halved < 33500, "device gain of 128 halves the rumble");

    pid_device_control_report disable {};
    disable.report_id = k_pid_device_control_report_id;
    disable.control = static_cast<std::uint8_t>(pid_device_control::disable_actuators);
    check(engine.device_control(disable), "disable actuators accepted");
    check(engine.rumble().low_frequency == 0, "disabled actuators produce no rumble");
  }

  // ---- Xbox Series profile ----
  {
    std::size_t xbox_size = 0;
    const std::uint8_t *const xbox = xbox_series_descriptor(&xbox_size);
    const walk_result_t xbox_parsed = walk(xbox, xbox_size);
    check(xbox_parsed.well_formed, "xbox descriptor parses: " + xbox_parsed.error);
    if (xbox_parsed.well_formed) {
      check(xbox_parsed.collection_depth == 0, "xbox collections balance");
      check(xbox_parsed.top_level_collections == 1, "xbox has one application collection");
      std::printf("xbox descriptor: %zu bytes, %zu items, %zu report ids\n",
                  xbox_size, xbox_parsed.item_count, xbox_parsed.reports.size());

      const auto input = xbox_parsed.reports.find(k_xbox_series_input_report_id);
      if (input == xbox_parsed.reports.end()) {
        check(false, "xbox input report exists");
      } else {
        check(input->second.input == 128,
              "xbox input report is 128 bits, got " + std::to_string(input->second.input));
        check(input->second.input / 8 + 1 == sizeof(xbox_series_input_report),
              "xbox input report matches its struct");
      }

      const auto output = xbox_parsed.reports.find(k_xbox_series_output_report_id);
      if (output == xbox_parsed.reports.end()) {
        check(false, "xbox output report exists");
      } else {
        check(output->second.output == 64,
              "xbox output report is 64 bits, got " + std::to_string(output->second.output));
        check(output->second.output / 8 + 1 == sizeof(xbox_series_output_report),
              "xbox output report matches its struct");
      }
    }

    // The hardware ID list is what puts the child on the XInput path, so its
    // REG_MULTI_SZ shape has to be right or PnP sees nothing.
    std::size_t id_bytes = 0;
    const wchar_t *const ids = xbox_series_hardware_ids(&id_bytes);
    check(ids != nullptr && id_bytes > 0, "hardware ids are present");
    const std::size_t id_chars = id_bytes / sizeof(wchar_t);
    check(id_chars >= 2 && ids[id_chars - 1] == L'\0' && ids[id_chars - 2] == L'\0',
          "hardware ids are double-null terminated");
    int id_count = 0;
    for (std::size_t i = 0; i + 1 < id_chars;) {
      if (ids[i] == L'\0') {
        break;
      }
      ++id_count;
      while (i < id_chars && ids[i] != L'\0') {
        ++i;
      }
      ++i;
    }
    check(id_count == 2, "two hardware ids are offered, got " + std::to_string(id_count));
  }

  {
    // Axis, trigger, hat and button translation.
    input_state_request state {};
    state.header.size = sizeof(state);
    state.header.version = k_protocol_version;

    state.left_x = 0;
    state.left_y = 0;
    state.right_x = 0;
    state.right_y = 0;
    xbox_series_input_report report = encode_xbox_series_input(state);
    check(report.report_id == k_xbox_series_input_report_id, "xbox report id");
    check(report.left_x == 32768, "centred X maps to mid scale");
    check(report.left_y == 32767, "centred Y maps to mid scale");
    check(report.hat == 0, "a centred d-pad reports the hat null value");
    check(report.buttons == 0, "no buttons pressed");

    // Vibeshine is positive-up; this device is positive-down.
    state.left_y = 32767;
    report = encode_xbox_series_input(state);
    check(report.left_y == 0, "stick fully up maps to 0");
    state.left_y = -32768;
    report = encode_xbox_series_input(state);
    check(report.left_y == 65535, "stick fully down maps to full scale");

    state.left_y = 0;
    state.left_trigger = 255;
    state.right_trigger = 0;
    report = encode_xbox_series_input(state);
    check(report.left_trigger == k_xbox_trigger_max, "full trigger maps to 10-bit maximum");
    check(report.right_trigger == 0, "released trigger is zero");

    state.left_trigger = 0;
    state.buttons = button_mask::dpad_up;
    check(encode_xbox_series_input(state).hat == 1, "up is hat 1");
    state.buttons = button_mask::dpad_up | button_mask::dpad_right;
    check(encode_xbox_series_input(state).hat == 2, "up-right is hat 2");
    state.buttons = button_mask::dpad_left;
    check(encode_xbox_series_input(state).hat == 7, "left is hat 7");
    state.buttons = button_mask::dpad_up | button_mask::dpad_down;
    check(encode_xbox_series_input(state).hat == 0, "opposing d-pad reports the null value");

    state.buttons = button_mask::south | button_mask::east | button_mask::west |
                    button_mask::north;
    report = encode_xbox_series_input(state);
    check(report.buttons == (xbox_a | xbox_b | xbox_x | xbox_y), "face buttons map in HID order");
    // Buttons 3, 6, 9 and 10 are unpopulated on real hardware.
    check((report.buttons & ((1u << 2) | (1u << 5) | (1u << 8) | (1u << 9))) == 0,
          "the vendor's unpopulated button gaps stay empty");

    state.buttons = button_mask::home;
    check(encode_xbox_series_input(state).buttons == xbox_guide, "home maps to Guide");
    state.buttons = button_mask::misc;
    check(encode_xbox_series_input(state).share == 1, "misc maps to Share");
    state.buttons = 0;
    check(encode_xbox_series_input(state).share == 0, "Share is released");
  }

  {
    // Rumble decode: the enable mask gates each actuator.
    xbox_series_output_report output {};
    output.report_id = k_xbox_series_output_report_id;
    output.enable_mask = k_xbox_enable_left_motor | k_xbox_enable_right_motor |
                         k_xbox_enable_left_trigger | k_xbox_enable_right_trigger;
    output.left_motor = k_xbox_rumble_max;
    output.right_motor = k_xbox_rumble_max / 2;
    output.left_trigger_motor = k_xbox_rumble_max;
    output.right_trigger_motor = 0;

    lvg::xbox_rumble_feedback rumble {};
    check(decode_xbox_series_output(output, &rumble), "rumble write decodes");
    check(rumble.low_frequency == 65535, "full left motor is full scale");
    check(rumble.high_frequency > 32000 && rumble.high_frequency < 33500,
          "half right motor is about half scale");
    check(rumble.left_trigger == 65535, "full left trigger motor is full scale");
    check(rumble.right_trigger == 0, "idle right trigger motor is zero");

    output.enable_mask = k_xbox_enable_left_motor;
    check(decode_xbox_series_output(output, &rumble), "masked rumble write decodes");
    check(rumble.low_frequency == 65535, "the enabled actuator still reports");
    check(rumble.high_frequency == 0 && rumble.left_trigger == 0,
          "actuators the mask disables report silence");

    output.report_id = 0x7F;
    check(!decode_xbox_series_output(output, &rumble), "a foreign report id is rejected");

    const feedback_event event = encode_xbox_series_feedback(5, rumble);
    check(event.type == feedback_type::xbox_rumble, "feedback is tagged xbox_rumble");
    check(event.controller_id == 5, "feedback carries the controller id");
    check(event.payload_size == sizeof(lvg::xbox_rumble_feedback), "feedback payload size");
    check(valid_request(&event, sizeof(event)), "feedback event is a valid protocol message");
  }

  if (g_failures == 0) {
    std::printf("all PID descriptor and engine checks passed\n");
    return 0;
  }
  std::printf("%d check(s) failed\n", g_failures);
  return 1;
}
