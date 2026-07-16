// SPDX-License-Identifier: Apache-2.0
#include "trace_emitter.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(__APPLE__)
#  include <pthread.h>
#endif
#if defined(__linux__)
#  include <sys/syscall.h>
#endif

namespace aevum::lab {
namespace {

// A per-thread id that is stable and distinct, for the trace's tid field.
uint64_t threadId() noexcept {
#if defined(__linux__)
  return static_cast<uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
  uint64_t t = 0;
  pthread_threadid_np(nullptr, &t);
  return t;
#else
  static std::atomic<uint64_t> ctr{1};
  static thread_local uint64_t id = ctr.fetch_add(1, std::memory_order_relaxed);
  return id;
#endif
}

std::string escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\t': o += "\\t";  break;
      default:   o += c;      break;
    }
  }
  return o;
}

}  // namespace

uint64_t traceNowUs() noexcept {
  // steady_clock maps to CLOCK_MONOTONIC (Linux) / mach_absolute_time (Darwin);
  // its epoch is boot-based and therefore shared by every process on the machine,
  // which is what lets separately-emitted fragments share one timeline.
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

TraceEmitter::TraceEmitter(const std::string& fragment_path,
                           const std::string& process_label) {
  fd_ = ::open(fragment_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd_ < 0) return;
  // Name this process's track in the UI.
  std::ostringstream os;
  os << R"({"name":"process_name","ph":"M","pid":)" << ::getpid()
     << R"(,"tid":0,"args":{"name":")" << escape(process_label) << R"("}})";
  writeLine(os.str());
}

TraceEmitter::~TraceEmitter() {
  if (fd_ >= 0) ::close(fd_);
}

void TraceEmitter::writeLine(const std::string& s) noexcept {
  if (fd_ < 0) return;
  std::string line = s;
  line.push_back('\n');
  // One O_APPEND write(2): atomic w.r.t. the file offset, so concurrent threads
  // and processes interleave whole records and never tear one. No lock taken.
  const char* p = line.data();
  size_t n = line.size();
  while (n > 0) {
    ssize_t w = ::write(fd_, p, n);
    if (w <= 0) break;              // a killed process simply stops mid-stream
    p += w;
    n -= static_cast<size_t>(w);
  }
}

void TraceEmitter::complete(const char* name, uint64_t ts_us, uint64_t dur_us,
                            const std::string& args_json) noexcept {
  std::ostringstream os;
  os << R"({"name":")" << name << R"(","cat":"aevum","ph":"X","ts":)" << ts_us
     << R"(,"dur":)" << dur_us << R"(,"pid":)" << ::getpid()
     << R"(,"tid":)" << threadId();
  if (!args_json.empty()) os << R"(,"args":)" << args_json;
  os << "}";
  writeLine(os.str());
}

void TraceEmitter::instant(const char* name, uint64_t ts_us,
                           const std::string& args_json) noexcept {
  std::ostringstream os;
  os << R"({"name":")" << name << R"(","cat":"aevum","ph":"i","s":"g","ts":)" << ts_us
     << R"(,"pid":)" << ::getpid() << R"(,"tid":)" << threadId();
  if (!args_json.empty()) os << R"(,"args":)" << args_json;
  os << "}";
  writeLine(os.str());
}

void TraceEmitter::counter(const char* name, uint64_t ts_us, const char* series,
                           int64_t value) noexcept {
  std::ostringstream os;
  os << R"({"name":")" << name << R"(","cat":"aevum","ph":"C","ts":)" << ts_us
     << R"(,"pid":)" << ::getpid() << R"(,"tid":0,"args":{")" << series << R"(":)"
     << value << "}}";
  writeLine(os.str());
}

size_t mergeFragments(const std::vector<std::string>& fragment_paths,
                      const std::string& out_json_path) {
  std::ofstream out(out_json_path, std::ios::trunc);
  if (!out) return 0;
  out << "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n";

  size_t count = 0;
  for (const auto& path : fragment_paths) {
    std::ifstream in(path);
    if (!in) continue;
    std::string line;
    while (std::getline(in, line)) {
      // Discard any incomplete record: a process SIGKILLed mid-write leaves at
      // most one truncated trailing line. A valid record is a single JSON object.
      if (line.size() < 2 || line.front() != '{' || line.back() != '}') continue;
      if (count++) out << ",\n";
      out << "  " << line;
    }
  }
  out << "\n]}\n";
  return count;
}

}  // namespace aevum::lab
