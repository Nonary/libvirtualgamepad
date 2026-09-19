// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Creates slot 7, checks the installed driver's native DS4 initialization path,
// then releases it. Never installs drivers or modifies existing controllers.
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <cstdio>
#include <array>
#include <vector>
#include "libvirtualgamepad/client.h"

int main() {
  constexpr unsigned slot = 7;
  lvg::client client;
  auto status = client.connect();
  if (status == ERROR_SUCCESS) status = client.create_controller(slot, lvg::profile::dualshock_4);
  if (status != ERROR_SUCCESS) {
    std::printf("Create DS4 slot %u: error %lu\n", slot, status); return 1;
  }
  GUID guid; HidD_GetHidGuid(&guid);
  HANDLE handle = INVALID_HANDLE_VALUE;
  for (int retry = 0; retry < 50 && handle == INVALID_HANDLE_VALUE; ++retry) {
    auto set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) break;
    for (DWORD index = 0;; ++index) {
      SP_DEVICE_INTERFACE_DATA item {}; item.cbSize = sizeof(item);
      if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &item)) break;
      DWORD size = 0;
      SetupDiGetDeviceInterfaceDetailW(set, &item, nullptr, 0, &size, nullptr);
      if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
      std::vector<unsigned char> storage(size);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
      detail->cbSize = sizeof(*detail);
      if (!SetupDiGetDeviceInterfaceDetailW(set, &item, detail, size, nullptr, nullptr)) continue;
      auto h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      HIDD_ATTRIBUTES attributes {}; attributes.Size = sizeof(attributes);
      std::array<unsigned char, 16> pairing {}; pairing[0] = 0x12;
      if (HidD_GetAttributes(h, &attributes) && attributes.VendorID == 0x054c &&
          attributes.ProductID == 0x09cc && HidD_GetFeature(h, pairing.data(), static_cast<ULONG>(pairing.size())) &&
          pairing[1] == slot && pairing[2] == 0x41 && pairing[6] == 0x02) {
        handle = h;
        std::printf("DS4 %04x:%04x version %04x\n", attributes.VendorID, attributes.ProductID, attributes.VersionNumber);
        break;
      }
      CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (handle == INVALID_HANDLE_VALUE) Sleep(100);
  }
  int failures = 0;
  const auto check = [&](bool ok, const char *label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
  };
  check(handle != INVALID_HANDLE_VALUE, "created device enumerates with its own pairing identity");
  if (handle != INVALID_HANDLE_VALUE) {
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps {};
    const bool have_caps = HidD_GetPreparsedData(handle, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
    check(have_caps, "HID capabilities readable");
    if (have_caps) {
      std::printf("input=%u output=%u feature=%u\n", caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength);
      check(caps.InputReportByteLength == 64 && caps.OutputReportByteLength == 32 && caps.FeatureReportByteLength >= 53,
            "native USB DS4 report-size gate");
      std::vector<HIDP_VALUE_CAPS> values(caps.NumberFeatureValueCaps);
      auto count = caps.NumberFeatureValueCaps;
      bool firmware_usage = false;
      if (HidP_GetValueCaps(HidP_Feature, values.data(), &count, pp) == HIDP_STATUS_SUCCESS) {
        for (const auto &value : values) {
          if (value.ReportID == 0xa3 && value.UsagePage == 0xff80 &&
              value.NotRange.Usage == 0x43 && value.ReportCount == 48) firmware_usage = true;
        }
      }
      check(firmware_usage, "native USB DS4 feature-usage gate");
    }
    if (pp) HidD_FreePreparsedData(pp);
    std::array<unsigned char, 49> firmware {}; firmware[0] = 0xa3;
    check(HidD_GetFeature(handle, firmware.data(), static_cast<ULONG>(firmware.size())) &&
          (firmware[35] | (firmware[36] << 8)) >= 0x3100, "native USB DS4 firmware gate");
    std::array<unsigned char, 16> pairing {}; pairing[0] = 0x12;
    check(HidD_GetFeature(handle, pairing.data(), static_cast<ULONG>(pairing.size())) &&
          pairing[7] == 0x08 && pairing[8] == 0x25 && pairing[9] == 0x00,
          "libScePad DS4 pairing magic");
    std::array<unsigned char, 17> command {}; command[0] = 0x14; command[1] = 2;
    check(HidD_SetFeature(handle, command.data(), static_cast<ULONG>(command.size())), "native USB sensor initialization");
    std::array<unsigned char, 37> calibration {}; calibration[0] = 2;
    check(HidD_GetFeature(handle, calibration.data(), static_cast<ULONG>(calibration.size())), "37-byte USB calibration read");
    lvg::input_state_request input {};
    input.header.size = sizeof(input); input.header.version = lvg::k_protocol_version;
    input.controller_id = slot; input.buttons = lvg::button_mask::south;
    check(client.submit_input_state(input) == ERROR_SUCCESS, "submit Cross button");
    OVERLAPPED operation {}; operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool received_cross = false;
    for (unsigned attempt = 0; attempt < 16 && operation.hEvent; ++attempt) {
      std::array<unsigned char, 64> report {};
      ResetEvent(operation.hEvent);
      DWORD bytes = 0;
      bool started = ReadFile(handle, report.data(), static_cast<DWORD>(report.size()), nullptr, &operation) != FALSE;
      if (!started && GetLastError() != ERROR_IO_PENDING) break;
      if (WaitForSingleObject(operation.hEvent, 1000) != WAIT_OBJECT_0) {
        CancelIoEx(handle, &operation);
        GetOverlappedResult(handle, &operation, &bytes, TRUE);
        break;
      }
      if (!GetOverlappedResult(handle, &operation, &bytes, FALSE)) break;
      if (bytes == 64 && report[0] == 1 && (report[5] & 0x20)) { received_cross = true; break; }
    }
    check(received_cross, "native HID reader receives Cross button report");
    if (operation.hEvent) CloseHandle(operation.hEvent);
    CloseHandle(handle);
  }
  check(client.destroy_controller(slot) == ERROR_SUCCESS, "release test controller");
  return failures ? 1 : 0;
}
