// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>

// Used by the native probe and its portable regression. A cycle qualifies only
// if it started after a resume event and all three client operations succeeded.
struct sleep_churn_progress {
  std::atomic<long> total {0}, post_resume {0};
  void completed(bool started_after_resume, bool submitted, bool destroyed) noexcept {
    if (!submitted || !destroyed) return;
    ++total;
    if (started_after_resume) ++post_resume;
  }
  bool recovered() const noexcept { return post_resume.load() > 0; }
};
