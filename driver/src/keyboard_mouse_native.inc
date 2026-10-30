// The control device is also used by gamepads and permits interactive users.
// HID keyboard/mouse injection needs a stronger boundary: authorize every
// request, not merely creation or possession of a file object. A failure to
// obtain the client token or verify its enabled Administrators/SYSTEM identity
// denies the request. The framework restores the driver token on return.
EVT_WDF_REQUEST_IMPERSONATE evt_keyboard_mouse_authorize;
void evt_keyboard_mouse_authorize(WDFREQUEST, PVOID context) {
  auto *allowed = static_cast<bool *>(context);
  HANDLE token = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) return;
  alignas(void *) unsigned char user_buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] {};
  DWORD bytes = 0;
  const bool is_system = GetTokenInformation(token, TokenUser, user_buffer,
    static_cast<DWORD>(sizeof(user_buffer)), &bytes) &&
    IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(user_buffer)->User.Sid, WinLocalSystemSid);
  unsigned char admin_sid[SECURITY_MAX_SID_SIZE] {};
  DWORD sid_size = static_cast<DWORD>(sizeof(admin_sid));
  BOOL is_admin = FALSE;
  const bool admin_checked = CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
    admin_sid, &sid_size) && CheckTokenMembership(token, admin_sid, &is_admin);
  CloseHandle(token);
  *allowed = is_system || (admin_checked && is_admin);
}

[[nodiscard]] bool keyboard_mouse_authorized(WDFREQUEST request) noexcept {
  bool allowed = false;
  const auto status = WdfRequestImpersonate(request, SecurityIdentification,
    evt_keyboard_mouse_authorize, &allowed);
  return NT_SUCCESS(status) && allowed;
}

// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Included once by driver.cpp inside its private namespace, after the actual
// device/locks/local-target definitions. No replacement WDF/VHF implementation.
using keyboard_mouse_endpoint = lvg::driver::keyboard_mouse_endpoint;
EVT_VHF_READY_FOR_NEXT_READ_REPORT evt_keyboard_mouse_ready;

#if defined(LVG_KM_TEST_HOOKS)
// The portable actual-body fixture pauses after rundown reservation and before
// the real VHF call. This seam is absent from shipping builds.
extern void keyboard_mouse_test_before_submit(VHFHANDLE handle);
#endif

[[nodiscard]] NTSTATUS keyboard_mouse_status(const keyboard_mouse_endpoint::result value) noexcept {
  using result = keyboard_mouse_endpoint::result;
  switch (value) {
    case result::accepted: case result::unchanged: return STATUS_SUCCESS;
    case result::invalid: return STATUS_INVALID_PARAMETER;
    case result::not_owner: return STATUS_ACCESS_DENIED;
    case result::not_ready: return STATUS_DEVICE_NOT_READY;
    case result::busy: return STATUS_DEVICE_BUSY;
    case result::unsupported: return STATUS_NOT_SUPPORTED;
  }
  return STATUS_INVALID_DEVICE_REQUEST;
}

void lock_keyboard_mouse_rundown(keyboard_mouse_slot &slot) noexcept {
  AcquireSRWLockExclusive(&slot.rundown_lock);
}

void unlock_keyboard_mouse_rundown(keyboard_mouse_slot &slot) noexcept {
  ReleaseSRWLockExclusive(&slot.rundown_lock);
}

void notify_keyboard_mouse_rundown(keyboard_mouse_slot &slot) noexcept {
  WakeAllConditionVariable(&slot.rundown_drained);
}

// state_lock and rundown_lock held. Retire the activation before closing the
// admission gate. Its callback context remains stable through Delete and the
// drain of entries already admitted, so callbacks during teardown can reject
// the retired generation before touching slot state.
void stop_keyboard_mouse_admission_locked(keyboard_mouse_slot &slot) noexcept {
  if (++slot.generation == 0) ++slot.generation;
  if (slot.state == slot_state::empty) return;
  if (slot.callback_context != nullptr) {
    InterlockedExchange(&slot.callback_context->retired, 1);
  }
  slot.rundown_stopping = true;
  slot.state = slot_state::stopping;
}

// state_lock and rundown_lock held; only before VhfCreate or after VhfDelete.
void reset_keyboard_mouse_locked(keyboard_mouse_slot &slot) noexcept {
  slot.core.reset_after_device_deleted();
  slot.vhf = nullptr;
  slot.callback_context = nullptr;
  slot.owner = nullptr;
  slot.state = slot_state::empty;
  slot.read_transfer = {};
  slot.submit_owners = 0;
  slot.pump_active = false;
  slot.rundown_stopping = false;
}

void reset_keyboard_mouse(device_context *const context) noexcept {
  auto &slot = context->keyboard_mouse;
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  reset_keyboard_mouse_locked(slot);
  unlock_context(context);
  notify_keyboard_mouse_rundown(slot);
  unlock_keyboard_mouse_rundown(slot);
}

// Callback callers never acquire lifetime_gate. Readiness can reenter while a
// serialized start/delete caller owns that gate, so this path uses only short
// local locks and never waits on lifetime_gate. expected_context rejects
// readiness from a retired activation.
[[nodiscard]] NTSTATUS pump_keyboard_mouse(
  device_context *const context,
  keyboard_mouse_callback_context *const expected_context = nullptr) noexcept {
  auto &slot = context->keyboard_mouse;
  lock_keyboard_mouse_rundown(slot);
  // A callback that races retirement must return using its activation context
  // and this embedded SRW lock before touching state_lock.
  if (expected_context != nullptr &&
      InterlockedCompareExchange(&expected_context->retired, 0, 0) != 0) {
    unlock_keyboard_mouse_rundown(slot);
    return STATUS_SUCCESS;
  }
  lock_context(context);
  const auto callback_is_current = [&]() noexcept {
    return expected_context == nullptr ||
      (slot.callback_context == expected_context &&
       slot.generation == expected_context->generation &&
       InterlockedCompareExchange(&expected_context->retired, 0, 0) == 0);
  };
  const bool can_pump = !slot.rundown_stopping && !context->stopping && context->powered &&
    slot.vhf != nullptr && callback_is_current() &&
    (slot.state == slot_state::starting || slot.state == slot_state::active) &&
    (slot.owner == nullptr || !get_keyboard_mouse_file_context(slot.owner)->cleanup_started);
  if (!can_pump || slot.pump_active) {
    unlock_context(context);
    unlock_keyboard_mouse_rundown(slot);
    return STATUS_SUCCESS;
  }
  slot.pump_active = true;
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);

  NTSTATUS result = STATUS_SUCCESS;
  for (;;) {
    VHFHANDLE handle = nullptr;
    const keyboard_mouse_endpoint::report *next = nullptr;
    lock_keyboard_mouse_rundown(slot);
    lock_context(context);
    const bool still_current = !slot.rundown_stopping && !context->stopping && context->powered &&
      slot.vhf != nullptr && callback_is_current() &&
      (slot.state == slot_state::starting || slot.state == slot_state::active) &&
      (slot.owner == nullptr || !get_keyboard_mouse_file_context(slot.owner)->cleanup_started);
    if (still_current) {
      next = slot.core.take_report();
      if (next != nullptr) {
        slot.read_transfer = {
          const_cast<PUCHAR>(next->bytes.data()), next->length, next->id};
        handle = slot.vhf;
        ++slot.submit_owners;
      }
    }
    if (next == nullptr) {
      // Disarm the pump in the same critical section that observes no report.
      // A readiness callback either granted credit before this check (so the
      // report is taken above) or runs after pump_active is cleared and starts
      // a new pump. There is no snapshot-to-disarm lost wakeup window.
      slot.pump_active = false;
      notify_keyboard_mouse_rundown(slot);
    }
    unlock_context(context);
    unlock_keyboard_mouse_rundown(slot);

    if (next == nullptr) return result;
#if defined(LVG_KM_TEST_HOOKS)
    keyboard_mouse_test_before_submit(handle);
#endif
    // The rundown owner pins handle, packet, and retained report storage until
    // this call returns. Neither WDF lock nor the rundown SRW lock spans it.
    const NTSTATUS status = VhfReadReportSubmit(handle, &slot.read_transfer);

    lock_keyboard_mouse_rundown(slot);
    if (expected_context != nullptr &&
        InterlockedCompareExchange(&expected_context->retired, 0, 0) != 0) {
      // Retirement may race the native call. Release the rundown reservation
      // without touching WDF state; teardown will discard the retained core.
      if (slot.submit_owners != 0) --slot.submit_owners;
      slot.pump_active = false;
      notify_keyboard_mouse_rundown(slot);
      unlock_keyboard_mouse_rundown(slot);
      return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }
    lock_context(context);
    if (!NT_SUCCESS(status)) {
      // Failure is uncertain delivery. Keep retained bytes until deletion and
      // forbid replay through SendInput or replacement readiness credit.
      slot.core.submission_failed();
      result = STATUS_DEVICE_NOT_READY;
    }
    if (slot.submit_owners != 0) --slot.submit_owners;
    if (!NT_SUCCESS(status)) slot.pump_active = false;
    unlock_context(context);
    notify_keyboard_mouse_rundown(slot);
    unlock_keyboard_mouse_rundown(slot);
    if (!NT_SUCCESS(status)) return result;
  }
}

void evt_keyboard_mouse_ready(PVOID client_context) {
  auto *const callback = static_cast<keyboard_mouse_callback_context *>(client_context);
  if (callback == nullptr) return;
  // Acquire an in-flight reference before reading parent/slot. Stop drains
  // admitted entries before reclaiming this per-activation context.
  InterlockedIncrement(&callback->active_entries);
  if (InterlockedCompareExchange(&callback->retired, 0, 0) != 0) {
    AcquireSRWLockExclusive(&callback->entry_lock);
    InterlockedDecrement(&callback->active_entries);
    WakeAllConditionVariable(&callback->entry_drained);
    ReleaseSRWLockExclusive(&callback->entry_lock);
    return;
  }
  auto *const context = callback->parent;
  if (context == nullptr) {
    AcquireSRWLockExclusive(&callback->entry_lock);
    InterlockedDecrement(&callback->active_entries);
    WakeAllConditionVariable(&callback->entry_drained);
    ReleaseSRWLockExclusive(&callback->entry_lock);
    return;
  }
  auto &slot = context->keyboard_mouse;
  if (callback->slot != &slot) {
    AcquireSRWLockExclusive(&callback->entry_lock);
    InterlockedDecrement(&callback->active_entries);
    WakeAllConditionVariable(&callback->entry_drained);
    ReleaseSRWLockExclusive(&callback->entry_lock);
    return;
  }

  bool admitted = false;
  lock_keyboard_mouse_rundown(slot);
  if (InterlockedCompareExchange(&callback->retired, 0, 0) == 0) {
    lock_context(context);
    if (!slot.rundown_stopping && !context->stopping && context->powered && slot.owner != nullptr &&
        slot.callback_context == callback && slot.generation == callback->generation &&
        slot.vhf != nullptr &&
        (slot.state == slot_state::starting || slot.state == slot_state::active) &&
        !get_keyboard_mouse_file_context(slot.owner)->cleanup_started) {
      slot.core.ready_for_report();
      admitted = true;
    }
    unlock_context(context);
  }
  unlock_keyboard_mouse_rundown(slot);
  if (!admitted) {
    AcquireSRWLockExclusive(&callback->entry_lock);
    InterlockedDecrement(&callback->active_entries);
    WakeAllConditionVariable(&callback->entry_drained);
    ReleaseSRWLockExclusive(&callback->entry_lock);
    return;
  }
  std::ignore = pump_keyboard_mouse(context, callback);
  AcquireSRWLockExclusive(&callback->entry_lock);
  InterlockedDecrement(&callback->active_entries);
  WakeAllConditionVariable(&callback->entry_drained);
  ReleaseSRWLockExclusive(&callback->entry_lock);
}

// lifetime_gate held by every caller. Waits release the rundown lock, and the
// fallback poll also leaves it released so an owner can always finish.
void delete_stopped_keyboard_mouse(
  device_context *const context, const bool retain_lost_owner = false) noexcept {
  auto &slot = context->keyboard_mouse;
  lock_keyboard_mouse_rundown(slot);
  while (slot.pump_active || slot.submit_owners != 0) {
    if (!SleepConditionVariableSRW(
          &slot.rundown_drained, &slot.rundown_lock, INFINITE, 0)) {
      // The condition arguments are valid for this SRW lock. If the runtime
      // nevertheless rejects the wait, poll without holding the lock so a
      // submit owner can always retire before deletion.
      unlock_keyboard_mouse_rundown(slot);
      Sleep(1);
      lock_keyboard_mouse_rundown(slot);
    }
  }
  unlock_keyboard_mouse_rundown(slot);

  VHFHANDLE handle = nullptr;
  keyboard_mouse_callback_context *retired_context = nullptr;
  WDFFILEOBJECT lost_owner = nullptr;
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (slot.state == slot_state::stopping) {
    if (retain_lost_owner) lost_owner = slot.owner;
    handle = slot.vhf;
    retired_context = slot.callback_context;
    slot.vhf = nullptr;
    slot.core.submission_failed();
  }
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);

  if (handle != nullptr) VhfDelete(handle, TRUE);

  // Keep the context alive through synchronous delete/cleanup and drain every
  // callback already admitted. Reclaiming it after this boundary relies on
  // VhfUm having quiesced ready-callback entry by VhfDelete(TRUE) return; the
  // official docs do not establish that exact guarantee, so native use stays
  // unqualified until that assumption is independently validated.
  if (retired_context != nullptr) {
    AcquireSRWLockExclusive(&retired_context->entry_lock);
    while (InterlockedCompareExchange(&retired_context->active_entries, 0, 0) != 0) {
      if (!SleepConditionVariableSRW(
            &retired_context->entry_drained, &retired_context->entry_lock, INFINITE, 0)) {
        ReleaseSRWLockExclusive(&retired_context->entry_lock);
        Sleep(1);
        AcquireSRWLockExclusive(&retired_context->entry_lock);
      }
    }
    ReleaseSRWLockExclusive(&retired_context->entry_lock);
  }

  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (slot.state == slot_state::stopping && slot.vhf == nullptr) {
    reset_keyboard_mouse_locked(slot);
    // No report/key/button state survives power loss. Keep only diagnostic
    // ownership until explicit reopen/abort/cleanup, never an active lease.
    if (lost_owner != nullptr && !get_keyboard_mouse_file_context(lost_owner)->cleanup_started) {
      slot.owner = lost_owner;
      slot.state = slot_state::lost;
    }
  }
  unlock_context(context);
  notify_keyboard_mouse_rundown(slot);
  unlock_keyboard_mouse_rundown(slot);
  delete retired_context;
}

[[nodiscard]] NTSTATUS open_keyboard_mouse(device_context *const context, const WDFFILEOBJECT owner) noexcept {
  auto &slot = context->keyboard_mouse;
  constexpr wchar_t instance[] = L"VibeshineKeyboardMouse";
  // Configuration Manager is an OS-owned call. Do not hold lifetime_gate or a
  // callback lock while checking that an old PnP instance finished removal.
  lock_context(context);
  const auto generation = slot.generation;
  const bool available = slot.state == slot_state::empty ||
    (slot.state == slot_state::lost && slot.owner == owner);
  const bool live = !context->stopping && context->powered &&
    !get_keyboard_mouse_file_context(owner)->cleanup_started;
  unlock_context(context);
  if (!live) return STATUS_DEVICE_NOT_READY;
  if (!available) return STATUS_DEVICE_BUSY;
  const bool child_present = previous_instance_present(instance);

  lock_lifetime(context);
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (context->stopping || !context->powered || get_keyboard_mouse_file_context(owner)->cleanup_started) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  if (slot.generation != generation || child_present ||
      (slot.state != slot_state::empty && !(slot.state == slot_state::lost && slot.owner == owner))) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_DEVICE_BUSY;
  }
  slot.rundown_stopping = false;
  slot.submit_owners = 0;
  slot.pump_active = false;
  if (++slot.generation == 0) ++slot.generation;
  slot.callback_context = nullptr;
  slot.vhf = nullptr;
  slot.owner = owner;
  slot.state = slot_state::starting;
  // Standard absolute mouse HID coordinates address the primary display.
  // Advertise that domain explicitly so the host can transform its geometry.
  const NTSTATUS admitted = keyboard_mouse_status(
    slot.core.open(reinterpret_cast<keyboard_mouse_endpoint::owner_token>(owner), lvg::km::k_primary_absolute_generation));
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  if (!NT_SUCCESS(admitted)) {
    reset_keyboard_mouse(context);
    unlock_lifetime(context);
    return admitted;
  }

  NTSTATUS status = ensure_vhf_target_open(context);
  if (!NT_SUCCESS(status)) {
    reset_keyboard_mouse(context);
    unlock_lifetime(context);
    return status;
  }

  lock_context(context);
  const HANDLE file = context->vhf_file_handle;
  const WDFIOTARGET target = context->local_vhf_target;
  unlock_context(context);
  if (file == nullptr || target == nullptr) {
    reset_keyboard_mouse(context);
    unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }

  auto *const callback = new (std::nothrow) keyboard_mouse_callback_context {};
  if (callback == nullptr) {
    reset_keyboard_mouse(context);
    unlock_lifetime(context);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  callback->parent = context;
  callback->slot = &slot;
  callback->generation = slot.generation;
  InitializeSRWLock(&callback->entry_lock);
  InitializeConditionVariable(&callback->entry_drained);

  VHF_CONFIG config;
  VHF_CONFIG_INIT(&config, file,
    static_cast<USHORT>(lvg::driver::keyboard_mouse_descriptor_size),
    const_cast<PUCHAR>(lvg::driver::keyboard_mouse_descriptor));
  config.VhfClientContext = callback;
  config.EvtVhfReadyForNextReadReport = evt_keyboard_mouse_ready;
  config.EvtVhfCleanup = evt_vhf_cleanup;
  // Stable instance/container separation from all controller slots. GUIDs do
  // not assign a USB VID/PID or establish project ownership of supplied IDs.
  slot.container_id = {0x5d43cc5c, 0x318a, 0x46f4, {0x8a, 0x8f, 0x2d, 0x1d, 0x3c, 0x99, 0x80, 0x21}};
  static_assert(sizeof(instance) <= sizeof(slot.instance_id));
  std::memcpy(slot.instance_id, instance, sizeof(instance));
  config.ContainerID = slot.container_id;
  config.InstanceID = slot.instance_id;
  config.InstanceIDLength = static_cast<USHORT>(sizeof(instance));
  config.VendorID = static_cast<USHORT>(LVG_KM_VENDOR_ID);
  config.ProductID = static_cast<USHORT>(LVG_KM_PRODUCT_ID);
  config.VersionNumber = 1;

  VHFHANDLE handle = nullptr;
  status = VhfCreate(&config, &handle);
  if (NT_SUCCESS(status) && handle == nullptr) status = STATUS_INVALID_HANDLE;
  if (!NT_SUCCESS(status)) {
    delete callback; // VHF does not call callbacks before VhfStart.
    reset_keyboard_mouse(context);
    unlock_lifetime(context);
    return status;
  }

  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  slot.callback_context = callback;
  slot.vhf = handle;
  const bool may_start = !context->stopping && context->powered &&
    !get_keyboard_mouse_file_context(owner)->cleanup_started;
  if (!may_start) stop_keyboard_mouse_admission_locked(slot);
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  if (!may_start) {
    delete_stopped_keyboard_mouse(context);
    unlock_lifetime(context);
    return STATUS_CANCELLED;
  }

  // All callback, generation, and packet storage is published before Start;
  // callbacks are allowed before VhfStart returns and can submit reentrantly.
  status = VhfStart(handle);
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  const bool adopted = NT_SUCCESS(status) && !context->stopping && context->powered &&
    !get_keyboard_mouse_file_context(owner)->cleanup_started &&
    slot.callback_context == callback && slot.owner == owner &&
    slot.state == slot_state::starting &&
    slot.core.lifecycle() == keyboard_mouse_endpoint::state::active;
  if (adopted) slot.state = slot_state::active;
  else stop_keyboard_mouse_admission_locked(slot);
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  if (!adopted) {
    delete_stopped_keyboard_mouse(context);
    unlock_lifetime(context);
    return NT_SUCCESS(status) ? STATUS_CANCELLED : status;
  }
  unlock_lifetime(context);
  return STATUS_SUCCESS;
}

template<class request_t>
[[nodiscard]] NTSTATUS submit_keyboard_mouse(device_context *const context,
  const WDFFILEOBJECT owner, const request_t &input) noexcept {
  lock_lifetime(context);
  lock_context(context);
  auto &slot = context->keyboard_mouse;
  NTSTATUS status = STATUS_DEVICE_NOT_READY;
  if (slot.owner != owner) status = STATUS_ACCESS_DENIED;
  else if (slot.state == slot_state::lost) status = STATUS_DEVICE_REMOVED;
  else if (context->powered && !context->stopping && !get_keyboard_mouse_file_context(owner)->cleanup_started &&
           slot.state == slot_state::active) {
    status = keyboard_mouse_status(slot.core.submit(
      reinterpret_cast<keyboard_mouse_endpoint::owner_token>(owner), input));
  }
  unlock_context(context);
  unlock_lifetime(context);
  // The queue mutation is serialized above. The pump reserves its own submit
  // owner under rundown_lock, so lifetime_gate can be released before VHF.
  if (NT_SUCCESS(status)) status = pump_keyboard_mouse(context);
  return status;
}

[[nodiscard]] NTSTATUS close_keyboard_mouse(device_context *const context, const WDFFILEOBJECT owner) noexcept {
  auto &slot = context->keyboard_mouse;
  lock_lifetime(context);
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (slot.owner != owner) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_ACCESS_DENIED;
  }
  if (slot.state == slot_state::lost) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_DEVICE_REMOVED;
  }
  if (context->stopping || !context->powered || slot.state != slot_state::active ||
      get_keyboard_mouse_file_context(owner)->cleanup_started) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_DEVICE_NOT_READY;
  }
  const auto phase = slot.core.lifecycle();
  if (phase == keyboard_mouse_endpoint::state::faulted || phase == keyboard_mouse_endpoint::state::drained) {
    stop_keyboard_mouse_admission_locked(slot);
    unlock_context(context);
    unlock_keyboard_mouse_rundown(slot);
    delete_stopped_keyboard_mouse(context);
    unlock_lifetime(context);
    // A faulted removal is not a successful neutral-drain acknowledgement.
    return phase == keyboard_mouse_endpoint::state::drained ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
  }
  const auto status = keyboard_mouse_status(slot.core.close(
    reinterpret_cast<keyboard_mouse_endpoint::owner_token>(owner)));
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  unlock_lifetime(context);
  const auto pumped = NT_SUCCESS(status) ? pump_keyboard_mouse(context) : status;
  // Never hold a WDF request waiting for HID consumption. The client retries;
  // cleanup/PnP can always force synchronous deletion if it never drains.
  return NT_SUCCESS(pumped) ? STATUS_DEVICE_BUSY : pumped;
}

[[nodiscard]] NTSTATUS abort_keyboard_mouse(device_context *const context, const WDFFILEOBJECT owner) noexcept {
  auto &slot = context->keyboard_mouse;
  lock_lifetime(context);
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (slot.state == slot_state::empty) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_SUCCESS;
  }
  if (slot.owner != owner) {
    unlock_context(context); unlock_keyboard_mouse_rundown(slot); unlock_lifetime(context);
    return STATUS_ACCESS_DENIED;
  }
  stop_keyboard_mouse_admission_locked(slot);
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  delete_stopped_keyboard_mouse(context);
  unlock_lifetime(context);
  return STATUS_SUCCESS; // Forced removal, never a neutral-drain claim.
}

void evt_keyboard_mouse_file_cleanup(WDFFILEOBJECT owner) {
  if (owner == nullptr) return; // The internal local-target create may have no file.
  auto *const context = get_device_context(WdfFileObjectGetDevice(owner));
  // Publish cleanup intent before waiting for a creator/submitter. Never wait
  // on lifetime_gate while holding state_lock: VhfStart may call back into it.
  lock_context(context);
  get_keyboard_mouse_file_context(owner)->cleanup_started = true;
  unlock_context(context);
  lock_lifetime(context);
  auto &slot = context->keyboard_mouse;
  lock_keyboard_mouse_rundown(slot);
  lock_context(context);
  if (slot.owner == owner) stop_keyboard_mouse_admission_locked(slot);
  const bool stopping = slot.owner == owner && slot.state == slot_state::stopping;
  unlock_context(context);
  unlock_keyboard_mouse_rundown(slot);
  if (stopping) delete_stopped_keyboard_mouse(context);
  unlock_lifetime(context);
}

template<class request_t>
[[nodiscard]] NTSTATUS retrieve_keyboard_mouse_request(WDFREQUEST request,
  const std::size_t input_length, request_t *const output) noexcept {
  // Callback length first, then actual raw-buffer length, before any typed read.
  if (input_length != sizeof(request_t)) return STATUS_INVALID_BUFFER_SIZE;
  PVOID raw = nullptr;
  std::size_t length = 0;
  const auto status = WdfRequestRetrieveInputBuffer(request, sizeof(request_t), &raw, &length);
  if (!NT_SUCCESS(status)) return status;
  if (raw == nullptr || length != sizeof(request_t)) return STATUS_INVALID_BUFFER_SIZE;
  std::memcpy(output, raw, sizeof(*output));
  return lvg::km::valid(*output) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

template<class request_t>
[[nodiscard]] NTSTATUS complete_keyboard_mouse_submission(device_context *const context,
  const WDFFILEOBJECT owner, WDFREQUEST request, const request_t &input,
  const std::size_t output_length, ULONG_PTR *const information) noexcept {
  using namespace lvg::km;
  // Validate the complete reply boundary before accepting any event. It can
  // alias the raw request, which has already been copied into local input.
  if (output_length != sizeof(submission_response)) return STATUS_INVALID_BUFFER_SIZE;
  PVOID raw = nullptr;
  std::size_t length = 0;
  const auto retrieved = WdfRequestRetrieveOutputBuffer(request, sizeof(submission_response), &raw, &length);
  if (!NT_SUCCESS(retrieved)) return retrieved;
  if (raw == nullptr || length != sizeof(submission_response)) return STATUS_INVALID_BUFFER_SIZE;
  const auto status = submit_keyboard_mouse(context, owner, input);
  if (!NT_SUCCESS(status) && status != STATUS_DEVICE_BUSY) return status;
  auto response = lvg::km::request<submission_response>();
  response.acceptance = status == STATUS_DEVICE_BUSY ? submission_acceptance::busy : submission_acceptance::accepted;
  std::memcpy(raw, &response, sizeof(response));
  *information = sizeof(response);
  return STATUS_SUCCESS;
}

[[nodiscard]] bool dispatch_keyboard_mouse(device_context *const context, const WDFFILEOBJECT owner,
  WDFREQUEST request, const std::size_t output_length, const std::size_t input_length,
  const ULONG code, NTSTATUS *const status, ULONG_PTR *const information) noexcept {
  using namespace lvg::km;
  // Unrecognized controls remain available to the gamepad dispatcher.
  switch (code) {
    case ioctl_query: case ioctl_open: case ioctl_close: case ioctl_abort:
    case ioctl_keyboard: case ioctl_button: case ioctl_relative: case ioctl_absolute: case ioctl_consumer:
      if (!keyboard_mouse_authorized(request)) {
        *status = STATUS_ACCESS_DENIED;
        *information = 0;
        return true;
      }
      break;
    default: return false;
  }
  switch (code) {
    case ioctl_query: {
      control_request input {};
      *status = retrieve_keyboard_mouse_request(request, input_length, &input);
      if (!NT_SUCCESS(*status)) break;
      if (output_length != sizeof(query_response)) { *status = STATUS_INVALID_BUFFER_SIZE; break; }
      PVOID raw = nullptr;
      std::size_t length = 0;
      *status = WdfRequestRetrieveOutputBuffer(request, sizeof(query_response), &raw, &length);
      if (!NT_SUCCESS(*status)) break;
      if (raw == nullptr || length != sizeof(query_response)) { *status = STATUS_INVALID_BUFFER_SIZE; break; }
      lock_context(context);
      const bool available = !context->stopping && context->powered && !get_keyboard_mouse_file_context(owner)->cleanup_started;
      unlock_context(context);
      if (!available) { *status = STATUS_DEVICE_NOT_READY; break; }
      auto response = lvg::km::request<query_response>();
      response.capabilities = k_primary_capabilities;
      response.queue_capacity = static_cast<std::uint32_t>(keyboard_mouse_endpoint::queue_capacity);
      response.absolute_target_generation = k_primary_absolute_generation;
      std::memcpy(raw, &response, sizeof(response));
      *information = sizeof(response);
      break;
    }
    case ioctl_open: case ioctl_close: case ioctl_abort: {
      control_request input {};
      *status = retrieve_keyboard_mouse_request(request, input_length, &input);
      if (NT_SUCCESS(*status) && output_length != 0) *status = STATUS_INVALID_BUFFER_SIZE;
      if (NT_SUCCESS(*status)) {
        if (code == ioctl_open) *status = open_keyboard_mouse(context, owner);
        else if (code == ioctl_abort) *status = abort_keyboard_mouse(context, owner);
        else *status = close_keyboard_mouse(context, owner);
      }
      break;
    }
#define LVG_KM_SUBMIT_CASE(control, type) \
    case control: { \
      type input {}; \
      *status = retrieve_keyboard_mouse_request(request, input_length, &input); \
      if (NT_SUCCESS(*status)) *status = complete_keyboard_mouse_submission(context, owner, request, input, output_length, information); \
      break; \
    }
    LVG_KM_SUBMIT_CASE(ioctl_keyboard, keyboard_request)
    LVG_KM_SUBMIT_CASE(ioctl_consumer, consumer_request)
    LVG_KM_SUBMIT_CASE(ioctl_button, button_request)
    LVG_KM_SUBMIT_CASE(ioctl_relative, relative_request)
    LVG_KM_SUBMIT_CASE(ioctl_absolute, absolute_request)
#undef LVG_KM_SUBMIT_CASE
    default: return false;
  }
  return true;
}
