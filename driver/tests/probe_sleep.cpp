// SPDX-License-Identifier: MIT
//
// Puts this PC to sleep with controllers on the installed driver, wakes it with
// a timer, and checks that every controller lost to the power-down reports
// ERROR_DEVICE_REMOVED, can be created again, and delivers input afterwards.
// Uses slots 0-4 and destroys them before exiting. Never installs drivers.
//
//   probe_sleep <awake|none|idle|churn|aware> [wake-seconds] [log-file]
//
//   awake  idle's controllers and checks without sleeping: a baseline for the
//          probe itself
//   none   no controllers: the driver is installed and idle
//   idle   Xbox Series, DualSense, DualShock 4, and Switch Pro, left alone
//   churn  idle, plus a second handle creating and destroying slot 4 for the
//          whole sleep
//   aware  churn, plus what a careful host does: destroy on PBT_APMSUSPEND
//          and create again after resume
//
// Run elevated (SetSuspendState needs SeShutdownPrivilege) on a machine with S3
// and wake timers enabled. With a driver that keeps controllers alive across
// D0Exit, the sleep never completes and the machine bugchecks with 0x9F about
// five minutes later; every line is written through to the log file first, so
// the record survives.
#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <powrprof.h>
#include <setupapi.h>
#include <wtsapi32.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <functional>
#include <map>
#include <mutex>
#include <iterator>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "libvirtualgamepad/client.h"

namespace {

using steady = std::chrono::steady_clock;

int failures = 0;
HANDLE log_file = nullptr;
std::mutex print_lock;
const steady::time_point started = steady::now();

double seconds() {
  return std::chrono::duration<double>(steady::now() - started).count();
}

double ms_since(const steady::time_point from) {
  return std::chrono::duration<double, std::milli>(steady::now() - from).count();
}

void line(const char *tag, const char *format, ...) {
  char text[1024];
  int length = std::snprintf(text, sizeof(text), "%s t=%.3f ", tag, seconds());
  va_list args;
  va_start(args, format);
  const int body = std::vsnprintf(text + length, sizeof(text) - length - 2, format, args);
  va_end(args);
  length = body < 0 ? length : std::min<int>(length + body, sizeof(text) - 2);
  text[length++] = '\n';
  text[length] = '\0';
  std::lock_guard lock {print_lock};
  std::fputs(text, stdout);
  std::fflush(stdout);
  if (log_file != nullptr) {
    DWORD written = 0;
    WriteFile(log_file, text, static_cast<DWORD>(length), &written, nullptr);
  }
}

void check(const bool ok, const char *label, const char *detail = "") {
  line(ok ? "PASS" : "FAIL", "%s %s", label, detail);
  if (!ok) ++failures;
}

bool wait_for(const int timeout_ms, const std::function<bool()> &done) {
  const auto start = steady::now();
  while (!done()) {
    if (ms_since(start) > timeout_ms) return false;
    Sleep(5);
  }
  return true;
}

// XInput reads zeros for every process while the console session is locked,
// which is how a machine usually wakes.
bool console_locked() {
  const DWORD session = WTSGetActiveConsoleSessionId();
  LPWSTR buffer = nullptr;
  DWORD bytes = 0;
  bool locked = false;
  if (session != 0xFFFFFFFF &&
      WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &bytes) &&
      buffer != nullptr) {
    const auto *info = reinterpret_cast<const WTSINFOEXW *>(buffer);
    locked = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    WTSFreeMemory(buffer);
  }
  return locked;
}

lvg::input_state_request input(const std::uint32_t id, const std::uint32_t buttons = 0, const std::int16_t left_x = 0) {
  lvg::input_state_request request {};
  request.header.size = sizeof(request);
  request.header.version = lvg::k_protocol_version;
  request.controller_id = id;
  request.buttons = buttons;
  request.left_x = left_x;
  return request;
}

// Opens the HID interface of controller `id`. VHF's child carries the instance
// ID ("...&VibeshineGamepad<id>"); the interface belongs to the HID collection
// HIDClass creates beneath it.
HANDLE open_hid_child(const std::uint32_t id) {
  wchar_t want[32];
  std::swprintf(want, 32, L"VibeshineGamepad%u", id);
  GUID guid;
  HidD_GetHidGuid(&guid);
  HANDLE result = INVALID_HANDLE_VALUE;
  const HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return result;
  for (DWORD index = 0; result == INVALID_HANDLE_VALUE; ++index) {
    SP_DEVICE_INTERFACE_DATA item {};
    item.cbSize = sizeof(item);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &item)) break;
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
    if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
    std::vector<unsigned char> storage(size);
    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
    detail->cbSize = sizeof(*detail);
    SP_DEVINFO_DATA device {};
    device.cbSize = sizeof(device);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, &device)) continue;
    DEVINST parent = 0;
    wchar_t instance[MAX_DEVICE_ID_LEN] {};
    if (CM_Get_Parent(&parent, device.DevInst, 0) != CR_SUCCESS ||
        CM_Get_Device_IDW(parent, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) continue;
    const wchar_t *const suffix = std::wcsrchr(instance, L'&');
    if (suffix == nullptr || _wcsicmp(suffix + 1, want) != 0) continue;
    result = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
  }
  SetupDiDestroyDeviceInfoList(set);
  return result;
}

// True once a HID client of controller `id` receives an input report while
// input keeps being submitted. After a resume PnP can take several seconds to
// start the new HID children, so this waits for the interface to appear.
bool hid_child_reads(lvg::client &client, const std::uint32_t id) {
  HANDLE handle = INVALID_HANDLE_VALUE;
  const auto start = steady::now();
  if (!wait_for(30000, [&] { return (handle = open_hid_child(id)) != INVALID_HANDLE_VALUE; })) return false;
  line("INFO", "slot %u HID interface opened after %.0f ms", id, ms_since(start));
  std::vector<unsigned char> report(128);
  PHIDP_PREPARSED_DATA preparsed = nullptr;
  HIDP_CAPS caps {};
  if (HidD_GetPreparsedData(handle, &preparsed) && HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS) {
    report.resize(caps.InputReportByteLength);
  }
  if (preparsed != nullptr) HidD_FreePreparsedData(preparsed);
  OVERLAPPED overlapped {};
  overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  DWORD bytes = 0;
  bool read = ReadFile(handle, report.data(), static_cast<DWORD>(report.size()), &bytes, &overlapped) != FALSE;
  if (!read && GetLastError() == ERROR_IO_PENDING) {
    for (int attempt = 0; attempt < 40 && !read; ++attempt) {
      std::ignore = client.submit_input_state(input(id, attempt % 2 ? lvg::south : 0));
      read = WaitForSingleObject(overlapped.hEvent, 50) == WAIT_OBJECT_0 &&
             GetOverlappedResult(handle, &overlapped, &bytes, FALSE) && bytes > 0;
    }
    if (!read) {
      CancelIoEx(handle, &overlapped);
      GetOverlappedResult(handle, &overlapped, &bytes, TRUE);
    }
  }
  CloseHandle(overlapped.hEvent);
  CloseHandle(handle);
  return read;
}

constexpr lvg::profile k_long_lived[] = {
  lvg::profile::xbox_series,
  lvg::profile::dualsense,
  lvg::profile::dualshock_4,
  lvg::profile::switch_pro,
};
constexpr std::uint32_t k_churn_slot = 4;

struct run_state {
  bool aware = false;
  std::uint32_t pads = 0;
  std::mutex pads_lock;
  lvg::client client;
  std::thread churn;
  std::atomic<bool> churn_stop {false};
  std::atomic<bool> churn_paused {false};
  std::mutex churn_lock;
  std::atomic<long> churn_cycles {0};
  std::map<DWORD, long> churn_errors;
  std::atomic<int> suspends {0};
  std::atomic<int> resumes {0};
};

DWORD create_long_lived(run_state &run) {
  for (std::uint32_t id = 0; id < run.pads; ++id) {
    const DWORD status = run.client.create_controller(id, k_long_lived[id]);
    if (status != ERROR_SUCCESS) return status;
    std::ignore = run.client.submit_input_state(input(id));
  }
  return ERROR_SUCCESS;
}

ULONG CALLBACK on_power(PVOID context, const ULONG type, PVOID) {
  auto &run = *static_cast<run_state *>(context);
  line("INFO", "power notification %lu (%s)", type,
       type == PBT_APMSUSPEND ? "suspend" : type == PBT_APMRESUMEAUTOMATIC ? "resume automatic"
                                          : type == PBT_APMRESUMESUSPEND   ? "resume suspend"
                                                                           : "other");
  if (type == PBT_APMSUSPEND) {
    ++run.suspends;
    if (run.aware) {
      const auto start = steady::now();
      run.churn_paused = true;
      { std::lock_guard wait_for_cycle {run.churn_lock}; }
      std::lock_guard lock {run.pads_lock};
      for (std::uint32_t id = 0; id < run.pads; ++id) std::ignore = run.client.destroy_controller(id);
      line("INFO", "aware: destroyed controllers before suspend in %.1f ms", ms_since(start));
    }
  } else if (type == PBT_APMRESUMEAUTOMATIC || type == PBT_APMRESUMESUSPEND) {
    ++run.resumes;
  }
  return ERROR_SUCCESS;
}

void churn(run_state *run) {
  lvg::client client;
  if (client.connect() != ERROR_SUCCESS) return;
  while (!run->churn_stop) {
    if (run->churn_paused) {
      Sleep(5);
      continue;
    }
    std::lock_guard lock {run->churn_lock};
    const DWORD status = client.create_controller(k_churn_slot, lvg::profile::xbox_series);
    if (status != ERROR_SUCCESS) {
      ++run->churn_errors[status];
      Sleep(5);
      continue;
    }
    std::ignore = client.submit_input_state(input(k_churn_slot, lvg::south));
    std::ignore = client.destroy_controller(k_churn_slot);
    ++run->churn_cycles;
  }
}

bool enable_shutdown_privilege() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
  TOKEN_PRIVILEGES privileges {1};
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) &&
                  AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
                  GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}

// The first XInput user index that is connected and was not in `before`.
int new_xinput_slot(const DWORD before) {
  for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
    XINPUT_STATE state {};
    if ((before & (1u << user)) == 0 && XInputGetState(user, &state) == ERROR_SUCCESS) return static_cast<int>(user);
  }
  return -1;
}

DWORD xinput_mask() {
  DWORD mask = 0;
  for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
    XINPUT_STATE state {};
    if (XInputGetState(user, &state) == ERROR_SUCCESS) mask |= 1u << user;
  }
  return mask;
}

}  // namespace

int main(int argc, char **argv) {
  const std::string mode = argc > 1 ? argv[1] : "idle";
  const int wake_seconds = argc > 2 ? std::atoi(argv[2]) : 45;
  if (mode != "awake" && mode != "none" && mode != "idle" && mode != "churn" && mode != "aware") {
    std::fprintf(stderr, "usage: probe_sleep <awake|none|idle|churn|aware> [wake-seconds] [log-file]\n");
    return 2;
  }
  if (argc > 3) {
    log_file = CreateFileA(argv[3], FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_FLAG_WRITE_THROUGH, nullptr);
    if (log_file == INVALID_HANDLE_VALUE) log_file = nullptr;
  }

  run_state run;
  run.aware = mode == "aware";
  run.pads = mode == "none" ? 0 : static_cast<std::uint32_t>(std::size(k_long_lived));
  line("INFO", "mode=%s wake=%d s, console session %s", mode.c_str(), wake_seconds,
       console_locked() ? "locked" : "unlocked");

  DWORD status = run.client.connect();
  check(status == ERROR_SUCCESS, "connect");
  if (status != ERROR_SUCCESS) return 1;
  if (run.pads != 0) {
    status = create_long_lived(run);
    check(status == ERROR_SUCCESS, "create controllers before sleep");
    if (status != ERROR_SUCCESS) return 1;
  }
  if (mode == "churn" || mode == "aware") run.churn = std::thread(churn, &run);
  Sleep(1000);  // Let the HID children start and xinputhid attach.

  DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS subscribe {on_power, &run};
  HPOWERNOTIFY registration = nullptr;
  PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &subscribe, &registration);

  const bool sleep = mode != "awake";
  if (sleep && !enable_shutdown_privilege()) {
    check(false, "SeShutdownPrivilege", "(run elevated)");
    return 1;
  }
  if (sleep) {
    const HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    LARGE_INTEGER due {};
    due.QuadPart = -static_cast<LONGLONG>(wake_seconds) * 10'000'000;
    if (timer == nullptr || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, TRUE)) {
      check(false, "wake timer");
      return 1;
    }
    line("INFO", "calling SetSuspendState");
    const auto before_sleep = steady::now();
    const BOOLEAN slept = SetSuspendState(FALSE, FALSE, FALSE);
    line("INFO", "SetSuspendState returned %d after %.1f s", slept, ms_since(before_sleep) / 1000);
    wait_for(5000, [&] { return run.resumes > 0; });
    CloseHandle(timer);
    check(run.suspends > 0 && run.resumes > 0, "suspend and resume");
  }
  const auto resumed = steady::now();

  if (run.churn.joinable()) {
    run.churn_paused = false;
    Sleep(1000);
    run.churn_stop = true;
    run.churn.join();
    std::string errors;
    for (const auto &[code, count] : run.churn_errors) errors += " " + std::to_string(code) + "x" + std::to_string(count);
    line("INFO", "churn: %ld cycles, create errors {code x count}:%s", run.churn_cycles.load(),
         errors.empty() ? " none" : errors.c_str());
    check(run.churn_cycles > 0, "churn kept cycling");
  }

  {
    std::lock_guard lock {run.pads_lock};
    if (sleep && !run.aware) {
      for (std::uint32_t id = 0; id < run.pads; ++id) {
        status = run.client.submit_input_state(input(id));
        char detail[64];
        std::snprintf(detail, sizeof(detail), "slot %u: error %lu", id, status);
        check(status == ERROR_DEVICE_REMOVED, "controller lost to the power-down reports ERROR_DEVICE_REMOVED", detail);
      }
    }
    bool recovered = true;
    for (std::uint32_t id = 0; id < run.pads; ++id) {
      std::ignore = run.client.destroy_controller(id);
      const bool created = wait_for(5000, [&] {
        status = run.client.create_controller(id, k_long_lived[id]);
        return status == ERROR_SUCCESS;
      });
      recovered = recovered && created;
      if (created) std::ignore = run.client.submit_input_state(input(id));
      line("INFO", "slot %u re-created %.0f ms after resume (last error %lu)", id, ms_since(resumed),
           created ? ERROR_SUCCESS : status);
    }
    check(recovered, "every controller can be created again after resume");
    for (std::uint32_t id = 1; id < run.pads; ++id) {
      char label[64];
      std::snprintf(label, sizeof(label), "slot %u HID child delivers input after resume", id);
      check(hid_child_reads(run.client, id), label);
    }
  }

  // A new Xbox controller after resume, end to end through XInput.
  {
    lvg::client fresh;
    if (run.pads != 0) wait_for(3000, [] { return xinput_mask() != 0; });
    Sleep(200);
    const DWORD before = xinput_mask();
    status = fresh.connect();
    if (status == ERROR_SUCCESS) status = fresh.create_controller(5, lvg::profile::xbox_series);
    int user = -1;
    const bool appeared = status == ERROR_SUCCESS && wait_for(5000, [&] { return (user = new_xinput_slot(before)) >= 0; });
    XINPUT_STATE state {};
    const bool input_ok = appeared && wait_for(10000, [&] {
      std::ignore = fresh.submit_input_state(input(5, lvg::south, 32767));
      return XInputGetState(user, &state) == ERROR_SUCCESS && (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) &&
             state.Gamepad.sThumbLX > 32000;
    });
    std::ignore = fresh.destroy_controller(5);
    char detail[96];
    std::snprintf(detail, sizeof(detail), "XInput user %d, create error %lu", user, status);
    if (appeared && !input_ok && console_locked()) {
      line("SKIP", "new Xbox controller input via XInput %s (console session locked: XInput reads zeros)", detail);
    } else {
      check(appeared && input_ok, "new Xbox controller input via XInput", detail);
    }
  }

  for (std::uint32_t id = 0; id < run.pads; ++id) std::ignore = run.client.destroy_controller(id);
  if (registration != nullptr) PowerUnregisterSuspendResumeNotification(registration);
  line("INFO", "%d failure(s)", failures);
  if (log_file != nullptr) CloseHandle(log_file);
  return failures == 0 ? 0 : 1;
}
