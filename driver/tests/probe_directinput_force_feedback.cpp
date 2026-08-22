// Asks DirectInput the question a force-feedback game asks: enumerate only
// force-feedback capable controllers, and see whether the generic PID profile
// is among them. This is the profile's entire reason to exist, so the answer
// decides whether it is worth offering.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <thread>
#include <chrono>

#include "libvirtualgamepad/client.h"

namespace {

bool g_found_in_ff_enum = false;
bool g_found_at_all = false;

BOOL CALLBACK enum_all(const DIDEVICEINSTANCEW *const instance, void *) {
  std::printf("    all controllers : %ls\n", instance->tszProductName);
  g_found_at_all = true;
  return DIENUM_CONTINUE;
}

BOOL CALLBACK enum_ff(const DIDEVICEINSTANCEW *const instance, void *) {
  std::printf("    force feedback  : %ls\n", instance->tszProductName);
  g_found_in_ff_enum = true;
  return DIENUM_CONTINUE;
}

}  // namespace

int main() {
  std::printf("PRIVATE TEST ONLY: generic_pid has no public PID and is refused by release drivers.\n");
  lvg::client client;
  if (client.connect() != ERROR_SUCCESS) {
    std::printf("connect failed\n");
    return 1;
  }
  constexpr std::uint32_t slot = 0;
  if (client.create_controller(slot, lvg::profile::generic_pid) != ERROR_SUCCESS) {
    std::printf("could not create a generic_pid controller\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));

  LPDIRECTINPUT8W di = nullptr;
  if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                IID_IDirectInput8W, reinterpret_cast<void **>(&di), nullptr))) {
    std::printf("DirectInput8Create failed\n");
    std::ignore = client.destroy_controller(slot);
    return 1;
  }

  std::printf("  what DirectInput enumerates:\n");
  di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_all, nullptr, DIEDFL_ATTACHEDONLY);
  di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_ff, nullptr,
                  DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK);

  std::printf("\n  enumerated as a controller      : %s\n", g_found_at_all ? "yes" : "no");
  std::printf("  enumerated as force feedback    : %s\n", g_found_in_ff_enum ? "yes" : "NO");

  di->Release();
  std::ignore = client.destroy_controller(slot);
  return g_found_in_ff_enum ? 0 : 3;
}
