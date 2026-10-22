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
using NTSTATUS = std::int32_t;
using PDRIVER_OBJECT = void *;
using PUNICODE_STRING = void *;
struct GUID {
  std::uint32_t Data1;
  std::uint16_t Data2, Data3;
  std::uint8_t Data4[8];
};
inline constexpr bool TRUE = true, FALSE = false;
inline constexpr NTSTATUS STATUS_SUCCESS = 0;
inline constexpr NTSTATUS STATUS_INVALID_PARAMETER = -1;
inline constexpr NTSTATUS STATUS_NOT_SUPPORTED = -2;
inline constexpr NTSTATUS STATUS_DEVICE_NOT_READY = -3;
inline constexpr NTSTATUS STATUS_DEVICE_BUSY = -4;
inline constexpr NTSTATUS STATUS_CANCELLED = -5;
inline constexpr NTSTATUS STATUS_ACCESS_DENIED = -6;
inline constexpr NTSTATUS STATUS_NOT_FOUND = -7;
inline constexpr NTSTATUS STATUS_INVALID_DEVICE_REQUEST = -8;
inline constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = -9;
inline constexpr NTSTATUS STATUS_NO_MORE_ENTRIES = -10;
inline constexpr NTSTATUS STATUS_INVALID_BUFFER_SIZE = -11;
inline constexpr NTSTATUS STATUS_INVALID_HANDLE = -12;
inline constexpr bool NT_SUCCESS(NTSTATUS status) { return status >= 0; }
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))

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
