// SPDX-License-Identifier: Apache-2.0
// Aevum headless test lab - multi-process LMKD kill / O(1) reattach simulator.
//
// Proves Aevum's core claims on a plain host, with no Android device, by using
// the very same primitives Android uses (Android's kernel is Linux; the arena's
// mmap/file-backed persistence, fork, SIGKILL and ASLR behave identically here):
//
//   Phase 1  APP process: create a 128 MB file-backed arena, build a 100,000-node
//            linked graph, commit it durably, then run the concurrent compactor
//            and a mutator at full tilt.
//   Phase 2  LMKD kill: the driver sends SIGKILL to the app MID-COMPACTION.
//            SIGKILL is uncatchable - no destructor, no atexit, no flush, no
//            cleanup whatsoever. LMKD is an external daemon, so an external kill is the
//            faithful model; self-signalling would be semantically identical,
//            since SIGKILL cannot be caught either way.
//   Phase 3  REINCARNATION process: a fresh fork+exec (new PID, new image, new
//            ASLR layout, deliberately perturbed further) attaches to the same
//            region, times attach+getRoot to nanosecond resolution, then walks all
//            100,000 nodes and verifies every checksum.
//
// Backing MUST be file-backed: a memfd region dies with its last fd, so a killed
// process would take it with it. A private file survives the kill (and a reboot).
//
// Integrity design note: the durable commit is not transactional with respect to
// an app-level multi-field update, so counter+checksum live in ONE aligned 8-byte
// word (counter<<32 | crc32(id,counter)). A single aligned store cannot tear, so
// any recovered state is self-consistent by construction - which means any
// mismatch this test reports is a REAL torn read, not a test artifact.
//
// Modes:  (default) driver | --app <region> <frag> <fd> | --reincarnate <region> <frag> <fd>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "arena.h"
#include "compactor.h"
#include "type_registry.h"
#include "trace_emitter.h"

using namespace aevum;
using namespace aevum::lab;
using hrc = std::chrono::high_resolution_clock;

// --------------------------------------------------------------------------
// Managed node: [id: u64 @0][packed: u64 @8][next: ref @16]  -> 24-byte payload
// --------------------------------------------------------------------------
struct Node {
  uint64_t id;
  uint64_t packed;                       // (counter << 32) | crc32(id, counter)
  SelfHealingOffsetPtr<Node> next;
};
static constexpr uint32_t kType = 1;
static constexpr uint32_t kNodeSize = sizeof(Node);
static constexpr uint32_t kOffNext = 16;
static constexpr uint64_t kHotPrefix = 512;                    // mutator working set

// Graph size is configurable (AEVUM_LAB_NODES) so the runner can sweep N and show
// that reattach latency does NOT grow with it - a single data point cannot prove
// O(1). Defaults are the specified 100,000 nodes / 128 MB arena / 32 MB semispace.
static uint64_t labNodes() noexcept {
  if (const char* e = std::getenv("AEVUM_LAB_NODES")) {
    const long long v = std::atoll(e);
    if (v > 0) return static_cast<uint64_t>(v);
  }
  return 100000;
}
// Each node occupies 16 B header + 32 B (24 B payload rounded to 16). Size the
// semispaces to hold the live set plus churn slack; floor at the specified sizes.
static uint64_t labSpaceBytes() noexcept {
  const uint64_t need = labNodes() * 48ull * 2ull + (16ull << 20);
  return need > (32ull << 20) ? need : (32ull << 20);
}
static uint64_t labArenaBytes() noexcept {
  const uint64_t need = 2 * labSpaceBytes() + (16ull << 20);
  return need > (128ull << 20) ? need : (128ull << 20);
}

// Reference-field offsets, registered once per process (the reflection-free path).
static const uint32_t kRefOffsets[] = { kOffNext };
static void registerNodeType() noexcept {
  TypeRegistry::registerLayout(kType, kRefOffsets, 1, kNodeSize);
}

// Castagnoli-style 32-bit mix; identical in both processes.
static uint32_t crc32Of(uint64_t id, uint32_t counter) noexcept {
  uint64_t h = id * 0x9E3779B97F4A7C15ull;
  h ^= (static_cast<uint64_t>(counter) + 0x165667B19E3779F9ull) * 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 31;
  return static_cast<uint32_t>(h) | 1u;   // never 0
}
static uint64_t packOf(uint64_t id, uint32_t counter) noexcept {
  return (static_cast<uint64_t>(counter) << 32) | crc32Of(id, counter);
}
static bool packValid(uint64_t id, uint64_t packed) noexcept {
  const uint32_t counter = static_cast<uint32_t>(packed >> 32);
  return static_cast<uint32_t>(packed & 0xFFFFFFFFull) == crc32Of(id, counter);
}

static void writeLine(int fd, const std::string& s) {
  std::string l = s + "\n";
  ssize_t unused = ::write(fd, l.data(), l.size());
  (void)unused;
}

static std::string readAll(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
  return out;
}

static std::string field(const std::string& blob, const std::string& key) {
  const std::string k = key + "=";
  size_t p = blob.find(k);
  if (p == std::string::npos) return "";
  p += k.size();
  size_t e = blob.find_first_of(" \n", p);
  return blob.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// ==========================================================================
// Phase 1 + 2 victim: build, commit, churn, and run until SIGKILL lands.
// ==========================================================================
static int runApp(const char* region, const char* frag, int report_fd) {
  TraceEmitter tr(frag, "Aevum APP (killed by LMKD)");
  registerNodeType();

  ::remove(region);
  Arena arena;
  if (!ok(Arena::createFile(region, labArenaBytes(), &arena))) {
    writeLine(report_fd, "ERROR=create_arena");
    return 1;
  }
  ManagedHeap heap;
  if (!ok(ManagedHeap::create(&arena, labSpaceBytes(), &heap))) {
    writeLine(report_fd, "ERROR=create_heap");
    return 1;
  }

  // ---- Aevum.GraphAllocation: 100,000 live, pointer-linked nodes ----
  {
    TraceEmitter::Scope s(tr, "Aevum.GraphAllocation");
    ManagedRef prev{ManagedRef::kNull};
    ManagedRef head{ManagedRef::kNull};
    const uint64_t N = labNodes();
    for (uint64_t i = 0; i < N; ++i) {
      ManagedRef n = heap.alloc(kType);
      if (n.isNull()) { writeLine(report_fd, "ERROR=alloc"); return 1; }
      {
        WriteScope w(&heap, n);
        Node* p = static_cast<Node*>(w.get());
        p->id = i;
        p->packed = packOf(i, 0);
      }
      if (prev.isNull()) head = n;
      else {
        WriteScope w(&heap, prev);
        w.as<Node>()->next.set(static_cast<Node*>(heap.resolveRead(n)));
      }
      prev = n;
    }
    heap.setRoot(0, head);
  }
  tr.counter("Aevum.live_bytes", traceNowUs(), "bytes",
             static_cast<int64_t>(heap.liveBumpBytes()));

  // Durable checkpoint: this is the state the reincarnation must recover.
  arena.commit();

  const void* base = arena.base();
  char msg[256];
  std::snprintf(msg, sizeof(msg), "READY pid=%d base=%p nodes=%llu",
                (int)::getpid(), base, (unsigned long long)labNodes());
  writeLine(report_fd, msg);
  ::close(report_fd);   // driver's read() returns EOF; we keep running until killed

  // ---- concurrent compaction + mutation, running when the kill lands ----
  std::atomic<bool> stop{false};
  std::thread compactor([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      TraceEmitter::Scope s(tr, "Aevum.ConcurrentCompaction");
      CompactStats st;
      heap.compact(&st);
      tr.counter("Aevum.reclaimed_bytes", traceNowUs(), "bytes",
                 static_cast<int64_t>(st.reclaimed_bytes));
    }
  });
  std::thread mutator([&] {
    uint64_t x = 0x243F6A8885A308D3ull;
    while (!stop.load(std::memory_order_relaxed)) {
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;          // xorshift
      const uint64_t target = x % kHotPrefix;
      // Navigate from the durable root: refs must never be cached across a
      // compaction that moves objects.
      ManagedRef r = heap.getRoot(0);
      for (uint64_t s = 0; s < target && !r.isNull(); ++s) {
        Node* n = static_cast<Node*>(heap.resolveRead(r));
        Node* nx = n->next.get();
        r = nx ? heap.refOf(nx) : ManagedRef{ManagedRef::kNull};
      }
      if (r.isNull()) continue;
      WriteScope w(&heap, r);
      Node* n = w.as<Node>();
      if (!n) continue;
      const uint32_t c = static_cast<uint32_t>(n->packed >> 32) + 1;
      n->packed = packOf(n->id, c);        // ONE aligned 8-byte store: cannot tear
    }
  });

  // Keep the graph durable while we churn, so the kill can land at any moment.
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    arena.commit();
  }
  // unreachable: this process is destroyed by SIGKILL, with zero cleanup.
  stop.store(true);
  compactor.join();
  mutator.join();
  return 0;
}

// ==========================================================================
// Phase 3: fresh process - attach, time, verify.
// ==========================================================================
static int runReincarnate(const char* region, const char* frag, int report_fd) {
  TraceEmitter tr(frag, "Aevum REINCARNATION (new PID)");
  registerNodeType();

  // Perturb this process's mmap layout so the arena provably lands at a base
  // different from the dead process's - the position-independence proof. (exec
  // already re-randomizes under ASLR; this guarantees a shift regardless.)
  const size_t spacer = (static_cast<size_t>(::getpid() % 97) + 1) * 1024 * 1024;
  void* pad = ::mmap(nullptr, spacer, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  Arena arena;
  ManagedHeap heap;
  ManagedRef root{ManagedRef::kNull};
  double reattach_us = 0.0;

  {
    TraceEmitter::Scope outer(tr, "Aevum.O1_Reattach_And_Verify");
    {
      // Reattach is O(1): mmap + header validate + root lookup. Nothing in this
      // scope touches the 100,000 nodes.
      TraceEmitter::Scope s(tr, "Aevum.O1_Reattach");
      const auto t0 = hrc::now();
      if (!ok(Arena::attachFile(region, &arena))) { writeLine(report_fd, "ERROR=attach_arena"); return 1; }
      if (!ok(ManagedHeap::attach(&arena, &heap))) { writeLine(report_fd, "ERROR=attach_heap"); return 1; }
      root = heap.getRoot(0);
      const auto t1 = hrc::now();
      reattach_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }

    // Verification is inherently O(N) - it is the app reading its data, NOT part
    // of the reattach claim. Timed and reported separately so the two are never
    // conflated.
    uint64_t seen = 0, bad_chk = 0, bad_order = 0;
    double verify_us = 0.0;
    {
      TraceEmitter::Scope s(tr, "Aevum.VerifyGraph");
      const auto v0 = hrc::now();
      ManagedRef cur = root;
      uint64_t expect = 0;
      const uint64_t N = labNodes();
      while (!cur.isNull() && seen <= N + 4) {
        Node* n = static_cast<Node*>(heap.resolveRead(cur));
        if (!n) break;
        if (n->id != expect) ++bad_order;
        if (!packValid(n->id, n->packed)) ++bad_chk;   // a real torn read if hit
        ++seen; ++expect;
        Node* nx = n->next.get();
        cur = nx ? heap.refOf(nx) : ManagedRef{ManagedRef::kNull};
      }
      const auto v1 = hrc::now();
      verify_us = std::chrono::duration<double, std::micro>(v1 - v0).count();
    }

    char msg[512];
    std::snprintf(msg, sizeof(msg),
                  "REINCARNATED pid=%d base=%p reattach_us=%.3f verify_us=%.1f "
                  "nodes_seen=%llu bad_checksum=%llu bad_order=%llu",
                  (int)::getpid(), arena.base(), reattach_us, verify_us,
                  (unsigned long long)seen, (unsigned long long)bad_chk,
                  (unsigned long long)bad_order);
    writeLine(report_fd, msg);
  }

  if (pad != MAP_FAILED) ::munmap(pad, spacer);
  ::close(report_fd);
  return 0;
}

// ==========================================================================
// Driver
// ==========================================================================
static pid_t spawn(const char* self, const char* mode, const char* region,
                   const char* frag, int write_fd) {
  pid_t pid = ::fork();
  if (pid != 0) return pid;                      // parent
  char fdbuf[16];
  std::snprintf(fdbuf, sizeof(fdbuf), "%d", write_fd);
  // exec => a brand-new process image with freshly randomized ASLR.
  ::execl(self, self, mode, region, frag, fdbuf, (char*)nullptr);
  std::perror("execl");
  ::_exit(127);
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "--app") == 0)
    return runApp(argv[2], argv[3], std::atoi(argv[4]));
  if (argc >= 5 && std::strcmp(argv[1], "--reincarnate") == 0)
    return runReincarnate(argv[2], argv[3], std::atoi(argv[4]));

  const std::string outdir = (argc > 1) ? argv[1] : ".";
  const std::string region = outdir + "/aevum_lab.region";
  const std::string frag_drv = outdir + "/.trace_driver.ndjson";
  const std::string frag_app = outdir + "/.trace_app.ndjson";
  const std::string frag_rei = outdir + "/.trace_reincarnate.ndjson";
  const std::string out_json = outdir + "/aevum_autonomous_proof.json";
  for (const auto& f : {frag_drv, frag_app, frag_rei, out_json}) ::remove(f.c_str());

  TraceEmitter tr(frag_drv, "LMKD Driver");

  // ---------------- Phase 1: spawn the app and wait for its graph ----------
  int pfd[2];
  if (::pipe(pfd) != 0) { std::perror("pipe"); return 1; }
  const pid_t app = spawn(argv[0], "--app", region.c_str(), frag_app.c_str(), pfd[1]);
  ::close(pfd[1]);
  const std::string ready = readAll(pfd[0]);
  ::close(pfd[0]);
  if (ready.find("READY") == std::string::npos) {
    std::fprintf(stderr, "app failed to become ready: %s\n", ready.c_str());
    ::kill(app, SIGKILL); ::waitpid(app, nullptr, 0);
    return 1;
  }
  const std::string app_base = field(ready, "base");
  const std::string app_pid  = field(ready, "pid");

  // ---------------- Phase 2: the LMKD kill, mid-compaction ----------------
  // Let the compactor and mutator run so the kill lands mid-cycle, with an
  // evacuation and a mutator write both in flight.
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const uint64_t kill_ts = traceNowUs();
  ::kill(app, SIGKILL);                      // uncatchable: zero cleanup runs
  int status = 0;
  ::waitpid(app, &status, 0);
  const bool killed_by_sigkill = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
  tr.instant("OS.LMKD_SIGKILL_Event", kill_ts,
             std::string(R"({"victim_pid":)") + app_pid + R"(,"signal":"SIGKILL"})");

  // ---------------- Phase 3: reincarnate and prove O(1) -------------------
  if (::pipe(pfd) != 0) { std::perror("pipe"); return 1; }
  const pid_t rei = spawn(argv[0], "--reincarnate", region.c_str(), frag_rei.c_str(), pfd[1]);
  ::close(pfd[1]);
  const std::string res = readAll(pfd[0]);
  ::close(pfd[0]);
  int rstatus = 0;
  ::waitpid(rei, &rstatus, 0);

  const size_t events = mergeFragments({frag_drv, frag_app, frag_rei}, out_json);
  for (const auto& f : {frag_drv, frag_app, frag_rei}) ::remove(f.c_str());

  // Machine-parsable result block for run_headless_lab.sh. Every value below was
  // physically measured by the processes above.
  std::printf("RESULT nodes=%llu\n", (unsigned long long)labNodes());
  std::printf("RESULT app_pid=%s app_base=%s\n", app_pid.c_str(), app_base.c_str());
  std::printf("RESULT sigkill_ok=%d\n", killed_by_sigkill ? 1 : 0);
  std::printf("RESULT rei_pid=%s rei_base=%s\n",
              field(res, "pid").c_str(), field(res, "base").c_str());
  std::printf("RESULT reattach_us=%s\n", field(res, "reattach_us").c_str());
  std::printf("RESULT verify_us=%s\n", field(res, "verify_us").c_str());
  std::printf("RESULT nodes_seen=%s bad_checksum=%s bad_order=%s\n",
              field(res, "nodes_seen").c_str(), field(res, "bad_checksum").c_str(),
              field(res, "bad_order").c_str());
  std::printf("RESULT base_shifted=%d\n", (app_base != field(res, "base")) ? 1 : 0);
  std::printf("RESULT trace=%s events=%zu\n", out_json.c_str(), events);
  std::fflush(stdout);

  ::remove(region.c_str());
  const bool pass = killed_by_sigkill &&
                    field(res, "nodes_seen") == std::to_string(labNodes()) &&
                    field(res, "bad_checksum") == "0" && field(res, "bad_order") == "0";
  return pass ? 0 : 1;
}
