// LiDE50 Scan - progress of long work (saving).
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#include "progress.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace {

struct Range {
    double from, to;   // absolute, 0..1 of the whole job
};

thread_local ProgressReport* t_report = nullptr;
thread_local std::vector<Range> t_stack;

Range current()
{
    return t_stack.empty() ? Range{0.0, 1.0} : t_stack.back();
}

} // namespace

// ------------------------------------------------------------------ report

double ProgressReport::fraction() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    double f = done_;
    if (creeping_ && creep_to_ > creep_from_) {
        const double t = std::chrono::duration<double>(Clock::now() - creep_start_).count();
        // eases out: about 63% of the step after the expected time, at most 95% of it
        const double share = std::min(0.95, 1.0 - std::exp(-t / std::max(0.05, creep_seconds_)));
        f = std::max(f, creep_from_ + (creep_to_ - creep_from_) * share);
    }
    shown_ = std::max(shown_, std::clamp(f, 0.0, 1.0));
    return shown_;
}

void ProgressReport::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = shown_ = 0.0;
    creeping_ = false;
}

void ProgressReport::set_done(double f)
{
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = std::max(done_, std::clamp(f, 0.0, 1.0));
}

void ProgressReport::set_creep(double from, double to, double expected_seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    creeping_ = true;
    creep_from_ = from;
    creep_to_ = to;
    creep_seconds_ = expected_seconds;
    creep_start_ = Clock::now();
}

void ProgressReport::clear_creep()
{
    std::lock_guard<std::mutex> lock(mutex_);
    creeping_ = false;
}

// ------------------------------------------------------------------ worker

void progress_bind(ProgressReport* report)
{
    t_report = report;
    t_stack.clear();
}

ProgressStep::ProgressStep(double from, double to, double expected_seconds)
{
    if (!t_report) return;
    const Range outer = current();
    from = std::clamp(from, 0.0, 1.0);
    to = std::clamp(to, from, 1.0);
    abs_from_ = outer.from + (outer.to - outer.from) * from;
    abs_to_ = outer.from + (outer.to - outer.from) * to;
    t_stack.push_back({abs_from_, abs_to_});
    active_ = true;
    t_report->set_done(abs_from_);
    if (expected_seconds > 0.0) {
        creep_ = true;
        t_report->set_creep(abs_from_, abs_to_, expected_seconds);
    }
}

ProgressStep::~ProgressStep()
{
    if (!active_ || !t_report) return;
    if (creep_) t_report->clear_creep();
    t_report->set_done(abs_to_);
    if (!t_stack.empty()) t_stack.pop_back();
}

void progress(double f)
{
    if (!t_report) return;
    const Range r = current();
    t_report->set_done(r.from + (r.to - r.from) * std::clamp(f, 0.0, 1.0));
}

void ProgressParts::next(double weight, double expected_seconds)
{
    finish();
    const double from = at_ / total_;
    at_ += std::max(0.0, weight);
    const double to = std::min(1.0, at_ / total_);
    step_ = new (storage_) ProgressStep(from, to, expected_seconds);
}

void ProgressParts::finish()
{
    if (step_) {
        step_->~ProgressStep();
        step_ = nullptr;
    }
}
