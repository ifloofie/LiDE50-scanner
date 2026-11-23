// LiDE50 Scan - progress of long work (saving), reported by the worker thread and
// read by the window.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
//
// The worker binds a ProgressReport to its thread, then divides the work into nested
// steps. Code deep inside (an encoder, a blur) only calls progress(f) with 0..1 for
// the step it is in; it doesn't need to know where that step sits in the whole job.
// On a thread with no report bound, every call here does nothing (tests, other threads).
//
// Some steps can't report as they go (the JPEG encoder is a library call). Those are
// opened with an expected duration: the bar then creeps through the step at that pace
// and slows down near its end, and jumps to the end when the step really finishes.
//
// Portable C++ (no Windows calls), so the encoders stay testable on any platform.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

class ProgressReport {
public:
    // Overall fraction done, 0..1, including the creep of a step that can't report.
    // Never goes backwards.
    double fraction() const;
    // Starts again at 0.
    void reset();

    // used by the worker side (progress.cpp)
    void set_done(double f);
    void set_creep(double from, double to, double expected_seconds);
    void clear_creep();

private:
    using Clock = std::chrono::steady_clock;
    mutable std::mutex mutex_;
    double done_ = 0.0;
    mutable double shown_ = 0.0;
    bool creeping_ = false;
    double creep_from_ = 0.0, creep_to_ = 0.0, creep_seconds_ = 1.0;
    Clock::time_point creep_start_{};
};

// Reports from this thread go to `report` (nullptr: stop reporting).
void progress_bind(ProgressReport* report);

// A part of the enclosing step, from `from` to `to` (both 0..1 of the enclosing step).
// With `expected_seconds` > 0 the step can't report and the bar creeps through it.
// When the step ends (destructor), the bar is at its end.
class ProgressStep {
public:
    ProgressStep(double from, double to, double expected_seconds = 0.0);
    ~ProgressStep();
    ProgressStep(const ProgressStep&) = delete;
    ProgressStep& operator=(const ProgressStep&) = delete;

private:
    double abs_from_ = 0.0, abs_to_ = 0.0;
    bool creep_ = false;
    bool active_ = false;
};

// Fraction (0..1) done of the innermost step on this thread.
void progress(double f);

// Splits a step into parts by weight: parts.next(w) opens the next part, which takes
// w out of the total weight. Keeps the ProgressStep alive until the next call.
class ProgressParts {
public:
    explicit ProgressParts(double total_weight) : total_(total_weight > 0 ? total_weight : 1.0) {}
    // the next part; `expected_seconds` as for ProgressStep
    void next(double weight, double expected_seconds = 0.0);
    // ends the current part
    void finish();
    ~ProgressParts() { finish(); }

private:
    double total_, at_ = 0.0;
    ProgressStep* step_ = nullptr;
    alignas(ProgressStep) unsigned char storage_[sizeof(ProgressStep)];
};
