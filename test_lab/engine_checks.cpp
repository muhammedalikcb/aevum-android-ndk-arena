// SPDX-License-Identifier: Apache-2.0
// Aevum headless test lab - focused engine checks.
//
// Small, single-process host tests for two properties the LMKD simulator does
// not exercise:
//
//   1. Managed-heap bounds validation on the C ABI that JNI sits on:
//      aev_mh_register_layout rejects bad layouts, aev_mh_read / begin_write /
//      capacity reject refs outside the heap, and aev_mh_read_ref / write_ref
//      reject field offsets that are not registered reference fields, without
//      touching the object.
//   2. errno preservation in the dirty tracker's SIGSEGV/SIGBUS handler, both for
//      a tracked write fault and for a fault chained to a previous handler.
//
// Usage: engine_checks <outdir>
// Prints one CHECK line per assertion and a final "CHECKS passed=N failed=M";
// exits non-zero if anything failed.

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "aevum/aevum.h"

namespace {

int g_passed = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
  std::printf("CHECK %s %s\n", ok ? "ok  " : "FAIL", what);
  if (ok) ++g_passed; else ++g_failed;
}

// ---------------------------------------------------------------------------
// 1. registerLayout validation
// ---------------------------------------------------------------------------
constexpr uint32_t kNode = 7;       // [value: u64 @0][next: ref @8][pad: u64 @16] = 24 B
constexpr uint32_t kNodeSize = 24;
constexpr uint32_t kOffNext = 8;
constexpr uint32_t kSpare = 8;      // used to prove a rejected call keeps the old layout

void checkRegisterLayout() {
  const uint32_t next[] = { kOffNext };
  check(aev_mh_register_layout(kNode, next, 1, kNodeSize) == AEV_OK,
        "registerLayout: valid layout accepted");
  check(aev_mh_register_layout(kNode, next, 1, kNodeSize) == AEV_OK,
        "registerLayout: re-registering the same layout is idempotent");

  const uint32_t none[] = { 0 };
  check(aev_mh_register_layout(kSpare, none, 0, 4) == AEV_OK,
        "registerLayout: small type with no ref fields accepted");
  check(aev_mh_register_layout(kSpare, none, 0, 0) == AEV_INVALID_ARG,
        "registerLayout: size 0 rejected");
  check(aev_mh_register_layout(kSpare, none, 0, 0x80000000u) == AEV_INVALID_ARG,
        "registerLayout: size from a negative jint rejected");
  check(aev_mh_register_layout(kSpare, none, 1, 4) == AEV_INVALID_ARG,
        "registerLayout: ref field larger than the object rejected");

  const uint32_t at_end[] = { kNodeSize };
  check(aev_mh_register_layout(kSpare, at_end, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: offset == size rejected");
  const uint32_t straddle[] = { 20 };
  check(aev_mh_register_layout(kSpare, straddle, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: field straddling the end rejected");
  const uint32_t unaligned[] = { 4 };
  check(aev_mh_register_layout(kSpare, unaligned, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: unaligned offset rejected");
  const uint32_t wrap[] = { 0xFFFFFFF8u };
  check(aev_mh_register_layout(kSpare, wrap, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: wrapping offset rejected");
  const uint32_t dup[] = { 8, 0, 8 };
  check(aev_mh_register_layout(kSpare, dup, 3, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: duplicate offset rejected");
  uint32_t many[33];
  for (uint32_t i = 0; i < 33; ++i) many[i] = i * 8;
  check(aev_mh_register_layout(kSpare, many, 33, 33 * 8) == AEV_INVALID_ARG,
        "registerLayout: more than 32 ref fields rejected");
  check(aev_mh_register_layout(kSpare, many, 32, 32 * 8) == AEV_OK,
        "registerLayout: exactly 32 ref fields accepted");
  check(aev_mh_register_layout(kSpare, nullptr, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: null offsets with count > 0 rejected");
  check(aev_mh_register_layout(1024, next, 1, kNodeSize) == AEV_INVALID_ARG,
        "registerLayout: type id out of range rejected");
}

// ---------------------------------------------------------------------------
// 2. managed-heap ref / size / field-offset validation
// ---------------------------------------------------------------------------
void checkManagedHeap(const std::string& outdir) {
  const std::string path = outdir + "/engine_checks.region";
  ::remove(path.c_str());
  constexpr uint64_t kRegion = 8ull << 20;
  int32_t st = 0;
  aev_arena* a = aev_create_file(path.c_str(), kRegion, &st);
  check(a != nullptr && st == AEV_OK, "heap: arena created");
  if (!a) return;
  aev_mheap* h = aev_mh_create(a, 1ull << 20, &st);
  check(h != nullptr && st == AEV_OK, "heap: managed heap created");
  if (!h) { aev_close(a); return; }

  // kSpare now holds the 32-field layout from above; the rejected calls after it
  // must not have replaced it.
  const aev_handle spare = aev_mh_alloc(h, kSpare);
  check(spare != AEV_NULL && aev_mh_capacity(h, spare) == 32 * 8,
        "heap: rejected registerLayout kept the earlier layout");

  const aev_handle obj = aev_mh_alloc(h, kNode);
  const aev_handle child = aev_mh_alloc(h, kNode);
  check(obj != AEV_NULL && child != AEV_NULL, "heap: objects allocated");

  // Capacity is the registered size rounded up to 16: the JNI view limit.
  check(aev_mh_capacity(h, obj) == 32, "capacity: valid ref reports 32 bytes");
  check(aev_mh_capacity(h, AEV_NULL) == 0, "capacity: null ref reports 0");
  check(aev_mh_capacity(h, obj + 8) == 0, "capacity: misaligned ref reports 0");
  check(aev_mh_capacity(h, kRegion - 16) == 0, "capacity: ref past the heap reports 0");
  check(aev_mh_capacity(h, kRegion + 4096) == 0, "capacity: ref past the mapping reports 0");
  check(aev_mh_capacity(h, ~0ull & ~15ull) == 0, "capacity: huge ref reports 0");

  check(aev_mh_read(h, obj) != nullptr, "read: valid ref resolves");
  check(aev_mh_read(h, obj + 8) == nullptr, "read: misaligned ref rejected");
  check(aev_mh_read(h, kRegion + 4096) == nullptr, "read: ref past the mapping rejected");
  check(aev_mh_read(h, 16) == nullptr, "read: ref inside the arena header rejected");

  void* w = aev_mh_begin_write(h, kRegion + 4096);
  aev_mh_end_write(h);
  check(w == nullptr, "beginWrite: ref past the mapping rejected (and pairs with endWrite)");
  w = aev_mh_begin_write(h, obj);
  if (w) std::memset(w, 0xAB, 8);   // the value field only
  aev_mh_end_write(h);
  check(w != nullptr, "beginWrite: valid ref resolves");

  // Valid field: round trip.
  check(aev_mh_write_ref(h, obj, kOffNext, child) == AEV_OK, "writeRef: registered field accepted");
  check(aev_mh_read_ref(h, obj, kOffNext) == child, "readRef: registered field returns child");

  // Snapshot the payload, then throw bad offsets and refs at it.
  unsigned char before[32];
  std::memcpy(before, aev_mh_read(h, obj), sizeof(before));

  check(aev_mh_read_ref(h, obj, 0) == AEV_NULL, "readRef: unregistered in-bounds offset rejected");
  check(aev_mh_read_ref(h, obj, 16) == AEV_NULL, "readRef: unregistered tail offset rejected");
  check(aev_mh_read_ref(h, obj, 32) == AEV_NULL, "readRef: offset at object end rejected");
  check(aev_mh_read_ref(h, obj, 4096) == AEV_NULL, "readRef: offset far past object rejected");
  check(aev_mh_read_ref(h, obj, 0xFFFFFFF8u) == AEV_NULL, "readRef: wrapping offset rejected");
  check(aev_mh_read_ref(h, obj, 9) == AEV_NULL, "readRef: unaligned offset rejected");
  check(aev_mh_read_ref(h, kRegion + 4096, kOffNext) == AEV_NULL, "readRef: bad ref rejected");

  check(aev_mh_write_ref(h, obj, 0, child) == AEV_INVALID_ARG,
        "writeRef: unregistered in-bounds offset rejected");
  check(aev_mh_write_ref(h, obj, 16, child) == AEV_INVALID_ARG,
        "writeRef: unregistered tail offset rejected");
  check(aev_mh_write_ref(h, obj, 4096, child) == AEV_INVALID_ARG,
        "writeRef: offset far past object rejected");
  check(aev_mh_write_ref(h, obj, 0xFFFFFFF8u, child) == AEV_INVALID_ARG,
        "writeRef: wrapping offset rejected");
  check(aev_mh_write_ref(h, kRegion + 4096, kOffNext, child) == AEV_INVALID_ARG,
        "writeRef: bad ref rejected");
  check(aev_mh_write_ref(h, obj, kOffNext, kRegion + 4096) == AEV_INVALID_ARG,
        "writeRef: bad child rejected");
  check(std::memcmp(before, aev_mh_read(h, obj), sizeof(before)) == 0,
        "writeRef: rejected writes left the object untouched");
  check(aev_mh_read_ref(h, obj, kOffNext) == child, "readRef: link intact after rejections");

  check(aev_mh_set_root(h, 0, kRegion + 4096) == AEV_INVALID_ARG, "setRoot: bad ref rejected");
  check(aev_mh_set_root(h, 0, obj) == AEV_OK && aev_mh_get_root(h, 0) == obj,
        "setRoot: valid ref accepted");
  check(aev_mh_set_root(h, 0, AEV_NULL) == AEV_OK, "setRoot: null accepted");

  // A compaction must still see a sane graph after all the rejected calls.
  check(aev_mh_set_root(h, 0, obj) == AEV_OK, "compact: root set");
  uint64_t live = 0, recl = 0;
  const int32_t cs = aev_mh_compact(h, &live, &recl);
  const aev_handle root = aev_mh_get_root(h, 0);
  check(cs == AEV_OK && live == 2 && aev_mh_capacity(h, aev_mh_read_ref(h, root, kOffNext)) == 32,
        "compact: graph intact after rejected calls");

  aev_mh_close(h);
  aev_close(a);
  ::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 3. errno preservation in the write-fault handler
// ---------------------------------------------------------------------------
constexpr int kSentinel = 4242;      // not a real errno value

long g_page = 0;
unsigned char* volatile g_guard = nullptr;
volatile sig_atomic_t g_app_faults = 0;

// Stands in for an app's own SIGSEGV/SIGBUS handler that Aevum chains to. It
// services faults on its guard page and, like many real handlers, makes a libc
// call that fails and sets errno.
void appHandler(int, siginfo_t* si, void*) {
  unsigned char* addr = static_cast<unsigned char*>(si ? si->si_addr : nullptr);
  unsigned char* g = g_guard;
  if (g && addr >= g && addr < g + g_page) {
    ::mprotect(g, static_cast<size_t>(g_page), PROT_READ | PROT_WRITE);
    ::close(-1);                     // EBADF: clobbers errno on purpose
    g_app_faults = g_app_faults + 1;
    return;
  }
  ::_exit(99);                       // a genuine crash in the test itself
}

void checkErrno(const std::string& outdir) {
  g_page = ::sysconf(_SC_PAGESIZE);

  // Install the "app" handler first so Aevum records it as the previous one.
  struct sigaction sa{};
  sa.sa_sigaction = appHandler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  check(::sigaction(SIGSEGV, &sa, nullptr) == 0 && ::sigaction(SIGBUS, &sa, nullptr) == 0,
        "errno: app fault handler installed");

  const std::string path = outdir + "/engine_checks_errno.region";
  ::remove(path.c_str());
  int32_t st = 0;
  aev_arena* a = aev_create_file(path.c_str(), 4ull << 20, &st);
  check(a != nullptr, "errno: arena created");
  if (!a) return;
  const aev_handle blk = aev_alloc(a, 256);
  auto* p = static_cast<volatile unsigned char*>(aev_resolve(a, blk));
  const int32_t inc = aev_enable_incremental(a);
  if (inc == AEV_PROBE_FAILED || inc == AEV_UNSUPPORTED) {
    std::printf("CHECK skip errno: incremental checkpoint unavailable on this host (%d)\n", inc);
    aev_close(a);
    ::remove(path.c_str());
    return;
  }
  check(inc == AEV_OK && p != nullptr, "errno: incremental checkpoint armed");

  // (a) Tracked write fault: Aevum's own path (mprotect, then return).
  errno = kSentinel;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  p[0] = 0x5A;                       // first write to a write-protected page: faults
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const int after_tracked = errno;
  check(p[0] == 0x5A, "errno: tracked write landed");
  check(after_tracked == kSentinel, "errno: preserved across a tracked write fault");

  // (b) Untracked fault: Aevum chains to the app handler, which clobbers errno.
  void* m = ::mmap(nullptr, static_cast<size_t>(g_page), PROT_READ,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  check(m != MAP_FAILED, "errno: guard page mapped");
  if (m != MAP_FAILED) {
    g_guard = static_cast<unsigned char*>(m);
    volatile unsigned char* gp = g_guard;
    errno = kSentinel;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gp[0] = 0x33;                    // not an Aevum range: chained to appHandler
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const int after_chain = errno;
    check(g_app_faults == 1 && gp[0] == 0x33, "errno: fault chained to the app handler");
    check(after_chain == kSentinel, "errno: preserved across a chained fault");
    g_guard = nullptr;
    ::munmap(m, static_cast<size_t>(g_page));
  }

  check(aev_commit(a) == AEV_OK, "errno: incremental commit succeeds afterwards");
  aev_close(a);
  ::remove(path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string outdir = (argc > 1) ? argv[1] : ".";
  checkRegisterLayout();
  checkManagedHeap(outdir);
  checkErrno(outdir);
  std::printf("CHECKS passed=%d failed=%d\n", g_passed, g_failed);
  std::fflush(stdout);
  return g_failed == 0 ? 0 : 1;
}
