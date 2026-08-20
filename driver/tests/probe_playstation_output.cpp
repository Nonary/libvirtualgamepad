// Drives the PlayStation output path end to end: writes a real output report
// into the device through the Windows HID stack and checks that what the client
// receives is what was written. The unit tests decode a buffer in-process, which
// cannot catch a descriptor that declares the wrong output length or a transfer
// that never reaches the driver at all.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#include "libvirtualgamepad/client.h"

namespace {

int g_failures = 0;

void check(const bool ok, const char *const what) {
  std::printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok) {
    ++g_failures;
  }
}

// Opens the HID device for a given vendor and product, with write access.
HANDLE open_device(const USHORT vid, const USHORT pid, USHORT *const output_length) {
  GUID hid {};
  HidD_GetHidGuid(&hid);
  const HDEVINFO set =
    SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE found = INVALID_HANDLE_VALUE;
  for (DWORD i = 0; found == INVALID_HANDLE_VALUE; ++i) {
    SP_DEVICE_INTERFACE_DATA d {};
    d.cbSize = sizeof(d);
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &d)) {
      break;
    }
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &d, nullptr, 0, &need, nullptr);
    if (need == 0) {
      continue;
    }
    std::vector<BYTE> buf(need);
    auto *const det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
    det->cbSize = sizeof(*det);
    if (!SetupDiGetDeviceInterfaceDetailW(set, &d, det, need, nullptr, nullptr)) {
      continue;
    }

    const HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    HIDD_ATTRIBUTES a {};
    a.Size = sizeof(a);
    if (HidD_GetAttributes(h, &a) && a.VendorID == vid && a.ProductID == pid) {
      PHIDP_PREPARSED_DATA pre = nullptr;
      if (HidD_GetPreparsedData(h, &pre)) {
        HIDP_CAPS caps {};
        if (HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
          *output_length = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(pre);
      }
      found = h;
      break;
    }
    CloseHandle(h);
  }

  SetupDiDestroyDeviceInfoList(set);
  return found;
}

// Waits for the driver to hand back the output the host just wrote.
bool await_feedback(lvg::client &client, const std::uint32_t slot, lvg::feedback_event *const out) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    if (client.poll_feedback(slot, out) == ERROR_SUCCESS) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

}  // namespace

int main() {
  lvg::client client;
  if (client.connect() != ERROR_SUCCESS) {
    std::printf("connect failed\n");
    return 1;
  }

  struct target {
    const char *name;
    lvg::profile profile;
    USHORT vid;
    USHORT pid;
    USHORT report_length;
  };

  const target targets[] = {
    {"DualSense", lvg::profile::dualsense, 0x054C, 0x0CE6, 48},
    {"DualShock 4", lvg::profile::dualshock_4, 0x054C, 0x09CC, 32},
  };

  for (const target &t : targets) {
    std::printf("\n%s\n", t.name);
    constexpr std::uint32_t slot = 0;
    if (client.create_controller(slot, t.profile) != ERROR_SUCCESS) {
      std::printf("  could not create the controller\n");
      ++g_failures;
      continue;
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));

    USHORT declared = 0;
    const HANDLE device = open_device(t.vid, t.pid, &declared);
    if (device == INVALID_HANDLE_VALUE) {
      std::printf("  device did not enumerate\n");
      ++g_failures;
      std::ignore = client.destroy_controller(slot);
      continue;
    }

    // A host sizes its writes from the descriptor. If this is wrong, every
    // output report a game sends is rejected before it reaches the driver.
    check(declared == t.report_length,
          declared == t.report_length ? "the descriptor declares the right output length"
                                      : "the descriptor declares the WRONG output length");

    std::vector<std::uint8_t> report(declared, 0);
    lvg::feedback_event event {};

    if (t.profile == lvg::profile::dualsense) {
      report[0] = 0x02;
      report[1] = 0x01 | 0x04 | 0x08;
      report[2] = 0x01 | 0x04 | 0x10;
      report[3] = 0x40;  // right, high frequency
      report[4] = 0x80;  // left, low frequency
      report[9] = 0x01;  // microphone LED
      report[11] = 0x26;  // right trigger effect
      report[22] = 0x21;  // left trigger effect
      report[44] = 0x05;  // player LEDs
      report[45] = 0x10;
      report[46] = 0x20;
      report[47] = 0x30;
    } else {
      report[0] = 0x05;
      report[1] = 0x01 | 0x02;
      report[4] = 0x30;  // right
      report[5] = 0x90;  // left
      report[6] = 0x11;
      report[7] = 0x22;
      report[8] = 0x33;
    }

    // Hosts do not agree on how to send an output report. hidapi and most
    // games write to the file handle, which becomes an interrupt transfer;
    // others call HidD_SetOutputReport, which becomes a control transfer.
    // Whichever the driver refuses is a host that silently gets no rumble.
    SetLastError(0);
    const bool by_control = HidD_SetOutputReport(device, report.data(),
                                                 static_cast<ULONG>(report.size()));
    const DWORD control_error = GetLastError();

    DWORD written = 0;
    SetLastError(0);
    const bool by_interrupt = WriteFile(device, report.data(),
                                        static_cast<DWORD>(report.size()), &written, nullptr);
    const DWORD interrupt_error = GetLastError();

    std::printf("    HidD_SetOutputReport: %s (error %lu)\n",
                by_control ? "accepted" : "refused", control_error);
    std::printf("    WriteFile           : %s (error %lu, %lu bytes)\n",
                by_interrupt ? "accepted" : "refused", interrupt_error, written);

    const bool sent = by_control || by_interrupt;
    check(sent, "the host can write an output report");

    if (sent && await_feedback(client, slot, &event)) {
      check(event.type == lvg::feedback_type::playstation_output,
            "the driver reports it as PlayStation output");

      lvg::playstation_output_feedback fb {};
      std::memcpy(&fb, event.payload, sizeof(fb));

      if (t.profile == lvg::profile::dualsense) {
        check(fb.low_frequency == 0x8000, "the left motor arrives as low frequency");
        check(fb.high_frequency == 0x4000, "the right motor arrives as high frequency");
        check(fb.red == 0x10 && fb.green == 0x20 && fb.blue == 0x30,
              "the light bar colour arrives intact");
        check(fb.player_leds == 0x05, "the player LEDs arrive");
        check(fb.microphone_led == 0x01, "the microphone LED arrives");
        check(fb.right_trigger.mode == 0x26, "the right trigger effect is not the left one");
        check(fb.left_trigger.mode == 0x21, "the left trigger effect is not the right one");
      } else {
        check(fb.low_frequency == 0x9000, "the left motor arrives as low frequency");
        check(fb.high_frequency == 0x3000, "the right motor arrives as high frequency");
        check(fb.red == 0x11 && fb.green == 0x22 && fb.blue == 0x33,
              "the light bar colour arrives intact");
      }
    } else if (sent) {
      check(false, "the write reached the client as feedback");
    }

    CloseHandle(device);
    std::ignore = client.destroy_controller(slot);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::printf("\n%s\n", g_failures == 0 ? "OUTPUT PATH OK" : "OUTPUT PATH FAILED");
  return g_failures == 0 ? 0 : 2;
}
