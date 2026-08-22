// Reports what DirectInput actually sees on the generic PID profile: which
// axes it exposes, the range it assigns each one, and where each sits at rest.
// The concern this answers is whether a resting trigger reads as "held", which
// is a claim worth measuring rather than inheriting.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "libvirtualgamepad/client.h"

namespace {

WORD g_want_vid = 0x1209;
WORD g_want_pid = 0x0001;
LPDIRECTINPUT8W g_di = nullptr;
LPDIRECTINPUTDEVICE8W g_device = nullptr;
std::wstring g_name;

BOOL CALLBACK enum_devices(const DIDEVICEINSTANCEW *const instance, void *) {
  if (g_device != nullptr) {
    return DIENUM_STOP;
  }
  LPDIRECTINPUTDEVICE8W device = nullptr;
  if (FAILED(g_di->CreateDevice(instance->guidInstance, &device, nullptr))) {
    return DIENUM_CONTINUE;
  }
  DIPROPDWORD vidpid {};
  vidpid.diph.dwSize = sizeof(vidpid);
  vidpid.diph.dwHeaderSize = sizeof(vidpid.diph);
  vidpid.diph.dwHow = DIPH_DEVICE;
  if (SUCCEEDED(device->GetProperty(DIPROP_VIDPID, &vidpid.diph))) {
    const WORD vid = LOWORD(vidpid.dwData);
    const WORD pid = HIWORD(vidpid.dwData);
    if (vid == g_want_vid && pid == g_want_pid) {
      g_device = device;
      g_name = instance->tszProductName;
      return DIENUM_STOP;
    }
  }
  device->Release();
  return DIENUM_CONTINUE;
}

struct axis_info {
  const char *name;
  int offset;
};

BOOL CALLBACK enum_axes(const DIDEVICEOBJECTINSTANCEW *const obj, void *const ctx) {
  auto *const found = static_cast<std::vector<std::wstring> *>(ctx);
  found->emplace_back(obj->tszName);

  DIPROPRANGE range {};
  range.diph.dwSize = sizeof(range);
  range.diph.dwHeaderSize = sizeof(range.diph);
  range.diph.dwHow = DIPH_BYID;
  range.diph.dwObj = obj->dwType;
  if (SUCCEEDED(g_device->GetProperty(DIPROP_RANGE, &range.diph))) {
    std::printf("    %-24ls range %6ld .. %6ld\n", obj->tszName, range.lMin, range.lMax);
  } else {
    std::printf("    %-24ls range unavailable\n", obj->tszName);
  }
  return DIENUM_CONTINUE;
}

}  // namespace

int main(int argc, char **argv) {
  std::printf("PRIVATE TEST ONLY: 1209:0001 is not valid for redistributed devices.\n");
  // Which profile to look at, so the same measurement can be taken of a
  // console-shaped pad for comparison.
  lvg::profile which = lvg::profile::generic_pid;
  const std::string arg = argc > 1 ? argv[1] : "generic";
  if (arg == "ds4") {
    which = lvg::profile::dualshock_4;
    g_want_vid = 0x054C;
    g_want_pid = 0x09CC;
  } else if (arg == "ds5") {
    which = lvg::profile::dualsense;
    g_want_vid = 0x054C;
    g_want_pid = 0x0CE6;
  } else if (arg == "xbox") {
    which = lvg::profile::xbox_series;
    g_want_vid = 0x045E;
    g_want_pid = 0x0B12;
  }
  std::printf("profile: %s\n", arg.c_str());

  lvg::client client;
  if (client.connect() != ERROR_SUCCESS) {
    std::printf("connect failed\n");
    return 1;
  }
  constexpr std::uint32_t slot = 0;
  if (client.create_controller(slot, which) != ERROR_SUCCESS) {
    std::printf("could not create a generic_pid controller\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));

  if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                IID_IDirectInput8W, reinterpret_cast<void **>(&g_di), nullptr))) {
    std::printf("DirectInput8Create failed\n");
    std::ignore = client.destroy_controller(slot);
    return 1;
  }

  g_di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_devices, nullptr, DIEDFL_ATTACHEDONLY);
  if (g_device == nullptr) {
    std::printf("DirectInput did not enumerate the generic profile\n");
    g_di->Release();
    std::ignore = client.destroy_controller(slot);
    return 2;
  }
  std::printf("DirectInput sees: %ls\n", g_name.c_str());

  DIDEVCAPS caps {};
  caps.dwSize = sizeof(caps);
  if (SUCCEEDED(g_device->GetCapabilities(&caps))) {
    std::printf("  axes %lu, buttons %lu, POVs %lu\n", caps.dwAxes, caps.dwButtons, caps.dwPOVs);
    const bool ff = (caps.dwFlags & DIDC_FORCEFEEDBACK) != 0;
    std::printf("  force feedback: %s\n", ff ? "YES" : "no");
  }

  if (FAILED(g_device->SetDataFormat(&c_dfDIJoystick2))) {
    std::printf("  SetDataFormat failed\n");
  }
  const HWND console = GetConsoleWindow();
  if (console != nullptr) {
    std::ignore = g_device->SetCooperativeLevel(console, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
  }

  std::printf("  axes as DirectInput ranges them:\n");
  std::vector<std::wstring> axes;
  g_device->EnumObjects(enum_axes, &axes, DIDFT_AXIS);

  if (FAILED(g_device->Acquire())) {
    std::printf("  could not acquire the device\n");
  }

  // Nothing has been submitted, so every control is at rest.
  DIJOYSTATE2 state {};
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  if (SUCCEEDED(g_device->Poll()) || true) {
    if (FAILED(g_device->GetDeviceState(sizeof(state), &state))) {
      std::printf("  GetDeviceState failed\n");
    }
  }

  std::printf("\n  at rest, with nothing submitted:\n");
  std::printf("    X  %6ld   Y  %6ld\n", state.lX, state.lY);
  std::printf("    Z  %6ld   Rz %6ld   <- the triggers\n", state.lZ, state.lRz);
  std::printf("    Rx %6ld   Ry %6ld\n", state.lRx, state.lRy);

  // Now hold both triggers fully and see which way the axes travel.
  lvg::input_state_request input {};
  input.header.size = sizeof(input);
  input.header.version = lvg::k_protocol_version;
  input.controller_id = slot;
  input.left_trigger = 255;
  input.right_trigger = 255;
  std::ignore = client.submit_input_state(input);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  DIJOYSTATE2 pressed {};
  std::ignore = g_device->Poll();
  if (SUCCEEDED(g_device->GetDeviceState(sizeof(pressed), &pressed))) {
    std::printf("\n  with both triggers held fully:\n");
    std::printf("    Z  %6ld   Rz %6ld\n", pressed.lZ, pressed.lRz);
  }

  const bool rest_is_low = state.lZ < pressed.lZ;
  std::printf("\n  a resting trigger reads %s than a held one, so it %s\n",
              rest_is_low ? "LOWER" : "HIGHER",
              rest_is_low ? "idles at minimum (expected)" : "idles at MAXIMUM");

  g_device->Unacquire();
  g_device->Release();
  g_di->Release();
  std::ignore = client.destroy_controller(slot);
  return 0;
}
