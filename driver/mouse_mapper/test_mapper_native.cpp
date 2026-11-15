// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#define LVG_MOUSE_MAPPER_TEST 1
#define LVG_KM_VENDOR_ID 65535
#define LVG_KM_PRODUCT_ID 65534
#include "mapper.cpp"
#include <cstdlib>
#include <iostream>
#include <future>
#include <thread>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (0)
static unsigned allowance=3;
static std::vector<MOUSE_INPUT_DATA> delivered;
static void class_callback(object*,MOUSE_INPUT_DATA *first,MOUSE_INPUT_DATA *last,ULONG *consumed) {
  CHECK(native_irql==2);
  const auto count=static_cast<unsigned>(last-first);
  *consumed=std::min(allowance,count);
  delivered.insert(delivered.end(),first,first+*consumed);
}
// Model the documented exclusive-create gate and automatic forwarding at the
// WDF boundary. Rejected second opens never enter the driver's callback.
static void native_open_file(object *device, object *request, object *file) {
  if(device->exclusive && device->open_count) {request->status=STATUS_SHARING_VIOLATION;return;}
  ++device->open_count;file->kind=100;
  reinterpret_cast<EVT_WDF_DEVICE_FILE_CREATE*>(device->create_callback)(device,request,file);
}
static void native_close_file(object *file) {
  if(file->kind!=100)return;
  auto *device=file->parent;
  reinterpret_cast<EVT_WDF_FILE_CLEANUP*>(device->cleanup_callback)(file);
  if(device->auto_forward) {++device->lower->lower_cleanups;++device->lower->lower_closes;}
  CHECK(device->open_count);--device->open_count;file->kind=102;
}
struct fixture {
 object *device{},*file{};
 device_context *c{};
 fixture(bool connect=true,bool open=true) {
   CHECK(add_device(nullptr,new device_init{})==0);
   device=native_last_device->parent; // add_device's last device is raw child.
   CHECK(device && device->child==native_last_device);
   c=context(device);
   CHECK(initialize(device)==0);
   if(connect) {
     CONNECT_DATA data{make_object(),reinterpret_cast<void*>(class_callback)};
     auto *r=make_object();r->buffer=&data;r->size=sizeof(data);
     internal_control(device->queue,r,0,sizeof(data),IOCTL_INTERNAL_MOUSE_CONNECT);
     CHECK(r->status==0);
   }
   if(open) open_file();
 }
 void open_file(bool run=true) {
   file=make_object(device);
   auto *r=make_object();
   native_open_file(device,r,file);
   CHECK(r->status==0);CHECK(c->reader_open);CHECK(c->validated);
   CHECK(c->reader->file==file);CHECK(c->reader->target==device->lower);
   if(run)native_run_dpcs();
 }
 void input(lvg::mouse_transport::report r) {
   CHECK(c->read->pending);
   std::memcpy(c->read->buffer,&r,sizeof(r));
   native_complete(c->read,0,sizeof(r));
   native_run_dpcs();
 }
 ~fixture() {cleanup(device);if(file)native_close_file(file);}
};
static lvg::mouse_transport::report report_for(std::uint32_t s) {return {2,1,0,0,0,0,0,0,0,s};}
int main() {
 {
  fixture f(false,false);
  CHECK(native_reads==0);CHECK(!f.c->active);CHECK(f.c->power_on);
  lvg::mouse_mapper::status status{};
  auto *q=make_object();q->buffer=&status;q->size=sizeof(status);
  status_control(f.device->child->queue,q,sizeof(status),0,lvg::mouse_mapper::k_ioctl_status);
  CHECK(q->status==0);CHECK(status.ready_flags!=7);CHECK(q->information==28);
 }
 {
  auto a=report_for(1);a.x=4;auto b=report_for(2);b.x=-9;
  native_inline_reads={a,b};const auto n=delivered.size();const auto t=native_timers;
  fixture f;CHECK(delivered.size()==n+2);CHECK(delivered[n].LastX==4);CHECK(delivered[n+1].LastX==-9);
  CHECK(f.c->read->pending);CHECK(native_timers==t);CHECK(native_acks.back().sequence==2);
 }
 const auto reads_before=native_reads;
 {
  fixture f;CHECK(native_reads==reads_before+1);
  auto r=report_for(1);r.buttons=1;r.x=7;r.wheel=-120;r.pan=240;
  allowance=1;const auto d=delivered.size();const auto timers=native_timers;const auto writes=native_writes;
  f.input(r);
  CHECK(delivered.size()==d+1);CHECK(delivered.back().ButtonFlags==1);CHECK(delivered.back().LastX==7);
  CHECK(f.c->core.remaining()==2);CHECK(!f.c->read->pending);CHECK(native_writes==writes);
  CHECK(native_timers==timers+1);
  allowance=0;native_fire_timer(f.c->timer);CHECK(f.c->core.remaining()==2);CHECK(native_writes==writes);
  allowance=1;native_fire_timer(f.c->timer);CHECK(f.c->core.remaining()==1);
  CHECK(delivered.back().ButtonFlags==0x400);CHECK(static_cast<std::int16_t>(delivered.back().ButtonData)==-120);
  native_inline_ack=false;native_fire_timer(f.c->timer);
  CHECK(delivered.back().ButtonFlags==0x800);CHECK(delivered.back().ButtonData==240);
  CHECK(f.c->ack_request->pending);CHECK(!f.c->read->pending);CHECK(native_writes==writes+1);
  CHECK(native_acks.back().sequence==1);CHECK(native_acks.back().report_id==5);
  native_complete(f.c->ack_request,0,8);native_run_dpcs();CHECK(f.c->read->pending);
  native_inline_ack=true;allowance=3;
  const auto t=native_timers;
  r=report_for(65);r.flags=lvg::mouse_transport::k_purge; // Explicit close marker explains purge gap.
  f.input(r);CHECK(delivered.back().ButtonFlags==2);CHECK(delivered.back().Flags==0);CHECK(native_acks.back().sequence==65);
  CHECK(f.c->read->pending);CHECK(native_timers==t); // no fixed normal-path millisecond.
  r=report_for(66);r.flags=3;r.x=65535;r.y=1;f.input(r);
  CHECK(delivered.back().Flags==3);CHECK(delivered.back().LastX==65535);
  r=report_for(67);r.x=-20;f.input(r);CHECK(delivered.back().Flags==0);CHECK(delivered.back().LastX==-20);
  // Raw sideband is readonly, immediately completes, and reports exact ready.
  lvg::mouse_mapper::status status{};auto *q=make_object();q->buffer=&status;q->size=sizeof(status);
  status_control(f.device->child->queue,q,28,0,lvg::mouse_mapper::k_ioctl_status);
  CHECK(lvg::mouse_mapper::valid_ready(status,q->information,lvg::mouse_mapper::k_container_guid));
  status_control(f.device->child->queue,q,28,0,0xdead);CHECK(q->status==STATUS_INVALID_DEVICE_REQUEST);
  auto *attr=make_object();MOUSE_ATTRIBUTES mouse{};attr->buffer=&mouse;attr->size=sizeof(mouse);
  internal_control(f.device->queue,attr,sizeof(mouse),0,IOCTL_MOUSE_QUERY_ATTRIBUTES);
  CHECK(attr->status==0);CHECK(mouse.NumberOfButtons==5);
  // Cancel/read rundown always orders timer -> DPC -> lower target.
  native_rundown.clear();CHECK(suspend(f.device)==0);CHECK(!f.c->pending);CHECK(!f.c->active);
  CHECK(native_rundown==std::vector<int>({1,2,4,3,4}));CHECK(restart(f.device)==0);native_run_dpcs();CHECK(f.c->read->pending);
 }
 {
  fixture f;auto r=report_for(1);r.x=1;
  // Suspend between successful completion and its queued DPC retains input.
  std::memcpy(f.c->read->buffer,&r,22);native_complete(f.c->read,0,22);
  const auto n=delivered.size();CHECK(suspend(f.device)==0);CHECK(f.c->core.remaining()==1);
  CHECK(delivered.size()==n);CHECK(restart(f.device)==0);native_run_dpcs();CHECK(delivered.size()==n+1);
  CHECK(native_acks.back().sequence==1);CHECK(f.c->read->pending);
 }
 {
  fixture f;auto r=report_for(1);r.x=1;native_inline_ack=false;f.input(r);
  CHECK(f.c->ack_request->pending);CHECK(suspend(f.device)==0);CHECK(f.c->terminal);
  CHECK(restart(f.device)==STATUS_DEVICE_NOT_READY);native_inline_ack=true;
 }
 {
  fixture f;const auto n=delivered.size();const auto w=native_writes;
  f.input(report_for(1));CHECK(delivered.size()==n);CHECK(native_writes==w+1);CHECK(f.c->read->pending);
  f.input(report_for(1));CHECK(f.c->terminal);CHECK(f.device->failed);CHECK(native_writes==w+1);
 }
 {
  fixture f;auto r=report_for(1);r.x=1;f.input(r);const auto w=native_writes;
  r=report_for(3);r.x=2;f.input(r);CHECK(f.c->terminal);CHECK(native_writes==w);CHECK(f.device->failed);
 }
 {
  fixture f;const auto n=delivered.size();auto r=report_for(7);r.flags=lvg::mouse_transport::k_purge;
  f.input(r);CHECK(delivered.size()==n);CHECK(native_acks.back().sequence==7);CHECK(f.c->read->pending);
 }
 {
  fixture f;auto r=report_for(1);r.buttons=31;r.flags=3;r.x=65535;f.input(r);
  r=report_for(65);r.flags=lvg::mouse_transport::k_purge;f.input(r);
  CHECK(delivered.back().Flags==0);CHECK(delivered.back().ButtonFlags==0x2aa);
  CHECK(delivered.back().LastX==0&&delivered.back().LastY==0);CHECK(native_acks.back().sequence==65);
 }
 for(unsigned field=0;field<6;++field) {
  fixture f;auto r=report_for(1);r.flags=lvg::mouse_transport::k_purge;
  switch(field) {
   case 0:r.buttons=1;break;case 1:r.x=1;break;case 2:r.y=-1;break;
   case 3:r.wheel=120;break;case 4:r.pan=-120;break;case 5:r.flags=5;break;
  }
  const auto w=native_writes;f.input(r);CHECK(f.c->terminal);CHECK(f.device->failed);CHECK(native_writes==w);
 }
 {
  fixture f;auto r=report_for(1);r.flags=1;const auto w=native_writes;f.input(r);
  CHECK(f.c->terminal);CHECK(f.device->failed);CHECK(native_writes==w);CHECK(!f.c->read->pending);
 }
 {
  fixture f;auto r=report_for(1);r.x=1;native_fail_write=true;f.input(r);
  CHECK(f.c->terminal);CHECK(f.device->failed);CHECK(!f.c->read->pending);native_fail_write=false;
 }
 {
  fixture f;auto *read=f.c->read;auto *ack=f.c->ack_request;auto *file=f.file;
  native_close_file(file);CHECK(read->deleted&&ack->deleted);CHECK(file->refs==0);CHECK(!f.c->reader_open);
 }
 // Stop is allowed to miss a completed request whose callback has not
 // entered yet. The per-send rundown must keep its request/FileObject alive.
 {
  fixture f;auto r=report_for(1);r.x=11;
  native_defer_completion=true;
  std::memcpy(f.c->read->buffer,&r,22);native_complete(f.c->read,0,22);
  native_defer_completion=false;
  CHECK(!f.c->read->pending);CHECK(f.c->io_callbacks.refs==1);
  bool waited=false;
  native_wait_hook=[&](EX_RUNDOWN_REF *domain) {
    CHECK(domain==&f.c->io_callbacks);CHECK(!f.c->active);CHECK(f.file->refs==1);
    CHECK(!f.c->read->deleted);CHECK(!f.c->ack_request->deleted);waited=true;
    native_flush_deferred();
  };
  CHECK(suspend(f.device)==0);native_wait_hook={};
  CHECK(waited);CHECK(native_deferred.empty());CHECK(f.c->core.remaining()==1);
  CHECK(f.c->io_callbacks.refs==0);CHECK(restart(f.device)==0);native_run_dpcs();
  CHECK(native_acks.back().sequence==1);CHECK(f.c->read->pending);
 }
 // Successful ACK completion removed from the target list is equally
 // protected until its delayed callback exits; resume must not replay it.
 {
  fixture f;native_inline_ack=false;auto r=report_for(1);r.x=1;f.input(r);
  CHECK(f.c->ack_request->pending);const auto writes=native_writes;
  native_defer_completion=true;native_complete(f.c->ack_request,0,8);native_defer_completion=false;
  CHECK(!f.c->ack_request->pending);CHECK(f.c->io_callbacks.refs==1);
  CHECK(suspend(f.device)==0);CHECK(native_deferred.empty());CHECK(!f.c->terminal);
  CHECK(restart(f.device)==0);native_run_dpcs();CHECK(f.c->read->pending);CHECK(native_writes==writes);
  native_inline_ack=true;
 }
 // A real second thread pauses the owning DPC after its I/O reservation but
 // before formatting. Cancellation must wait for that send window to close.
 {
  fixture f(true,false);std::promise<void> entered,resume;auto entered_future=entered.get_future();auto resume_future=resume.get_future();
  native_format_hook=[&](object *request){CHECK(request==f.c->read);entered.set_value();resume_future.wait();};
  f.open_file(false);CHECK(f.c->dpc->queued);f.c->dpc->queued=false;
  const auto reads=native_reads;
  std::thread sender([&]{pump_dpc(f.c->dpc);});
  entered_future.wait();CHECK(f.c->pump_owner);CHECK(f.c->io_callbacks.refs==1);
  CHECK(ExAcquireRundownProtection(&f.c->pump_callbacks));pump_dpc(f.c->dpc);
  CHECK(native_reads==reads);CHECK(f.c->pump_owner);CHECK(!f.c->read->pending);
  bool waited=false;native_rundown.clear();
  native_wait_hook=[&](EX_RUNDOWN_REF *domain){
    CHECK(domain==&f.c->pump_callbacks);CHECK(!f.c->active);CHECK(!f.c->read->deleted);CHECK(f.file->refs==1);
    CHECK(std::find(native_rundown.begin(),native_rundown.end(),3)==native_rundown.end());
    waited=true;native_wait_hook={};resume.set_value();
  };
  CHECK(suspend(f.device)==0);sender.join();native_format_hook={};native_wait_hook={};
  CHECK(waited);CHECK(native_reads==reads+1);CHECK(!f.c->pending);CHECK(!f.c->read->pending);
  CHECK(f.c->pump_callbacks.refs==0&&f.c->io_callbacks.refs==0);
  CHECK(restart(f.device)==0);native_run_dpcs();CHECK(f.c->read->pending);
 }
 // A second processor enters the same DPC while the owner has reserved a
 // read but has not formatted/sent it. It may only set the rerun latch.
 {
  fixture f(true,false);bool overlapped=false;
  native_format_hook=[&](object *request) {
    if(overlapped)return;
    overlapped=true;CHECK(request==f.c->read);CHECK(f.c->pump_owner);
    const auto reads=native_reads;
    CHECK(ExAcquireRundownProtection(&f.c->pump_callbacks));
    pump_dpc(f.c->dpc);
    CHECK(f.c->pump_owner);CHECK(native_reads==reads);CHECK(!request->pending);
  };
  f.open_file();native_format_hook={};CHECK(overlapped);CHECK(f.c->read->pending);
 }
 // Cancel may remove a queued instance while another invocation has already
 // been dispatched. Explicit pump rundown must wait for that entry too.
 {
  fixture f;
  CHECK(ExAcquireRundownProtection(&f.c->pump_callbacks)); // dispatched entry
  CHECK(ExAcquireRundownProtection(&f.c->pump_callbacks)); // queued entry
  f.c->dpc->queued=true;
  bool waited=false;
  native_wait_hook=[&](EX_RUNDOWN_REF *domain) {
    CHECK(domain==&f.c->pump_callbacks);CHECK(!f.c->active);CHECK(!f.c->dpc->queued);
    CHECK(f.c->read->pending);CHECK(f.file->refs==1);waited=true;
    pump_dpc(f.c->dpc); // delayed dispatched entry observes inactive and exits.
  };
  CHECK(suspend(f.device)==0);native_wait_hook={};CHECK(waited);
  CHECK(f.c->pump_callbacks.refs==0);CHECK(f.c->io_callbacks.refs==0);
 }
 // Expired retry timer with a dispatched callback also retains its arm token
 // even when WdfTimerStop reports that no timer remains in the timer queue.
 {
  fixture f;auto r=report_for(1);r.x=2;allowance=0;f.input(r);allowance=3;
  CHECK(f.c->timer_armed);f.c->timer->scheduled=false;
  bool waited=false;
  native_wait_hook=[&](EX_RUNDOWN_REF *domain) {
    CHECK(domain==&f.c->pump_callbacks);CHECK(!f.c->active);waited=true;
    retry_timer(f.c->timer);
  };
  CHECK(suspend(f.device)==0);native_wait_hook={};CHECK(waited);CHECK(!f.c->timer_armed);
  CHECK(restart(f.device)==0);native_run_dpcs();CHECK(native_acks.back().sequence==1);
 }
 // Inline chains are bounded per owner turn and continue with another DPC.
 {
  const auto runs=native_dpc_runs;
  for(unsigned i=1;i<=40;++i){auto r=report_for(i);r.x=1;native_inline_reads.push_back(r);}
  fixture f;CHECK(native_inline_reads.empty());CHECK(native_acks.back().sequence==40);
  CHECK(native_dpc_runs>runs+1);CHECK(f.c->read->pending);
 }
 // Framework exclusivity rejects a second open before invoking CREATE;
 // every CREATE actually received by the driver reaches lower HIDClass.
 {
  fixture f;CHECK(f.device->exclusive&&f.device->auto_forward);
  const auto creates=f.device->lower->lower_creates;
  auto *r=make_object();auto *second=make_object(f.device);r->mode=UserMode;
  native_open_file(f.device,r,second);CHECK(r->status==STATUS_SHARING_VIOLATION);
  CHECK(f.device->lower->lower_creates==creates);CHECK(second->kind!=100);
  native_close_file(f.file);
  CHECK(f.device->lower->lower_cleanups==creates);CHECK(f.device->lower->lower_closes==creates);
 }
 {
  fixture f(true,false);auto *r=make_object();auto *file=make_object(f.device);r->mode=UserMode;
  native_lower_create_status=STATUS_ACCESS_DENIED;native_open_file(f.device,r,file);
  native_lower_create_status=0;CHECK(r->status==STATUS_ACCESS_DENIED);
  CHECK(f.device->lower->lower_creates==1);CHECK(!f.c->reader_open);
  native_close_file(file);CHECK(f.device->lower->lower_cleanups==1&&f.device->lower->lower_closes==1);
 }
 {
  fixture f(true,false);auto *r=make_object();auto *file=make_object(f.device);
  native_fail_request_create=true;native_open_file(f.device,r,file);native_fail_request_create=false;
  CHECK(r->status==STATUS_SUCCESS);CHECK(f.device->failed);CHECK(!f.c->reader_open);
  CHECK(file->refs==0);native_close_file(file);
  CHECK(f.device->lower->lower_creates==1&&f.device->lower->lower_cleanups==1&&f.device->lower->lower_closes==1);
 }
 std::cout << "native mapper callback/ACK/rundown contract passed\n";
}
