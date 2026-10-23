// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <vector>

// A non-recursive real mutex: same-thread reentry fails immediately instead of
// hanging a test. Other threads block, permitting deterministic teardown tests.
struct fake_wdf_object {
  void *context {};
  fake_wdf_object *parent {};
  std::mutex mutex;
  std::atomic<int> waiters {};
  bool armed {};
  // Deterministic METHOD_BUFFERED boundary for actual dispatcher fixtures.
  void *input_buffer {}, *output_buffer {};
  size_t input_length {}, output_length {};
  NTSTATUS input_status {STATUS_SUCCESS}, output_status {STATUS_SUCCESS};
  NTSTATUS completion_status {STATUS_NOT_SUPPORTED};
  ULONG_PTR information {};
  int completions {};
};
using WDFOBJECT = fake_wdf_object *;
using WDFDEVICE = WDFOBJECT;
using WDFDRIVER = WDFOBJECT;
using WDFWAITLOCK = WDFOBJECT;
using WDFIOTARGET = WDFOBJECT;
using WDFFILEOBJECT = WDFOBJECT;
using WDFTIMER = WDFOBJECT;
using WDFREQUEST = WDFOBJECT;
using WDFQUEUE = WDFOBJECT;
using WDFCMRESLIST = WDFOBJECT;
using PWDFDEVICE_INIT = WDFOBJECT;
using EVT_WDF_DRIVER_DEVICE_ADD = NTSTATUS(WDFDRIVER, PWDFDEVICE_INIT);
using EVT_WDF_DEVICE_PREPARE_HARDWARE = NTSTATUS(WDFDEVICE, WDFCMRESLIST, WDFCMRESLIST);
using EVT_WDF_DEVICE_RELEASE_HARDWARE = NTSTATUS(WDFDEVICE, WDFCMRESLIST);
using EVT_WDF_OBJECT_CONTEXT_CLEANUP = void(WDFOBJECT);
using EVT_WDF_OBJECT_CONTEXT_DESTROY = void(WDFOBJECT);
using EVT_WDF_DEVICE_FILE_CREATE = void(WDFDEVICE, WDFREQUEST, WDFFILEOBJECT);
using EVT_WDF_FILE_CLOSE = void(WDFFILEOBJECT);
using EVT_WDF_FILE_CLEANUP = void(WDFFILEOBJECT);
using EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL = void(WDFQUEUE, WDFREQUEST, size_t, size_t, ULONG);
using EVT_WDF_TIMER = void(WDFTIMER);
inline thread_local std::vector<WDFWAITLOCK> fake_held_locks;
inline bool fake_holds(WDFWAITLOCK lock) {
  return std::find(fake_held_locks.begin(), fake_held_locks.end(), lock) != fake_held_locks.end();
}
inline void fake_require(bool condition, const char *message) {
  if (!condition) { std::fprintf(stderr, "WDF/VHF boundary violation: %s\n", message); std::abort(); }
}
inline NTSTATUS WdfWaitLockAcquire(WDFWAITLOCK lock, void *) {
  fake_require(lock && !fake_holds(lock), "invalid/recursive wait lock");
  ++lock->waiters;
  lock->mutex.lock();
  --lock->waiters;
  fake_held_locks.push_back(lock);
  return STATUS_SUCCESS;
}
inline void WdfWaitLockRelease(WDFWAITLOCK lock) {
  fake_require(fake_holds(lock), "release without ownership");
  fake_held_locks.erase(std::find(fake_held_locks.begin(), fake_held_locks.end(), lock));
  lock->mutex.unlock();
}
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type, name) inline type *name(WDFOBJECT o) { return static_cast<type *>(o->context); }
struct WDF_IO_TARGET_OPEN_PARAMS {};
inline void WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_FILE(WDF_IO_TARGET_OPEN_PARAMS *, void *) {}
inline NTSTATUS fake_open_status = STATUS_SUCCESS;
inline bool fake_missing_file = false;
inline int fake_opens {}, fake_closes {};
inline std::function<void()> fake_close_hook;
inline NTSTATUS WdfIoTargetOpen(WDFIOTARGET, WDF_IO_TARGET_OPEN_PARAMS *) { ++fake_opens; return fake_open_status; }
inline HANDLE WdfIoTargetWdmGetTargetFileHandle(WDFIOTARGET target) { return fake_missing_file ? nullptr : target; }
inline void WdfIoTargetClose(WDFIOTARGET) { ++fake_closes; if (fake_close_hook) fake_close_hook(); }
inline bool WdfTimerStart(WDFTIMER timer, LONG) { timer->armed = true; return true; }
inline bool WdfTimerStop(WDFTIMER timer, BOOLEAN) { timer->armed = false; return true; }
inline WDFOBJECT WdfTimerGetParentObject(WDFTIMER timer) { return timer->parent; }
#define WDF_REL_TIMEOUT_IN_MS(ms) (ms)
inline WDFDEVICE WdfFileObjectGetDevice(WDFFILEOBJECT o) { return o->parent; }
inline WDFDEVICE WdfIoQueueGetDevice(WDFQUEUE o) { return o->parent; }
inline WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST o) { return o->parent; }

// Deterministic raw request boundary; this does not model Windows/UMDF
// marshalling, OS power transitions, automatic object deletion, or IRQL.
inline NTSTATUS WdfRequestRetrieveInputBuffer(WDFREQUEST r, size_t minimum, PVOID *out, size_t *length) {
  if (!r || !out) return STATUS_INVALID_PARAMETER;
  if (!NT_SUCCESS(r->input_status)) return r->input_status;
  if (!r->input_buffer || r->input_length < minimum) return STATUS_BUFFER_TOO_SMALL;
  *out = r->input_buffer; if (length) *length = r->input_length; return STATUS_SUCCESS;
}
inline NTSTATUS WdfRequestRetrieveOutputBuffer(WDFREQUEST r, size_t minimum, PVOID *out, size_t *length) {
  if (!r || !out) return STATUS_INVALID_PARAMETER;
  if (!NT_SUCCESS(r->output_status)) return r->output_status;
  if (!r->output_buffer || r->output_length < minimum) return STATUS_BUFFER_TOO_SMALL;
  *out = r->output_buffer; if (length) *length = r->output_length; return STATUS_SUCCESS;
}
inline void WdfRequestComplete(WDFREQUEST r, NTSTATUS status) {
  if (!r) return;
  ++r->completions; r->completion_status = status; r->information = 0;
}
inline void WdfRequestCompleteWithInformation(WDFREQUEST r, NTSTATUS status, ULONG_PTR information) {
  WdfRequestComplete(r, status); if (r) r->information = information;
}
struct WDF_FILEOBJECT_CONFIG {
  EVT_WDF_DEVICE_FILE_CREATE *EvtDeviceFileCreate {};
  EVT_WDF_FILE_CLOSE *EvtFileClose {};
  EVT_WDF_FILE_CLEANUP *EvtFileCleanup {};
  bool AutoForwardCleanupClose {true};
};
inline void WDF_FILEOBJECT_CONFIG_INIT(
  WDF_FILEOBJECT_CONFIG *c,
  EVT_WDF_DEVICE_FILE_CREATE *create,
  EVT_WDF_FILE_CLOSE *close,
  EVT_WDF_FILE_CLEANUP *cleanup) {
  *c = {create, close, cleanup, true};
}
struct WDF_OBJECT_ATTRIBUTES {
  WDFOBJECT ParentObject {};
  EVT_WDF_OBJECT_CONTEXT_CLEANUP *EvtCleanupCallback {};
  EVT_WDF_OBJECT_CONTEXT_DESTROY *EvtDestroyCallback {};
  size_t ContextSize {};
};
inline void WDF_OBJECT_ATTRIBUTES_INIT(WDF_OBJECT_ATTRIBUTES *a) { *a = {}; }
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a, type) do { WDF_OBJECT_ATTRIBUTES_INIT(a); (a)->ContextSize = sizeof(type); } while (false)
using WDF_POWER_DEVICE_STATE = int;
using EVT_WDF_DEVICE_D0_ENTRY = NTSTATUS(WDFDEVICE, WDF_POWER_DEVICE_STATE);
using EVT_WDF_DEVICE_D0_EXIT = NTSTATUS(WDFDEVICE, WDF_POWER_DEVICE_STATE);
struct WDF_PNPPOWER_EVENT_CALLBACKS {
  EVT_WDF_DEVICE_D0_ENTRY *EvtDeviceD0Entry {};
  EVT_WDF_DEVICE_D0_EXIT *EvtDeviceD0Exit {};
  EVT_WDF_DEVICE_PREPARE_HARDWARE *EvtDevicePrepareHardware {};
  EVT_WDF_DEVICE_RELEASE_HARDWARE *EvtDeviceReleaseHardware {};
};
inline void WDF_PNPPOWER_EVENT_CALLBACKS_INIT(WDF_PNPPOWER_EVENT_CALLBACKS *p) { *p = {}; }
struct WDF_TIMER_CONFIG { bool AutomaticSerialization {}; };
inline void WDF_TIMER_CONFIG_INIT_PERIODIC(WDF_TIMER_CONFIG *, EVT_WDF_TIMER *, LONG) {}
enum WDF_TRI_STATE { WdfFalse, WdfTrue, WdfUseDefault };
enum WDF_IO_QUEUE_DISPATCH_TYPE { WdfIoQueueDispatchInvalid, WdfIoQueueDispatchSequential };
struct WDF_IO_QUEUE_CONFIG {
  WDF_TRI_STATE PowerManaged {WdfUseDefault};
  WDF_IO_QUEUE_DISPATCH_TYPE DispatchType {};
  bool DefaultQueue {};
  EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL *EvtIoDeviceControl {};
};
inline void WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(WDF_IO_QUEUE_CONFIG *c, WDF_IO_QUEUE_DISPATCH_TYPE dispatch) {
  *c = {}; c->DispatchType = dispatch; c->DefaultQueue = true;
}
struct WDF_DRIVER_CONFIG {
  ULONG DriverPoolTag {};
  EVT_WDF_DRIVER_DEVICE_ADD *EvtDriverDeviceAdd {};
};
inline void WDF_DRIVER_CONFIG_INIT(WDF_DRIVER_CONFIG *c, EVT_WDF_DRIVER_DEVICE_ADD *add) {
  *c = {}; c->EvtDriverDeviceAdd = add;
}

// Supply storage at the OS allocation boundary, never a copy of device-add
// logic. Record the configuration actually passed to WdfIoQueueCreate without
// accepting/rejecting it based on the policy the test intends to verify.
struct fake_wdf_initialization {
  WDFDEVICE device {};
  WDFWAITLOCK lifetime {}, state {};
  WDFIOTARGET target {};
  WDFTIMER timer {};
  WDFQUEUE queue {};
  EVT_WDF_DRIVER_DEVICE_ADD *add {};
  WDF_FILEOBJECT_CONFIG files {};
  size_t file_context_size {};
  WDF_PNPPOWER_EVENT_CALLBACKS pnp {};
  WDF_IO_QUEUE_CONFIG queue_config {};
  EVT_WDF_OBJECT_CONTEXT_CLEANUP *target_cleanup {};
  bool filter {}, filter_at_device_create {}, interface_created {};
  int lock_creates {}, queue_creates {};
  NTSTATUS timer_status {STATUS_SUCCESS}, queue_status {STATUS_SUCCESS};
};
inline fake_wdf_initialization *fake_initialization {};
inline void WdfFdoInitSetFilter(PWDFDEVICE_INIT) {
  if (fake_initialization) fake_initialization->filter = true;
}
inline void WdfDeviceInitSetFileObjectConfig(PWDFDEVICE_INIT, WDF_FILEOBJECT_CONFIG *c, WDF_OBJECT_ATTRIBUTES *a) {
  if (fake_initialization) {
    fake_initialization->files = *c;
    fake_initialization->file_context_size = a->ContextSize;
  }
}
inline void WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT, WDF_PNPPOWER_EVENT_CALLBACKS *p) {
  if (fake_initialization) fake_initialization->pnp = *p;
}
inline NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *init, WDF_OBJECT_ATTRIBUTES *, WDFDEVICE *device) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  auto &f = *fake_initialization;
  fake_require(init && *init && f.device && f.device->context, "missing initialization storage");
  f.filter_at_device_create = f.filter;
  *device = f.device; *init = nullptr;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfTimerCreate(WDF_TIMER_CONFIG *, WDF_OBJECT_ATTRIBUTES *a, WDFTIMER *timer) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  auto &f = *fake_initialization;
  if (!NT_SUCCESS(f.timer_status)) return f.timer_status;
  *timer = f.timer; (*timer)->parent = a->ParentObject;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES *a, WDFWAITLOCK *lock) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  auto &f = *fake_initialization;
  fake_require(f.lock_creates < 2, "unexpected initialization lock");
  *lock = f.lock_creates++ == 0 ? f.lifetime : f.state;
  (*lock)->parent = a->ParentObject;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfIoTargetCreate(WDFDEVICE, WDF_OBJECT_ATTRIBUTES *a, WDFIOTARGET *target) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  auto &f = *fake_initialization;
  *target = f.target; (*target)->parent = a->ParentObject;
  f.target_cleanup = a->EvtCleanupCallback;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfDeviceCreateDeviceInterface(WDFDEVICE, const GUID *, void *) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  fake_initialization->interface_created = true;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfObjectCreate(WDF_OBJECT_ATTRIBUTES *, WDFOBJECT *) { return STATUS_NOT_SUPPORTED; }
inline void WdfObjectDelete(WDFOBJECT) {}
inline NTSTATUS WdfIoQueueCreate(WDFDEVICE device, WDF_IO_QUEUE_CONFIG *c, void *, WDFQUEUE *queue) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  auto &f = *fake_initialization;
  ++f.queue_creates; f.queue_config = *c;
  if (!NT_SUCCESS(f.queue_status)) return f.queue_status;
  f.queue->parent = device;
  if (queue) *queue = f.queue;
  return STATUS_SUCCESS;
}
inline NTSTATUS WdfDriverCreate(PDRIVER_OBJECT, PUNICODE_STRING, void *, WDF_DRIVER_CONFIG *c, void *) {
  if (!fake_initialization) return STATUS_NOT_SUPPORTED;
  fake_initialization->add = c->EvtDriverDeviceAdd;
  return STATUS_SUCCESS;
}
#define WDF_NO_EVENT_CALLBACK nullptr
#define WDF_NO_OBJECT_ATTRIBUTES nullptr
#define WDF_NO_HANDLE nullptr
