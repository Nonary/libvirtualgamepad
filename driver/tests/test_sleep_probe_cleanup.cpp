// SPDX-License-Identifier: MIT
// Execute the actual probe main against inert OS/client boundaries. Never
// changes privileges, registers OS callbacks, opens a device or suspends a PC.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <set>
#include "scope_exit.h"
#include "sleep_churn_progress.h"
using DWORD = unsigned long;
using LONGLONG = long long;
using BOOLEAN = bool;
using HANDLE = void *;
using HPOWERNOTIFY = HANDLE;
constexpr bool TRUE = true, FALSE = false;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_DEVICE_REMOVED = 2;
constexpr int FILE_APPEND_DATA=0, FILE_SHARE_READ=0, OPEN_ALWAYS=0, FILE_FLAG_WRITE_THROUGH=0, DEVICE_NOTIFY_CALLBACK=0, XINPUT_GAMEPAD_A=1;
const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(-1);
using steady = std::chrono::steady_clock;
enum class fault {none, connect, create, registration, privilege, timer_create, timer_arm, suspend};
fault injected {};
std::set<HANDLE> live_handles;
int failures=0, cancelled=0, registrations=0, destroyed=0;
std::atomic<int> workers {0};
HANDLE log_file=nullptr;
HANDLE allocate() { static std::uintptr_t next=1; auto h=reinterpret_cast<HANDLE>(next++); live_handles.insert(h); return h; }
void require(bool b) { if (!b) std::abort(); }
void CloseHandle(HANDLE h) { require(live_handles.erase(h)==1); }
HANDLE CreateFileA(const char*,int,int,void*,int,int,void*) { return allocate(); }
void Sleep(int) { std::this_thread::yield(); }
void line(const char*,const char*,...) {}
void check(bool ok,const char*,const char* = nullptr) { if (!ok) ++failures; }
bool console_locked() { return false; }
double ms_since(steady::time_point) { return 0; }
namespace lvg {
enum class profile {xbox_series};
constexpr unsigned south=1;
struct client {
 DWORD connect() { return injected==fault::connect?1:0; }
 DWORD create_controller(unsigned,profile) { return injected==fault::create?1:0; }
 DWORD submit_input_state(int) { return ERROR_DEVICE_REMOVED; }
 DWORD destroy_controller(unsigned) { ++destroyed; return 0; }
};
}
constexpr lvg::profile k_long_lived[] {lvg::profile::xbox_series};
struct run_state {
 bool aware=false; unsigned pads=0; std::mutex pads_lock;
 lvg::client client; std::thread churn; std::atomic<bool> churn_stop {false}, churn_paused {false};
 sleep_churn_progress churn_progress; std::map<DWORD,long> churn_errors;
 std::atomic<int> suspends {1}, resumes {1};
};
void churn(run_state *run) { ++workers; while (!run->churn_stop) std::this_thread::yield(); --workers; }
DWORD create_long_lived(run_state &run) { return run.client.create_controller(0,lvg::profile::xbox_series); }
int on_power=0;
struct DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS { int callback; run_state *state; };
DWORD PowerRegisterSuspendResumeNotification(int,DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS*,HPOWERNOTIFY *out) {
 if (injected==fault::registration) return 1;
 *out=allocate(); ++registrations; return 0;
}
void PowerUnregisterSuspendResumeNotification(HPOWERNOTIFY h) { CloseHandle(h); --registrations; }
bool enable_shutdown_privilege() { return injected!=fault::privilege; }
struct LARGE_INTEGER { LONGLONG QuadPart; };
HANDLE CreateWaitableTimerW(void*,bool,void*) { return injected==fault::timer_create?nullptr:allocate(); }
bool SetWaitableTimer(HANDLE,LARGE_INTEGER*,int,void*,void*,bool) { return injected!=fault::timer_arm; }
void CancelWaitableTimer(HANDLE h) { require(live_handles.count(h)==1); ++cancelled; }
BOOLEAN SetSuspendState(bool,bool,bool) { return injected!=fault::suspend; }
template<class F> bool wait_for(int,F f) { return f(); }
int input(unsigned,unsigned=0,int=0) { return 0; }
bool hid_child_reads(lvg::client&,unsigned) { return true; }
DWORD xinput_mask() { return 0; }
int new_xinput_slot(DWORD) { return 0; }
struct XINPUT_STATE { struct { unsigned wButtons=XINPUT_GAMEPAD_A; int sThumbLX=32767; } Gamepad; };
DWORD XInputGetState(int,XINPUT_STATE*) { return 0; }

// Generated from probe_sleep.cpp, not a duplicate of its exit/cleanup logic.
#include "sleep_probe_main.h"

int main() {
 int cases=0;
 for (auto mode : {"churn","aware","idle","none","awake"}) {
  for (auto f : {fault::none,fault::connect,fault::create,fault::registration,fault::privilege,fault::timer_create,fault::timer_arm,fault::suspend}) {
   injected=f; failures=cancelled=registrations=destroyed=0;
   char program[]="inert-probe", delay[]="45", log[]="inert-log";
   char *args[] {program,const_cast<char*>(mode),delay,log};
   (void)probe_main(4,args);
   require(live_handles.empty()); require(registrations==0); require(workers==0); require(log_file==nullptr);
   if (std::string(mode)!="awake" && (f==fault::timer_arm || f==fault::suspend || f==fault::none)) require(cancelled==1);
   ++cases;
  }
 }
 std::printf("Actual sleep probe main: %d inert exit scenarios passed\n",cases);
}
