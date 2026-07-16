// SPDX-License-Identifier: Apache-2.0
// Aevum - Perfetto / atrace instrumentation.
//
// Exports Aevum's internal phases (compact, mark, evacuate, fixup, reclaim,
// commit) to the Android system tracer via android/trace.h, so a Perfetto
// capture shows exactly where compaction time goes and - crucially - that ART's
// GC slices never overlap Aevum memory. On non-Android hosts every macro
// compiles to nothing.
//
// Hot-path discipline: a trace scope first checks ATrace_isEnabled() (a single
// relaxed load of a global the platform flips when a trace is active); when no
// trace is running the scope does no syscall and no string work, so mutator
// fast paths pay at most one predictable branch. Section names are compile-time
// string literals - never formatted - so there is no per-event allocation.

#ifndef AEVUM_PROFILER_H_
#define AEVUM_PROFILER_H_

#include <cstdint>

namespace aevum {

// Thin wrappers so the rest of the engine never includes android/trace.h
// directly and the host build links cleanly.
bool traceEnabled() noexcept;                 // ATrace_isEnabled(), or false off-Android
void traceBegin(const char* section) noexcept;  // ATrace_beginSection
void traceEnd() noexcept;                      // ATrace_endSection
void traceCounter(const char* name, int64_t value) noexcept;  // ATrace_setCounter (API 29+)

// RAII trace scope. Records whether it opened a section so it closes exactly the
// sections it opened, even if tracing was toggled mid-scope.
class ScopedTrace {
 public:
  explicit ScopedTrace(const char* section) noexcept : open_(traceEnabled()) {
    if (open_) traceBegin(section);
  }
  ~ScopedTrace() noexcept { if (open_) traceEnd(); }
  ScopedTrace(const ScopedTrace&) = delete;
  ScopedTrace& operator=(const ScopedTrace&) = delete;

 private:
  bool open_;
};

}  // namespace aevum

// AEVUM_TRACE("name") opens a system-trace section for the enclosing C++ scope.
// Compiles to nothing when built without atrace (non-Android) so there is not
// even a branch on the host.
#if defined(__ANDROID__)
#  define AEVUM_TRACE(name)  ::aevum::ScopedTrace AEVUM_TRACE_CAT(_aev_tr_, __LINE__)(name)
#  define AEVUM_TRACE_CAT(a, b) AEVUM_TRACE_CAT_(a, b)
#  define AEVUM_TRACE_CAT_(a, b) a##b
#  define AEVUM_COUNTER(name, v) ::aevum::traceCounter((name), (v))
#else
#  define AEVUM_TRACE(name) do {} while (0)
#  define AEVUM_COUNTER(name, v) do {} while (0)
#endif

#endif  // AEVUM_PROFILER_H_
