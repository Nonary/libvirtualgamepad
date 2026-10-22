// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
using HDEVINFO = HANDLE;
inline constexpr DWORD DIGCF_PRESENT=2, DIGCF_DEVICEINTERFACE=16;
struct SP_DEVICE_INTERFACE_DATA { DWORD cbSize; };
struct SP_DEVICE_INTERFACE_DETAIL_DATA_W { DWORD cbSize; wchar_t DevicePath[1]; };
HDEVINFO SetupDiGetClassDevsW(const GUID *, const wchar_t *, HANDLE, DWORD);
BOOL SetupDiEnumDeviceInterfaces(HDEVINFO, void *, const GUID *, DWORD, SP_DEVICE_INTERFACE_DATA *);
BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO, SP_DEVICE_INTERFACE_DATA *, SP_DEVICE_INTERFACE_DETAIL_DATA_W *, DWORD, DWORD *, void *);
BOOL SetupDiDestroyDeviceInfoList(HDEVINFO);
