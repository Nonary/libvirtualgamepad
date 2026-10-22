// SPDX-License-Identifier: MIT
#pragma once
#include <utility>

template<class F> class scope_exit {
 public:
  explicit scope_exit(F action): action_(std::move(action)) {}
  scope_exit(const scope_exit &) = delete;
  scope_exit &operator=(const scope_exit &) = delete;
  ~scope_exit() noexcept { action_(); }
 private:
  F action_;
};
