// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#if defined(LVG_MOUSE_MAPPER_TEST)
#include "test_native_support.h"
#else
#include <ntddk.h>
#include <wdf.h>
#include <hidclass.h>
#include <hidpddi.h>
#include <hidpi.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include <initguid.h>
#include <devpkey.h>
#endif
#include <libvirtualgamepad/mouse_mapper_control.h>
#include "mapper_core.h"

#ifndef LVG_KM_VENDOR_ID
#define LVG_KM_VENDOR_ID 0
#endif
#ifndef LVG_KM_PRODUCT_ID
#define LVG_KM_PRODUCT_ID 0
#endif

namespace {
using lvg::mouse_mapper::stream;
using lvg::mouse_mapper::verdict;
using lvg::mouse_transport::report;
static_assert(MOUSE_MOVE_ABSOLUTE == 1 && MOUSE_VIRTUAL_DESKTOP == 2);
static_assert(MOUSE_WHEEL == 0x0400 && MOUSE_HWHEEL == 0x0800);
static_assert(MOUSE_LEFT_BUTTON_DOWN == 1 && MOUSE_BUTTON_5_UP == 0x0200);
constexpr auto &interface_id = lvg::mouse_mapper::k_interface_guid;
constexpr GUID mapper_interface = {interface_id.data1,interface_id.data2,interface_id.data3,
  {interface_id.data4[0],interface_id.data4[1],interface_id.data4[2],interface_id.data4[3],
   interface_id.data4[4],interface_id.data4[5],interface_id.data4[6],interface_id.data4[7]}};
static_assert(LVG_KM_VENDOR_ID >= 0 && LVG_KM_VENDOR_ID <= 65535, "Vendor ID must fit in 16 bits");
static_assert(LVG_KM_PRODUCT_ID >= 0 && LVG_KM_PRODUCT_ID <= 65535, "Product ID must fit in 16 bits");
constexpr GUID expected_container = {0x5d43cc5c,0x318a,0x46f4,{0x8a,0x8f,0x2d,0x1d,0x3c,0x99,0x80,0x21}};
struct device_context {
  WDFDEVICE device;
  WDFSPINLOCK lock;
  WDFWAITLOCK lifecycle;
  EX_RUNDOWN_REF pump_callbacks;
  EX_RUNDOWN_REF io_callbacks;
  bool rundowns_closed;
  bool pump_owner;
  bool pump_rerun;
  bool timer_armed;
  PFILE_OBJECT reader_file;
  bool reader_open;
  bool power_on;
  WDFTIMER timer;
  WDFDPC dpc;
  WDFREQUEST ack_request;
  WDFMEMORY ack_memory;
  lvg::mouse_transport::ack_report ack;
  lvg::mouse_transport::uint32_t delivering_sequence;
  NTSTATUS ack_status;
  bool ack_pending;
  bool ack_completed;
  WDFIOTARGET reader;
  WDFREQUEST read;
  WDFMEMORY memory;
  CONNECT_DATA connection;
  GUID container;
  stream core;
  report input;
  NTSTATUS read_status;
  ULONG_PTR read_bytes;
  bool active;
  bool validated;
  bool pending;
  bool completed;
  bool terminal;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(device_context, context)

EVT_WDF_DRIVER_DEVICE_ADD add_device;
EVT_WDF_DEVICE_FILE_CREATE file_create;
EVT_WDF_FILE_CLEANUP file_cleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL status_control;
NTSTATUS query_collection(device_context *c);
NTSTATUS validate_container(device_context *c);
void quiesce(device_context *c);
void queue_pump_locked(device_context *c);
void arm_retry_locked(device_context *c);
void reopen_rundowns(device_context *c);
EVT_WDF_IO_QUEUE_IO_READ forward_read;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL device_control;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL internal_control;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT initialize;
EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND suspend;
EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART restart;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP cleanup;
EVT_WDF_TIMER retry_timer;
EVT_WDF_DPC pump_dpc;
EVT_WDF_REQUEST_COMPLETION_ROUTINE ack_complete;
EVT_WDF_REQUEST_COMPLETION_ROUTINE read_complete;

void forward(WDFDEVICE device, WDFREQUEST request) {
  WdfRequestFormatRequestUsingCurrentType(request);
  WDF_REQUEST_SEND_OPTIONS options;
  WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
  if (!WdfRequestSend(request, WdfDeviceGetIoTarget(device), &options))
    WdfRequestComplete(request, WdfRequestGetStatus(request));
}
void file_create(WDFDEVICE device, WDFREQUEST request, WDFFILEOBJECT file) {
  auto *c = context(device);
  // Mouclass applies its trusted-opener policy above this port. Preserve the
  // forwarded CREATE request's mode; an upper class request need not have
  // been originated by a kernel-mode caller.
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  // MouseClass opens its port before requesting input. Forward its CREATE so
  // HIDClass initializes this FileObject's private context. A raw self-open by
  // interface name would instead target Mouclass at the top of the stack.
  WdfRequestFormatRequestUsingCurrentType(request);
  WDF_REQUEST_SEND_OPTIONS options;
  WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
  (void)WdfRequestSend(request, WdfDeviceGetIoTarget(device), &options);
  auto result = WdfRequestGetStatus(request);
  const bool lower_created = NT_SUCCESS(result);
  if (lower_created) {
    c->reader_file = WdfFileObjectWdmGetFileObject(file);
    ObReferenceObject(c->reader_file);
    WDF_IO_TARGET_OPEN_PARAMS open;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(&open,
      WdfIoTargetWdmGetTargetDeviceObject(WdfDeviceGetIoTarget(device)));
    open.TargetFileObject = c->reader_file;
    result = WdfIoTargetOpen(c->reader, &open);
    if (NT_SUCCESS(result)) {
      c->reader_open = true;
      WDF_OBJECT_ATTRIBUTES request_attributes;
      WDF_OBJECT_ATTRIBUTES_INIT(&request_attributes); request_attributes.ParentObject = c->device;
      result = WdfRequestCreate(&request_attributes, c->reader, &c->read);
      if (NT_SUCCESS(result)) result = WdfRequestCreate(&request_attributes, c->reader, &c->ack_request);
      if (NT_SUCCESS(result)) result = validate_container(c);
      if (NT_SUCCESS(result)) result = query_collection(c);
      if (NT_SUCCESS(result)) {
        reopen_rundowns(c);
        WdfSpinLockAcquire(c->lock);
        c->validated = true;
        c->active = c->power_on && !c->terminal;
        if (c->active) queue_pump_locked(c);
        WdfSpinLockRelease(c->lock);
      } else {
        if (c->read) { WdfObjectDelete(c->read); c->read = nullptr; }
        if (c->ack_request) { WdfObjectDelete(c->ack_request); c->ack_request = nullptr; }
        WdfIoTargetClose(c->reader); c->reader_open = false;
      }
    }
    if (!NT_SUCCESS(result)) { ObDereferenceObject(c->reader_file); c->reader_file = nullptr; }
  }
  if (lower_created && !NT_SUCCESS(result)) {
    WdfSpinLockAcquire(c->lock); c->terminal = true; WdfSpinLockRelease(c->lock);
  }
  WdfWaitLockRelease(c->lifecycle);
  // Preserve a successful lower CREATE so normal CLEANUP/CLOSE can release
  // its HID context even if mapper initialization failed afterwards.
  WdfRequestComplete(request, lower_created ? STATUS_SUCCESS : result);
  if (lower_created && !NT_SUCCESS(result)) WdfDeviceSetFailed(device, WdfDeviceFailedNoRestart);
}
void file_cleanup(WDFFILEOBJECT file) {
  auto *c = context(WdfFileObjectGetDevice(file));
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  if (c->reader_file == WdfFileObjectWdmGetFileObject(file)) {
    quiesce(c);
    if (c->read) { WdfObjectDelete(c->read); c->read = nullptr; }
    if (c->ack_request) { WdfObjectDelete(c->ack_request); c->ack_request = nullptr; }
    WdfIoTargetClose(c->reader);
    c->reader_open = false;
    WdfSpinLockAcquire(c->lock);
    c->validated = false;
    // Class-file teardown cannot acknowledge data it did not consume. Keep
    // the endpoint terminal if any report lost its consumer during close.
    if (c->delivering_sequence) c->terminal = true;
    c->core.reset();
    c->delivering_sequence = 0;
    WdfSpinLockRelease(c->lock);
    ObDereferenceObject(c->reader_file); c->reader_file = nullptr;
  }
  // Framework forwards lower CLEANUP after this callback returns, so the HID
  // context cannot be freed while a mapper read still references it.
  WdfWaitLockRelease(c->lifecycle);
}
void forward_read(WDFQUEUE queue, WDFREQUEST request, size_t) {
  // Only this kernel mapper reads raw vendor reports. The public interface is
  // status-only; a user-mode file cannot steal reports from the mapper stream.
  if (WdfRequestGetRequestorMode(request) != KernelMode) {
    WdfRequestComplete(request, STATUS_ACCESS_DENIED);
    return;
  }
  forward(WdfIoQueueGetDevice(queue), request);
}

void status_control(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
  auto *c = context(WdfPdoGetParent(WdfIoQueueGetDevice(queue)));
  if (code == lvg::mouse_mapper::k_ioctl_status) {
    lvg::mouse_mapper::status *output = nullptr;
    const auto result = WdfRequestRetrieveOutputBuffer(request, sizeof(*output), reinterpret_cast<void **>(&output), nullptr);
    if (!NT_SUCCESS(result)) { WdfRequestComplete(request, result); return; }
    RtlZeroMemory(output, sizeof(*output));
    output->size = sizeof(*output);
    output->version = lvg::mouse_mapper::k_status_version;
    WdfSpinLockAcquire(c->lock);
    if (c->connection.ClassService) output->ready_flags |= 1;
    if (c->active) output->ready_flags |= 2;
    if (c->validated && c->active && !c->terminal) output->ready_flags |= 4;
    RtlCopyMemory(&output->container_id, &c->container, sizeof(c->container));
    WdfSpinLockRelease(c->lock);
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*output));
    return;
  }
  WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
}
void device_control(WDFQUEUE queue, WDFREQUEST request, size_t, size_t, ULONG code) {
  auto *c = context(WdfIoQueueGetDevice(queue));
  if (code == IOCTL_MOUSE_QUERY_ATTRIBUTES) {
    PMOUSE_ATTRIBUTES attributes = nullptr;
    const auto result = WdfRequestRetrieveOutputBuffer(request, sizeof(*attributes), reinterpret_cast<void **>(&attributes), nullptr);
    if (!NT_SUCCESS(result)) { WdfRequestComplete(request, result); return; }
    RtlZeroMemory(attributes, sizeof(*attributes));
    attributes->MouseIdentifier = MOUSE_HID_HARDWARE;
    attributes->NumberOfButtons = 5;
    attributes->SampleRate = 120;
    attributes->InputDataQueueLength = 3 * sizeof(MOUSE_INPUT_DATA);
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*attributes));
    return;
  }
  // HID collection queries are needed only by our kernel reader. Never expose
  // a write/injection control surface via the status interface.
  if (WdfRequestGetRequestorMode(request) != KernelMode) {
    WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
    return;
  }
  forward(c->device, request);
}
void internal_control(WDFQUEUE queue, WDFREQUEST request, size_t output_length, size_t input_length, ULONG code) {
  auto *c = context(WdfIoQueueGetDevice(queue));
  // Mouclass changes this query to IRP_MJ_INTERNAL_DEVICE_CONTROL.
  if (code == IOCTL_MOUSE_QUERY_ATTRIBUTES) {
    device_control(queue, request, output_length, input_length, code); return;
  }
  if (code == IOCTL_INTERNAL_MOUSE_CONNECT) {
    PCONNECT_DATA data = nullptr;
    auto result = WdfRequestRetrieveInputBuffer(request, sizeof(*data), reinterpret_cast<void **>(&data), nullptr);
    if (!NT_SUCCESS(result)) { WdfRequestComplete(request, result); return; }
    WdfSpinLockAcquire(c->lock);
    if (c->connection.ClassService) result = STATUS_SHARING_VIOLATION;
    else if (!data->ClassService || !data->ClassDeviceObject) result = STATUS_INVALID_PARAMETER;
    else {
      c->connection = *data;
      if (c->active && c->validated && !c->terminal) queue_pump_locked(c);
    }
    WdfSpinLockRelease(c->lock);
    WdfRequestComplete(request, result);
    return;
  }
  if (code == IOCTL_INTERNAL_MOUSE_DISCONNECT) {
    // Like the documented port contract, disconnect is unsupported. PnP
    // suspend/removal performs synchronous callback/read rundown instead.
    WdfRequestComplete(request, STATUS_NOT_IMPLEMENTED);
    return;
  }
  forward(c->device, request);
}

void read_complete(WDFREQUEST request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS parameters, WDFCONTEXT value) {
  auto *c = static_cast<device_context *>(value);
  const auto result = parameters->IoStatus.Status;
  const auto bytes = parameters->IoStatus.Information;
  WDF_REQUEST_REUSE_PARAMS reuse;
  WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
  const auto reused = WdfRequestReuse(request, &reuse);
  WdfSpinLockAcquire(c->lock);
  c->pending = false;
  c->completed = true;
  c->read_status = NT_SUCCESS(reused) ? result : reused;
  c->read_bytes = bytes;
  if (c->active) queue_pump_locked(c);
  WdfSpinLockRelease(c->lock);
  ExReleaseRundownProtection(&c->io_callbacks);
}

void ack_complete(WDFREQUEST request, WDFIOTARGET, PWDF_REQUEST_COMPLETION_PARAMS parameters, WDFCONTEXT value) {
  auto *c = static_cast<device_context *>(value);
  const auto result = parameters->IoStatus.Status;
  const auto bytes = parameters->IoStatus.Information;
  WDF_REQUEST_REUSE_PARAMS reuse;
  WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
  const auto reused = WdfRequestReuse(request, &reuse);
  WdfSpinLockAcquire(c->lock);
  c->ack_pending = false;
  c->ack_completed = true;
  c->ack_status = NT_SUCCESS(reused) ? result : reused;
  if (NT_SUCCESS(c->ack_status) && bytes != sizeof(c->ack)) c->ack_status = STATUS_DEVICE_DATA_ERROR;
  if (c->active) queue_pump_locked(c);
  WdfSpinLockRelease(c->lock);
  ExReleaseRundownProtection(&c->io_callbacks);
}
void retry_timer(WDFTIMER timer) {
  auto *c = context(WdfTimerGetParentObject(timer));
  WdfSpinLockAcquire(c->lock);
  c->timer_armed = false;
  if (c->active) queue_pump_locked(c);
  WdfSpinLockRelease(c->lock);
  ExReleaseRundownProtection(&c->pump_callbacks);
}
void pump_once(device_context *c) {
  bool send_read = false;
  bool send_ack = false;
  bool failed = false;
  WdfSpinLockAcquire(c->lock);
  if (!c->active || !c->validated || c->terminal || !c->connection.ClassService) {
    WdfSpinLockRelease(c->lock); return;
  }
  if (c->ack_completed) {
    c->ack_completed = false;
    if (!NT_SUCCESS(c->ack_status)) c->terminal = true;
    else c->delivering_sequence = 0;
  }
  if (c->completed) {
    c->completed = false;
    if (!NT_SUCCESS(c->read_status) || c->read_bytes != sizeof(report) ||
        c->core.admit(c->input) != verdict::accepted) c->terminal = true;
    else c->delivering_sequence = c->input.sequence;
  }
  if (!c->terminal && !c->core.empty()) {
    MOUSE_INPUT_DATA packets[3]{};
    const auto count = c->core.remaining();
    for (unsigned i=0; i<count; ++i) {
      const auto &source = c->core.data()[i];
      packets[i].Flags = source.flags;
      packets[i].ButtonFlags = source.button_flags;
      packets[i].ButtonData = source.button_data;
      packets[i].LastX = source.x;
      packets[i].LastY = source.y;
    }
    ULONG consumed = 0;
    // WDF DPC + this private spin lock guarantee DISPATCH_LEVEL
    // and serialize callbacks, connection changes, and power-state changes.
    reinterpret_cast<PSERVICE_CALLBACK_ROUTINE>(c->connection.ClassService)(
      c->connection.ClassDeviceObject, packets, packets + count, &consumed);
    if (!c->core.consume(consumed)) c->terminal = true;
  }
  if (c->terminal) failed = true;
  else if (!c->core.empty()) arm_retry_locked(c);
  else if (c->delivering_sequence) {
    if (!c->ack_pending) {
      c->ack = {lvg::mouse_transport::k_ack_report_id, lvg::mouse_transport::k_version, 0, c->delivering_sequence};
      c->ack_pending = true;
      send_ack = true;
    }
  } else if (!c->pending) { c->pending = true; send_read = true; }
  if ((send_read || send_ack) && !ExAcquireRundownProtection(&c->io_callbacks)) {
    if (send_ack) c->ack_pending = false; else c->pending = false;
    c->terminal = true; failed = true; send_read = send_ack = false;
  }
  WdfSpinLockRelease(c->lock);
  if (failed) { WdfDeviceSetFailed(c->device, WdfDeviceFailedNoRestart); return; }
  if (!send_read && !send_ack) return;

  const auto request = send_ack ? c->ack_request : c->read;
  auto result = send_ack
    ? WdfIoTargetFormatRequestForWrite(c->reader, request, c->ack_memory, nullptr, nullptr)
    : WdfIoTargetFormatRequestForRead(c->reader, request, c->memory, nullptr, nullptr);
  if (NT_SUCCESS(result)) {
    WdfRequestSetCompletionRoutine(request, send_ack ? ack_complete : read_complete, c);
    if (WdfRequestSend(request, c->reader, nullptr)) return;
    result = WdfRequestGetStatus(request);
  }
  // A failed ACK is uncertain and never replayed. DPC rundown closes the
  // reservation -> send window before power/file cleanup cancels targets.
  WdfSpinLockAcquire(c->lock);
  if (send_ack) c->ack_pending = false; else c->pending = false;
  c->terminal = true;
  WdfSpinLockRelease(c->lock);
  UNREFERENCED_PARAMETER(result);
  WdfDeviceSetFailed(c->device, WdfDeviceFailedNoRestart);
  ExReleaseRundownProtection(&c->io_callbacks);
}

// Caller holds the state lock. One rundown token belongs to each accepted
// queued callback, not merely to callbacks which have entered driver code.
void queue_pump_locked(device_context *c) {
  if (!c->active) return;
  c->pump_rerun = true;
  if (c->pump_owner) return;
  if (!ExAcquireRundownProtection(&c->pump_callbacks)) return;
  if (!WdfDpcEnqueue(c->dpc)) ExReleaseRundownProtection(&c->pump_callbacks);
}
void arm_retry_locked(device_context *c) {
  if (!c->active || c->timer_armed) return;
  if (!ExAcquireRundownProtection(&c->pump_callbacks)) return;
  c->timer_armed = true;
  WdfTimerStart(c->timer, WDF_REL_TIMEOUT_IN_MS(1));
}
void pump_dpc(WDFDPC dpc) {
  auto *c = context(WdfDpcGetParentObject(dpc));
  WdfSpinLockAcquire(c->lock);
  if (c->pump_owner) {
    c->pump_rerun = true;
    WdfSpinLockRelease(c->lock);
    ExReleaseRundownProtection(&c->pump_callbacks);
    return;
  }
  c->pump_owner = true;
  WdfSpinLockRelease(c->lock);
  // Inline completions request another owner iteration instead of reusing a
  // request concurrently on another processor before WdfRequestSend returns.
  for (unsigned iteration = 0; iteration != 16; ++iteration) {
    WdfSpinLockAcquire(c->lock);
    c->pump_rerun = false;
    WdfSpinLockRelease(c->lock);
    pump_once(c);
    WdfSpinLockAcquire(c->lock);
    if (!c->active || c->terminal || !c->pump_rerun || iteration == 15) {
      c->pump_owner = false;
      if (c->active && !c->terminal && c->pump_rerun) queue_pump_locked(c);
      WdfSpinLockRelease(c->lock);
      break;
    }
    WdfSpinLockRelease(c->lock);
  }
  ExReleaseRundownProtection(&c->pump_callbacks);
}
// Lifecycle lock held; both domains have already been drained by quiesce.
void reopen_rundowns(device_context *c) {
  if (!c->rundowns_closed) return;
  ExReInitializeRundownProtection(&c->pump_callbacks);
  ExReInitializeRundownProtection(&c->io_callbacks);
  c->rundowns_closed = false;
}

NTSTATUS query_collection(device_context *c) {
  HID_COLLECTION_INFORMATION information{};
  WDF_MEMORY_DESCRIPTOR output;
  WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &information, sizeof(information));
  auto result = WdfIoTargetSendIoctlSynchronously(c->reader, nullptr, IOCTL_HID_GET_COLLECTION_INFORMATION,
    nullptr, &output, nullptr, nullptr);
  if (!NT_SUCCESS(result)) return result;
  if (information.VendorID != LVG_KM_VENDOR_ID || information.ProductID != LVG_KM_PRODUCT_ID ||
      information.VersionNumber != lvg::mouse_transport::k_hardware_revision || information.DescriptorSize == 0 || information.DescriptorSize > 65536)
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  WDFMEMORY descriptor = nullptr;
  void *bytes = nullptr;
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = c->device;
  result = WdfMemoryCreate(&attributes, NonPagedPoolNx, 'mGvL', information.DescriptorSize, &descriptor, &bytes);
  if (!NT_SUCCESS(result)) return result;
  WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&output, descriptor, nullptr);
  result = WdfIoTargetSendIoctlSynchronously(c->reader, nullptr, IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
    nullptr, &output, nullptr, nullptr);
  HIDP_CAPS caps{};
  if (NT_SUCCESS(result)) {
    result = HidP_GetCaps(static_cast<PHIDP_PREPARSED_DATA>(bytes), &caps);
    if (result == HIDP_STATUS_SUCCESS) result = STATUS_SUCCESS;
    else result = STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  if (NT_SUCCESS(result) && (caps.UsagePage != lvg::mouse_transport::k_usage_page ||
      caps.Usage != lvg::mouse_transport::k_usage || caps.InputReportByteLength != sizeof(report) ||
      caps.OutputReportByteLength != sizeof(c->ack)))
    result = STATUS_DEVICE_CONFIGURATION_ERROR;
  WdfObjectDelete(descriptor);
  return result;
}

NTSTATUS validate_container(device_context *c) {
  DEVPROPTYPE type = 0;
  ULONG required = 0;
  GUID container{};
  const auto result = IoGetDevicePropertyData(WdfDeviceWdmGetPhysicalDevice(c->device), &DEVPKEY_Device_ContainerId,
    LOCALE_NEUTRAL, 0, sizeof(container), &container, &required, &type);
  if (!NT_SUCCESS(result)) return result;
  if (type != DEVPROP_TYPE_GUID || required != sizeof(GUID) || !IsEqualGUID(container, expected_container))
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  WdfSpinLockAcquire(c->lock);
  c->container = container;
  WdfSpinLockRelease(c->lock);
  return STATUS_SUCCESS;
}
NTSTATUS initialize(WDFDEVICE device) {
  auto *c = context(device);
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  const auto result = validate_container(c);
  if (NT_SUCCESS(result)) {
    reopen_rundowns(c);
    WdfSpinLockAcquire(c->lock);
    c->power_on = true;
    c->active = c->validated;
    if (c->active) queue_pump_locked(c);
    WdfSpinLockRelease(c->lock);
  }
  WdfWaitLockRelease(c->lifecycle);
  return result;
}

void quiesce(device_context *c) {
  WdfSpinLockAcquire(c->lock);
  c->active = false;
  c->pump_rerun = false;
  WdfSpinLockRelease(c->lock);
  // A running+requeued WDF DPC can be removed from the queue without waiting
  // for its running instance. Our queued token and running token are distinct.
  const bool timer_cancelled = WdfTimerStop(c->timer, TRUE) != FALSE;
  WdfSpinLockAcquire(c->lock);
  if (timer_cancelled && c->timer_armed) {
    c->timer_armed = false;
    ExReleaseRundownProtection(&c->pump_callbacks);
  }
  WdfSpinLockRelease(c->lock);
  if (WdfDpcCancel(c->dpc, TRUE)) ExReleaseRundownProtection(&c->pump_callbacks);
  ExWaitForRundownProtectionRelease(&c->pump_callbacks);
  // No sender can now be between reservation, formatting, and send. Target
  // Stop may miss an already-completed request whose callback has not entered;
  // the per-send token remains until the callback's last driver access.
  if (c->reader_open) WdfIoTargetStop(c->reader, WdfIoTargetCancelSentIo);
  ExWaitForRundownProtectionRelease(&c->io_callbacks);
  c->rundowns_closed = true;
  WdfSpinLockAcquire(c->lock);
  if (c->completed) {
    c->completed = false;
    if (NT_SUCCESS(c->read_status)) {
      if (c->read_bytes != sizeof(report) || c->core.admit(c->input) != verdict::accepted) c->terminal = true;
      else c->delivering_sequence = c->input.sequence;
    } else if (c->read_status != STATUS_CANCELLED) c->terminal = true;
  }
  if (c->ack_completed) {
    c->ack_completed = false;
    if (!NT_SUCCESS(c->ack_status)) c->terminal = true;
    else c->delivering_sequence = 0;
  }
  WdfSpinLockRelease(c->lock);
}
NTSTATUS suspend(WDFDEVICE device) {
  auto *c = context(device);
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  c->power_on = false;
  quiesce(c);
  WdfWaitLockRelease(c->lifecycle);
  return STATUS_SUCCESS;
}
NTSTATUS restart(WDFDEVICE device) {
  auto *c = context(device);
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  auto result = c->terminal ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
  if (NT_SUCCESS(result) && c->reader_open) result = WdfIoTargetStart(c->reader);
  if (NT_SUCCESS(result)) {
    reopen_rundowns(c);
    WdfSpinLockAcquire(c->lock);
    c->power_on = true;
    c->active = c->validated;
    if (c->active) queue_pump_locked(c);
    WdfSpinLockRelease(c->lock);
  }
  WdfWaitLockRelease(c->lifecycle);
  return result;
}
void cleanup(WDFDEVICE device) {
  auto *c = context(device);
  WdfWaitLockAcquire(c->lifecycle, nullptr);
  c->power_on = false;
  quiesce(c);
  if (c->read) { WdfObjectDelete(c->read); c->read = nullptr; }
  if (c->ack_request) { WdfObjectDelete(c->ack_request); c->ack_request = nullptr; }
  if (c->reader_open) { WdfIoTargetClose(c->reader); c->reader_open = false; }
  if (c->reader_file) { ObDereferenceObject(c->reader_file); c->reader_file = nullptr; }
  WdfSpinLockAcquire(c->lock);
  c->validated = false;
  c->connection = {};
  WdfSpinLockRelease(c->lock);
  WdfWaitLockRelease(c->lifecycle);
}

NTSTATUS create_status_child(WDFDEVICE parent) {
  auto *init = WdfPdoInitAllocate(parent);
  if (!init) return STATUS_INSUFFICIENT_RESOURCES;
  DECLARE_CONST_UNICODE_STRING(device_id, L"VIBESHINE\\ORDERED_MOUSE_STATUS");
  DECLARE_CONST_UNICODE_STRING(instance, L"0");
  DECLARE_CONST_UNICODE_STRING(container, L"{5d43cc5c-318a-46f4-8a8f-2d1d3c998021}");
  DECLARE_CONST_UNICODE_STRING(security, L"D:P(A;;GR;;;SY)(A;;GR;;;BA)(A;;GR;;;LS)");
  // A separate raw PDO is necessary: Mouclass owns/open-protects the Mouse
  // function stack. This sideband has a status-only queue and no HID reads.
  auto result = WdfPdoInitAssignRawDevice(init, &mapper_interface);
  if (NT_SUCCESS(result)) result = WdfDeviceInitAssignSDDLString(init, &security);
  if (NT_SUCCESS(result)) result = WdfPdoInitAssignDeviceID(init, &device_id);
  if (NT_SUCCESS(result)) result = WdfPdoInitAssignInstanceID(init, &instance);
  if (NT_SUCCESS(result)) result = WdfPdoInitAssignContainerID(init, &container);
  if (!NT_SUCCESS(result)) { WdfDeviceInitFree(init); return result; }
  WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
  WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
  WDFDEVICE child = nullptr;
  result = WdfDeviceCreate(&init, WDF_NO_OBJECT_ATTRIBUTES, &child);
  if (!NT_SUCCESS(result)) { if (init) WdfDeviceInitFree(init); return result; }
  WDF_IO_QUEUE_CONFIG queue;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
  queue.PowerManaged = WdfFalse;
  queue.EvtIoDeviceControl = status_control;
  result = WdfIoQueueCreate(child, &queue, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
  if (NT_SUCCESS(result)) result = WdfDeviceCreateDeviceInterface(child, &mapper_interface, nullptr);
  if (NT_SUCCESS(result)) result = WdfFdoAddStaticChild(parent, child);
  if (!NT_SUCCESS(result)) WdfObjectDelete(child);
  return result;
}

NTSTATUS add_device(WDFDRIVER, PWDFDEVICE_INIT init) {
  WdfDeviceInitSetDeviceType(init, FILE_DEVICE_MOUSE);
  WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
  WdfDeviceInitSetExclusive(init, TRUE);
  WDF_FILEOBJECT_CONFIG files;
  WDF_FILEOBJECT_CONFIG_INIT(&files, file_create, nullptr, file_cleanup);
  files.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
  files.AutoForwardCleanupClose = WdfTrue;
  WDF_OBJECT_ATTRIBUTES file_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&file_attributes);
  file_attributes.ExecutionLevel = WdfExecutionLevelPassive;
  file_attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  WdfDeviceInitSetFileObjectConfig(init, &files, &file_attributes);
  WDF_PNPPOWER_EVENT_CALLBACKS pnp;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
  pnp.EvtDeviceSelfManagedIoInit = initialize;
  pnp.EvtDeviceSelfManagedIoSuspend = suspend;
  pnp.EvtDeviceSelfManagedIoRestart = restart;
  pnp.EvtDeviceSelfManagedIoCleanup = cleanup;
  WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, device_context);
  attributes.ExecutionLevel = WdfExecutionLevelDispatch;
  attributes.SynchronizationScope = WdfSynchronizationScopeNone;
  WDFDEVICE device = nullptr;
  auto result = WdfDeviceCreate(&init, &attributes, &device);
  if (!NT_SUCCESS(result)) return result;
  auto *c = context(device); c->device = device; c->core.reset();
  ExInitializeRundownProtection(&c->pump_callbacks);
  ExInitializeRundownProtection(&c->io_callbacks);
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes); attributes.ParentObject = device;
  result = WdfWaitLockCreate(&attributes, &c->lifecycle);
  if (!NT_SUCCESS(result)) return result;
  result = WdfSpinLockCreate(&attributes, &c->lock);
  if (!NT_SUCCESS(result)) return result;
  WDF_TIMER_CONFIG timer;
  WDF_TIMER_CONFIG_INIT(&timer, retry_timer); timer.AutomaticSerialization = FALSE;
  result = WdfTimerCreate(&timer, &attributes, &c->timer);
  if (!NT_SUCCESS(result)) return result;
  WDF_DPC_CONFIG dpc;
  WDF_DPC_CONFIG_INIT(&dpc, pump_dpc); dpc.AutomaticSerialization = FALSE;
  result = WdfDpcCreate(&dpc, &attributes, &c->dpc);
  if (!NT_SUCCESS(result)) return result;
  result = WdfIoTargetCreate(device, &attributes, &c->reader);
  if (!NT_SUCCESS(result)) return result;
  result = WdfMemoryCreatePreallocated(&attributes, &c->input, sizeof(c->input), &c->memory);
  if (!NT_SUCCESS(result)) return result;
  result = WdfMemoryCreatePreallocated(&attributes, &c->ack, sizeof(c->ack), &c->ack_memory);
  if (!NT_SUCCESS(result)) return result;
  WDF_IO_QUEUE_CONFIG queue;
  WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchParallel);
  queue.PowerManaged = WdfFalse;
  queue.EvtIoRead = forward_read;
  queue.EvtIoDeviceControl = device_control;
  queue.EvtIoInternalDeviceControl = internal_control;
  result = WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, nullptr);
  if (!NT_SUCCESS(result)) return result;
  return create_status_child(device);
}
} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, add_device);
  return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
