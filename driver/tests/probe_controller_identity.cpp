// SPDX-License-Identifier: MIT
//
// Read-only inventory: compare HID, DirectInput, WGI, GameInput, and XInput.
// Does not create controllers, submit reports, or install/change drivers.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <hidsdi.h>
#include <GameInput.h>
#include <Xinput.h>
#include <cstdio>
#include <cstdint>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

namespace {
IDirectInput8W *direct_input = nullptr;
unsigned gameinput_count = 0;
unsigned gameinput_xbox_count = 0;

void CALLBACK inspect_gameinput(GameInputCallbackToken, void *, IGameInputDevice *device,
                                std::uint64_t, GameInputDeviceStatus current, GameInputDeviceStatus) {
  if (!(current & GameInputDeviceConnected)) return;
  const auto *info = device->GetDeviceInfo();
  ++gameinput_count;
  if (info->vendorId == 0x045e && (info->productId == 0x0b12 || info->productId == 0x02ea)) {
    ++gameinput_xbox_count;
  }
  std::printf("GameInput: %s VID/PID=%04x:%04x revision=%04x input=%08x\n",
              info->displayName ? info->displayName->data : "(unnamed)",
              info->vendorId, info->productId, info->revisionNumber,
              static_cast<unsigned>(info->supportedInput));
}

void inspect_xinput() {
  HMODULE library = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (library == nullptr) {
    std::printf("XInput is unavailable: %lu\n", GetLastError());
    return;
  }
  const auto capabilities = reinterpret_cast<decltype(&XInputGetCapabilities)>(
      GetProcAddress(library, "XInputGetCapabilities"));
  if (capabilities == nullptr) {
    std::printf("XInputGetCapabilities is unavailable\n");
    FreeLibrary(library);
    return;
  }
  unsigned connected = 0;
  for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
    XINPUT_CAPABILITIES info {};
    const DWORD result = capabilities(slot, 0, &info);
    if (result == ERROR_SUCCESS) {
      ++connected;
      std::printf("XInput: slot=%lu type=%u subtype=%u flags=%04x\n",
                  slot, info.Type, info.SubType, info.Flags);
    } else if (result != ERROR_DEVICE_NOT_CONNECTED) {
      std::printf("XInput: slot=%lu query failed: %lu\n", slot, result);
    }
  }
  std::printf("XInput connected slot count: %u (OS slots, not Steam Input reservations)\n",
              connected);
  FreeLibrary(library);
}

BOOL CALLBACK inspect(const DIDEVICEINSTANCEW *instance, void *) {
  IDirectInputDevice8W *device = nullptr;
  if (FAILED(direct_input->CreateDevice(instance->guidInstance, &device, nullptr))) {
    return DIENUM_CONTINUE;
  }
  DIPROPDWORD identity {};
  identity.diph = {sizeof(identity), sizeof(identity.diph), 0, DIPH_DEVICE};
  const HRESULT identity_status = device->GetProperty(DIPROP_VIDPID, &identity.diph);
  DIPROPGUIDANDPATH path {};
  path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
  const HRESULT path_status = device->GetProperty(DIPROP_GUIDANDPATH, &path.diph);
  std::printf("DirectInput: %ls VID/PID=%04x:%04x status=%08lx\n",
              instance->tszProductName, LOWORD(identity.dwData), HIWORD(identity.dwData),
              static_cast<unsigned long>(identity_status));
  if (SUCCEEDED(path_status)) {
    std::printf("  path: %ls\n", path.wszPath);
    HANDLE handle = CreateFileW(path.wszPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
      HIDD_ATTRIBUTES attributes {};
      attributes.Size = sizeof(attributes);
      if (HidD_GetAttributes(handle, &attributes)) {
        std::printf("  same-path HID: %04x:%04x version=%04x%s\n",
                    attributes.VendorID, attributes.ProductID, attributes.VersionNumber,
                    SUCCEEDED(identity_status) &&
                      (attributes.VendorID != LOWORD(identity.dwData) ||
                       attributes.ProductID != HIWORD(identity.dwData)) ? " IDENTITY MISMATCH" : "");
      }
      CloseHandle(handle);
    } else {
      std::printf("  HID open failed: %lu\n", GetLastError());
    }
  }
  device->Release();
  return DIENUM_CONTINUE;
}
}

int main() {
  const HRESULT status = DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
      IID_IDirectInput8W, reinterpret_cast<void **>(&direct_input), nullptr);
  if (FAILED(status)) {
    std::printf("DirectInput8Create failed: %08lx\n", static_cast<unsigned long>(status));
    return 1;
  }
  const HRESULT enumeration = direct_input->EnumDevices(DI8DEVCLASS_GAMECTRL, inspect, nullptr,
                                                        DIEDFL_ATTACHEDONLY);
  direct_input->Release();
  try {
    winrt::init_apartment();
    auto controllers = winrt::Windows::Gaming::Input::RawGameController::RawGameControllers();
    // Give the WinRT controller provider a moment to populate its inventory.
    Sleep(1000);
    controllers = winrt::Windows::Gaming::Input::RawGameController::RawGameControllers();
    std::printf("Windows.Gaming.Input controller count: %u\n", controllers.Size());
    for (const auto &controller : controllers) {
      std::printf("Windows.Gaming.Input: %ls VID/PID=%04x:%04x\n",
                  controller.DisplayName().c_str(), controller.HardwareVendorId(),
                  controller.HardwareProductId());
    }
  } catch (const winrt::hresult_error &error) {
    std::printf("Windows.Gaming.Input failed: %08lx\n", static_cast<unsigned long>(error.code().value));
    return 1;
  }
  HMODULE library = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (library != nullptr) {
    const auto create = reinterpret_cast<decltype(&GameInputCreate)>(GetProcAddress(library, "GameInputCreate"));
    IGameInput *gameinput = nullptr;
    if (create != nullptr && SUCCEEDED(create(&gameinput))) {
      GameInputCallbackToken token {};
      const HRESULT result = gameinput->RegisterDeviceCallback(nullptr,
          static_cast<GameInputKind>(GameInputKindController | GameInputKindGamepad),
          GameInputDeviceConnected, GameInputBlockingEnumeration, nullptr, inspect_gameinput, &token);
      if (SUCCEEDED(result)) {
        gameinput->StopCallback(token);
        gameinput->UnregisterCallback(token, UINT64_MAX);
        std::printf("GameInput controller count: %u; native Xbox One/Series count: %u\n",
                    gameinput_count, gameinput_xbox_count);
      } else {
        std::printf("GameInput enumeration failed: %08lx\n", static_cast<unsigned long>(result));
      }
      gameinput->Release();
    }
    FreeLibrary(library);
  } else {
    std::printf("GameInput is unavailable: %lu\n", GetLastError());
  }
  inspect_xinput();
  return FAILED(enumeration) ? 1 : 0;
}
