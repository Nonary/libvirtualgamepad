// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Actual UMDF driver+client bodies at deterministic API boundaries. This never
// invokes a Windows driver/API/device. Synthetic IDs are fixture-only sentinels,
// not assigned, approved, shipped or accepted project-owned identities.
#include "../src/driver.cpp"
#include "../../client/client.cpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <vector>

// The inherited native reset uses memset on controller_slot. WDF handles are
// pointers in both SDK and fixture; no fake mutex/vector lives in that slot.
static_assert(std::is_trivially_copyable_v<controller_slot>);
static_assert(std::is_trivially_destructible_v<controller_slot>);
static_assert(std::is_trivially_copyable_v<device_context>);
static_assert(std::is_trivially_destructible_v<device_context>);
namespace {
std::atomic<int> checks {}, failures {};
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); } } while(false)
NTSTATUS create_result=STATUS_SUCCESS,start_result=STATUS_SUCCESS,submit_result=STATUS_SUCCESS;
std::function<void(VHFHANDLE)> on_create,on_start,on_delete,on_submit;
std::function<void(VHFHANDLE)> on_before_submit;
std::atomic<bool> fail_rundown_wait {};
std::atomic<int> failed_rundown_waits {};
std::atomic<int> creates{},starts{},deletes{},submits{};
int ioctls{},opens{},closes{};
DWORD last_error{},forced_reply_error{};
std::function<void(DWORD,void *,DWORD *)> on_reply;
struct fake_condition {
  std::condition_variable cv;
  std::atomic<int> waiters {};
};
std::vector<std::unique_ptr<std::mutex>> fake_srw_states;
std::vector<std::unique_ptr<fake_condition>> fake_condition_states;
thread_local int fake_rundown_depth {};
}
enum class fake_vhf_kind { keyboard_mouse, controller };

struct fake_vhf {
  VHF_CONFIG config;
  fake_vhf_kind kind {fake_vhf_kind::keyboard_mouse};
  bool deleted{},deleting{},credit{};
  PHID_XFER_PACKET retained{};
  HID_XFER_PACKET packet{};
  std::vector<UCHAR> bytes;
  int submit_depth{},max_submit_depth{};
};
namespace {
std::vector<std::unique_ptr<fake_vhf>> handles;
std::vector<fake_vhf_kind> delete_order;
fake_vhf *mixed_keyboard_mouse_handle{};
fake_vhf *mixed_controller_handle{};
WDFFILEOBJECT mixed_controller_owner{};
bool observe_mixed_teardown{};
std::function<void(VHFHANDLE)> on_controller_delete;
keyboard_mouse_callback_context &callback_of(VHFHANDLE h) {
  return *static_cast<keyboard_mouse_callback_context *>(h->config.VhfClientContext);
}
keyboard_mouse_slot &slot_of(VHFHANDLE h) { return *callback_of(h).slot; }
controller_slot &controller_slot_of(VHFHANDLE h) {
  return *static_cast<controller_slot *>(h->config.VhfClientContext);
}
device_context &context_of(VHFHANDLE h) {
  return h->kind == fake_vhf_kind::keyboard_mouse
    ? *slot_of(h).parent : *controller_slot_of(h).parent;
}
void retained_unchanged(VHFHANDLE h) {
  if (!h->retained) return;
  CHECK(h->retained->reportBuffer==h->packet.reportBuffer);
  CHECK(h->retained->reportBufferLen==h->packet.reportBufferLen);
  CHECK(h->retained->reportId==h->packet.reportId);
  CHECK(std::memcmp(h->packet.reportBuffer,h->bytes.data(),h->bytes.size())==0);
}
void ready(VHFHANDLE h) {
  retained_unchanged(h); h->retained=nullptr; h->credit=true;
  h->config.EvtVhfReadyForNextReadReport(h->config.VhfClientContext);
}
void boundary(VHFHANDLE h) {
  CHECK(!fake_holds(context_of(h).state_lock));CHECK(fake_rundown_depth==0);
}
struct fixture {
  device_context context{};
  target_context target_data{};
  keyboard_mouse_file_context owner_data{},stranger_data{};
  std::mutex rundown_mutex;
  fake_condition rundown_drained;
  fake_wdf_object lifetime,state,target,device,timer,owner,stranger,queue;
  fixture() {
    create_result=start_result=submit_result=fake_open_status=STATUS_SUCCESS;
    forced_reply_error=0;on_reply={};
    on_create=on_start=on_delete=on_submit=on_before_submit={}; on_controller_delete={};
    fake_close_hook={}; fake_missing_file=false;
    fail_rundown_wait=false;failed_rundown_waits=0;
    creates=starts=deletes=submits=ioctls=opens=closes=fake_opens=fake_closes=0; handles.clear();
    delete_order.clear();mixed_keyboard_mouse_handle=nullptr;mixed_controller_handle=nullptr;
    mixed_controller_owner=nullptr;
    observe_mixed_teardown=false;
    fake_srw_states.clear();fake_condition_states.clear();
    context.powered=true;
    fake_cm_ids={0,0};fake_cm_hook={};fake_cm_queries=0;
    fake_cm_size_result=fake_cm_list_result=CR_SUCCESS;fake_heap_failure=false;
    device.context=&context; target.context=&target_data; target_data.device=&context;
    owner.parent=stranger.parent=timer.parent=queue.parent=&device;
    owner.context=&owner_data; stranger.context=&stranger_data;
    context.lifetime_gate=&lifetime; context.state_lock=&state; context.local_vhf_target=&target;
    context.pid_timer=&timer; context.keyboard_mouse.parent=&context;
    context.keyboard_mouse.rundown_lock.state=&rundown_mutex;
    context.keyboard_mouse.rundown_drained.state=&rundown_drained;
    for (std::uint32_t i=0;i<lvg::k_max_controllers;++i) {
      context.controllers[i].parent=&context;context.controllers[i].controller_id=i;
    }
  }
  ~fixture() {
    on_create=on_start=on_delete=on_submit=on_before_submit={}; on_controller_delete={}; fake_close_hook={};
    stop_owned_controllers(&context,false);
    for(auto &h:handles) CHECK(h->deleted);
    CHECK(fake_held_locks.empty());
  }
  NTSTATUS raw(DWORD code, void *input, size_t callback_in, size_t actual_in,
    void *output=nullptr,size_t callback_out=0,size_t actual_out=0,WDFFILEOBJECT file=nullptr,
    ULONG_PTR *information=nullptr,NTSTATUS input_status=STATUS_SUCCESS) {
    fake_wdf_object request;
    request.parent=file?file:&owner;request.input_buffer=input;request.input_length=actual_in;
    request.output_buffer=output;request.output_length=actual_out;request.input_status=input_status;
    evt_io_device_control(&queue,&request,callback_out,callback_in,code);
    CHECK(request.completions==1);
    if (!NT_SUCCESS(request.completion_status)) CHECK(request.information==0);
    if(information)*information=request.information;
    return request.completion_status;
  }
  template<class T> NTSTATUS send(DWORD code,T &input,WDFFILEOBJECT file=nullptr) {
    if((code>=lvg::km::ioctl_keyboard && code<=lvg::km::ioctl_absolute) || code==lvg::km::ioctl_consumer) {
      lvg::km::submission_response reply{};ULONG_PTR information=0;
      const auto status=raw(code,&input,sizeof(input),sizeof(input),&reply,sizeof(reply),sizeof(reply),file,&information);
      if(!NT_SUCCESS(status))return status;
      CHECK(information==sizeof(reply));CHECK(lvg::km::valid(reply));CHECK(reply.reserved==0);
      CHECK(reply.acceptance==lvg::km::submission_acceptance::accepted || reply.acceptance==lvg::km::submission_acceptance::busy);
      return reply.acceptance==lvg::km::submission_acceptance::busy ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    }
    return raw(code,&input,sizeof(input),sizeof(input),nullptr,0,0,file);
  }
  NTSTATUS open(WDFFILEOBJECT file=nullptr) {
    auto request=lvg::km::request<lvg::km::control_request>();return send(lvg::km::ioctl_open,request,file);
  }
  NTSTATUS close() { auto request=lvg::km::request<lvg::km::control_request>();return send(lvg::km::ioctl_close,request); }
  NTSTATUS abort() { auto request=lvg::km::request<lvg::km::control_request>();return send(lvg::km::ioctl_abort,request); }
};
fixture *current{};
} // anonymous test helpers
void InitializeSRWLock(SRWLOCK *lock) {
  fake_srw_states.push_back(std::make_unique<std::mutex>());
  lock->state=fake_srw_states.back().get();
}
void InitializeConditionVariable(CONDITION_VARIABLE *condition) {
  fake_condition_states.push_back(std::make_unique<fake_condition>());
  condition->state=fake_condition_states.back().get();
}
void AcquireSRWLockExclusive(SRWLOCK *lock) {
  static_cast<std::mutex *>(lock->state)->lock();++fake_rundown_depth;
}
void ReleaseSRWLockExclusive(SRWLOCK *lock) {
  --fake_rundown_depth;static_cast<std::mutex *>(lock->state)->unlock();
}
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *condition,SRWLOCK *lock,DWORD milliseconds,ULONG flags) {
  if(fail_rundown_wait.load() || milliseconds!=INFINITE || flags!=0) {
    if(fail_rundown_wait.load())++failed_rundown_waits;
    return FALSE;
  }
  auto *state=static_cast<fake_condition *>(condition->state);
  std::unique_lock<std::mutex> held(*static_cast<std::mutex *>(lock->state),std::adopt_lock);
  --fake_rundown_depth;++state->waiters;state->cv.wait(held);--state->waiters;++fake_rundown_depth;held.release();
  return TRUE;
}
void WakeAllConditionVariable(CONDITION_VARIABLE *condition) {
  static_cast<fake_condition *>(condition->state)->cv.notify_all();
}
void Sleep(DWORD milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
LONG InterlockedExchange(volatile LONG *destination,LONG value) {
  return std::atomic_ref<LONG>(*const_cast<LONG *>(destination)).exchange(value);
}
LONG InterlockedCompareExchange(volatile LONG *destination,LONG exchange,LONG comparand) {
  auto &word=*const_cast<LONG *>(destination);std::atomic_ref<LONG> value(word);
  LONG expected=comparand;value.compare_exchange_strong(expected,exchange);return expected;
}
LONG InterlockedIncrement(volatile LONG *destination) {
  return std::atomic_ref<LONG>(*const_cast<LONG *>(destination)).fetch_add(1)+1;
}
LONG InterlockedDecrement(volatile LONG *destination) {
  return std::atomic_ref<LONG>(*const_cast<LONG *>(destination)).fetch_sub(1)-1;
}
namespace {
void keyboard_mouse_test_before_submit(VHFHANDLE handle) {
  if(on_before_submit)on_before_submit(handle);
}
DWORD win_error(NTSTATUS status) {
  switch(status) {
    case STATUS_SUCCESS:return ERROR_SUCCESS;case STATUS_DEVICE_BUSY:return ERROR_BUSY;
    case STATUS_DEVICE_NOT_READY:return ERROR_NOT_READY;case STATUS_ACCESS_DENIED:return ERROR_ACCESS_DENIED;
    case STATUS_INVALID_PARAMETER:case STATUS_INVALID_BUFFER_SIZE:return ERROR_INVALID_PARAMETER;
    case STATUS_NOT_SUPPORTED:return ERROR_NOT_SUPPORTED;case STATUS_INVALID_HANDLE:return ERROR_INVALID_HANDLE;
    default:return ERROR_INVALID_FUNCTION;
  }
}
void test_keyboard_mouse_authorization() {
  fixture f;
  fake_client_is_admin = false; fake_client_is_system = false;
  CHECK(f.open() == STATUS_ACCESS_DENIED); CHECK(creates == 0);
  auto control = lvg::km::request<lvg::km::control_request>();
  auto query = lvg::km::request<lvg::km::query_response>();
  CHECK(f.raw(lvg::km::ioctl_query, &control, sizeof(control), sizeof(control),
    &query, sizeof(query), sizeof(query)) == STATUS_ACCESS_DENIED);
  fake_client_is_system = true; CHECK(f.open() == STATUS_SUCCESS);
  fake_client_is_system = false;
  fake_token_query_failure = true; CHECK(f.open() == STATUS_ACCESS_DENIED);
  fake_token_query_failure = false;
  auto key = lvg::km::request<lvg::km::keyboard_request>(); key.usage = 4; key.pressed = 1;
  CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_ACCESS_DENIED);
  CHECK(f.abort() == STATUS_ACCESS_DENIED); CHECK(deletes == 0);
  fake_client_is_admin = true; fake_impersonation_status = STATUS_ACCESS_DENIED;
  CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_ACCESS_DENIED);
  fake_impersonation_status = STATUS_SUCCESS; fake_token_open_failure = true;
  CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_ACCESS_DENIED);
  fake_token_open_failure = false; fake_sid_failure = true;
  CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_ACCESS_DENIED);
  fake_sid_failure = false; fake_membership_failure = true;
  CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_ACCESS_DENIED);
  fake_membership_failure = false; CHECK(f.send(lvg::km::ioctl_keyboard, key) == STATUS_SUCCESS);
  CHECK(f.abort() == STATUS_SUCCESS); CHECK(deletes == 1);
}
void test_raw_boundaries() {
  fixture f; auto control=lvg::km::request<lvg::km::control_request>();
  for(size_t bad:{size_t{0},sizeof(control)-1,sizeof(control)+1}) {
    CHECK(f.raw(lvg::km::ioctl_open,&control,bad,sizeof(control))==STATUS_INVALID_BUFFER_SIZE);
    CHECK(f.raw(lvg::km::ioctl_open,&control,sizeof(control),bad)!=STATUS_SUCCESS);
  }
  control.prefix.version++; CHECK(f.send(lvg::km::ioctl_open,control)==STATUS_INVALID_PARAMETER);
  control=lvg::km::request<lvg::km::control_request>();control.prefix.reserved=1;
  CHECK(f.send(lvg::km::ioctl_open,control)==STATUS_INVALID_PARAMETER);
  control=lvg::km::request<lvg::km::control_request>();control.prefix.size++;
  CHECK(f.send(lvg::km::ioctl_open,control)==STATUS_INVALID_PARAMETER);
  control=lvg::km::request<lvg::km::control_request>();
  CHECK(f.raw(lvg::km::ioctl_open,&control,sizeof(control),sizeof(control),nullptr,0,0,nullptr,nullptr,STATUS_ACCESS_DENIED)==STATUS_ACCESS_DENIED);
  CHECK(creates==0);
  // Aliased METHOD_BUFFERED query validates/copies input before overwriting it.
  std::array<std::byte,sizeof(lvg::km::query_response)> shared{};
  std::memcpy(shared.data(),&control,sizeof(control));ULONG_PTR information=0;
  CHECK(f.raw(lvg::km::ioctl_query,shared.data(),sizeof(control),sizeof(control),shared.data(),shared.size(),shared.size(),nullptr,&information)==STATUS_SUCCESS);
  lvg::km::query_response query{};std::memcpy(&query,shared.data(),sizeof(query));
  CHECK(information==sizeof(query));CHECK(lvg::km::valid(query));
  CHECK(query.capabilities==lvg::km::k_primary_capabilities);CHECK(query.absolute_target_generation==lvg::km::k_primary_absolute_generation);CHECK(query.reserved==0);CHECK(query.queue_capacity==64);
  CHECK(f.raw(lvg::km::ioctl_query,&control,sizeof(control),sizeof(control),&query,sizeof(query)+1,sizeof(query))==STATUS_INVALID_BUFFER_SIZE);
  CHECK(f.raw(lvg::km::ioctl_query,&control,sizeof(control),sizeof(control),&query,sizeof(query),sizeof(query)+1)==STATUS_INVALID_BUFFER_SIZE);
  CHECK(f.open()==STATUS_SUCCESS);CHECK(f.open(&f.stranger)==STATUS_DEVICE_BUSY);
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key,&f.stranger)==STATUS_ACCESS_DENIED);
  key.reserved=1;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_INVALID_PARAMETER);
  key.reserved=0;key.usage=3;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_INVALID_PARAMETER);
  auto absolute=lvg::km::request<lvg::km::absolute_request>();absolute.target_generation=2;
  CHECK(f.send(lvg::km::ioctl_absolute,absolute)==STATUS_DEVICE_NOT_READY);
  CHECK(f.context.keyboard_mouse.core.pending()==0);CHECK(submits==0);
  absolute.target_generation=lvg::km::k_primary_absolute_generation;
  absolute.x=65535;absolute.y=32768;
  CHECK(f.send(lvg::km::ioctl_absolute,absolute)==STATUS_SUCCESS);
  CHECK(f.context.keyboard_mouse.core.pending()==1);CHECK(submits==0);
  // Missing owner is rejected by the actual dispatch entry.
  fake_wdf_object request;request.input_buffer=&control;request.input_length=sizeof(control);
  evt_io_device_control(&f.queue,&request,0,sizeof(control),lvg::km::ioctl_open);
  CHECK(request.completion_status==STATUS_INVALID_HANDLE);CHECK(request.completions==1);
}
void test_fifo_and_drain() {
  fixture f;on_start=[](VHFHANDLE h){CHECK(slot_of(h).state==slot_state::starting);ready(h);};
  CHECK(f.open()==STATUS_SUCCESS);auto *h=handles.back().get();
  auto relative=lvg::km::request<lvg::km::relative_request>();relative.x=13;
  CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_SUCCESS);CHECK(submits==1);
  auto first=h->bytes;
  for(size_t i=0;i<keyboard_mouse_endpoint::queue_capacity;++i) CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_SUCCESS);
  CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_DEVICE_BUSY);retained_unchanged(h);CHECK(h->bytes==first);
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_DEVICE_BUSY);
  ready(h);CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  // Close purges queued input but not the retained report.
  CHECK(f.close()==STATUS_DEVICE_BUSY);CHECK(f.context.keyboard_mouse.core.pending()==3);retained_unchanged(h);
  CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_DEVICE_NOT_READY);
  ready(h);CHECK(h->packet.reportId==lvg::km::k_keyboard_report_id);
  for(size_t i=1;i<h->bytes.size();++i)CHECK(h->bytes[i]==0);
  CHECK(f.close()==STATUS_DEVICE_BUSY);CHECK(deletes==0);
  ready(h);CHECK(h->packet.reportId==lvg::km::k_relative_report_id);
  for(size_t i=1;i<h->bytes.size();++i)CHECK(h->bytes[i]==0);
  CHECK(f.context.keyboard_mouse.core.pending()==1);
  ready(h);CHECK(h->packet.reportId==lvg::km::k_consumer_report_id);
  for(size_t i=1;i<h->bytes.size();++i)CHECK(h->bytes[i]==0);
  CHECK(f.context.keyboard_mouse.core.pending()==0);
  CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::closing);
  CHECK(f.close()==STATUS_DEVICE_BUSY);CHECK(deletes==0);
  ready(h);CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::drained);
  CHECK(f.close()==STATUS_SUCCESS);CHECK(deletes==1);CHECK(h->deleted);
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);CHECK(f.open()==STATUS_SUCCESS);
}
void test_consumer_transport() {
  fixture f; on_start=[](VHFHANDLE h){ready(h);};
  CHECK(f.open()==STATUS_SUCCESS); auto *h=handles.back().get();
  auto media=lvg::km::request<lvg::km::consumer_request>();
  media.usage=0x00e9;media.pressed=1;
  CHECK(f.send(lvg::km::ioctl_consumer,media)==STATUS_SUCCESS);
  CHECK(h->packet.reportId==lvg::km::k_consumer_report_id);
  CHECK(h->bytes.size()==2 && h->bytes[1]==2);
  media.usage=0x00e2;CHECK(f.send(lvg::km::ioctl_consumer,media)==STATUS_SUCCESS);
  ready(h);CHECK(h->bytes[1]==3);
  media.usage=0x00e9;media.pressed=0;CHECK(f.send(lvg::km::ioctl_consumer,media)==STATUS_SUCCESS);
  ready(h);CHECK(h->bytes[1]==1);
  media.usage=0x8888;CHECK(f.send(lvg::km::ioctl_consumer,media)==STATUS_INVALID_PARAMETER);
  CHECK(f.abort()==STATUS_SUCCESS);
}
void test_absolute_mouse_drag_transport() {
  fixture f; on_start=[](VHFHANDLE h){ready(h);};
  CHECK(f.open()==STATUS_SUCCESS); auto *h=handles.back().get();
  auto button=lvg::km::request<lvg::km::button_request>();button.button=1;button.pressed=1;
  CHECK(f.send(lvg::km::ioctl_button,button)==STATUS_SUCCESS);
  CHECK(h->packet.reportId==lvg::km::k_relative_report_id && h->bytes[1]==1);
  auto absolute=lvg::km::request<lvg::km::absolute_request>();
  absolute.target_generation=lvg::km::k_primary_absolute_generation;absolute.x=0x1234;absolute.y=0x5678;
  CHECK(f.send(lvg::km::ioctl_absolute,absolute)==STATUS_SUCCESS);
  button.pressed=0;CHECK(f.send(lvg::km::ioctl_button,button)==STATUS_SUCCESS);
  ready(h);CHECK(h->packet.reportId==lvg::km::k_absolute_report_id);
  CHECK(h->bytes.size()==6 && h->bytes[1]==1 && h->bytes[2]==0x34 && h->bytes[3]==0x12 && h->bytes[4]==0x78 && h->bytes[5]==0x56);
  ready(h);CHECK(h->packet.reportId==lvg::km::k_relative_report_id && h->bytes[1]==0);
  CHECK(f.abort()==STATUS_SUCCESS);
}
void test_failures_and_forced_delete() {
  for(int mode=0;mode<5;++mode) {
    fixture f;
    if(mode==0)fake_open_status=STATUS_DEVICE_NOT_READY;
    if(mode==1)fake_missing_file=true;
    if(mode==2)create_result=STATUS_NOT_SUPPORTED;
    if(mode==3)start_result=STATUS_CANCELLED;
    if(mode==4)on_start=[&](VHFHANDLE){lock_context(&f.context);f.owner_data.cleanup_started=true;unlock_context(&f.context);};
    CHECK(!NT_SUCCESS(f.open()));CHECK(f.context.keyboard_mouse.state==slot_state::empty);
    CHECK(deletes==(mode>=3?1:0));
  }
  for(int mode=0;mode<4;++mode) {
    fixture f;on_start=[](VHFHANDLE h){ready(h);};CHECK(f.open()==STATUS_SUCCESS);auto *h=handles.back().get();
    auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=0xe7;key.pressed=1;
    if(mode==0)submit_result=STATUS_DEVICE_BUSY; // Must not leak as retryable busy.
    CHECK(f.send(lvg::km::ioctl_keyboard,key)==(mode==0?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS));
    if(mode==0) {CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::faulted);CHECK(f.close()==STATUS_DEVICE_NOT_READY);}
    if(mode==1)evt_keyboard_mouse_file_cleanup(&f.owner);
    if(mode==2)CHECK(evt_release_hardware(&f.device,nullptr)==STATUS_SUCCESS);
    if(mode==3)evt_vhf_target_cleanup(&f.target);
    CHECK(h->deleted);CHECK(deletes==1);CHECK(f.context.keyboard_mouse.state==slot_state::empty);
    if(mode==1)CHECK(f.open()==STATUS_DEVICE_NOT_READY);
    if(mode==2) {CHECK(fake_closes==1);CHECK(evt_prepare_hardware(&f.device,nullptr,nullptr)==STATUS_SUCCESS);CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);CHECK(f.open()==STATUS_SUCCESS);}
  }
}
void test_cleanup_during_start() {
  fixture f;std::future<void> cleanup;
  on_start=[&](VHFHANDLE h) {
    cleanup=std::async(std::launch::async,[&]{evt_keyboard_mouse_file_cleanup(&f.owner);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    bool marked=false;
    while(std::chrono::steady_clock::now()<deadline) {
      lock_context(&f.context);marked=f.owner_data.cleanup_started;unlock_context(&f.context);
      if(marked && f.lifetime.waiters>0)break;
      std::this_thread::yield();
    }
    CHECK(marked);CHECK(f.lifetime.waiters==1);ready(h);
  };
  CHECK(f.open()==STATUS_CANCELLED);cleanup.get();CHECK(deletes==1);
  CHECK(f.open()==STATUS_DEVICE_NOT_READY);
}
void test_submit_rundown_and_late_delete_callback() {
  fixture f;
  CHECK(f.open()==STATUS_SUCCESS);
  auto *const first=handles.back().get();
  const auto first_generation=f.context.keyboard_mouse.generation;
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  auto relative=lvg::km::request<lvg::km::relative_request>();relative.x=7;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_SUCCESS);
  int inline_ready=0;on_submit=[&](VHFHANDLE h){if(inline_ready++==0)ready(h);};
  ready(first);on_submit={};
  CHECK(submits==2);CHECK(first->packet.reportId==lvg::km::k_relative_report_id);
  CHECK(first->credit==false);CHECK(first->max_submit_depth==1);
  CHECK(f.context.keyboard_mouse.core.pending()==0);

  // A separate READY entry during a paused VhfReadReportSubmit must record a
  // second credit without starting a concurrent pump or losing the next item.
  // Usage 4 is still held from the first phase, so press a fresh key here.
  key.usage=5;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  relative.x=11;CHECK(f.send(lvg::km::ioctl_relative,relative)==STATUS_SUCCESS);
  std::promise<void> submit_entered_promise,release_submit_promise;
  auto submit_entered=submit_entered_promise.get_future();
  auto release_submit=release_submit_promise.get_future().share();
  std::atomic<int> paused_submits{};
  on_submit=[&](VHFHANDLE){if(paused_submits++==0){submit_entered_promise.set_value();release_submit.wait();}};
  const int before_concurrent_ready=submits;
  auto first_ready=std::async(std::launch::async,[&]{ready(first);});
  const bool submit_paused=submit_entered.wait_for(std::chrono::seconds(3))==std::future_status::ready;
  CHECK(submit_paused);
  if(!submit_paused) {
    // Release and join the callback before fixture teardown; do not schedule
    // dependent READY/abort phases after the setup precondition failed.
    release_submit_promise.set_value();first_ready.get();on_submit={};return;
  }
  auto second_ready=std::async(std::launch::async,[&]{ready(first);});
  const bool second_ready_returned=second_ready.wait_for(std::chrono::seconds(3))==std::future_status::ready;
  CHECK(second_ready_returned);
  if(!second_ready_returned) {
    release_submit_promise.set_value();first_ready.get();second_ready.get();on_submit={};return;
  }
  second_ready.get();
  const bool one_submit_still_inflight=submits==before_concurrent_ready+1;
  CHECK(one_submit_still_inflight);
  if(!one_submit_still_inflight) {
    release_submit_promise.set_value();first_ready.get();on_submit={};return;
  }
  lock_keyboard_mouse_rundown(f.context.keyboard_mouse);lock_context(&f.context);
  const bool owner_state=f.context.keyboard_mouse.submit_owners==1 && f.context.keyboard_mouse.pump_active;
  CHECK(f.context.keyboard_mouse.submit_owners==1);CHECK(f.context.keyboard_mouse.pump_active);
  unlock_context(&f.context);unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
  if(!owner_state) {
    release_submit_promise.set_value();first_ready.get();on_submit={};return;
  }
  release_submit_promise.set_value();first_ready.get();on_submit={};
  const bool concurrent_ready_drained=submits==before_concurrent_ready+2 && !first->credit &&
    first->max_submit_depth==1 && f.context.keyboard_mouse.core.pending()==0;
  CHECK(submits==before_concurrent_ready+2);CHECK(first->credit==false);
  CHECK(first->max_submit_depth==1);CHECK(f.context.keyboard_mouse.core.pending()==0);
  if(!concurrent_ready_drained) return;

  // Arrange another report, then pause the real pump after it owns the handle,
  // packet, report storage, and submit right but before it enters VHF.
  key.usage=6;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  std::promise<void> reserved_promise,resume_promise;
  auto reserved=reserved_promise.get_future();auto resume=resume_promise.get_future().share();
  on_before_submit=[&](VHFHANDLE){reserved_promise.set_value();resume.wait();};
  auto callback=std::async(std::launch::async,[&]{ready(first);});
  const bool reservation_entered=reserved.wait_for(std::chrono::seconds(3))==std::future_status::ready;
  CHECK(reservation_entered);
  if(!reservation_entered) {
    // A failed reservation precondition must join the callback before returning;
    // otherwise later abort/state assertions can race live fixture state.
    resume_promise.set_value();callback.get();on_before_submit={};return;
  }
  lock_context(&f.context);f.context.controllers[0].feedback_pending=true;unlock_context(&f.context);
  auto abort=std::async(std::launch::async,[&]{return f.abort();});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(std::chrono::steady_clock::now()<deadline && f.rundown_drained.waiters.load()==0)
    std::this_thread::yield();
  CHECK(f.rundown_drained.waiters.load()==1);CHECK(deletes==0);
  lock_keyboard_mouse_rundown(f.context.keyboard_mouse);lock_context(&f.context);
  CHECK(f.context.keyboard_mouse.vhf==first);CHECK(f.context.keyboard_mouse.submit_owners==1);
  CHECK(f.context.keyboard_mouse.pump_active);
  CHECK(f.context.keyboard_mouse.read_transfer.reportBuffer!=nullptr);
  CHECK(f.context.keyboard_mouse.read_transfer.reportBufferLen==30);
  CHECK(f.context.keyboard_mouse.read_transfer.reportId==lvg::km::k_keyboard_report_id);
  CHECK(f.context.keyboard_mouse.read_transfer.reportBuffer[0]==lvg::km::k_keyboard_report_id);
  CHECK(f.context.keyboard_mouse.read_transfer.reportBuffer[1]==0x70);
  CHECK(f.context.keyboard_mouse.core.pending()==0);
  CHECK(f.context.controllers[0].feedback_pending);
  unlock_context(&f.context);unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
  resume_promise.set_value();callback.get();CHECK(abort.get()==STATUS_SUCCESS);
  on_before_submit={};CHECK(deletes==1);CHECK(first->deleted);
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);
  lock_context(&f.context);CHECK(f.context.controllers[0].feedback_pending);unlock_context(&f.context);

  // A new activation receives a distinct callback context after the prior
  // context passed synchronous delete/cleanup and admitted-entry drain.
  CHECK(f.open()==STATUS_SUCCESS);auto *const second=handles.back().get();
  CHECK(f.context.keyboard_mouse.generation!=first_generation);
  key.usage=6;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  const int before_new_ready=submits;ready(second);CHECK(submits==before_new_ready+1);
}
void test_rundown_wait_failure_fallback_drains() {
  fixture f;CHECK(f.open()==STATUS_SUCCESS);auto *const h=handles.back().get();
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  std::promise<void> reserved_promise,resume_promise;
  auto reserved=reserved_promise.get_future();auto resume=resume_promise.get_future().share();
  on_before_submit=[&](VHFHANDLE){reserved_promise.set_value();resume.wait();};
  auto callback=std::async(std::launch::async,[&]{ready(h);});
  const bool reservation_entered=reserved.wait_for(std::chrono::seconds(3))==std::future_status::ready;
  CHECK(reservation_entered);
  if(!reservation_entered) {
    resume_promise.set_value();callback.get();on_before_submit={};return;
  }
  fail_rundown_wait=true;
  auto release=std::async(std::launch::async,[&]{return evt_release_hardware(&f.device,nullptr);});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  bool stopping=false;
  while(std::chrono::steady_clock::now()<deadline &&
        (!stopping || failed_rundown_waits.load()==0)) {
    lock_keyboard_mouse_rundown(f.context.keyboard_mouse);
    stopping=f.context.keyboard_mouse.rundown_stopping;
    unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
    if(!stopping)std::this_thread::yield();
  }
  CHECK(stopping);CHECK(failed_rundown_waits.load()>0);CHECK(deletes==0);
  lock_keyboard_mouse_rundown(f.context.keyboard_mouse);lock_context(&f.context);
  CHECK(f.context.keyboard_mouse.vhf==h);CHECK(f.context.vhf_target_open);
  CHECK(f.context.local_vhf_target==&f.target);CHECK(f.context.vhf_file_handle!=nullptr);
  unlock_context(&f.context);unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
  fail_rundown_wait=false;resume_promise.set_value();callback.get();on_before_submit={};
  CHECK(release.get()==STATUS_SUCCESS);
  CHECK(deletes==1);CHECK(h->deleted);CHECK(!f.context.vhf_target_open);
}

void test_mixed_keyboard_mouse_and_controller_teardown_order() {
  fixture f;
  CHECK(f.open()==STATUS_SUCCESS);
  CHECK(handles.size()==1);
  if(handles.empty())return;
  mixed_keyboard_mouse_handle=handles.front().get();
  mixed_controller_owner=&f.owner;
  CHECK(mixed_keyboard_mouse_handle->kind==fake_vhf_kind::keyboard_mouse);

  lvg::create_controller_request controller{};
  controller.header.size=sizeof(controller);
  controller.header.version=lvg::k_protocol_version;
  controller.controller_id=0;
  controller.requested_profile=lvg::profile::xbox_series;
  CHECK(f.send(lvg::ioctl_create_controller,controller)==STATUS_SUCCESS);
  CHECK(handles.size()==2);
  if(handles.size()<2)return;
  mixed_controller_handle=handles.back().get();
  CHECK(mixed_controller_handle->kind==fake_vhf_kind::controller);
  CHECK(controller_slot_of(mixed_controller_handle).parent==&f.context);
  CHECK(controller_slot_of(mixed_controller_handle).state==slot_state::active);
  CHECK(f.context.vhf_file_handle!=nullptr);
  CHECK(f.context.vhf_target_open);

  auto key=lvg::km::request<lvg::km::keyboard_request>();
  key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);

  std::promise<void> submit_entered_promise,release_submit_promise;
  auto submit_entered=submit_entered_promise.get_future();
  auto release_submit=release_submit_promise.get_future().share();
  on_before_submit=[&](VHFHANDLE h) {
    CHECK(h==mixed_keyboard_mouse_handle);
    submit_entered_promise.set_value();
    release_submit.wait();
  };
  observe_mixed_teardown=true;
  auto callback=std::async(std::launch::async,[&]{ready(mixed_keyboard_mouse_handle);});
  const bool submit_paused=submit_entered.wait_for(std::chrono::seconds(3))==std::future_status::ready;
  CHECK(submit_paused);
  if(!submit_paused) {
    release_submit_promise.set_value();callback.get();on_before_submit={};
    observe_mixed_teardown=false;return;
  }

  auto stopping=std::async(std::launch::async,[&]{return stop_owned_controllers(&f.context,false);});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(std::chrono::steady_clock::now()<deadline && f.rundown_drained.waiters.load()==0)
    std::this_thread::yield();
  const bool rundown_waiting=f.rundown_drained.waiters.load()==1;
  CHECK(rundown_waiting);
  if(rundown_waiting) {
    lock_keyboard_mouse_rundown(f.context.keyboard_mouse);lock_context(&f.context);
    CHECK(f.context.stopping);
    CHECK(f.context.vhf_file_handle!=nullptr);
    CHECK(f.context.vhf_target_open);
    CHECK(f.context.local_vhf_target==&f.target);
    CHECK(f.context.keyboard_mouse.state==slot_state::stopping);
    CHECK(f.context.keyboard_mouse.vhf==mixed_keyboard_mouse_handle);
    CHECK(f.context.keyboard_mouse.submit_owners==1);
    CHECK(f.context.keyboard_mouse.pump_active);
    CHECK(f.context.controllers[0].state==slot_state::stopping);
    CHECK(f.context.controllers[0].vhf==mixed_controller_handle);
    CHECK(f.context.controllers[0].owner==&f.owner);
    unlock_context(&f.context);unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
    CHECK(deletes==0);
  }

  release_submit_promise.set_value();
  callback.get();
  stopping.get();
  on_before_submit={};
  observe_mixed_teardown=false;
  CHECK(delete_order.size()==2);
  if(delete_order.size()==2) {
    CHECK(delete_order[0]==fake_vhf_kind::keyboard_mouse);
    CHECK(delete_order[1]==fake_vhf_kind::controller);
  }
  CHECK(deletes==2);
  CHECK(mixed_keyboard_mouse_handle->deleted);
  CHECK(mixed_controller_handle->deleted);
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);
  CHECK(f.context.controllers[0].state==slot_state::empty);
  CHECK(f.context.vhf_file_handle==nullptr);
  CHECK(f.context.vhf_target_open);
  CHECK(f.context.local_vhf_target==&f.target);
}

template<class predicate_t> bool wait_until(predicate_t predicate) {
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(std::chrono::steady_clock::now()<deadline) {
    if(predicate())return true;
    std::this_thread::yield();
  }
  return predicate();
}
NTSTATUS query(fixture &f) {
  auto control=lvg::km::request<lvg::km::control_request>();
  lvg::km::query_response response{};ULONG_PTR information{};
  const auto status=f.raw(lvg::km::ioctl_query,&control,sizeof(control),sizeof(control),
    &response,sizeof(response),sizeof(response),nullptr,&information);
  if(NT_SUCCESS(status)) {
    CHECK(information==sizeof(response));CHECK(response.capabilities==lvg::km::k_primary_capabilities);
    CHECK(response.absolute_target_generation==lvg::km::k_primary_absolute_generation);
  }
  return status;
}
void create_mixed_controller(fixture &f) {
  lvg::create_controller_request controller{};
  controller.header.size=sizeof(controller);controller.header.version=lvg::k_protocol_version;
  controller.requested_profile=lvg::profile::xbox_series;
  CHECK(f.send(lvg::ioctl_create_controller,controller)==STATUS_SUCCESS);
}
void test_power_admission_and_reentry() {
  fixture f;f.context.powered=false;
  CHECK(query(f)==STATUS_DEVICE_NOT_READY);CHECK(f.open()==STATUS_DEVICE_NOT_READY);
  CHECK(creates==0);CHECK(fake_cm_queries==0);
  CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);
  CHECK(query(f)==STATUS_SUCCESS);CHECK(f.open()==STATUS_SUCCESS);
  auto *const old=handles.back().get();
  create_mixed_controller(f);auto *const controller=handles.back().get();
  const auto generation=f.context.keyboard_mouse.generation;
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);ready(old);
  key.usage=5;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  const int before=submits;
  on_delete=[&](VHFHANDLE h) {
    CHECK(!f.context.powered);CHECK(f.context.vhf_file_handle!=nullptr);
    CHECK(fake_closes==0);CHECK(submits==before);
    if(h==old)CHECK(f.context.keyboard_mouse.generation!=generation);
  };
  CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);on_delete={};
  CHECK(deletes==2);CHECK(old->deleted && controller->deleted);CHECK(submits==before);
  CHECK(f.context.keyboard_mouse.state==slot_state::lost);
  CHECK(f.context.keyboard_mouse.owner==&f.owner);
  CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::closed);
  CHECK(f.context.keyboard_mouse.core.pending()==0);CHECK(f.context.keyboard_mouse.callback_context==nullptr);
  CHECK(f.context.keyboard_mouse.read_transfer.reportBuffer==nullptr);
  CHECK(f.context.controllers[0].state==slot_state::lost);
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_DEVICE_REMOVED);
  CHECK(f.close()==STATUS_DEVICE_REMOVED);CHECK(query(f)==STATUS_DEVICE_NOT_READY);
  CHECK(f.open()==STATUS_DEVICE_NOT_READY);
  CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);CHECK(deletes==2);
  CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);
  CHECK(query(f)==STATUS_SUCCESS);CHECK(f.open(&f.stranger)==STATUS_DEVICE_BUSY);
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_DEVICE_REMOVED);
  // PnP removal can lag D0 entry. No duplicate instance is created until gone.
  constexpr wchar_t id[]=L"HID\\fixture&vibeshinekeyboardmouse";
  fake_cm_ids.assign(id,id+std::size(id));fake_cm_ids.push_back(0);
  CHECK(f.open()==STATUS_DEVICE_BUSY);CHECK(creates==2);
  fake_cm_ids={0,0};
  CHECK(f.open()==STATUS_SUCCESS);CHECK(f.context.keyboard_mouse.generation!=generation);
  auto *const fresh=handles.back().get();
  CHECK(f.context.keyboard_mouse.core.pending()==0);CHECK(submits==before);
  // A separate, still-live synthetic context with the old generation models a
  // stale ready entry without calling through freed post-Delete context memory.
  keyboard_mouse_callback_context stale{};
  stale.parent=&f.context;stale.slot=&f.context.keyboard_mouse;stale.generation=generation;
  InitializeSRWLock(&stale.entry_lock);InitializeConditionVariable(&stale.entry_drained);
  key.usage=4;CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
  evt_keyboard_mouse_ready(&stale);CHECK(stale.active_entries==0);CHECK(submits==before);
  CHECK(f.context.keyboard_mouse.core.pending()==1);ready(fresh);CHECK(submits==before+1);
  // Old held key 5 and queued input were not replayed across power loss.
  CHECK((fresh->bytes[1]&0x10)!=0);CHECK((fresh->bytes[1]&0x20)==0);
  CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);
  evt_keyboard_mouse_file_cleanup(&f.owner);
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);CHECK(f.context.keyboard_mouse.owner==nullptr);
  CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);CHECK(f.open()==STATUS_DEVICE_NOT_READY);
  CHECK(f.open(&f.stranger)==STATUS_SUCCESS);
}
void test_power_during_create_and_start() {
  for(bool during_start:{false,true}) {
    fixture f;std::future<NTSTATUS> power;
    const auto transition=[&](VHFHANDLE h) {
      power=std::async(std::launch::async,[&]{return evt_d0_exit(&f.device,0);});
      CHECK(wait_until([&]{
        lock_context(&f.context);const bool off=!f.context.powered;unlock_context(&f.context);
        return off && f.lifetime.waiters>0;
      }));
      CHECK(query(f)==STATUS_DEVICE_NOT_READY);
      if(during_start) {
        // Readiness arrives reentrantly before Start returns, after D0 intent.
        lock_context(&f.context);
        auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
        CHECK(f.context.keyboard_mouse.core.submit(reinterpret_cast<keyboard_mouse_endpoint::owner_token>(&f.owner),key)==keyboard_mouse_endpoint::result::accepted);
        unlock_context(&f.context);
        ready(h);CHECK(submits==0);
      }
    };
    if(during_start)on_start=transition;else on_create=transition;
    CHECK(f.open()==STATUS_CANCELLED);
    if(power.valid())CHECK(power.get()==STATUS_SUCCESS);
    on_create=on_start={};
    CHECK(creates==1);CHECK(starts==(during_start?1:0));CHECK(deletes==1);CHECK(submits==0);
    CHECK(f.context.keyboard_mouse.state==slot_state::empty);
    CHECK(f.context.keyboard_mouse.owner==nullptr);CHECK(f.context.keyboard_mouse.callback_context==nullptr);
    CHECK(f.context.keyboard_mouse.core.pending()==0);
    CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);CHECK(f.open()==STATUS_SUCCESS);
  }
}
void test_power_during_enumeration() {
  fixture f;
  // A complete off/on cycle while the OS snapshot is in flight invalidates the
  // reservation even though the final powered bool has returned to true.
  fake_cm_hook=[&]{
    CHECK(!fake_holds(f.context.lifetime_gate));CHECK(!fake_holds(f.context.state_lock));
    CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);
  };
  CHECK(f.open()==STATUS_DEVICE_BUSY);CHECK(creates==0);
  fake_cm_hook={};
  for(int failure=0;failure<4;++failure) {
    if(failure==0)fake_cm_size_result=1;
    if(failure==1)fake_cm_list_result=1;
    if(failure==2)fake_heap_failure=true;
    if(failure==3)fake_cm_ids={L'x',L'y'};
    CHECK(f.open()==STATUS_DEVICE_BUSY);CHECK(creates==0);
    fake_cm_size_result=fake_cm_list_result=CR_SUCCESS;fake_heap_failure=false;fake_cm_ids={0,0};
  }
  CHECK(f.open()==STATUS_SUCCESS);
}
void test_power_rundown_with_cleanup() {
  for(bool cleanup_during_power:{false,true}) {
    fixture f;CHECK(f.open()==STATUS_SUCCESS);auto *const h=handles.back().get();
    create_mixed_controller(f);
    auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
    CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
    std::promise<void> reserved_promise,resume_promise;
    auto reserved=reserved_promise.get_future();auto resume=resume_promise.get_future().share();
    on_before_submit=[&](VHFHANDLE){reserved_promise.set_value();resume.wait();};
    auto callback=std::async(std::launch::async,[&]{ready(h);});
    const bool paused=reserved.wait_for(std::chrono::seconds(3))==std::future_status::ready;
    CHECK(paused);
    if(!paused){resume_promise.set_value();callback.get();on_before_submit={};return;}
    auto power=std::async(std::launch::async,[&]{return evt_d0_exit(&f.device,0);});
    CHECK(wait_until([&]{return f.rundown_drained.waiters.load()>0;}));
    CHECK(deletes==0);CHECK(query(f)==STATUS_DEVICE_NOT_READY);
    lock_keyboard_mouse_rundown(f.context.keyboard_mouse);lock_context(&f.context);
    CHECK(!f.context.powered);CHECK(f.context.keyboard_mouse.state==slot_state::stopping);
    CHECK(f.context.keyboard_mouse.submit_owners==1);CHECK(f.context.keyboard_mouse.pump_active);
    CHECK(f.context.keyboard_mouse.vhf==h);CHECK(f.context.vhf_file_handle!=nullptr);
    CHECK(f.context.controllers[0].state==slot_state::stopping);
    unlock_context(&f.context);unlock_keyboard_mouse_rundown(f.context.keyboard_mouse);
    // Retired readiness must return without entering the blocked submit or
    // minting a second credit; the admitted owner may still finish once.
    evt_keyboard_mouse_ready(h->config.VhfClientContext);CHECK(submits==0);
    std::future<void> cleanup;
    if(cleanup_during_power) {
      cleanup=std::async(std::launch::async,[&]{evt_keyboard_mouse_file_cleanup(&f.owner);});
      CHECK(wait_until([&]{lock_context(&f.context);const bool marked=f.owner_data.cleanup_started;unlock_context(&f.context);return marked;}));
    }
    resume_promise.set_value();callback.get();CHECK(power.get()==STATUS_SUCCESS);
    if(cleanup.valid())cleanup.get();
    on_before_submit={};CHECK(deletes==2);CHECK(submits==1);CHECK(h->deleted);
    CHECK(f.context.keyboard_mouse.state==(cleanup_during_power?slot_state::empty:slot_state::lost));
    CHECK(f.context.keyboard_mouse.submit_owners==0);CHECK(!f.context.keyboard_mouse.pump_active);
    CHECK(f.context.keyboard_mouse.core.pending()==0);
    CHECK(f.abort()==STATUS_SUCCESS);CHECK(f.context.keyboard_mouse.state==slot_state::empty);
    CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);
    CHECK(f.open()==(cleanup_during_power?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS));
  }
}
void test_power_during_neutral_drain_and_fault() {
  for(bool faulted:{false,true}) {
    fixture f;on_start=[](VHFHANDLE h){ready(h);};CHECK(f.open()==STATUS_SUCCESS);
    auto *const h=handles.back().get();
    auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
    if(faulted)submit_result=STATUS_DEVICE_BUSY;
    CHECK(f.send(lvg::km::ioctl_keyboard,key)==(faulted?STATUS_DEVICE_NOT_READY:STATUS_SUCCESS));
    if(!faulted)CHECK(f.close()==STATUS_DEVICE_BUSY);
    const int before=submits;
    CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);CHECK(submits==before);
    CHECK(h->deleted);CHECK(f.close()==STATUS_DEVICE_REMOVED);CHECK(f.abort()==STATUS_SUCCESS);
    CHECK(evt_d0_entry(&f.device,0)==STATUS_SUCCESS);submit_result=STATUS_SUCCESS;
    CHECK(f.open()==STATUS_SUCCESS);
    CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);
    CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::active);
  }
}
void test_source_initialization() {
  fixture f;
  fake_wdf_object init;
  fake_wdf_initialization initialization;
  initialization.device=&f.device;initialization.lifetime=&f.lifetime;initialization.state=&f.state;
  initialization.target=&f.target;initialization.timer=&f.timer;initialization.queue=&f.queue;
  CHECK(fake_initialization==nullptr);fake_initialization=&initialization;
  CHECK(DriverEntry(nullptr,nullptr)==STATUS_SUCCESS);CHECK(initialization.add!=nullptr);
  CHECK(initialization.add(nullptr,&init)==STATUS_SUCCESS);fake_initialization=nullptr;
  CHECK(initialization.filter_at_device_create);CHECK(initialization.queue_config.PowerManaged==WdfFalse);
  CHECK(initialization.files.EvtFileCleanup==evt_keyboard_mouse_file_cleanup);
  CHECK(initialization.files.AutoForwardCleanupClose==WdfFalse);
  CHECK(initialization.file_context_size==sizeof(keyboard_mouse_file_context));
  CHECK(initialization.pnp.EvtDeviceD0Entry==evt_d0_entry);
  CHECK(initialization.pnp.EvtDeviceD0Exit==evt_d0_exit);
  CHECK(f.context.keyboard_mouse.parent==&f.context);CHECK(f.context.keyboard_mouse.core.pending()==0);
  CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::closed);
  CHECK(!f.context.powered);CHECK(f.open()==STATUS_DEVICE_NOT_READY);
  fake_wdf_object request;
  initialization.files.EvtDeviceFileCreate(&f.device,&request,nullptr);
  CHECK(request.completions==1 && request.completion_status==STATUS_SUCCESS);
  initialization.files.EvtFileCleanup(nullptr);initialization.files.EvtFileClose(nullptr);
  CHECK(initialization.pnp.EvtDevicePrepareHardware(&f.device,nullptr,nullptr)==STATUS_SUCCESS);
  CHECK(initialization.pnp.EvtDeviceD0Entry(&f.device,0)==STATUS_SUCCESS);
  CHECK(f.open()==STATUS_SUCCESS);create_mixed_controller(f);
  CHECK(initialization.pnp.EvtDeviceD0Exit(&f.device,0)==STATUS_SUCCESS);
  CHECK(f.context.keyboard_mouse.state==slot_state::lost);CHECK(deletes==2);
  CHECK(initialization.pnp.EvtDeviceReleaseHardware(&f.device,nullptr)==STATUS_SUCCESS);
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);CHECK(fake_closes==1);
}
void test_power_retirement_minimal() {
  fixture f;CHECK(f.open()==STATUS_SUCCESS);auto *const km=handles.back().get();
  create_mixed_controller(f);auto *const controller=handles.back().get();
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  CHECK(f.send(lvg::km::ioctl_keyboard,key)==STATUS_SUCCESS);ready(km);
  CHECK(evt_d0_exit(&f.device,0)==STATUS_SUCCESS);
  CHECK(km->deleted);CHECK(controller->deleted);CHECK(deletes==2);
  CHECK(f.context.keyboard_mouse.state==slot_state::lost);
  CHECK(f.context.keyboard_mouse.vhf==nullptr);CHECK(f.context.keyboard_mouse.callback_context==nullptr);
  CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::closed);
  CHECK(f.context.controllers[0].state==slot_state::lost);
}
void test_power_contracts() {
  test_power_retirement_minimal();test_source_initialization();test_power_admission_and_reentry();test_power_during_create_and_start();test_power_during_enumeration();
  test_power_rundown_with_cleanup();test_power_during_neutral_drain_and_fault();
}

void test_submission_reply_validation() {
  for(int mode=0;mode<5;++mode) {
    fixture f;current=&f;lvg::client client;CHECK(client.connect()==ERROR_SUCCESS);
    on_start=[](VHFHANDLE h){ready(h);};CHECK(client.open_keyboard_mouse()==ERROR_SUCCESS);
    on_reply=[&](DWORD code,void *raw,DWORD *bytes) {
      if(code!=lvg::km::ioctl_keyboard)return;
      auto *reply=static_cast<lvg::km::submission_response *>(raw);
      if(mode==0)--*bytes;
      if(mode==1)reply->prefix.version++;
      if(mode==2)reply->prefix.reserved=1;
      if(mode==3)reply->reserved=1;
      if(mode==4)reply->acceptance=static_cast<lvg::km::submission_acceptance>(99);
    };
    auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
    CHECK(client.submit_keyboard_mouse(key)==ERROR_INVALID_DATA);CHECK(submits==1);
    CHECK(lvg::km::classify_submission(ERROR_INVALID_DATA)==lvg::km::submission_outcome::uncertain);
    on_reply={};CHECK(client.abort_keyboard_mouse()==ERROR_SUCCESS);client.close();current=nullptr;
  }
}

void test_client_chain() {
  fixture f;current=&f;lvg::client client;
  CHECK(client.connect()==ERROR_SUCCESS);CHECK(opens==1);CHECK(client.connected());
  const auto gamepad_features=client.available_features();CHECK((gamepad_features&lvg::feature_input_state)!=0);
  lvg::km::query_response query{};CHECK(client.query_keyboard_mouse(&query)==ERROR_SUCCESS);
  CHECK(query.capabilities==lvg::km::k_primary_capabilities);CHECK(query.absolute_target_generation==lvg::km::k_primary_absolute_generation);
  for(int mode=0;mode<5;++mode) {
    on_reply=[&](DWORD code,void *raw,DWORD *bytes) {
      if(code!=lvg::km::ioctl_query)return;
      auto *response=static_cast<lvg::km::query_response *>(raw);
      if(mode==0)--*bytes;
      if(mode==1)response->prefix.version++;
      if(mode==2)response->reserved=1;
      if(mode==3)response->capabilities&=~lvg::km::bound_absolute_mouse;
      if(mode==4)response->absolute_target_generation=0;
    };
    CHECK(client.query_keyboard_mouse(&query)==ERROR_INVALID_DATA);
    CHECK(query.capabilities==0);CHECK(query.absolute_target_generation==0);
  }
  on_reply={};
  CHECK(client.available_features()==gamepad_features);
  on_start=[](VHFHANDLE h){ready(h);};CHECK(client.open_keyboard_mouse()==ERROR_SUCCESS);
  auto key=lvg::km::request<lvg::km::keyboard_request>();key.usage=4;key.pressed=1;
  auto media=lvg::km::request<lvg::km::consumer_request>();media.usage=0xffff;media.pressed=1;
  CHECK(client.submit_keyboard_mouse(media)==ERROR_INVALID_PARAMETER);
  media.usage=0x00e9;CHECK(client.submit_keyboard_mouse(media)==ERROR_SUCCESS);
  auto *media_handle=handles.back().get();
  CHECK(media_handle->packet.reportId==lvg::km::k_consumer_report_id && media_handle->bytes[1]==2);
  ready(media_handle);media.pressed=0;CHECK(client.submit_keyboard_mouse(media)==ERROR_SUCCESS);
  CHECK(media_handle->bytes[1]==0);ready(media_handle);
  const int before=ioctls;key.reserved=1;CHECK(client.submit_keyboard_mouse(key)==ERROR_INVALID_PARAMETER);CHECK(ioctls==before);key.reserved=0;
  CHECK(client.submit_keyboard_mouse(key)==ERROR_SUCCESS);
  auto relative=lvg::km::request<lvg::km::relative_request>();relative.x=1;
  for(size_t i=0;i<keyboard_mouse_endpoint::queue_capacity;++i)CHECK(client.submit_keyboard_mouse(relative)==ERROR_SUCCESS);
  CHECK(client.submit_keyboard_mouse(relative)==ERROR_BUSY); // Validated producer busy acknowledgement.
  auto *h=handles.back().get();CHECK(client.close_keyboard_mouse()==ERROR_BUSY);
  ready(h);ready(h);CHECK(client.close_keyboard_mouse()==ERROR_BUSY);ready(h);ready(h);
  CHECK(client.close_keyboard_mouse()==ERROR_SUCCESS);
  CHECK(client.open_keyboard_mouse()==ERROR_SUCCESS);
  submit_result=STATUS_DEVICE_BUSY;CHECK(client.submit_keyboard_mouse(key)==ERROR_NOT_READY);
  CHECK(lvg::km::classify_submission(ERROR_BUSY)==lvg::km::submission_outcome::retry_without_acceptance);
  CHECK(lvg::km::classify_submission(ERROR_NOT_READY)==lvg::km::submission_outcome::uncertain);
  CHECK(client.abort_keyboard_mouse()==ERROR_SUCCESS);CHECK(client.connected());CHECK(client.available_features()==gamepad_features);
  CHECK(client.abort_keyboard_mouse()==ERROR_SUCCESS);CHECK(closes==0);
  submit_result=STATUS_SUCCESS;CHECK(client.open_keyboard_mouse()==ERROR_SUCCESS);
  forced_reply_error=ERROR_BUSY;
  CHECK(client.submit_keyboard_mouse(key)==ERROR_NOT_READY); // Transport busy cannot authorize a retry.
  forced_reply_error=ERROR_INVALID_PARAMETER;
  CHECK(client.submit_keyboard_mouse(key)==ERROR_INVALID_PARAMETER);
  CHECK(lvg::km::classify_submission(ERROR_INVALID_PARAMETER)==lvg::km::submission_outcome::uncertain);
  CHECK(f.context.keyboard_mouse.core.lifecycle()==keyboard_mouse_endpoint::state::active);
  forced_reply_error=ERROR_NOT_READY;
  CHECK(client.abort_keyboard_mouse()==ERROR_NOT_READY); // Server removed it; client still cannot prove removal.
  CHECK(f.context.keyboard_mouse.state==slot_state::empty);
  forced_reply_error=0;CHECK(client.abort_keyboard_mouse()==ERROR_SUCCESS);
  client.close();CHECK(closes==1);current=nullptr;
}
} // namespace

NTSTATUS VhfCreate(VHF_CONFIG *config,VHFHANDLE *out) {
  ++creates;
  const bool keyboard_mouse=config->ReportDescriptor==lvg::driver::keyboard_mouse_descriptor;
  if(keyboard_mouse) {
    auto *callback=static_cast<keyboard_mouse_callback_context *>(config->VhfClientContext);
    auto *slot=callback->slot;
    CHECK(fake_rundown_depth==0);
    CHECK(!fake_holds(slot->parent->state_lock));CHECK(fake_holds(slot->parent->lifetime_gate));
    CHECK(config->FileHandle!=nullptr);CHECK(config->VendorID==LVG_KM_VENDOR_ID);CHECK(config->ProductID==LVG_KM_PRODUCT_ID);
    CHECK(config->ReportDescriptor==lvg::driver::keyboard_mouse_descriptor);
  } else {
    auto *slot=static_cast<controller_slot *>(config->VhfClientContext);
    CHECK(fake_rundown_depth==0);
    CHECK(!fake_holds(slot->parent->state_lock));CHECK(fake_holds(slot->parent->lifetime_gate));
    CHECK(config->FileHandle==slot->parent->vhf_file_handle);
    CHECK(config->ReportDescriptor!=nullptr);CHECK(config->VendorID!=0);CHECK(config->ProductID!=0);
    CHECK(slot->state==slot_state::starting);CHECK(slot->vhf==nullptr);
    CHECK(config->EvtVhfReadyForNextReadReport==evt_vhf_ready_for_next_report);
  }
  if(!NT_SUCCESS(create_result))return create_result;
  handles.push_back(std::make_unique<fake_vhf>());*out=handles.back().get();(*out)->config=*config;
  (*out)->kind=keyboard_mouse?fake_vhf_kind::keyboard_mouse:fake_vhf_kind::controller;
  if(on_create)on_create(*out);
  return STATUS_SUCCESS;
}
NTSTATUS VhfStart(VHFHANDLE h) {
  ++starts;
  boundary(h);
  CHECK(fake_holds(context_of(h).lifetime_gate));
  if(h->kind==fake_vhf_kind::keyboard_mouse && on_start)on_start(h);
  return start_result;
}
void VhfDelete(VHFHANDLE h,BOOLEAN wait) {
  ++deletes;boundary(h);CHECK(wait);CHECK(!h->deleted);CHECK(fake_holds(context_of(h).lifetime_gate));
  if(h->kind==fake_vhf_kind::keyboard_mouse) {
    auto &slot=slot_of(h);auto *const context=slot.parent;
    CHECK(slot.state==slot_state::stopping);CHECK(slot.vhf==nullptr);
    CHECK(slot.pump_active==false);CHECK(slot.submit_owners==0);
    CHECK(InterlockedCompareExchange(&callback_of(h).retired,0,0)!=0);
    if(observe_mixed_teardown && h==mixed_keyboard_mouse_handle) {
      CHECK(context->stopping);
      CHECK(context->vhf_file_handle!=nullptr);
      CHECK(context->vhf_target_open);
      CHECK(context->local_vhf_target!=nullptr);
      CHECK(context->controllers[0].state==slot_state::stopping);
      CHECK(context->controllers[0].vhf==mixed_controller_handle);
      CHECK(context->controllers[0].owner==mixed_controller_owner);
      CHECK(mixed_controller_handle!=nullptr && !mixed_controller_handle->deleted);
      CHECK(delete_order.empty());
    }
    delete_order.push_back(fake_vhf_kind::keyboard_mouse);
  } else {
    auto &slot=controller_slot_of(h);auto *const context=slot.parent;
    CHECK(slot.state==slot_state::stopping);CHECK(slot.vhf==nullptr);
    if(observe_mixed_teardown && h==mixed_controller_handle) {
      CHECK(context->stopping);
      CHECK(context->vhf_file_handle==nullptr);
      CHECK(context->keyboard_mouse.state==slot_state::empty);
      CHECK(context->keyboard_mouse.vhf==nullptr);
      CHECK(context->keyboard_mouse.callback_context==nullptr);
      CHECK(context->keyboard_mouse.submit_owners==0);
      CHECK(context->keyboard_mouse.pump_active==false);
      CHECK(mixed_keyboard_mouse_handle!=nullptr && mixed_keyboard_mouse_handle->deleted);
      CHECK(delete_order.size()==1);
      if(delete_order.size()==1)CHECK(delete_order.front()==fake_vhf_kind::keyboard_mouse);
    }
    delete_order.push_back(fake_vhf_kind::controller);
    if(on_controller_delete)on_controller_delete(h);
  }
  h->deleting=true;retained_unchanged(h);
  if(on_delete)on_delete(h);
  // Late readiness during delete must not mutate retained bytes or submit.
  const int submit_count=submits.load();
  h->config.EvtVhfReadyForNextReadReport(h->config.VhfClientContext);retained_unchanged(h);
  CHECK(submits==submit_count);
  h->config.EvtVhfCleanup(h->config.VhfClientContext);h->retained=nullptr;h->deleted=true;h->deleting=false;
}
NTSTATUS VhfReadReportSubmit(VHFHANDLE h,PHID_XFER_PACKET packet) {
  ++submits;boundary(h);CHECK(h->kind==fake_vhf_kind::keyboard_mouse);CHECK(!h->deleted && !h->deleting);CHECK(h->credit);h->credit=false;
  CHECK(slot_of(h).pump_active);CHECK(slot_of(h).submit_owners==1);
  ++h->submit_depth;h->max_submit_depth=std::max(h->max_submit_depth,h->submit_depth);
  CHECK(packet->reportBuffer!=nullptr);CHECK(packet->reportBufferLen<=30);
  h->retained=packet;h->packet=*packet;h->bytes.assign(packet->reportBuffer,packet->reportBuffer+packet->reportBufferLen);
  if(on_submit)on_submit(h);
  --h->submit_depth;
  return submit_result;
}
NTSTATUS VhfAsyncOperationComplete(VHFOPERATIONHANDLE op,NTSTATUS status) {++op->completions;op->status=status;return STATUS_SUCCESS;}
DWORD GetLastError(){return last_error;}
void *LocalAlloc(DWORD,size_t bytes){return std::calloc(1,bytes);}
void *LocalFree(void *p){std::free(p);return nullptr;}
HDEVINFO SetupDiGetClassDevsW(const GUID *,const wchar_t *,HANDLE,DWORD){return current;}
BOOL SetupDiEnumDeviceInterfaces(HDEVINFO,void *,const GUID *,DWORD index,SP_DEVICE_INTERFACE_DATA *) {
  if(index==0)return TRUE;
  last_error=ERROR_NO_MORE_ITEMS;return FALSE;
}
BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO,SP_DEVICE_INTERFACE_DATA *,SP_DEVICE_INTERFACE_DETAIL_DATA_W *out,DWORD,DWORD *bytes,void *) {
  if(!out){*bytes=sizeof(*out)+sizeof(wchar_t);last_error=ERROR_INSUFFICIENT_BUFFER;return FALSE;}
  out->DevicePath[0]=L'x';return TRUE;
}
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO){return TRUE;}
HANDLE CreateFileW(const wchar_t *,DWORD access,DWORD sharing,void *,DWORD disposition,DWORD flags,HANDLE) {
  CHECK(current!=nullptr);CHECK(access==(GENERIC_READ|GENERIC_WRITE));CHECK(sharing==0);CHECK(disposition==OPEN_EXISTING);
  CHECK(flags == (FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | SECURITY_CONTEXT_TRACKING));
  ++opens;fake_wdf_object request;evt_file_create(&current->device,&request,&current->owner);
  return &current->owner;
}
BOOL CloseHandle(HANDLE handle){if(handle==fake_token_handle())return TRUE;++closes;evt_file_close(static_cast<WDFFILEOBJECT>(handle));return TRUE;}
BOOL DeviceIoControl(HANDLE handle,DWORD code,void *input,DWORD input_size,void *output,DWORD output_size,DWORD *bytes,void *) {
  ++ioctls;ULONG_PTR information=0;
  const auto status=current->raw(code,input,input_size,input_size,output,output_size,output_size,static_cast<WDFFILEOBJECT>(handle),&information);
  *bytes=static_cast<DWORD>(information);last_error=win_error(status);
  if(NT_SUCCESS(status) && on_reply)on_reply(code,output,bytes);
  if(forced_reply_error!=ERROR_SUCCESS){last_error=forced_reply_error;return FALSE;}
  return NT_SUCCESS(status)?TRUE:FALSE;
}
int main(int argc,char **argv){if(argc==2 && std::strcmp(argv[1],"--retirement-only")==0){test_power_retirement_minimal();}else if(argc==2 && std::strcmp(argv[1],"--power-only")==0){test_power_contracts();}else{test_keyboard_mouse_authorization();test_power_contracts();test_raw_boundaries();test_fifo_and_drain();test_consumer_transport();test_absolute_mouse_drag_transport();test_failures_and_forced_delete();test_cleanup_during_start();test_submit_rundown_and_late_delete_callback();test_rundown_wait_failure_fallback_drains();test_mixed_keyboard_mouse_and_controller_teardown_order();test_submission_reply_validation();test_client_chain();}std::printf("KM actual driver/client transport: %d checks, %d failures\n",checks.load(),failures.load());return failures==0?0:1;}
