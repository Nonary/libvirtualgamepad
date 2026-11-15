// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Included inside driver.cpp's anonymous namespace. The native fixture supplies
// the same boundary so missing/late mapper admission exercises the real OPEN.
#if defined(LVG_KM_TEST_HOOKS)
[[nodiscard]] bool wait_for_ordered_mouse_mapper(const GUID &container_id, DWORD timeout_ms) noexcept;
#else
[[nodiscard]] bool ordered_mouse_same_container(DEVINST node, const GUID &expected) noexcept {
  GUID found {};
  DEVPROPTYPE type = 0;
  ULONG bytes = sizeof(found);
  return CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type,
    reinterpret_cast<PBYTE>(&found), &bytes, 0) == CR_SUCCESS &&
    type == DEVPROP_TYPE_GUID && bytes == sizeof(found) &&
    std::memcmp(&found, &expected, sizeof(found)) == 0;
}

[[nodiscard]] bool ordered_mouse_hardware_matches(DEVINST node) noexcept {
  // Do not accept a generic mouse, a vendor-page compatible ID, or a different
  // revision/collection. The release packager supplies the same owned IDs to
  // both producer and mapper. Zero development defaults can never bind.
  if (LVG_KM_VENDOR_ID == 0 || LVG_KM_PRODUCT_ID == 0) return false;
  wchar_t expected[80] {};
  const int length = swprintf_s(expected, _countof(expected),
    L"HID\\VID_%04X&PID_%04X&REV_%04X&Col02",
    static_cast<unsigned>(LVG_KM_VENDOR_ID), static_cast<unsigned>(LVG_KM_PRODUCT_ID),
    static_cast<unsigned>(lvg::mouse_transport::k_hardware_revision));
  if (length <= 0) return false;
  wchar_t ids[2048] {};
  DEVPROPTYPE type = 0;
  ULONG bytes = sizeof(ids);
  if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_HardwareIds, &type,
      reinterpret_cast<PBYTE>(ids), &bytes, 0) != CR_SUCCESS ||
      type != DEVPROP_TYPE_STRING_LIST || bytes < 2 * sizeof(wchar_t) ||
      bytes > sizeof(ids) || bytes % sizeof(wchar_t) != 0) return false;
  const std::size_t count = bytes / sizeof(wchar_t);
  if (ids[count - 1] != L'\0' || ids[count - 2] != L'\0') return false;
  for (const wchar_t *id = ids; *id != L'\0'; id += wcslen(id) + 1) {
    if (_wcsicmp(id, expected) == 0) return true;
  }
  return false;
}

[[nodiscard]] bool ordered_mouse_query_interfaces(const wchar_t *device_id,
    const GUID &container_id) noexcept {
  namespace mapper = lvg::mouse_mapper;
  GUID interface_id {};
  static_assert(sizeof(interface_id) == sizeof(mapper::k_interface_guid));
  std::memcpy(&interface_id, &mapper::k_interface_guid, sizeof(interface_id));
  ULONG count = 0;
  if (CM_Get_Device_Interface_List_SizeW(&count, &interface_id,
      const_cast<PWSTR>(device_id), CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
      count < 2 || count > 1024 * 1024) return false;
  auto *const list = new (std::nothrow) wchar_t[count] {};
  if (list == nullptr) return false;
  bool ready = false;
  if (CM_Get_Device_Interface_ListW(&interface_id, const_cast<PWSTR>(device_id), list,
      count, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
      list[count - 1] == L'\0' && list[count - 2] == L'\0') {
    for (const wchar_t *path = list; *path != L'\0'; path += wcslen(path) + 1) {
      // This interface exposes a status IOCTL only. Opening it never submits
      // mouse reports or takes over the mapper's private HID reader.
      const HANDLE file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (file == INVALID_HANDLE_VALUE) continue;
      mapper::status status {};
      DWORD received = 0;
      const BOOL queried = DeviceIoControl(file, mapper::k_ioctl_status,
        nullptr, 0, &status, sizeof(status), &received, nullptr);
      CloseHandle(file);
      mapper::guid expected {};
      std::memcpy(&expected, &container_id, sizeof(expected));
      if (queried && mapper::valid_ready(status, received, expected)) {
        ready = true;
        break;
      }
    }
  }
  delete[] list;
  return ready;
}

[[nodiscard]] bool ordered_mouse_mapper_present(const GUID &container_id) noexcept {
  constexpr ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
  ULONG count = 0;
  if (CM_Get_Device_ID_List_SizeW(&count, L"HID", flags) != CR_SUCCESS ||
      count < 2 || count > 1024 * 1024) return false;
  auto *const list = new (std::nothrow) wchar_t[count] {};
  if (list == nullptr) return false;
  bool ready = false;
  if (CM_Get_Device_ID_ListW(L"HID", list, count, flags) == CR_SUCCESS &&
      list[count - 1] == L'\0' && list[count - 2] == L'\0') {
    for (wchar_t *id = list; *id != L'\0'; id += wcslen(id) + 1) {
      DEVINST node = 0;
      // Resolve each candidate from the current snapshot: device removal or
      // malformed properties never fall back to a different container.
      if (CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
          ordered_mouse_same_container(node, container_id) &&
          ordered_mouse_hardware_matches(node)) {
        // The sideband status PDO is a direct child of this exact HID TLC.
        // The MouseClass stack may own exclusive opens on its parent, so do
        // not open that stack or accept a status interface elsewhere.
        DEVINST child = 0;
        if (CM_Get_Child(&child, node, 0) != CR_SUCCESS) continue;
        for (unsigned children = 0; children < 64; ++children) {
          wchar_t child_id[512] {};
          if (ordered_mouse_same_container(child, container_id) &&
              CM_Get_Device_IDW(child, child_id, _countof(child_id), 0) == CR_SUCCESS &&
              child_id[0] != L'\0' && child_id[_countof(child_id) - 1] == L'\0' &&
              ordered_mouse_query_interfaces(child_id, container_id)) {
            ready = true;
            break;
          }
          DEVINST sibling = 0;
          if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS || sibling == child) break;
          child = sibling;
        }
        if (ready) break;
      }
    }
  }
  delete[] list;
  return ready;
}

[[nodiscard]] bool wait_for_ordered_mouse_mapper(const GUID &container_id, DWORD timeout_ms) noexcept {
  if (LVG_KM_VENDOR_ID == 0 || LVG_KM_PRODUCT_ID == 0) return false;
  // PnP startup is asynchronous. Bound retries without inferring readiness
  // from capability flags or observing cursor/global button state.
  const ULONGLONG started = GetTickCount64();
  const DWORD timeout = (std::min)(timeout_ms, DWORD {5000});
  do {
    if (ordered_mouse_mapper_present(container_id)) return true;
    const ULONGLONG elapsed = GetTickCount64() - started;
    if (elapsed >= timeout) return false;
    Sleep(static_cast<DWORD>((std::min)(ULONGLONG {10}, timeout - elapsed)));
  } while (true);
}
#endif
