// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT

// This is a UMDF2 VHF source driver, not a HID minidriver. Its WDF device
// exposes a private control interface to Vibeshine; VHF creates the HID child
// for each active controller.

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <wdf.h>
#include <vhf.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "libvirtualgamepad/protocol.h"
#include "pid_ff.h"
#include "profile.h"
#include "xbox_series.h"

namespace {

using lvg::driver::encode_generic_feedback;
using lvg::driver::encode_generic_input;
using lvg::driver::find_profile;
using lvg::driver::generic_input_report;
using lvg::driver::generic_output_report;
using lvg::driver::k_generic_input_report_id;
using lvg::driver::k_generic_output_report_id;
using lvg::driver::pid_engine;
using lvg::driver::pid_rumble_t;
using lvg::driver::profile_definition;

// How often playing force-feedback effects are advanced. Envelopes and finite
// durations only need to look continuous to a human hand, and the timer stops
// itself as soon as nothing is playing.
constexpr LONG k_pid_tick_ms = 10;

enum class slot_state : std::uint8_t {
  empty,
  starting,
  active,
  stopping,
};

struct device_context;

struct controller_slot {
  device_context *parent;
  VHFHANDLE vhf;
  WDFFILEOBJECT owner;
  lvg::profile selected_profile;
  std::uint32_t controller_id;
  slot_state state;
  bool feedback_pending;
  lvg::feedback_event feedback;
  // Set from the profile, so an output report is only interpreted as PID when
  // the descriptor actually declared the PID collection.
  bool force_feedback;
  bool pid_rumble_valid;
  pid_rumble_t pid_rumble;
  // Zeroed by reset_slot and initialized by create_controller: WDF allocates
  // the device context as raw memory, so no constructor runs for this member.
  pid_engine pid;
};

struct device_context {
  WDFWAITLOCK lifetime_gate;
  WDFWAITLOCK state_lock;
  WDFIOTARGET local_vhf_target;
  HANDLE vhf_file_handle;
  bool vhf_target_open;
  bool stopping;
  WDFTIMER pid_timer;
  controller_slot controllers[lvg::k_max_controllers];
};

struct target_context {
  device_context *device;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_context, get_device_context);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(target_context, get_target_context);

EVT_WDF_DRIVER_DEVICE_ADD evt_device_add;
EVT_WDF_DEVICE_PREPARE_HARDWARE evt_prepare_hardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE evt_release_hardware;
EVT_WDF_OBJECT_CONTEXT_CLEANUP evt_vhf_target_cleanup;
EVT_WDF_DEVICE_FILE_CREATE evt_file_create;
EVT_WDF_FILE_CLOSE evt_file_close;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL evt_io_device_control;
EVT_VHF_ASYNC_OPERATION evt_vhf_write_report;
EVT_VHF_ASYNC_OPERATION evt_vhf_get_feature;
EVT_VHF_ASYNC_OPERATION evt_vhf_set_feature;
EVT_VHF_CLEANUP evt_vhf_cleanup;
EVT_WDF_TIMER evt_pid_tick;

void lock_context(device_context *const context) noexcept {
  WdfWaitLockAcquire(context->state_lock, nullptr);
}

void unlock_context(device_context *const context) noexcept {
  WdfWaitLockRelease(context->state_lock);
}

void lock_lifetime(device_context *const context) noexcept {
  WdfWaitLockAcquire(context->lifetime_gate, nullptr);
}

void unlock_lifetime(device_context *const context) noexcept {
  WdfWaitLockRelease(context->lifetime_gate);
}

// Opening the local I/O target by file is a create against this device's own
// stack, so it only succeeds once PnP has started the device. Callers own the
// lifetime gate; the open is idempotent so both the start path and the first
// controller creation can drive it.
[[nodiscard]] NTSTATUS ensure_vhf_target_open(device_context *const context) noexcept {
  lock_context(context);
  const WDFIOTARGET target = context->local_vhf_target;
  const bool opened = context->vhf_file_handle != nullptr;
  const bool stopping = context->stopping;
  unlock_context(context);

  if (opened) {
    return STATUS_SUCCESS;
  }
  if (stopping || target == nullptr) {
    return STATUS_DEVICE_NOT_READY;
  }

  WDF_IO_TARGET_OPEN_PARAMS open_params;
  WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(&open_params, nullptr);
  const NTSTATUS status = WdfIoTargetOpen(target, &open_params);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  const HANDLE file_handle = WdfIoTargetWdmGetTargetFileHandle(target);
  if (file_handle == nullptr) {
    WdfIoTargetClose(target);
    return STATUS_DEVICE_NOT_READY;
  }

  lock_context(context);
  context->vhf_file_handle = file_handle;
  context->vhf_target_open = true;
  unlock_context(context);
  return STATUS_SUCCESS;
}

[[nodiscard]] bool is_owned_by(
  const controller_slot &slot,
  const WDFFILEOBJECT owner) noexcept {
  return slot.owner == owner;
}

void reset_slot(controller_slot *slot) noexcept {
  auto *const parent = slot->parent;
  const auto controller_id = slot->controller_id;
  std::memset(slot, 0, sizeof(*slot));
  slot->parent = parent;
  slot->controller_id = controller_id;
  slot->state = slot_state::empty;
}

void release_starting_slot(
  device_context *const context,
  controller_slot *const slot,
  const WDFFILEOBJECT owner) noexcept {
  lock_context(context);
  if (slot->owner == owner &&
      (slot->state == slot_state::starting || slot->state == slot_state::stopping)) {
    reset_slot(slot);
  }
  unlock_context(context);
}

// Detaches the VHF handle before calling VhfDelete. A VHF output callback can
// still arrive while VhfDelete waits, but it will observe stopping state and
// complete without publishing a new feedback event.
void destroy_owned_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const std::uint32_t controller_id) noexcept {
  if (controller_id >= lvg::k_max_controllers) {
    return;
  }

  auto &slot = context->controllers[controller_id];
  VHFHANDLE vhf = nullptr;

  lock_lifetime(context);
  lock_context(context);
  if (!is_owned_by(slot, owner) || slot.state == slot_state::empty) {
    unlock_context(context);
    unlock_lifetime(context);
    return;
  }

  if (slot.state == slot_state::starting) {
    // VhfStart may invoke callbacks before it returns. The creator owns the
    // uncommitted handle and will delete it when it observes this state.
    slot.state = slot_state::stopping;
    unlock_context(context);
    unlock_lifetime(context);
    return;
  }

  slot.state = slot_state::stopping;
  slot.feedback_pending = false;
  vhf = slot.vhf;
  slot.vhf = nullptr;
  unlock_context(context);

  if (vhf != nullptr) {
    VhfDelete(vhf, TRUE);
  }

  lock_context(context);
  if (is_owned_by(slot, owner) && slot.state == slot_state::stopping) {
    reset_slot(&slot);
  }
  unlock_context(context);
  unlock_lifetime(context);
}

[[nodiscard]] NTSTATUS create_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::create_controller_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }
  if (request.reserved != 0) {
    return STATUS_INVALID_PARAMETER;
  }

  const profile_definition *const definition = find_profile(request.requested_profile);
  if (definition == nullptr) {
    return STATUS_NOT_SUPPORTED;
  }

  auto &slot = context->controllers[request.controller_id];

  // The lifetime gate is distinct from state_lock. VhfStart can invoke the
  // output callback before returning, and that callback needs state_lock.
  // Holding only the outer gate across VHF calls prevents target cleanup from
  // invalidating the FileHandle without deadlocking the callback.
  lock_lifetime(context);
  lock_context(context);
  if (context->stopping) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (slot.state != slot_state::empty) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_BUSY;
  }

  slot.owner = owner;
  slot.selected_profile = request.requested_profile;
  slot.state = slot_state::starting;
  slot.feedback_pending = false;
  slot.force_feedback = definition->force_feedback;
  slot.pid_rumble_valid = false;
  slot.pid_rumble = {};
  // reset_slot zeroed this storage, so the engine has to be put into its
  // documented initial state explicitly before any report can reach it.
  slot.pid.reset();
  unlock_context(context);

  // The start path already opens this target. Retrying here keeps a source
  // device that started before its own stack could answer a create usable.
  const NTSTATUS open_status = ensure_vhf_target_open(context);
  if (!NT_SUCCESS(open_status)) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return open_status;
  }

  lock_context(context);
  const HANDLE file_handle = context->vhf_file_handle;
  unlock_context(context);
  if (file_handle == nullptr) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }

  // State is completely initialized before VhfStart: the framework is allowed
  // to enter an output callback before VhfStart returns.
  VHF_CONFIG config;
  VHF_CONFIG_INIT(
    &config,
    file_handle,
    static_cast<USHORT>(definition->report_descriptor_size),
    const_cast<PUCHAR>(definition->report_descriptor));
  config.VhfClientContext = &slot;
  config.EvtVhfAsyncOperationWriteReport = evt_vhf_write_report;
  config.EvtVhfCleanup = evt_vhf_cleanup;
  // Without an identity the HID child enumerates as VID/PID 0000:0000, which
  // leaves Windows and applications nothing to match on.
  config.VendorID = definition->vendor_id;
  config.ProductID = definition->product_id;
  config.VersionNumber = definition->version_number;
  if (definition->hardware_ids != nullptr && definition->hardware_ids_bytes != 0) {
    // Windows attaches xinputhid.sys by hardware ID, so this is what puts the
    // child on the XInput path. It is set at runtime and never appears in the
    // signed INF.
    config.HardwareIDs = const_cast<PWSTR>(definition->hardware_ids);
    config.HardwareIDsLength = static_cast<USHORT>(definition->hardware_ids_bytes);
  }
  if (definition->force_feedback) {
    // DirectInput discovers effect capacity and allocates effect blocks through
    // feature reports, so a PID profile is not usable without these.
    config.EvtVhfAsyncOperationGetFeature = evt_vhf_get_feature;
    config.EvtVhfAsyncOperationSetFeature = evt_vhf_set_feature;
  }

  VHFHANDLE vhf = nullptr;
  NTSTATUS status = VhfCreate(&config, &vhf);
  if (!NT_SUCCESS(status)) {
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return status;
  }

  status = VhfStart(vhf);
  if (!NT_SUCCESS(status)) {
    release_starting_slot(context, &slot, owner);
    VhfDelete(vhf, TRUE);
    unlock_lifetime(context);
    return status;
  }

  bool adopted = false;
  lock_context(context);
  if (!context->stopping && slot.owner == owner && slot.state == slot_state::starting) {
    slot.vhf = vhf;
    slot.state = slot_state::active;
    adopted = true;
  }
  unlock_context(context);

  if (!adopted) {
    VhfDelete(vhf, TRUE);
    release_starting_slot(context, &slot, owner);
    unlock_lifetime(context);
    return STATUS_CANCELLED;
  }

  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

[[nodiscard]] NTSTATUS destroy_controller(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::controller_id_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_context(context);
  const bool owned = is_owned_by(slot, owner);
  const bool exists = slot.state != slot_state::empty;
  unlock_context(context);

  if (!owned) {
    return STATUS_ACCESS_DENIED;
  }
  if (!exists) {
    return STATUS_NOT_FOUND;
  }

  destroy_owned_controller(context, owner, request.controller_id);
  return STATUS_SUCCESS;
}

[[nodiscard]] NTSTATUS submit_input_state(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::input_state_request &request) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }
  if (request.reserved != 0) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_lifetime(context);
  lock_context(context);
  if (context->stopping || !is_owned_by(slot, owner) ||
      slot.state != slot_state::active || slot.vhf == nullptr) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (slot.selected_profile == lvg::profile::xbox_series) {
    const lvg::driver::xbox_series_input_report xbox_report =
      lvg::driver::encode_xbox_series_input(request);
    HID_XFER_PACKET xbox_transfer {
      reinterpret_cast<PUCHAR>(const_cast<lvg::driver::xbox_series_input_report *>(&xbox_report)),
      sizeof(xbox_report),
      lvg::driver::k_xbox_series_input_report_id,
    };

    const VHFHANDLE xbox_vhf = slot.vhf;
    unlock_context(context);
    const NTSTATUS xbox_status = VhfReadReportSubmit(xbox_vhf, &xbox_transfer);
    unlock_lifetime(context);
    return xbox_status;
  }

  if (slot.selected_profile != lvg::profile::generic_hid &&
      slot.selected_profile != lvg::profile::generic_pid) {
    unlock_context(context);
    unlock_lifetime(context);
    return STATUS_NOT_SUPPORTED;
  }

  const generic_input_report report = encode_generic_input(request);
  HID_XFER_PACKET transfer {
    reinterpret_cast<PUCHAR>(const_cast<generic_input_report *>(&report)),
    sizeof(report),
    k_generic_input_report_id,
  };

  const VHFHANDLE vhf = slot.vhf;
  unlock_context(context);

  // Default VHF buffering permits this synchronous submit. The lifetime gate
  // keeps VhfDelete and target cleanup from invalidating vhf or FileHandle.
  const NTSTATUS status = VhfReadReportSubmit(vhf, &transfer);
  unlock_lifetime(context);
  return status;
}

[[nodiscard]] NTSTATUS poll_feedback(
  device_context *const context,
  const WDFFILEOBJECT owner,
  const lvg::controller_id_request &request,
  lvg::feedback_event *const output) noexcept {
  if (request.controller_id >= lvg::k_max_controllers) {
    return STATUS_INVALID_PARAMETER;
  }

  auto &slot = context->controllers[request.controller_id];
  lock_context(context);
  NTSTATUS status = STATUS_SUCCESS;
  if (!is_owned_by(slot, owner)) {
    status = STATUS_ACCESS_DENIED;
  } else if (slot.state != slot_state::active) {
    status = STATUS_DEVICE_NOT_READY;
  } else if (!slot.feedback_pending) {
    status = STATUS_NO_MORE_ENTRIES;
  } else {
    *output = slot.feedback;
    slot.feedback_pending = false;
  }
  unlock_context(context);
  return status;
}

// Copies a PID output report out of the transfer packet. HID pads the buffer
// to the report's declared length, so a longer buffer is normal; a shorter one
// means the report is not the one the descriptor declared.
template<class report_t>
[[nodiscard]] bool read_pid_report(const PHID_XFER_PACKET transfer, report_t *const out) noexcept {
  if (transfer->reportBuffer == nullptr || transfer->reportBufferLen < sizeof(report_t)) {
    return false;
  }
  std::memcpy(out, transfer->reportBuffer, sizeof(report_t));
  return out->report_id == transfer->reportId;
}

// Applies one PID output report. The caller owns state_lock.
[[nodiscard]] NTSTATUS apply_pid_output(
  controller_slot *const slot,
  const PHID_XFER_PACKET transfer) noexcept {
  using namespace lvg::driver;

  bool handled = false;
  switch (transfer->reportId) {
    case k_pid_set_effect_report_id: {
      pid_set_effect_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_effect(report);
      break;
    }
    case k_pid_set_envelope_report_id: {
      pid_set_envelope_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_envelope(report);
      break;
    }
    case k_pid_set_condition_report_id: {
      pid_set_condition_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_condition(report);
      break;
    }
    case k_pid_set_periodic_report_id: {
      pid_set_periodic_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_periodic(report);
      break;
    }
    case k_pid_set_constant_force_report_id: {
      pid_set_constant_force_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_constant_force(report);
      break;
    }
    case k_pid_set_ramp_force_report_id: {
      pid_set_ramp_force_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.set_ramp_force(report);
      break;
    }
    case k_pid_effect_operation_report_id: {
      pid_effect_operation_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.effect_operation(report);
      break;
    }
    case k_pid_block_free_report_id: {
      pid_block_free_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.block_free(report);
      break;
    }
    case k_pid_device_control_report_id: {
      pid_device_control_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.device_control(report);
      break;
    }
    case k_pid_device_gain_report_id: {
      pid_device_gain_report report {};
      handled = read_pid_report(transfer, &report) && slot->pid.device_gain(report);
      break;
    }
    default:
      return STATUS_INVALID_DEVICE_REQUEST;
  }

  return handled ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

// Publishes the engine's current rumble as a feedback event when it changed.
// The caller owns state_lock.
void publish_pid_rumble(controller_slot *const slot) noexcept {
  const pid_rumble_t rumble = slot->pid.rumble();
  if (slot->pid_rumble_valid &&
      rumble.low_frequency == slot->pid_rumble.low_frequency &&
      rumble.high_frequency == slot->pid_rumble.high_frequency) {
    return;
  }

  slot->pid_rumble = rumble;
  slot->pid_rumble_valid = true;

  // Built here rather than through encode_generic_feedback so the event is
  // tagged rumble-only: a PID effect carries no colour, and forwarding a black
  // LED would switch off the light on the client's real controller.
  lvg::feedback_event event {};
  event.header.size = sizeof(event);
  event.header.version = lvg::k_protocol_version;
  event.controller_id = slot->controller_id;
  event.type = lvg::feedback_type::generic_rumble;
  const lvg::generic_rumble_rgb_feedback payload {
    rumble.low_frequency,
    rumble.high_frequency,
    0, 0, 0, 0};
  event.payload_size = sizeof(payload);
  std::memcpy(event.payload, &payload, sizeof(payload));

  slot->feedback = event;
  slot->feedback_pending = true;
}

// Applies an Xbox rumble write. The caller owns state_lock.
[[nodiscard]] NTSTATUS apply_xbox_output(
  controller_slot *const slot,
  const PHID_XFER_PACKET transfer) noexcept {
  using namespace lvg::driver;

  if (transfer->reportId != k_xbox_series_output_report_id ||
      transfer->reportBuffer == nullptr ||
      transfer->reportBufferLen < sizeof(xbox_series_output_report)) {
    return STATUS_INVALID_DEVICE_REQUEST;
  }

  xbox_series_output_report output {};
  std::memcpy(&output, transfer->reportBuffer, sizeof(output));

  lvg::xbox_rumble_feedback rumble {};
  if (!decode_xbox_series_output(output, &rumble)) {
    return STATUS_INVALID_PARAMETER;
  }

  slot->feedback = encode_xbox_series_feedback(slot->controller_id, rumble);
  slot->feedback_pending = true;  // Coalesce to the current actuator state.
  return STATUS_SUCCESS;
}

void evt_vhf_write_report(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  bool arm_tick = false;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->selected_profile == lvg::profile::xbox_series) {
    auto *const context = slot->parent;
    lock_context(context);
    if (!context->stopping && slot->state == slot_state::active) {
      status = apply_xbox_output(slot, transfer);
    } else {
      status = STATUS_DEVICE_NOT_READY;
    }
    unlock_context(context);

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportId != k_generic_output_report_id) {
    auto *const context = slot->parent;
    lock_context(context);
    if (!context->stopping && slot->state == slot_state::active) {
      status = apply_pid_output(slot, transfer);
      if (NT_SUCCESS(status)) {
        publish_pid_rumble(slot);
        arm_tick = slot->pid.needs_tick();
      }
    } else {
      status = STATUS_DEVICE_NOT_READY;
    }
    unlock_context(context);

    // Started outside the lock: the tick callback takes state_lock itself.
    if (arm_tick && context->pid_timer != nullptr) {
      WdfTimerStart(context->pid_timer, WDF_REL_TIMEOUT_IN_MS(k_pid_tick_ms));
    }

    if (operation_handle != nullptr) {
      VhfAsyncOperationComplete(operation_handle, status);
    }
    return;
  }

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      transfer->reportId == k_generic_output_report_id &&
      transfer->reportBuffer != nullptr &&
      transfer->reportBufferLen >= sizeof(generic_output_report)) {
    generic_output_report output {};
    std::memcpy(&output, transfer->reportBuffer, sizeof(output));

    if (output.report_id != k_generic_output_report_id) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      auto *const context = slot->parent;
      lock_context(context);
      if (!context->stopping && slot->state == slot_state::active &&
          (slot->selected_profile == lvg::profile::generic_hid ||
           slot->selected_profile == lvg::profile::generic_pid)) {
        slot->feedback = encode_generic_feedback(slot->controller_id, output);
        slot->feedback_pending = true;  // Coalesce to the current controller state.
        status = STATUS_SUCCESS;
      } else {
        status = STATUS_DEVICE_NOT_READY;
      }
      unlock_context(context);
    }
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

// DirectInput allocates an effect block by writing Create New Effect and then
// reading PID Block Load; it sizes its effect list from PID Pool.
void evt_vhf_get_feature(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  using namespace lvg::driver;

  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportBuffer != nullptr) {
    auto *const context = slot->parent;
    lock_context(context);
    if (context->stopping || slot->state != slot_state::active) {
      status = STATUS_DEVICE_NOT_READY;
    } else {
      switch (transfer->reportId) {
        case k_pid_block_load_report_id: {
          const pid_block_load_report report = slot->pid.block_load();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        case k_pid_pool_report_id: {
          const pid_pool_report report = slot->pid.pool();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        case k_pid_state_report_id: {
          const pid_state_report report = slot->pid.state();
          if (transfer->reportBufferLen >= sizeof(report)) {
            std::memcpy(transfer->reportBuffer, &report, sizeof(report));
            status = STATUS_SUCCESS;
          } else {
            status = STATUS_BUFFER_TOO_SMALL;
          }
          break;
        }
        default:
          status = STATUS_INVALID_DEVICE_REQUEST;
          break;
      }
    }
    unlock_context(context);
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

void evt_vhf_set_feature(
  PVOID vhf_client_context,
  VHFOPERATIONHANDLE operation_handle,
  PVOID,
  PHID_XFER_PACKET transfer) {
  using namespace lvg::driver;

  auto *const slot = static_cast<controller_slot *>(vhf_client_context);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

  if (slot != nullptr && slot->parent != nullptr && transfer != nullptr &&
      slot->force_feedback && transfer->reportId == k_pid_create_new_effect_report_id) {
    pid_create_new_effect_report report {};
    if (!read_pid_report(transfer, &report)) {
      status = STATUS_INVALID_PARAMETER;
    } else {
      auto *const context = slot->parent;
      lock_context(context);
      if (context->stopping || slot->state != slot_state::active) {
        status = STATUS_DEVICE_NOT_READY;
      } else {
        // A full pool is a normal answer, not a transport failure: the host
        // learns about it by reading Block Load next.
        static_cast<void>(slot->pid.create_new_effect(report));
        status = STATUS_SUCCESS;
      }
      unlock_context(context);
    }
  }

  if (operation_handle != nullptr) {
    VhfAsyncOperationComplete(operation_handle, status);
  }
}

// Advances every playing effect and stops itself once nothing is playing, so an
// idle device does not keep a 100 Hz timer alive.
void evt_pid_tick(WDFTIMER timer) {
  auto *const context = get_device_context(
    reinterpret_cast<WDFDEVICE>(WdfTimerGetParentObject(timer)));
  if (context == nullptr) {
    return;
  }

  bool still_playing = false;
  lock_context(context);
  for (auto &slot : context->controllers) {
    if (slot.state != slot_state::active || !slot.force_feedback) {
      continue;
    }
    slot.pid.advance(static_cast<std::uint32_t>(k_pid_tick_ms));
    publish_pid_rumble(&slot);
    still_playing = still_playing || slot.pid.needs_tick();
  }
  const bool stopping = context->stopping;
  unlock_context(context);

  if (!still_playing || stopping) {
    // A timer may stop itself; passing FALSE keeps this from waiting on the
    // callback it is already running inside.
    WdfTimerStop(timer, FALSE);
  }
}

void evt_vhf_cleanup(PVOID) {
  // VhfDelete(..., TRUE) waits until this callback runs and guarantees that no
  // asynchronous VHF operation remains. Slot storage is static in the WDF
  // device context, so no callback-owned memory needs freeing here.
}

template<class request_t>
[[nodiscard]] NTSTATUS retrieve_request(
  WDFREQUEST request,
  request_t **const output) noexcept {
  PVOID raw = nullptr;
  size_t bytes = 0;
  NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(lvg::request_header), &raw, &bytes);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  auto *const typed = static_cast<request_t *>(raw);
  if (!lvg::valid_request(typed, bytes)) {
    return STATUS_INVALID_BUFFER_SIZE;
  }
  *output = typed;
  return STATUS_SUCCESS;
}

void evt_io_device_control(
  WDFQUEUE queue,
  WDFREQUEST request,
  const size_t output_buffer_length,
  const size_t,
  const ULONG io_control_code) {
  auto *const context = get_device_context(WdfIoQueueGetDevice(queue));
  const WDFFILEOBJECT owner = WdfRequestGetFileObject(request);
  NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
  ULONG_PTR information = 0;

  if (owner == nullptr) {
    WdfRequestComplete(request, STATUS_INVALID_HANDLE);
    return;
  }

  switch (io_control_code) {
    case lvg::ioctl_query_info: {
      lvg::query_info_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (!NT_SUCCESS(status)) {
        break;
      }
      if (output_buffer_length < sizeof(lvg::query_info_response)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      lvg::query_info_response *output = nullptr;
      status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(*output),
        reinterpret_cast<PVOID *>(&output),
        nullptr);
      if (NT_SUCCESS(status)) {
        *output = {};
        output->header.size = sizeof(*output);
        output->header.version = lvg::k_protocol_version;
        output->minimum_protocol_version = lvg::k_protocol_version;
        output->maximum_protocol_version = lvg::k_protocol_version;
        output->available_profiles = lvg::driver::available_profiles();
        output->available_features = lvg::feature_input_state | lvg::feature_feedback;
        output->maximum_controllers = lvg::k_max_controllers;
        information = sizeof(*output);
      }
      break;
    }
    case lvg::ioctl_create_controller: {
      lvg::create_controller_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = create_controller(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_submit_touch_state: {
      lvg::touch_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status) && input->reserved != 0) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status)) {
        status = STATUS_NOT_SUPPORTED;
      }
      break;
    }
    case lvg::ioctl_submit_motion_state: {
      lvg::motion_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        const bool reserved_is_zero = input->reserved0[0] == 0 &&
                                      input->reserved0[1] == 0 &&
                                      input->reserved0[2] == 0;
        status = reserved_is_zero ? STATUS_NOT_SUPPORTED : STATUS_INVALID_PARAMETER;
      }
      break;
    }
    case lvg::ioctl_submit_battery_state: {
      lvg::battery_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status) && input->reserved != 0) {
        status = STATUS_INVALID_PARAMETER;
      }
      if (NT_SUCCESS(status)) {
        status = STATUS_NOT_SUPPORTED;
      }
      break;
    }
    case lvg::ioctl_destroy_controller: {
      lvg::controller_id_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = destroy_controller(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_submit_input_state: {
      lvg::input_state_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (NT_SUCCESS(status)) {
        status = submit_input_state(context, owner, *input);
      }
      break;
    }
    case lvg::ioctl_poll_feedback: {
      lvg::controller_id_request *input = nullptr;
      status = retrieve_request(request, &input);
      if (!NT_SUCCESS(status)) {
        break;
      }
      if (output_buffer_length < sizeof(lvg::feedback_event)) {
        status = STATUS_BUFFER_TOO_SMALL;
        break;
      }
      lvg::feedback_event *output = nullptr;
      status = WdfRequestRetrieveOutputBuffer(
        request,
        sizeof(*output),
        reinterpret_cast<PVOID *>(&output),
        nullptr);
      if (NT_SUCCESS(status)) {
        status = poll_feedback(context, owner, *input, output);
        if (NT_SUCCESS(status)) {
          information = sizeof(*output);
        }
      }
      break;
    }
    default:
      break;
  }

  WdfRequestCompleteWithInformation(request, status, information);
}

void evt_file_create(WDFDEVICE, WDFREQUEST request, WDFFILEOBJECT) {
  WdfRequestComplete(request, STATUS_SUCCESS);
}

void evt_file_close(WDFFILEOBJECT file_object) {
  const WDFDEVICE device = WdfFileObjectGetDevice(file_object);
  auto *const context = get_device_context(device);
  for (std::uint32_t controller_id = 0; controller_id < lvg::k_max_controllers; ++controller_id) {
    destroy_owned_controller(context, file_object, controller_id);
  }
}

// Tears down every controller this device owns and drops the VHF file handle.
// Callers must hold neither lock. forget_target is set only when the target
// object itself is going away.
void stop_owned_controllers(device_context *const context, const bool forget_target) noexcept {
  VHFHANDLE handles[lvg::k_max_controllers] {};

  lock_lifetime(context);
  lock_context(context);
  const WDFTIMER timer = context->pid_timer;
  context->stopping = true;
  context->vhf_file_handle = nullptr;
  if (forget_target) {
    context->local_vhf_target = nullptr;
    context->vhf_target_open = false;
  }
  for (std::uint32_t index = 0; index < lvg::k_max_controllers; ++index) {
    auto &slot = context->controllers[index];
    slot.feedback_pending = false;
    if (slot.state == slot_state::active && slot.vhf != nullptr) {
      slot.state = slot_state::stopping;
      handles[index] = slot.vhf;
      slot.vhf = nullptr;
    } else if (slot.state == slot_state::starting) {
      slot.state = slot_state::stopping;
    }
  }
  unlock_context(context);

  for (const auto handle : handles) {
    if (handle != nullptr) {
      VhfDelete(handle, TRUE);
    }
  }

  // Stopped outside state_lock because the tick callback acquires it.
  if (timer != nullptr) {
    WdfTimerStop(timer, TRUE);
  }
  unlock_lifetime(context);
}

// WDF guarantees that an I/O target's FileHandle remains valid through this
// target cleanup callback when the target is deleted while still open. The
// target is parented below state_lock and lifetime_gate, so both locks and the
// device context remain valid for the complete callback.
//
// Do not close the target here. It is being deleted while still open, which
// keeps its UMDF file handle valid through this callback; WDF closes it after
// VhfDelete has synchronously removed every virtual HID child.
void evt_vhf_target_cleanup(WDFOBJECT object) {
  auto *const target = get_target_context(reinterpret_cast<WDFIOTARGET>(object));
  auto *const context = target->device;
  if (context == nullptr) {
    return;
  }
  stop_owned_controllers(context, true);
}

// A UMDF source driver cannot open its own local target before the device is
// started: EvtDeviceAdd runs ahead of PnP start, the create fails with
// STATUS_DEVICE_NOT_READY, and the whole device stops with CM_PROB_FAILED_ADD.
// The documented VHF flow opens the target from the start path instead.
NTSTATUS evt_prepare_hardware(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
  auto *const context = get_device_context(device);

  lock_lifetime(context);
  lock_context(context);
  context->stopping = false;
  unlock_context(context);
  // A source device that cannot reach VHF yet still answers protocol queries
  // and retries the open when a controller is created, so a failure here must
  // not keep the device from starting.
  static_cast<void>(ensure_vhf_target_open(context));
  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

NTSTATUS evt_release_hardware(WDFDEVICE device, WDFCMRESLIST) {
  auto *const context = get_device_context(device);
  stop_owned_controllers(context, false);

  lock_lifetime(context);
  lock_context(context);
  const WDFIOTARGET target = context->vhf_target_open ? context->local_vhf_target : nullptr;
  context->vhf_target_open = false;
  unlock_context(context);
  if (target != nullptr) {
    WdfIoTargetClose(target);
  }
  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

NTSTATUS evt_device_add(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  WDF_FILEOBJECT_CONFIG file_config;
  WDF_FILEOBJECT_CONFIG_INIT(
    &file_config,
    evt_file_create,
    evt_file_close,
    WDF_NO_EVENT_CALLBACK);

  WDF_OBJECT_ATTRIBUTES file_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&file_attributes);
  WdfDeviceInitSetFileObjectConfig(device_init, &file_config, &file_attributes);

  WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
  pnp_callbacks.EvtDevicePrepareHardware = evt_prepare_hardware;
  pnp_callbacks.EvtDeviceReleaseHardware = evt_release_hardware;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_callbacks);

  WDF_OBJECT_ATTRIBUTES device_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, device_context);

  WDFDEVICE device = nullptr;
  NTSTATUS status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto *const context = get_device_context(device);
  std::memset(context, 0, sizeof(*context));
  for (std::uint32_t index = 0; index < lvg::k_max_controllers; ++index) {
    context->controllers[index].parent = context;
    context->controllers[index].controller_id = index;
    context->controllers[index].state = slot_state::empty;
  }

  // The effect clock is an enhancement, not a prerequisite. Failing the whole
  // EvtDeviceAdd when it cannot be created takes the entire device down with
  // STATUS_NOT_SUPPORTED and leaves the control interface registered but
  // disabled, which reads as "driver installed but unusable". Without the timer
  // force feedback still tracks every report the host sends; only unattended
  // duration expiry and envelope shaping are lost.
  WDF_TIMER_CONFIG timer_config;
  WDF_TIMER_CONFIG_INIT_PERIODIC(&timer_config, evt_pid_tick, k_pid_tick_ms);
  timer_config.AutomaticSerialization = FALSE;
  WDF_OBJECT_ATTRIBUTES timer_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&timer_attributes);
  timer_attributes.ParentObject = device;
  if (!NT_SUCCESS(WdfTimerCreate(&timer_config, &timer_attributes, &context->pid_timer))) {
    context->pid_timer = nullptr;
  }

  WDF_OBJECT_ATTRIBUTES lifetime_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&lifetime_attributes);
  lifetime_attributes.ParentObject = device;
  status = WdfWaitLockCreate(&lifetime_attributes, &context->lifetime_gate);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES state_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&state_attributes);
  state_attributes.ParentObject = context->lifetime_gate;
  status = WdfWaitLockCreate(&state_attributes, &context->state_lock);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES target_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&target_attributes, target_context);
  target_attributes.ParentObject = context->state_lock;
  target_attributes.EvtCleanupCallback = evt_vhf_target_cleanup;
  status = WdfIoTargetCreate(device, &target_attributes, &context->local_vhf_target);
  if (!NT_SUCCESS(status)) {
    return status;
  }
  get_target_context(context->local_vhf_target)->device = context;

  status = WdfDeviceCreateDeviceInterface(device, &lvg::k_device_interface_guid, nullptr);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_IO_QUEUE_CONFIG queue_config;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchSequential);
  queue_config.EvtIoDeviceControl = evt_io_device_control;
  return WdfIoQueueCreate(device, &queue_config, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
}

}  // namespace

extern "C" NTSTATUS DriverEntry(
  PDRIVER_OBJECT driver_object,
  PUNICODE_STRING registry_path) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, evt_device_add);
  config.DriverPoolTag = 'gVLV';

  return WdfDriverCreate(
    driver_object,
    registry_path,
    WDF_NO_OBJECT_ATTRIBUTES,
    &config,
    WDF_NO_HANDLE);
}
