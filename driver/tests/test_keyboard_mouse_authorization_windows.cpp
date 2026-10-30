// Copyright (c) 2026 Chase Payne
// SPDX-License-Identifier: MIT
// Run the shipping callback against real Windows identification tokens. This
// is not a test of WDF requestor capture and never opens a driver device.
#include <windows.h>

#include <cstdio>
#include <vector>

using WDFREQUEST = void *;
using EVT_WDF_REQUEST_IMPERSONATE = void(WDFREQUEST, PVOID);
#include "keyboard_mouse_authorization_callback.inc"

namespace {
  struct token_handle_t {
    HANDLE value {};
    ~token_handle_t() { if (value) CloseHandle(value); }
    token_handle_t(const token_handle_t &) = delete;
    token_handle_t &operator=(const token_handle_t &) = delete;
    token_handle_t() = default;
  };

  bool user_is_system(HANDLE token, bool &is_system) {
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length < sizeof(TOKEN_USER)) return false;
    std::vector<unsigned char> buffer(length);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), length, &length)) return false;
    is_system = IsWellKnownSid(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, WinLocalSystemSid) != FALSE;
    return true;
  }

  bool check_callback(HANDLE token, bool expected, const char *label) {
    if (!SetThreadToken(nullptr, token)) {
      std::fprintf(stderr, "%s: SetThreadToken failed: %lu\n", label, GetLastError());
      return false;
    }
    bool allowed = false;
    evt_keyboard_mouse_authorize(nullptr, &allowed);
    const bool reverted = RevertToSelf() != FALSE;
    if (!reverted || allowed != expected) {
      std::fprintf(stderr, "%s: expected allowed=%d, got %d (reverted=%d)\n", label, expected, allowed, reverted);
      return false;
    }
    return true;
  }
}

int main() {
  // The driver initializes this result to false, so missing impersonation
  // must never grant access.
  bool allowed_without_token = false;
  evt_keyboard_mouse_authorize(nullptr, &allowed_without_token);
  if (allowed_without_token) {
    std::fputs("A missing thread token was authorized\n", stderr);
    return 1;
  }

  token_handle_t process, identification, restricted, restricted_identification;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process.value) ||
      !DuplicateTokenEx(process.value, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
                        SecurityIdentification, TokenImpersonation, &identification.value)) {
    std::fprintf(stderr, "Could not create an identification token: %lu\n", GetLastError());
    return 1;
  }

  alignas(void *) unsigned char admin_sid[SECURITY_MAX_SID_SIZE] {};
  DWORD sid_size = sizeof(admin_sid);
  BOOL is_admin = FALSE;
  bool is_system = false;
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &sid_size) ||
      !CheckTokenMembership(identification.value, admin_sid, &is_admin) ||
      !user_is_system(identification.value, is_system)) {
    std::fprintf(stderr, "Could not inspect identification-token membership: %lu\n", GetLastError());
    return 1;
  }
  if (!check_callback(identification.value, is_system || is_admin != FALSE, "current token")) return 1;

  SID_AND_ATTRIBUTES disabled_admin {admin_sid, 0};
  if (!CreateRestrictedToken(process.value, DISABLE_MAX_PRIVILEGE, 1, &disabled_admin,
                             0, nullptr, 0, nullptr, &restricted.value) ||
      !DuplicateTokenEx(restricted.value, TOKEN_QUERY | TOKEN_IMPERSONATE, nullptr,
                        SecurityIdentification, TokenImpersonation, &restricted_identification.value)) {
    std::fprintf(stderr, "Could not create a restricted identification token: %lu\n", GetLastError());
    return 1;
  }
  // A restricted LocalSystem token is still LocalSystem. Otherwise, disabling
  // the Administrators SID must make the production callback deny it.
  if (!check_callback(restricted_identification.value, is_system, "restricted token")) return 1;
  std::puts("Real Windows identification-token authorization passed");
  return 0;
}
