// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
// Fixed Windows ABI types for portable driver-body tests. No OS implementation.
#include <cstddef>
#include <cstdint>
#include <cwchar>
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using LONG = std::int32_t;
using ULONG_PTR = std::uintptr_t;
using USHORT = std::uint16_t;
using UCHAR = std::uint8_t;
using PUCHAR = UCHAR *;
using PWSTR = wchar_t *;
using PVOID = void *;
using HANDLE = void *;
using BOOLEAN = bool;
using BOOL = std::int32_t;
using NTSTATUS = std::int32_t;
using PDRIVER_OBJECT = void *;
using PUNICODE_STRING = void *;
struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2, Data3;
  std::uint8_t Data4[8];
};
// POD stand-ins for the user-mode synchronization ABI. The fixture supplies
// the backing mutex/condition variable through opaque pointers; these are not
// Windows implementations and make no UMDF runtime qualification claim.
struct SRWLOCK { void *state; };
struct CONDITION_VARIABLE { void *state; };
void InitializeSRWLock(SRWLOCK *lock);
void InitializeConditionVariable(CONDITION_VARIABLE *condition);
void AcquireSRWLockExclusive(SRWLOCK *lock);
void ReleaseSRWLockExclusive(SRWLOCK *lock);
BOOL SleepConditionVariableSRW(
  CONDITION_VARIABLE *condition, SRWLOCK *lock, DWORD milliseconds, ULONG flags);
void WakeAllConditionVariable(CONDITION_VARIABLE *condition);
void Sleep(DWORD milliseconds);
LONG InterlockedExchange(volatile LONG *destination, LONG value);
LONG InterlockedCompareExchange(volatile LONG *destination, LONG exchange, LONG comparand);
LONG InterlockedIncrement(volatile LONG *destination);
LONG InterlockedDecrement(volatile LONG *destination);
inline constexpr DWORD INFINITE = 0xffffffffu;
inline constexpr ULONG CONDITION_VARIABLE_LOCKMODE_SHARED = 0x1;
inline constexpr bool TRUE = true, FALSE = false;
inline constexpr NTSTATUS STATUS_SUCCESS = 0;
inline constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
inline constexpr NTSTATUS STATUS_NOT_SUPPORTED = -2;
inline constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
inline constexpr NTSTATUS STATUS_DEVICE_BUSY = -4;
inline constexpr NTSTATUS STATUS_CANCELLED = -5;
inline constexpr NTSTATUS STATUS_INSUFFICIENT_RESOURCES = -15;
inline constexpr NTSTATUS STATUS_ACCESS_DENIED = -6;
inline constexpr NTSTATUS STATUS_NOT_FOUND = -7;
inline constexpr NTSTATUS STATUS_INVALID_DEVICE_REQUEST = -8;
inline constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -9;
inline constexpr NTSTATUS STATUS_NO_MORE_ENTRIES = -10;
inline constexpr NTSTATUS STATUS_INVALID_BUFFER_SIZE = -11;
inline constexpr NTSTATUS STATUS_INVALID_HANDLE = -12;
inline constexpr bool NT_SUCCESS(NTSTATUS status) { return status >= 0; }
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))

// Security boundary used by the source-stage driver. These stand-ins model
// authorization results; they do not establish Windows impersonation semantics.
using PSID = void *;
struct SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; };
struct TOKEN_USER { SID_AND_ATTRIBUTES User; };
enum WELL_KNOWN_SID_TYPE { WinLocalSystemSid, WinBuiltinAdministratorsSid };
enum TOKEN_INFORMATION_CLASS { TokenUser };
inline constexpr DWORD TOKEN_QUERY = 8, SECURITY_MAX_SID_SIZE = 68;
inline bool fake_token_open_failure = false, fake_token_query_failure = false;
inline bool fake_sid_failure = false, fake_membership_failure = false;
inline bool fake_client_is_system = false, fake_client_is_admin = true;
inline HANDLE fake_token_handle() { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x7777)); }
inline HANDLE fake_process_handle() { return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x8888)); }
inline constexpr DWORD PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
inline bool fake_process_open_failure = false;
inline int fake_process_opens = 0, fake_process_closes = 0;
inline HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
  if (access != PROCESS_QUERY_LIMITED_INFORMATION || inherit || pid == 0 || fake_process_open_failure) return nullptr;
  ++fake_process_opens;
  return fake_process_handle();
}
inline HANDLE GetCurrentThread() { return nullptr; }
inline BOOL OpenThreadToken(HANDLE, DWORD, BOOL, HANDLE *token) {
  if (fake_token_open_failure) return FALSE;
  *token = fake_token_handle(); return TRUE;
}
inline BOOL GetTokenInformation(HANDLE, TOKEN_INFORMATION_CLASS, void *data, DWORD, DWORD *bytes) {
  if (fake_token_query_failure) return FALSE;
  static_cast<TOKEN_USER *>(data)->User.Sid = fake_client_is_system ? reinterpret_cast<PSID>(1) : reinterpret_cast<PSID>(2);
  *bytes = sizeof(TOKEN_USER); return TRUE;
}
inline BOOL IsWellKnownSid(PSID sid, WELL_KNOWN_SID_TYPE type) {
  return type == WinLocalSystemSid && sid == reinterpret_cast<PSID>(1);
}
inline BOOL CreateWellKnownSid(WELL_KNOWN_SID_TYPE, PSID, void *, DWORD *) { return !fake_sid_failure; }
inline BOOL CheckTokenMembership(HANDLE, PSID, BOOL *result) {
  if (fake_membership_failure) return FALSE;
  *result = fake_client_is_admin; return TRUE;
}

// User-mode timing and threading behind the Steam Controller keep-alive worker
// (start_sc26_keepalive and friends in driver.cpp). The lifecycle fixture never
// creates the worker's timer or stop event, so no worker runs here: handle
// creation and the waits fail closed, while the performance counter (the report
// clock) and SwitchToThread (the submit drain) do their real job.
#include <chrono>
#include <thread>
using LONGLONG = std::int64_t;
using LPVOID = void *;
using LPCWSTR = const wchar_t *;
using LPSECURITY_ATTRIBUTES = void *;
using LPTHREAD_START_ROUTINE = DWORD (*)(LPVOID);
#define WINAPI
struct LARGE_INTEGER { LONGLONG QuadPart; };
inline constexpr DWORD WAIT_OBJECT_0 = 0;
inline constexpr DWORD WAIT_FAILED = 0xFFFFFFFFu;
inline constexpr DWORD TIMER_ALL_ACCESS = 0x1F0003u;
inline constexpr int THREAD_PRIORITY_HIGHEST = 2;
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency) {
  using period = std::chrono::steady_clock::period;
  frequency->QuadPart = static_cast<LONGLONG>(period::den / period::num);
  return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER *counter) {
  counter->QuadPart = static_cast<LONGLONG>(std::chrono::steady_clock::now().time_since_epoch().count());
  return 1;
}
inline BOOL SwitchToThread() { std::this_thread::yield(); return 1; }
inline HANDLE CreateEventW(LPSECURITY_ATTRIBUTES, BOOL, BOOL, LPCWSTR) { return nullptr; }
inline HANDLE CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD) { return nullptr; }
inline BOOL SetWaitableTimer(HANDLE, const LARGE_INTEGER *, LONG, void *, LPVOID, BOOL) { return 0; }
inline BOOL CancelWaitableTimer(HANDLE) { return 0; }
inline BOOL SetEvent(HANDLE) { return 0; }
inline BOOL ResetEvent(HANDLE) { return 0; }
inline HANDLE CreateThread(LPSECURITY_ATTRIBUTES, std::size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD *) { return nullptr; }
inline BOOL SetThreadPriority(HANDLE, int) { return 0; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return WAIT_FAILED; }
inline DWORD WaitForMultipleObjects(DWORD, const HANDLE *, BOOL, DWORD) { return WAIT_FAILED; }
// Supplied by the fixture (the keyboard/mouse transport test counts closes).
BOOL CloseHandle(HANDLE handle);

// Configuration Manager and clock-free power lifecycle test boundaries.
#include <cstdlib>
#include <cwctype>
inline constexpr NTSTATUS STATUS_DEVICE_REMOVED = -13, STATUS_DEVICE_POWERED_OFF = -14;
inline bool fake_heap_failure = false;
inline HANDLE GetProcessHeap() { return nullptr; }
inline void *HeapAlloc(HANDLE, DWORD, std::size_t bytes) { return fake_heap_failure ? nullptr : std::calloc(1, bytes); }
inline void HeapFree(HANDLE, DWORD, void *p) { std::free(p); }
inline int _wcsicmp(const wchar_t *a, const wchar_t *b) {
  while (*a && std::towlower(*a) == std::towlower(*b)) { ++a; ++b; }
  return int(std::towlower(*a)) - int(std::towlower(*b));
}
