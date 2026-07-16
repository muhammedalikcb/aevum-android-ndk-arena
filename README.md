# Aevum: an experimental GC-invisible object arena for the Android NDK, built through AI-orchestrated systems engineering

*[Türkçe sürüm](README.tr.md)*

> *aevum* (scholastic Latin): the mode of existence between time and eternity;
> persistence that stays unchanged while the world around it is born and dies.

Aevum is a native engine that lets an Android app keep a live, pointer-linked
object graph in memory with two properties:

1. **The ART garbage collector never sees it.** The Concurrent Mark-Compact (CMC)
   collector never marks, scans or relocates a byte of it, so the graph adds zero
   GC root-scan time and zero compaction demand-faults, whatever its size.
2. **It survives process death in O(1).** After the OS kills and restarts the
   process, the graph is re-mapped at a fresh (ASLR-randomized) address with no
   deserialization and no pointer fixup, because every internal link is stored as
   a self-relative offset.

It only uses syscalls an unprivileged app can reach on stock Android 14/15
(`memfd_create`, `mmap`, `mprotect`, `msync`, `sigaction`, `sched_setaffinity`,
plus LSE atomics). No root, no platform signature, no blocked syscalls.

### Scope, and who should not use this

This is a proof of concept that came out of an exploratory NDK investigation. For
roughly 95% of Android apps it is overkill. If your state fits in a `ViewModel`, a
Room database or a few megabytes of JSON, use the platform's own tools. Aevum
would cost you a mutator contract (§4.3) and a native dependency to solve a
problem you don't have.

It only pays for its complexity when GC pause artifacts and state-recovery cost
dominate the workload: on-device LLM context caching, high-frequency telemetry,
large scene graphs, heavy state recovery. The rest of this README is written for
that kind of workload, and the trade-offs are written down where they apply.

### Methodology and AI transparency

Aevum is an experiment in AI-orchestrated systems engineering. The method is part
of the result, so it is documented here instead of being left for the reader to
guess.

The human architect set the problem frame and the constraints, and deliberately
held back the answer. The opening directive was to find an unsolved Android/NDK
bottleneck, with no hints. From there the architect:

- fixed the properties the design had to meet: no stop-the-world, no mutator
  blocked for even 1 µs, a lock-free visitor, cache-line isolation across
  big.LITTLE, and durability across an LMKD kill;
- fixed the engineering standards: no mocks, no stubs, no TODOs, and no made-up
  numbers (every figure in this repository comes from an executed run);
- commissioned the headless verification lab when no ARM64 hardware was
  available;
- reviewed, rejected and re-scoped the output at each stage.

The AI (Claude Opus 4.8), orchestrated by the architect, did the work inside that
frame: the subsystem survey that picked CMC's distributed fault storm as the
target, the red-team analysis that killed the first design, the memory-layout
math, the C++20 implementation, the concurrent-compaction algorithm, the
TSan/ASan/UBSan cycles, the headless proof lab, and this documentation.

That split is the experiment. It does not mean the process was clean:

- The first design, an app-side scheduler that pre-relocates pages ahead of the
  render thread, was killed by its own red team (§0) after the case for
  implementing it had already been made.
- Three data races in the compactor got through design review and were caught
  only by ThreadSanitizer. Running the code found them; the argument written to
  justify the code did not. The clean sanitizer result in §4.4 is the state
  *after* those fixes.
- An automated documentation pass corrupted this README with a search anchor that
  was too short to be unique. A structural parity check caught it. The model that
  made the edit did not.

The engine got to this state because every claim had to survive execution, not
because the generation step was reliable. Give weight to the executed results
(§3 and §4.4 below, and §6 of the [whitepaper](docs/WHITEPAPER.md)) and none to
the prose.

### Reference docs

| Document | What it is |
|---|---|
| [**WHITEPAPER**](docs/WHITEPAPER.md) | The formal systems paper: O(1)-reattach theorem, concurrent-compaction correctness, Dekker proof of the ragged safepoint, falsifiable claims, executed results |
| [**ADR-001…003**](docs/architecture/ADR-001-to-003.md) | Why no `userfaultfd`, why no JVM reflection, why LSE over CAS loops |
| [**INTEGRATION_GUIDE**](INTEGRATION_GUIDE.md) | Consuming the AAR, lifecycle wiring, `WriteScope` best practices |
| [**SECURITY**](SECURITY.md) | Threat model: sandbox isolation, memory-safety disclosures, data at rest |
| [`scripts/`](scripts/) | `field_proof_harness.sh` + `parse_perfetto.py`: one-command on-device proof |
| [**`./run_headless_lab.sh`**](run_headless_lab.sh) | No device needed. Builds and runs [`test_lab/`](test_lab/): 100k-node graph → real `SIGKILL` mid-compaction → a fresh PID reattaches at a new ASLR base → every node is verified and a Perfetto trace is written |

### Reproducing the claim without an Android device

```bash
./run_headless_lab.sh --sweep     # ~10s; every number is measured, not claimed
```

Android's kernel is Linux, and the primitives these claims depend on (file-backed
`mmap` persistence, `fork`, an uncatchable `SIGKILL`, ASLR) behave the same on a
Linux or macOS host. That is why the lab counts as a real test and not a mock. You
need `cmake` and a C++20 compiler. The root script forwards to
`test_lab/run_headless_lab.sh`, which also takes `-o <outdir>`, `--asan` (ASan +
UBSan build) and `--keep` (keep the build directory).

Measured with it:

| N (nodes) | reattach + getRoot | full O(N) walk | after a mid-compaction SIGKILL |
|----------:|-------------------:|---------------:|:-------------------------------|
| 1,000 | ~42 µs | 7.6 µs | 1,000/1,000 · 0 torn reads |
| 100,000 | ~50 µs | 743 µs | 100,000/100,000 · 0 torn reads |
| **1,000,000** | **~43 µs** | 8,617 µs | 1,000,000/1,000,000 · 0 torn reads |

Reattach stays flat across a 1000× range of N, which is what O(1) looks like. The
same instrument shows the full walk growing linearly. That walk is the control:
it shows the flat reattach curve comes from the system and not from the
measurement. Drop the generated `test_lab/aevum_autonomous_proof.json` onto
<https://ui.perfetto.dev> to see the SIGKILL and the reattach that follows on a
new PID at a different base address.

---

## 0. Motivation: the blind spot Aevum addresses

An autonomous survey of six Android subsystems, with each candidate filtered for
prior art and judged, turned up this structural dead end:

> **Android 14+'s CMC collector did not remove the GC pause. It spread it into an
> unmeasured, per-thread `SIGBUS` fault storm that gets charged to mutator time.**

The ~1.8 ms "GC pause" that every tool reports (Perfetto GC slices, Studio
profiler, ANR traces) is only the `MarkingPause` + `FlipThreadRoots` window. The
rest of compaction runs concurrently. A mutator that touches a moving-space page
that hasn't been relocated yet takes a `userfaultfd` demand-fault and does the
page copy plus the in-place reference fixup on its own thread's clock. None of
that shows up in the "pause" number. A UI or render thread that happens to be
walking a large object graph when compaction starts takes a burst of these. The
cost wasn't removed. It was moved somewhere no trace shows it.

### Why Aevum, and not a "fix" for CMC

A red team then tried to break the obvious response, an app-side scheduler that
pre-relocates pages ahead of the render thread. It didn't survive, and Aevum takes
that verdict as is:

- You can't touch ART's heap from an app. `userfaultfd` is single-owner per VMA,
  so a second `UFFDIO_REGISTER` over the moving space returns `EEXIST`. On top of
  that, the app seccomp policy blocks `userfaultfd` outright (`SIGSYS`), and
  `vm.unprivileged_userfaultfd=0`. ART only uses it through a privileged setup
  path.
- Compaction work is conserved. Each live page is copied exactly once, and no
  scheduler can reduce the total work of a conserved-work phase.

The red team's own conclusion is this design's premise: **conserved GC work can
only be avoided by taking the objects out of the heap entirely.** That is Aevum.
It doesn't schedule ART's compaction. It makes the data ART would compact *not
exist* from ART's point of view, while keeping it a live, mutable, persistent
object graph.

The assumption it drops: *"a Kotlin/Java object's lifetime is bounded by its
process's lifetime and by the ART heap's invariants."* Aevum objects outlive the
process and were never in the heap.

On prior art: self-relative pointers (boost `offset_ptr`) and pointer swizzling at
page-fault time (Wilson, 1991) are not new ideas. Tencent's MMKV is the closest
shipping Android artifact, but it is a flat key-value store, not a live
pointer-linked graph. What's new here is putting the pieces together under
Android's exact constraints: a GC-invisible, O(1) orthogonally persistent,
self-relative object graph, with a big.LITTLE-aware lock-free allocator and an
mprotect-fault incremental checkpoint. We haven't found a shipping Android library
that provides that combination.

---

## 1. The falsifiable claim

> For an object graph of N nodes held in an Aevum arena:
> **(a)** its contribution to ART GC pause time and CMC compaction demand-faults
> is exactly **zero**, independent of N; and
> **(b)** the time to make the graph live again after process death is **O(1)** in
> N (a fixed `mmap` + header validation), not the O(N) of deserializing an
> equivalent Java object graph.
>
> Aevum is falsified if either an ART GC trace attributes any mark/compaction cost
> to arena memory, or reattach latency grows with graph size.

Both follow from the construction. (a) The region is never registered with the
ART heap, so the collector has no root into it. (b) Every link is
position-independent, so relocation is a no-op.

---

## 2. Architecture

### 2.1 Core mechanism

- A region is backed either by a `memfd_create` object (tmpfs; it survives
  process death as long as some holder keeps the fd) or by a private file
  (`mmap`+`MAP_SHARED`; it survives process death *and* reboot).
  `F_SEAL_SHRINK|GROW` freezes a memfd's size.
- Objects point at each other through [`OffsetPtr<T>`](native/src/offset_ptr.h),
  a single 64-bit word holding `target − &field`. Moving the whole region shifts
  every field and its target by the same delta, so the stored distance doesn't
  change and reattach needs zero fixup. Null is encoded as offset `1`, which no
  16-aligned target can produce. Durable entry points into the graph live in a
  64-slot root table, addressed by base-relative
  [`Handle`](native/src/offset_ptr.h).
- The [lock-free segregated-fit allocator](native/src/allocator.cpp) has 34 size
  classes on a ~1.25x geometric ladder from 16 B to 64 KB. Each class is a
  lock-free Treiber stack whose head packs a `48-bit offset + 16-bit ABA tag` and
  is swung with one 64-bit CAS. A lock-free bump pointer backs empty classes. The
  bump pointer, the free-list head array, the root table and the commit lock each
  start on their own cache line. The per-class heads inside the array are 8 bytes
  apart, so eight classes share each line. A 16-byte block header holds the class
  index and an alloc/free magic for best-effort double-free detection.
- [`commit()`](native/src/arena.cpp) is crash-consistent. It snapshots the live
  control block and roots into the *inactive* one of two double-buffered
  checkpoint slots, seals it with CRC32C, calls `msync`, then atomically flips the
  active slot. A crash in the middle of the write can't corrupt anything, because
  attach picks the newest slot whose CRC validates. A committed graph only loses
  the mutations made since the last commit.

### 2.2 Optional fault-driven accelerator (same idea, no uffd needed)

[`DirtyTracker`](native/src/dirty_tracker.cpp) is an opt-in incremental
checkpoint. It write-protects the live data pages and installs a chained
`SIGSEGV`/`SIGBUS` handler. The first write to a page in each epoch faults. The
async-signal-safe handler (atomics plus one `mprotect`, no libc) marks the page
dirty and unprotects it, and the write retries transparently. The next `commit()`
flushes only the dirtied pages: `O(working set)` instead of `O(region)`.

This is the same fault-driven page-servicing approach CMC uses, rebuilt on
`mprotect` + `sigaction`. Both are reachable from an app, unlike the `userfaultfd`
that the sandbox blocks.

### 2.3 Degradation ladder: never worse than the platform default

| Tier | Requires | If unavailable → |
|------|----------|------------------|
| Incremental checkpoint | `mprotect`+`sigaction` accepted | `enableIncrementalCheckpoint()` returns false; `commit()` full-flushes. Correct, higher cost. |
| memfd backing | `memfd_create` (Linux/Android) | `Aevum.createInMemory` throws `AevumException(UNSUPPORTED)`; use `createFile` (also more durable). |
| Hardware CRC32 / LSE | ARMv8.1 (universal on target) | none at runtime: both are compiled in (`-march=armv8-a+crc+lse`), so the arm64 build needs ARMv8.1. |
| 16 KB pages | Android 15 large-page devices | page size read at runtime via `sysconf`; mappings are page-agnostic. |

Apart from CRC32 and LSE, which are chosen at compile time, nothing is assumed to
be present. Every other capability is probed at runtime, and every failure falls
back to a slower path that is still correct. The self-relative
arena, which is the part everything else depends on, only needs
`memfd_create`/`mmap`, and both are confirmed allowed.

### 2.4 Language and stack

Kotlin/Java can't express this. The JVM owns object layout and identity, the GC
can move objects, and there's no way to put a Java object graph in an unmanaged
mapping or keep raw cross-object references stable across a re-`mmap`. The engine
is C++20 (`-fno-exceptions -fno-rtti`) for deterministic layout, explicit memory
ordering on ARM's weak memory model, and direct syscalls. Kotlin is a thin layer
on top that is hard to misuse. A [C ABI](native/include/aevum/aevum.h) also
exposes the engine to NDK/C++ consumers.

### 2.5 Public API (Kotlin)

```kotlin
Aevum.createFile("/data/.../graph.aevum", 64L * 1024 * 1024).use { arena ->
    val node = arena.alloc(64)                 // Ref, a durable handle
    arena.view(node).apply {                   // zero-copy ByteBuffer, native order
        putLong(0, 42); putLong(8, child.handle)
    }
    arena.setRoot(0, node)                      // durable graph entry point
    arena.enableIncrementalCheckpoint()         // opt-in; false if unsupported
    arena.commit()                              // crash-consistent checkpoint
}
// after process death:
Aevum.attachFile("/data/.../graph.aevum").use { arena ->
    val root = arena.getRoot(0)                 // graph is live again, O(1)
}
```

`Ref` is a `@JvmInline value class`. It costs nothing at runtime and keeps you
from passing a raw `Long` where a handle is expected. Reads and writes go through a
direct `ByteBuffer` that aliases the payload, so the hot path is one JNI call to
get the buffer and none after that. Calls on a closed arena throw, and `close()`
is idempotent.

### 2.6 Threading and lifecycle contract

`alloc`/`free`/`view`/`setRoot`/`getRoot`/`commit` are safe to call concurrently
on one arena (the allocator is lock-free). Construction and `close()` must not
race other calls on the same instance. The only code the native side runs on its
own is the `DirtyTracker` fault handler, and it never calls into the JVM. So there
is no `AttachCurrentThread`/`Detach` discipline to get wrong and no `JNIEnv` that
can leak. `~Arena` unmaps and flushes the arena (RAII). `close()` disarms write
protection before unmapping, so the handler never touches freed memory.

---

## 3. Verification

The pure algorithmic core is compiled and actually executed on a host, not just
asserted. The platform-specific paths are cross-compiled against the real NDK
sysroot.

- Position independence and O(1) reattach: a 65,535-node graph is built,
  committed, then re-mapped into two instances at once at distinct bases 64 MB
  apart. Both traverse to the same checksum with zero fixup.
- Allocator: 40,000 randomized alloc/free ops. No overlapping payloads,
  double-free is rejected, and recycling bounds the high-water mark.
- Concurrency: 1,600,000 ops across 8 threads with zero cross-thread corruption.
  ThreadSanitizer-clean, after the Treiber intrusive-link accesses were moved to
  `__atomic` relaxed.
- No UB or memory errors: Address and UndefinedBehavior sanitizers are clean,
  including the incremental write-protect fault handler.
- Incremental checkpoint: a graph is built entirely under write protection (every
  write faults → handler → `mprotect` → retry), committed, mutated, re-committed
  and reattached. The persisted image is correct.
- Build: all translation units cross-compile to `aarch64-linux-android`.
  `libaevum.so` links with `-Wl,--no-undefined`, exports `JNI_OnLoad` + the C ABI,
  and depends only on `libc/libm/libdl/liblog/libandroid/libc++_shared`. The 15
  arena-API Kotlin `external` descriptors match the JNI `RegisterNatives` table
  byte-for-byte.

---

## 4. Aevum-Defrag: app-owned concurrent compaction

The arena above hides objects from ART's GC but has no compaction of its own, so a
long-running graph that churns allocations will fragment. Aevum-Defrag is an
app-owned copying (mark-compact) collector with no stop-the-world, for a managed
object heap. Aevum reclaims its own space on its own schedule, using the same
class of mechanism ART uses, entirely in userspace.

### 4.1 Model

The managed heap is two semispaces carved out of an arena region. Objects are
bump-allocated into the active space. There is no per-object free: reclamation
*is* compaction. Types register an O(1) [`TraceFn`](native/src/type_registry.h)
that visits their reference fields (there is no reflection), and each object
carries a 16-byte [`ObjectHeader`](native/src/type_registry.h) with a forwarding
word, a mark epoch and a `type_id`. References are
[`SelfHealingOffsetPtr`](native/src/self_healing_ptr.h): self-relative like
`OffsetPtr`, plus barriers.

`compact()` runs on a background thread, pinned to the LITTLE cluster by reading
`cpu_capacity`. It has three phases and runs concurrently with mutators the whole
time:

1. **Concurrent mark (SATB).** Starting from the durable roots, trace the live
   graph through `TraceFn` and stamp a mark epoch. A snapshot-at-the-beginning
   deletion barrier on reference writes keeps liveness correct while other threads
   unlink objects. Marking runs to a fixpoint, and on SATB overflow or
   non-convergence it aborts the whole cycle cleanly without changing anything.
2. **Evacuate + forward.** Copy each marked from-space object into to-space,
   re-base its self-relative fields, then install forwarding on the old header
   with a winner-take-all `CAS` (LSE). Mutators that need to write cooperate by
   evacuating on write through the same protocol, so writes always land on the
   canonical copy.
3. **Proactive fixup + reclaim.** Walk the evacuated objects and the roots,
   rewrite every forwarded reference to point at its to-space copy, then advance
   the durable commit and abandon from-space.

### 4.2 How the four hard constraints are met

- No stop-the-world. A mutator path never takes a mutex or spinlock and never
  calls `sleep` or `yield`. Coordination happens through a forwarding read barrier
  (follow the chain, read-only), an evacuate-on-write barrier, and lock-free
  `CAS`/atomic stores. The only thing that ever spins is the compactor's
  end-of-phase handshake, never a mutator.
- Lock-free visitor. The read barrier is strictly read-only, so from-space stays
  byte-for-byte read-only for the whole cycle and the evacuating `memcpy` can
  never race a mutator write. The only shared per-cycle state a mutator touches is
  one atomic `phase` word.
- big.LITTLE isolation. The hot cross-thread words (the packed allocation cursor,
  each thread's `in_write` flag, the SATB head/tail, the root table) are
  `alignas(64)` on their own cache lines, so the E-core compactor and a P-core
  120 Hz mutator don't ping-pong a line.
- LMKD / process-death durability. The durable checkpoint (double-buffered,
  CRC-sealed, in [`ManagedHeader`](native/src/compactor.h)) keeps pointing at
  *from-space* until Phase 3 has fully finished, and the compactor never
  overwrites from-space. A kill in the middle of compaction leaves from-space and
  its committed roots intact, so `attach()` recovers the exact pre-compaction
  graph and ignores the half-built to-space.

### 4.3 Mutator contract

Fully general concurrent compaction without barriers is impossible. Every
production concurrent collector (ART CC, Shenandoah, ZGC) uses barriers, and so
does this one. The difference is that here they are explicit in the API instead
of being inserted by the compiler:

- Read through `resolveRead(ref)` / `SelfHealingOffsetPtr::get()`.
- Write through a scoped [`WriteScope`](native/src/compactor.h). It does
  evacuate-on-write and registers the write as in flight so it can't straddle a
  cycle.
- Don't cache a raw payload pointer across an `alloc()` or `compact()`. Re-derive
  it from the roots. Local pointers aren't roots, so the collector can't heal
  them, the same as in any managed runtime without stack scanning.

The first version of this boundary had one throughput cost. The compactor waited
for a global in-flight counter to drain to zero, so under *saturating, continuous*
write load its throughput dropped (it waited longer for an idle instant). It never
blocked a mutator and never corrupted anything. §5.1 replaces that global drain
with a ragged safepoint and per-thread handshake.

### 4.4 Defrag verification (executed)

- Concurrent integrity: a 4,096-node backbone runs under 4→24 concurrent mutator
  threads, plus garbage churners, plus a live compactor. After the run the
  backbone is intact and every node's `checksum == hash(id, counter)` holds (no
  torn or lost writes), across ~1,000 compaction cycles and hundreds of thousands
  of mutations.
- Reclamation: after churning ~200k garbage objects, live bytes go back to exactly
  the backbone size. Compaction reclaims the garbage.
- Crash-safe reattach: close and reattach after concurrent compaction, and all
  4,096 nodes come back from the durable commit.
- ThreadSanitizer-clean (content mutators + churners + compactor), and clean
  under the Address and UndefinedBehavior sanitizers.
- Cross-compiles to `aarch64-linux-android` and is built into `libaevum.so`.

---

## 5. Core scaling, field proof, product surface

### 5.1 Ragged safepoints + thread-local SATB (`safepoint.{h,cpp}`)

The earlier mutator/compactor boundary used one shared MPSC SATB buffer (every
write did a CAS on a shared head) and a global in-flight counter that the
compactor drained to zero. Under saturating write load that counter almost never
reaches zero, so compaction throughput collapsed above ~16 threads. Both pieces
were replaced:

- Thread-local SATB. Each mutator thread owns a cache-line-isolated SPSC ring
  ([`ThreadLocalSatbBuffer`](native/src/safepoint.h)), and the write barrier does a
  single-producer append with no cross-thread CAS. The compactor drains each
  thread's ring in turn.
- Ragged safepoint. To make from-space read-only for evacuation, the compactor no
  longer waits for a global zero. It walks the mutator registry and handshakes
  with each thread individually: it watches the thread pass through one
  `in_write == 0` gap, then moves on. Threads never wait on each other, and each
  one is caught in a gap within one write scope. Correctness rests on a
  store-then-`seq_cst`-load (Dekker) pairing between `beginWrite` and the
  handshake, plus a second handshake *before the space flip*, so no cooperative
  evacuation can straddle a cycle boundary into the wrong space. That last case
  was a real bug, caught and fixed by this section's tests.

Measured scaling (host, 64-node backbone, per-thread mutation + live compactor,
compaction cycles completed in a fixed window, higher is better):

| mutator threads | global drain (rejected) | ragged safepoint |
|---:|---:|---:|
| 16 | ~120 | **~1,600** |
| 32 | not reported (worse) | **~500** |
| 64 | not reported | **correct, ~50** |

Correct (`bad=0`) and clean under ThreadSanitizer, ASan and UBSan at 8→64
threads. This removes the throughput cost the global-drain design had.

### 5.2 Field-proof instrumentation and benchmark (`profiler.{h,cpp}`, `benchmark/`)

- Perfetto / atrace. [`profiler.h`](native/src/profiler.h) exports `compact`,
  `mark`, `safepoint`, `evacuate`, `fixup` and `commit` as system-trace sections
  and live counters through `android/trace.h`. A trace scope checks
  `ATrace_isEnabled()` first, so when nothing is capturing, the hot path pays one
  branch and no syscall. Off Android it compiles to nothing.
- What was actually run here (on a host, no device). A host benchmark drives the
  real engine:
  - O(1) reattach: reattach + `getRoot` stays flat at ~30 µs whether the graph has
    1 k, 10 k, 100 k or 1,000,000 nodes (29.4 / 31.0 / 32.8 / 31.5 µs).
    Deserializing a 1 M-node object graph is O(N), which means hundreds of ms.
  - No stop-the-world: during 200 back-to-back `compact()` cycles on a 100 k-node
    graph, 8 mutator threads logged 95 million mutations. They were never paused.
- What still needs a device. [`AevumBenchmark.kt`](benchmark/AevumBenchmark.kt) is
  the on-device driver: `reattachVsDeserialize()` (Aevum mmap vs. an O(N) Java
  rebuild) and `gcPressure()` (ART GC pause/count from `Debug.getRuntimeStat`,
  with the graph on the ART heap vs. in an Aevum arena). It computes everything
  from real runtime counters. The ART-vs-Aevum GC comparison and 120 Hz jank
  capture require running it on a phone. That hasn't been done, and no numbers are
  given for it.

### 5.3 Product surface: AAR + persistent-LLM-state showcase

- Managed heap from Kotlin. Types are registered by the byte offsets of their
  reference fields ([`TypeRegistry::registerLayout`](native/src/type_registry.h)
  plus a generic offset-walking tracer), so no per-type native code is needed. A C
  ABI (`aev_mh_*`), JNI, and the `ManagedHeap` class in
  [`AevumManaged.kt`](kotlin/AevumManaged.kt) expose alloc, a scoped `write` (the
  barrier), `readRef`/`writeRef` for graph edges, roots, and `compact`. All 29
  Kotlin `external` descriptors (15 arena, 14 managed heap) match the JNI table
  byte-for-byte, and every translation unit cross-compiles into `libaevum.so`.
- AAR. [`build.gradle.kts`](build.gradle.kts) packages `libaevum.so`
  (`arm64-v8a` + `armeabi-v7a`, 16 KB-page aligned) and the Kotlin API into one
  `.aar`, and exports the C ABI headers through CMake Prefab.
- Showcase (Jetpack Compose).
  [`ShowcaseApp.kt`](showcase/src/main/kotlin/dev/aevum/showcase/ShowcaseApp.kt)
  stores an LLM conversation as a token chain in an Aevum managed heap. A "Trigger
  LMKD Process Death" button hard-kills the process, and `onCreate` reattaches the
  graph in O(1) and picks up at the exact token with no deserialization. A live
  HUD plots the 120 Hz Choreographer frame interval next to the `compact()` state,
  so you can watch the GC-invisible claim. JNI and arena lifetimes are
  RAII/`AutoCloseable`, and the arena is deliberately *not* closed on the kill
  path, since surviving that kill is the whole point of the demo.

What has not been built or run: the Gradle/AGP build and the Compose app can't be
built or run in this environment (no device, no Gradle/AGP). The Kotlin API layer
(`Aevum.kt`, `AevumManaged.kt`) is compiled and signature-checked here. The
`build.gradle.kts`, manifest, Compose UI and benchmark are complete code written
for a real Android build. They have not been verified by execution here, and
nothing in them is stubbed or faked. The repo also has no Gradle wrapper or
`settings.gradle.kts`, so you include the module from your own Gradle project.

## 6. Layout

```
native/include/aevum/aevum.h    C ABI (stable, exported)
native/src/offset_ptr.h         self-relative pointer + Handle
native/src/region_layout.h      on-region binary layout (header, classes, checkpoints)
native/src/platform.h           syscalls, atomics, CNTVCT timing, CRC32C
native/src/allocator.{h,cpp}    lock-free segregated-fit allocator
native/src/arena.{h,cpp}        RAII region owner, create/attach/commit
native/src/dirty_tracker.{h,cpp} mprotect-fault incremental checkpoint
native/src/type_registry.h      object types (TraceFn + offset layout), ObjectHeader  (Defrag)
native/src/self_healing_ptr.h   forwarding read barrier + SATB write barrier  (Defrag)
native/src/safepoint.{h,cpp}    thread-local SATB + ragged-safepoint registry
native/src/compactor.{h,cpp}    concurrent copying compactor + WriteScope     (Defrag)
native/src/profiler.{h,cpp}     Perfetto/atrace instrumentation
native/src/aevum_c.cpp          C ABI over Arena + managed heap
native/jni/aevum_jni.cpp        JNI bridge (RegisterNatives)
native/CMakeLists.txt           libaevum.so build (arm64-v8a + armeabi-v7a)
kotlin/Aevum.kt                 public Kotlin arena API
kotlin/AevumManaged.kt          public Kotlin managed-heap API
build.gradle.kts                AAR + Prefab packaging
showcase/…/ShowcaseApp.kt       persistent-LLM-cache Compose app
benchmark/AevumBenchmark.kt     on-device ART-vs-Aevum driver
test_lab/lmkd_simulator.cpp     multi-process SIGKILL / O(1) reattach simulator
test_lab/trace_emitter.{h,cpp}  lock-free incremental NDJSON trace emitter
test_lab/run_headless_lab.sh    one-button autonomous proof console
scripts/field_proof_harness.sh  ADB-based on-device audit harness
scripts/parse_perfetto.py       Perfetto trace analyzer (Aevum vs ART GC)
```

---

## 7. What this engine is for

For a to-do list app this is over-engineering. It's aimed at the edge cases of
mobile software where the platform default doesn't do well:

- On-device inference (LLM / ML). Keep the model's K-V cache and chain-of-thought
  state in the arena. The app can be killed in the background, and when the user
  comes back the model picks up from the exact token after a reattach measured at
  ~30 µs on the host benchmark (§5.2), instead of reloading a cache.
- Game engines and heavy graphics pipelines. Keep the whole scene graph in the
  arena. ART never sees those objects, so they can't cause GC frame drops at
  120 Hz.
- Crash and state dumps. Write app state into the arena without locks or copies,
  and recover it on the next launch with no database query and no
  deserialization.
