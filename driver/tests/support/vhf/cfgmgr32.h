// SPDX-License-Identifier: MIT
#pragma once
#include "windows.h"
#include <functional>
#include <vector>
using CONFIGRET = ULONG;
inline constexpr ULONG CM_GETIDLIST_FILTER_ENUMERATOR = 1, CM_GETIDLIST_FILTER_PRESENT = 2;
inline constexpr CONFIGRET CR_SUCCESS = 0, CR_BUFFER_SMALL = 1, CR_FAILURE = 2;
inline CONFIGRET fake_cm_size_result = CR_SUCCESS, fake_cm_list_result = CR_SUCCESS;
inline std::vector<wchar_t> fake_cm_ids {0, 0};
inline int fake_cm_queries = 0;
inline std::function<void()> fake_cm_hook;
inline CONFIGRET CM_Get_Device_ID_List_SizeW(ULONG *size, const wchar_t *, ULONG) {
  ++fake_cm_queries;
  *size = static_cast<ULONG>(fake_cm_ids.size());
  if (fake_cm_hook) fake_cm_hook();
  return fake_cm_size_result;
}
inline CONFIGRET CM_Get_Device_ID_ListW(const wchar_t *, wchar_t *buffer, ULONG size, ULONG) {
  if (size < fake_cm_ids.size()) return CR_BUFFER_SMALL;
  std::copy(fake_cm_ids.begin(), fake_cm_ids.end(), buffer);
  return fake_cm_list_result;
}
