// Deterministic WDF/DDI boundary for compiling the unchanged native callbacks.
// This is not a WDK ABI shim or native driver qualification.
#pragma once
#include <libvirtualgamepad/mouse_transport.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <vector>
using NTSTATUS=std::int32_t; using ULONG=std::uint32_t; using ULONG_PTR=std::uintptr_t;
using DEVPROPTYPE=ULONG; using size_t=std::size_t;
using GUID=struct guid_native {std::uint32_t Data1;std::uint16_t Data2,Data3;std::uint8_t Data4[8];};
using UNICODE_STRING=struct unicode_string {const wchar_t *value;};using PUNICODE_STRING=UNICODE_STRING*;
struct object;
using WDFOBJECT=object*;using WDFDEVICE=object*;using WDFSPINLOCK=object*;using WDFWAITLOCK=object*;
using WDFTIMER=object*;using WDFDPC=object*;using WDFIOTARGET=object*;using WDFREQUEST=object*;
using WDFMEMORY=object*;using WDFFILEOBJECT=object*;using WDFQUEUE=object*;using WDFDRIVER=object*;
using PFILE_OBJECT=object*;using PDEVICE_OBJECT=object*;using PDRIVER_OBJECT=object*;using WDFCONTEXT=void*;
struct device_init {object *parent{};bool exclusive{},auto_forward{};void *create_callback{},*cleanup_callback{};};using PWDFDEVICE_INIT=device_init*;
struct WDF_REQUEST_COMPLETION_PARAMS {struct {NTSTATUS Status;ULONG_PTR Information;} IoStatus;};
using PWDF_REQUEST_COMPLETION_PARAMS=WDF_REQUEST_COMPLETION_PARAMS*;
using EVT_WDF_DRIVER_DEVICE_ADD=NTSTATUS(WDFDRIVER,PWDFDEVICE_INIT);
using EVT_WDF_DEVICE_FILE_CREATE=void(WDFDEVICE,WDFREQUEST,WDFFILEOBJECT);
using EVT_WDF_FILE_CLEANUP=void(WDFFILEOBJECT);
using EVT_WDF_IO_QUEUE_IO_READ=void(WDFQUEUE,WDFREQUEST,size_t);
using EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL=void(WDFQUEUE,WDFREQUEST,size_t,size_t,ULONG);
using EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL=EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL;
using EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT=NTSTATUS(WDFDEVICE);
using EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND=NTSTATUS(WDFDEVICE);
using EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART=NTSTATUS(WDFDEVICE);
using EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP=void(WDFDEVICE);
using EVT_WDF_TIMER=void(WDFTIMER);using EVT_WDF_DPC=void(WDFDPC);
using EVT_WDF_REQUEST_COMPLETION_ROUTINE=void(WDFREQUEST,WDFIOTARGET,PWDF_REQUEST_COMPLETION_PARAMS,WDFCONTEXT);
using DRIVER_INITIALIZE=NTSTATUS(PDRIVER_OBJECT,PUNICODE_STRING);
struct WDF_OBJECT_ATTRIBUTES {object *ParentObject{};int ExecutionLevel{},SynchronizationScope{};};
struct WDF_FILEOBJECT_CONFIG {int FileObjectClass{},AutoForwardCleanupClose{};EVT_WDF_DEVICE_FILE_CREATE *create{};EVT_WDF_FILE_CLEANUP *cleanup{};};
struct WDF_PNPPOWER_EVENT_CALLBACKS {
 EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT *EvtDeviceSelfManagedIoInit{};
 EVT_WDF_DEVICE_SELF_MANAGED_IO_SUSPEND *EvtDeviceSelfManagedIoSuspend{};
 EVT_WDF_DEVICE_SELF_MANAGED_IO_RESTART *EvtDeviceSelfManagedIoRestart{};
 EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP *EvtDeviceSelfManagedIoCleanup{};
};
struct WDF_IO_QUEUE_CONFIG {int PowerManaged{};EVT_WDF_IO_QUEUE_IO_READ *EvtIoRead{};EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL *EvtIoDeviceControl{},*EvtIoInternalDeviceControl{};};
struct WDF_TIMER_CONFIG {EVT_WDF_TIMER *callback{};bool AutomaticSerialization{};};
struct WDF_DPC_CONFIG {EVT_WDF_DPC *callback{};bool AutomaticSerialization{};};
struct WDF_REQUEST_SEND_OPTIONS {unsigned flags{};};struct WDF_REQUEST_REUSE_PARAMS {};
struct WDF_IO_TARGET_OPEN_PARAMS {object *TargetDeviceObject{},*TargetFileObject{};};
struct WDF_MEMORY_DESCRIPTOR {void *data;size_t size;};
struct WDF_DRIVER_CONFIG {};
struct object {
 std::mutex boundary_mutex;
 alignas(std::max_align_t) std::array<std::byte,2048> storage{};
 object *parent{},*file{},*target{},*child{},*queue{},*lower{};
 void *buffer{};size_t size{};int kind{},mode{},refs{},held{};
 bool opened{},pending{},queued{},scheduled{},deleted{},failed{},exclusive{},auto_forward{};
 unsigned open_count{},lower_creates{},lower_cleanups{},lower_closes{};
 void *create_callback{},*cleanup_callback{};
 NTSTATUS status{};ULONG_PTR information{};
 EVT_WDF_REQUEST_COMPLETION_ROUTINE *completion{};void *completion_context{};
 EVT_WDF_TIMER *timer_callback{};EVT_WDF_DPC *dpc_callback{};
 WDF_IO_QUEUE_CONFIG queue_config{};
 std::vector<std::byte> owned;
};
inline std::vector<std::unique_ptr<object>> native_objects;
inline object *native_last_device;
inline unsigned native_reads{},native_writes{},native_timers{},native_dpc_runs{};
inline bool native_inline_ack=true,native_fail_write=false,native_fail_reuse=false,native_fail_request_create=false;
inline thread_local unsigned native_irql=0;
struct EX_RUNDOWN_REF {std::mutex mutex;std::condition_variable condition;unsigned refs{};bool closed{};};
inline std::function<void(EX_RUNDOWN_REF*)> native_wait_hook;
inline std::function<void(object*)> native_format_hook;
inline NTSTATUS native_lower_create_status=0;
struct deferred_completion {object *request;NTSTATUS status;size_t bytes;EVT_WDF_REQUEST_COMPLETION_ROUTINE *callback;void *value;};
inline std::vector<deferred_completion> native_deferred;
inline bool native_defer_completion=false;
inline void native_flush_deferred();
inline std::vector<lvg::mouse_transport::ack_report> native_acks;
inline std::vector<lvg::mouse_transport::report> native_inline_reads;
inline std::vector<int> native_rundown;
inline object *make_object(object *parent=nullptr) {auto p=std::make_unique<object>();p->parent=parent;auto *r=p.get();native_objects.push_back(std::move(p));return r;}
constexpr NTSTATUS STATUS_SUCCESS=0,STATUS_NOT_IMPLEMENTED=-1,STATUS_ACCESS_DENIED=-2,STATUS_SHARING_VIOLATION=-3,
 STATUS_INVALID_PARAMETER=-4,STATUS_INVALID_DEVICE_REQUEST=-5,STATUS_DEVICE_CONFIGURATION_ERROR=-6,
 STATUS_DEVICE_NOT_READY=-7,STATUS_CANCELLED=-8,STATUS_INSUFFICIENT_RESOURCES=-9,STATUS_DEVICE_DATA_ERROR=-10,
 STATUS_BUFFER_TOO_SMALL=-11;
constexpr NTSTATUS HIDP_STATUS_SUCCESS=0x110000;
#define NT_SUCCESS(x) ((x)>=0)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define TRUE true
#define FALSE false
constexpr int KernelMode=0,UserMode=1,WdfTrue=1,WdfFalse=0,WdfExecutionLevelDispatch=1,WdfExecutionLevelPassive=2,
 WdfSynchronizationScopeNone=0,WdfFileObjectWdfCannotUseFsContexts=1,WdfDeviceIoBuffered=0,
 WdfIoQueueDispatchParallel=0,WdfDeviceFailedNoRestart=0,WdfIoTargetCancelSentIo=0,NonPagedPoolNx=0;
constexpr unsigned FILE_DEVICE_MOUSE=15,FILE_DEVICE_UNKNOWN=34,LOCALE_NEUTRAL=0,DEVPROP_TYPE_GUID=13;
inline int DEVPKEY_Device_ContainerId;
constexpr unsigned WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET=1,WDF_REQUEST_SEND_OPTION_SYNCHRONOUS=2,WDF_REQUEST_REUSE_NO_FLAGS=0;
constexpr unsigned IOCTL_MOUSE_QUERY_ATTRIBUTES=1,IOCTL_INTERNAL_MOUSE_CONNECT=2,IOCTL_INTERNAL_MOUSE_DISCONNECT=3,
 IOCTL_HID_GET_COLLECTION_INFORMATION=4,IOCTL_HID_GET_COLLECTION_DESCRIPTOR=5;
constexpr unsigned MOUSE_HID_HARDWARE=3,MOUSE_MOVE_ABSOLUTE=1,MOUSE_VIRTUAL_DESKTOP=2,
 MOUSE_WHEEL=0x400,MOUSE_HWHEEL=0x800,MOUSE_LEFT_BUTTON_DOWN=1,MOUSE_BUTTON_5_UP=0x200;
struct MOUSE_INPUT_DATA {std::uint16_t UnitId{},Flags{},ButtonFlags{},ButtonData{};ULONG RawButtons{};std::int32_t LastX{},LastY{};ULONG ExtraInformation{};};
struct MOUSE_ATTRIBUTES {std::uint16_t MouseIdentifier{},NumberOfButtons{},SampleRate{};ULONG InputDataQueueLength{};};using PMOUSE_ATTRIBUTES=MOUSE_ATTRIBUTES*;
struct CONNECT_DATA {object *ClassDeviceObject{};void *ClassService{};};using PCONNECT_DATA=CONNECT_DATA*;
using PSERVICE_CALLBACK_ROUTINE=void(*)(object*,MOUSE_INPUT_DATA*,MOUSE_INPUT_DATA*,ULONG*);
struct HID_COLLECTION_INFORMATION {ULONG DescriptorSize;std::uint16_t VendorID,ProductID,VersionNumber;};
struct HIDP_CAPS {std::uint16_t UsagePage{},Usage{},InputReportByteLength{},OutputReportByteLength{};};
inline HID_COLLECTION_INFORMATION native_collection_information {16,LVG_KM_VENDOR_ID,LVG_KM_PRODUCT_ID,0x100};
inline HIDP_CAPS native_collection_caps {0xff53,1,22,8};
inline GUID native_container {0x5d43cc5c,0x318a,0x46f4,{0x8a,0x8f,0x2d,0x1d,0x3c,0x99,0x80,0x21}};
using PHIDP_PREPARSED_DATA=void*;
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type,name) inline type *name(object *o) {return reinterpret_cast<type*>(o->storage.data());}
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(a,t) (*(a)={})
#define WDF_NO_OBJECT_ATTRIBUTES nullptr
#define WDF_NO_HANDLE nullptr
#define DECLARE_CONST_UNICODE_STRING(name,value) const UNICODE_STRING name{value}
inline void WDF_OBJECT_ATTRIBUTES_INIT(WDF_OBJECT_ATTRIBUTES *a){*a={};}
inline void WDF_FILEOBJECT_CONFIG_INIT(WDF_FILEOBJECT_CONFIG *a,EVT_WDF_DEVICE_FILE_CREATE*f,void*,EVT_WDF_FILE_CLEANUP*c){*a={};a->create=f;a->cleanup=c;}
inline void WDF_PNPPOWER_EVENT_CALLBACKS_INIT(WDF_PNPPOWER_EVENT_CALLBACKS *a){*a={};}
inline void WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(WDF_IO_QUEUE_CONFIG *a,int){*a={};}
inline void WDF_TIMER_CONFIG_INIT(WDF_TIMER_CONFIG *a,EVT_WDF_TIMER *f){*a={f,false};}
inline void WDF_DPC_CONFIG_INIT(WDF_DPC_CONFIG *a,EVT_WDF_DPC *f){*a={f,false};}
inline void WDF_DRIVER_CONFIG_INIT(WDF_DRIVER_CONFIG*,EVT_WDF_DRIVER_DEVICE_ADD*){}
inline void WDF_REQUEST_SEND_OPTIONS_INIT(WDF_REQUEST_SEND_OPTIONS *a,unsigned f){a->flags=f;}
inline void WDF_REQUEST_REUSE_PARAMS_INIT(WDF_REQUEST_REUSE_PARAMS*,unsigned,NTSTATUS){}
inline void WDF_IO_TARGET_OPEN_PARAMS_INIT_EXISTING_DEVICE(WDF_IO_TARGET_OPEN_PARAMS*a,object*d){*a={d,nullptr};}
inline void WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(WDF_MEMORY_DESCRIPTOR*a,void*p,size_t n){*a={p,n};}
inline void WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(WDF_MEMORY_DESCRIPTOR*a,object*m,void*){*a={m->buffer,m->size};}
inline int WDF_REL_TIMEOUT_IN_MS(int n){return n;}
inline void RtlZeroMemory(void*p,size_t n){std::memset(p,0,n);}inline void RtlCopyMemory(void*d,const void*s,size_t n){std::memcpy(d,s,n);}
inline bool IsEqualGUID(const GUID&a,const GUID&b){return std::memcmp(&a,&b,sizeof(a))==0;}
inline void ObReferenceObject(object*o){++o->refs;}inline void ObDereferenceObject(object*o){assert(o->refs>0);--o->refs;}
inline void WdfDeviceInitSetDeviceType(PWDFDEVICE_INIT,unsigned){}inline void WdfDeviceInitSetIoType(PWDFDEVICE_INIT,int){}
inline void WdfDeviceInitSetFileObjectConfig(PWDFDEVICE_INIT i,WDF_FILEOBJECT_CONFIG*c,WDF_OBJECT_ATTRIBUTES*){i->create_callback=reinterpret_cast<void*>(c->create);i->cleanup_callback=reinterpret_cast<void*>(c->cleanup);i->auto_forward=c->AutoForwardCleanupClose==WdfTrue;}
inline void WdfDeviceInitSetExclusive(PWDFDEVICE_INIT i,bool value){i->exclusive=value;}
inline void WdfDeviceInitSetPnpPowerEventCallbacks(PWDFDEVICE_INIT,WDF_PNPPOWER_EVENT_CALLBACKS*){}
inline NTSTATUS WdfDeviceCreate(PWDFDEVICE_INIT *i,WDF_OBJECT_ATTRIBUTES*,object**out){*out=make_object((*i)->parent);(*out)->exclusive=(*i)->exclusive;(*out)->auto_forward=(*i)->auto_forward;(*out)->create_callback=(*i)->create_callback;(*out)->cleanup_callback=(*i)->cleanup_callback;(*out)->lower=make_object(*out);(*out)->lower->opened=true;native_last_device=*out;delete *i;*i=nullptr;return 0;}
inline NTSTATUS WdfSpinLockCreate(WDF_OBJECT_ATTRIBUTES*a,object**o){*o=make_object(a->ParentObject);return 0;}
inline NTSTATUS WdfWaitLockCreate(WDF_OBJECT_ATTRIBUTES*a,object**o){return WdfSpinLockCreate(a,o);}
inline void WdfSpinLockAcquire(object*o){o->boundary_mutex.lock();assert(!o->held);o->held=1;native_irql=2;}inline void WdfSpinLockRelease(object*o){assert(o->held);o->held=0;native_irql=0;o->boundary_mutex.unlock();}
inline void WdfWaitLockAcquire(object*o,void*){o->boundary_mutex.lock();assert(!o->held);o->held=1;}inline void WdfWaitLockRelease(object*o){assert(o->held);o->held=0;o->boundary_mutex.unlock();}
inline NTSTATUS WdfTimerCreate(WDF_TIMER_CONFIG*c,WDF_OBJECT_ATTRIBUTES*a,object**o){*o=make_object(a->ParentObject);(*o)->timer_callback=c->callback;return 0;}
inline NTSTATUS WdfDpcCreate(WDF_DPC_CONFIG*c,WDF_OBJECT_ATTRIBUTES*a,object**o){*o=make_object(a->ParentObject);(*o)->dpc_callback=c->callback;return 0;}
inline bool WdfTimerStart(object*o,int){o->scheduled=true;++native_timers;return true;}
inline bool WdfDpcEnqueue(object*o){if(o->queued)return false;o->queued=true;return true;}
inline bool WdfTimerStop(object*o,bool){assert(!native_irql);const bool queued=o->scheduled;o->scheduled=false;native_rundown.push_back(1);return queued;}
inline bool WdfDpcCancel(object*o,bool){assert(!native_irql);const bool queued=o->queued;o->queued=false;native_rundown.push_back(2);return queued;}
inline object *WdfTimerGetParentObject(object*o){return o->parent;}inline object *WdfDpcGetParentObject(object*o){return o->parent;}
inline NTSTATUS WdfIoTargetCreate(object*,WDF_OBJECT_ATTRIBUTES*a,object**o){*o=make_object(a->ParentObject);return 0;}
inline NTSTATUS WdfIoTargetOpen(object*o,WDF_IO_TARGET_OPEN_PARAMS*p){assert(p->TargetFileObject&&p->TargetDeviceObject);o->opened=true;o->file=p->TargetFileObject;o->target=p->TargetDeviceObject;return 0;}
inline void WdfIoTargetClose(object*o){o->opened=false;}
inline NTSTATUS WdfIoTargetStart(object*o){assert(o->opened);return 0;}
inline object *WdfDeviceGetIoTarget(object*o){return o->lower;}
inline object *WdfIoTargetWdmGetTargetDeviceObject(object*o){return o;}
inline object *WdfDeviceWdmGetPhysicalDevice(object*o){return o;}
inline object *WdfFileObjectGetDevice(object*o){return o->parent;}inline object *WdfFileObjectWdmGetFileObject(object*o){return o;}
inline void WdfDeviceSetFailed(object*o,int){o->failed=true;}
inline NTSTATUS WdfRequestCreate(WDF_OBJECT_ATTRIBUTES*a,object*t,object**o){assert(t->opened);if(native_fail_request_create)return STATUS_INSUFFICIENT_RESOURCES;*o=make_object(a->ParentObject);(*o)->target=t;return 0;}
inline NTSTATUS WdfMemoryCreatePreallocated(WDF_OBJECT_ATTRIBUTES*a,void*p,size_t n,object**o){*o=make_object(a->ParentObject);(*o)->buffer=p;(*o)->size=n;return 0;}
inline NTSTATUS WdfMemoryCreate(WDF_OBJECT_ATTRIBUTES*a,int,unsigned,size_t n,object**o,void**p){*o=make_object(a->ParentObject);(*o)->owned.resize(n);(*o)->buffer=(*o)->owned.data();(*o)->size=n;*p=(*o)->buffer;return 0;}
inline void WdfObjectDelete(object*o){assert(!o->pending);o->deleted=true;}
inline void WdfRequestFormatRequestUsingCurrentType(object*){}
inline NTSTATUS WdfIoTargetFormatRequestForRead(object*,object*r,object*m,void*,void*){if(native_format_hook)native_format_hook(r);r->kind=1;r->buffer=m->buffer;r->size=m->size;return 0;}
inline NTSTATUS WdfIoTargetFormatRequestForWrite(object*,object*r,object*m,void*,void*){if(native_format_hook)native_format_hook(r);r->kind=2;r->buffer=m->buffer;r->size=m->size;return 0;}
inline void WdfRequestSetCompletionRoutine(object*r,EVT_WDF_REQUEST_COMPLETION_ROUTINE*f,void*c){r->completion=f;r->completion_context=c;}
inline void native_complete(object*r,NTSTATUS s,size_t n){r->pending=false;r->status=s;r->information=n;WDF_REQUEST_COMPLETION_PARAMS p{{s,n}};if(r->completion){if(native_defer_completion)native_deferred.push_back({r,s,n,r->completion,r->completion_context});else r->completion(r,r->target,&p,r->completion_context);}}
inline bool WdfRequestSend(object*r,object*t,WDF_REQUEST_SEND_OPTIONS*o){assert(t->opened);r->target=t;if(o){++t->lower_creates;r->status=native_lower_create_status;return true;}assert(!r->pending);r->pending=true;if(r->kind==1){++native_reads;if(!native_inline_reads.empty()){auto report=native_inline_reads.front();native_inline_reads.erase(native_inline_reads.begin());std::memcpy(r->buffer,&report,sizeof(report));native_complete(r,0,sizeof(report));}return true;}++native_writes;native_acks.push_back(*static_cast<lvg::mouse_transport::ack_report*>(r->buffer));if(native_fail_write){r->pending=false;r->status=-10;return false;}if(native_inline_ack)native_complete(r,0,r->size);return true;}
inline void native_flush_deferred(){auto copy=std::move(native_deferred);native_deferred.clear();for(const auto&d:copy){assert(!d.request->deleted);WDF_REQUEST_COMPLETION_PARAMS p{{d.status,d.bytes}};d.callback(d.request,d.request->target,&p,d.value);}}
inline void ExInitializeRundownProtection(EX_RUNDOWN_REF*r){r->refs=0;r->closed=false;}
inline bool ExAcquireRundownProtection(EX_RUNDOWN_REF*r){std::lock_guard lock(r->mutex);if(r->closed)return false;++r->refs;return true;}
inline void ExReleaseRundownProtection(EX_RUNDOWN_REF*r){std::lock_guard lock(r->mutex);assert(r->refs);if(!--r->refs)r->condition.notify_all();}
inline void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF*r){
 assert(!native_irql);std::unique_lock lock(r->mutex);r->closed=true;native_rundown.push_back(4);
 for(unsigned i=0;r->refs;++i){
  assert(i<16);auto hook=native_wait_hook;lock.unlock();
  if(hook)hook(r);else if(!native_deferred.empty())native_flush_deferred();
  else{lock.lock();assert(r->condition.wait_for(lock,std::chrono::seconds(2),[&]{return !r->refs;}));continue;}
  lock.lock();
 }
}
inline void ExReInitializeRundownProtection(EX_RUNDOWN_REF*r){std::lock_guard lock(r->mutex);assert(!native_irql&&r->closed&&!r->refs);r->closed=false;}
inline NTSTATUS WdfRequestGetStatus(object*r){return r->status;}
inline NTSTATUS WdfRequestReuse(object*r,WDF_REQUEST_REUSE_PARAMS*){r->completion=nullptr;return native_fail_reuse ? -10 : 0;}
inline void WdfIoTargetStop(object*t,int){assert(!native_irql);native_rundown.push_back(3);for(auto &p:native_objects)if(p->pending&&p->target==t)native_complete(p.get(),STATUS_CANCELLED,0);}
inline int WdfRequestGetRequestorMode(object*r){return r->mode;}
inline NTSTATUS WdfRequestRetrieveOutputBuffer(object*r,size_t n,void**p,void*){if(r->size<n)return STATUS_BUFFER_TOO_SMALL;*p=r->buffer;return 0;}
inline NTSTATUS WdfRequestRetrieveInputBuffer(object*r,size_t n,void**p,void*v){return WdfRequestRetrieveOutputBuffer(r,n,p,v);}
inline void WdfRequestComplete(object*r,NTSTATUS s){r->status=s;}inline void WdfRequestCompleteWithInformation(object*r,NTSTATUS s,size_t n){r->status=s;r->information=n;}
inline NTSTATUS WdfIoTargetSendIoctlSynchronously(object*,void*,unsigned code,void*,WDF_MEMORY_DESCRIPTOR*out,void*,void*){if(code==IOCTL_HID_GET_COLLECTION_INFORMATION)*static_cast<HID_COLLECTION_INFORMATION*>(out->data)=native_collection_information;return 0;}
inline NTSTATUS HidP_GetCaps(void*,HIDP_CAPS*c){*c=native_collection_caps;return HIDP_STATUS_SUCCESS;}
inline NTSTATUS IoGetDevicePropertyData(object*,int*,unsigned,unsigned,size_t n,void*p,ULONG*required,DEVPROPTYPE*t){assert(n==sizeof(native_container));std::memcpy(p,&native_container,n);*required=sizeof(native_container);*t=DEVPROP_TYPE_GUID;return 0;}
inline object *WdfIoQueueGetDevice(object*q){return q->parent;}
inline NTSTATUS WdfIoQueueCreate(object*d,WDF_IO_QUEUE_CONFIG*c,void*,object**o){d->queue=make_object(d);d->queue->queue_config=*c;if(o)*o=d->queue;return 0;}
inline PWDFDEVICE_INIT WdfPdoInitAllocate(object*p){return new device_init{p};}
inline NTSTATUS WdfPdoInitAssignRawDevice(PWDFDEVICE_INIT,const GUID*){return 0;}
inline NTSTATUS WdfDeviceInitAssignSDDLString(PWDFDEVICE_INIT,const UNICODE_STRING*){return 0;}
inline NTSTATUS WdfPdoInitAssignDeviceID(PWDFDEVICE_INIT,const UNICODE_STRING*){return 0;}
inline NTSTATUS WdfPdoInitAssignInstanceID(PWDFDEVICE_INIT,const UNICODE_STRING*){return 0;}
inline NTSTATUS WdfPdoInitAssignContainerID(PWDFDEVICE_INIT,const UNICODE_STRING*){return 0;}
inline void WdfDeviceInitFree(PWDFDEVICE_INIT i){delete i;}
inline NTSTATUS WdfDeviceCreateDeviceInterface(object*,const GUID*,void*){return 0;}
inline NTSTATUS WdfFdoAddStaticChild(object*p,object*c){p->child=c;return 0;}inline object *WdfPdoGetParent(object*c){return c->parent;}
inline NTSTATUS WdfDriverCreate(object*,PUNICODE_STRING,void*,WDF_DRIVER_CONFIG*,void*){return 0;}
inline void native_run_dpcs(){for(unsigned count=0;;++count){assert(count<1000);object *next=nullptr;for(auto&p:native_objects)if(p->queued){next=p.get();break;}if(!next)return;next->queued=false;++native_dpc_runs;next->dpc_callback(next);}}
inline void native_fire_timer(object*t){assert(t->scheduled);t->scheduled=false;t->timer_callback(t);native_run_dpcs();}
