// SPDX-License-Identifier: Apache-2.0
// Aevum headless test lab - autonomous trace emitter.
//
// Produces a Chrome Trace Event / Perfetto-compatible trace WITHOUT a device and
// without atrace: every process in the lab streams its own events, and the driver
// merges them into one `.json` that opens directly in https://ui.perfetto.dev.
//
// Two properties matter here and drive the design:
//
//  1. A traced process is about to be SIGKILLed. SIGKILL is uncatchable - no
//     destructor, no atexit, no buffered flush runs. So events are written
//     INCREMENTALLY as NDJSON (one JSON object per line) with a single O_APPEND
//     write(2) per event. Whatever was written before the kill survives on disk;
//     at worst the final partial line is lost, and the merger discards it. The
//     app's trace therefore survives the app's own SIGKILL, like a real
//     ring-buffer capture.
//  2. No locks. O_APPEND + one write(2) per record is atomic with respect to the
//     file offset, so concurrent threads (and processes) append safely with no
//     mutex on the emitting path.
//
// Timestamps come from a system-wide monotonic clock, so slices emitted by
// different processes land on one shared timeline.

#ifndef AEVUM_TEST_LAB_TRACE_EMITTER_H_
#define AEVUM_TEST_LAB_TRACE_EMITTER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace aevum::lab {

// Microseconds on a process-shared monotonic timeline.
uint64_t traceNowUs() noexcept;

/// Streams Chrome Trace Events as NDJSON to a fragment file. Lock-free.
class TraceEmitter {
 public:
  /// Opens `fragment_path` for append. `process_label` names this process's track
  /// in the UI. Emits the process_name metadata record immediately.
  TraceEmitter(const std::string& fragment_path, const std::string& process_label);
  ~TraceEmitter();

  TraceEmitter(const TraceEmitter&) = delete;
  TraceEmitter& operator=(const TraceEmitter&) = delete;

  bool valid() const noexcept { return fd_ >= 0; }

  /// Complete ("X") slice: [ts, ts+dur).
  void complete(const char* name, uint64_t ts_us, uint64_t dur_us,
                const std::string& args_json = "") noexcept;
  /// Instant ("i") marker, global scope.
  void instant(const char* name, uint64_t ts_us, const std::string& args_json = "") noexcept;
  /// Counter ("C") sample.
  void counter(const char* name, uint64_t ts_us, const char* series, int64_t value) noexcept;

  /// RAII scope: emits one complete slice covering its lifetime.
  class Scope {
   public:
    Scope(TraceEmitter& e, const char* name) noexcept
        : e_(e), name_(name), t0_(traceNowUs()) {}
    ~Scope() noexcept { e_.complete(name_, t0_, traceNowUs() - t0_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
   private:
    TraceEmitter& e_;
    const char* name_;
    uint64_t t0_;
  };

 private:
  void writeLine(const std::string& s) noexcept;   // one atomic O_APPEND write(2)
  int fd_ = -1;
};

/// Merges NDJSON fragments into one Chrome/Perfetto JSON trace. Any trailing
/// partial line (a process killed mid-write) is discarded. Returns event count.
size_t mergeFragments(const std::vector<std::string>& fragment_paths,
                      const std::string& out_json_path);

}  // namespace aevum::lab

#endif  // AEVUM_TEST_LAB_TRACE_EMITTER_H_
