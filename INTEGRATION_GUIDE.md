# Aevum: Integration Guide

*[Türkçe sürüm](INTEGRATION_GUIDE.tr.md)*

How to consume Aevum from a Jetpack Compose or NDK project, wire it to the Android
lifecycle correctly, and use the write barrier safely. Every snippet matches the
shipped API (`kotlin/Aevum.kt`, `kotlin/AevumManaged.kt`).

---

## 1. Add the dependency

Aevum ships as a single `.aar` that contains `libaevum.so` (`arm64-v8a` +
`armeabi-v7a`, 16 KB-page aligned) and the Kotlin API. It uses CMake Prefab, so NDK
code can link the C ABI too.

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories { google(); mavenCentral(); /* + your Maven for dev.aevum:aevum */ }
}

// app/build.gradle.kts
android {
    defaultConfig {
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a") }
    }
    // Recommended so the 16 KB-aligned .so is packaged uncompressed & page-aligned:
    packaging { jniLibs.useLegacyPackaging = false }
    buildFeatures { prefab = true }        // only if your own NDK code links the C ABI
}

dependencies {
    implementation("dev.aevum:aevum:1.0.0")
}
```

The AAR's `consumer-rules.pro` is applied automatically, so you don't need to copy
any ProGuard rules. If you maintain an aggressive global R8 config, check that it
does not `-repackageclasses` `dev.aevum.AevumNative`. JNI binds that class by its
exact name, and the consumer rules already keep it.

### NDK consumers (optional)

```cmake
find_package(aevum REQUIRED CONFIG)
target_link_libraries(your_native aevum::aevum)   # then #include <aevum/aevum.h>
```

---

## 2. Minimum requirements

* minSdk 26 (`memfd_create` / ASharedMemory baseline). Where `memfd` is not
  available, `Aevum.createInMemory` throws an `AevumException` with status
  `UNSUPPORTED`; use file backing (`Aevum.createFile`) instead. There is no
  automatic fallback.
* An ARMv8.1 arm64 device. The `arm64-v8a` library is compiled with LSE atomics and
  hardware CRC enabled (`-march=armv8-a+crc+lse`). The software CRC and generic
  atomics are only used by builds compiled without those extensions, such as
  `armeabi-v7a`. The engine targets Android 14/15-class hardware.

---

## 3. The two layers

| Layer | Use it for | Entry point |
|---|---|---|
| Arena (`Aevum`) | raw, GC-invisible byte blocks that survive process death, plus durable roots | `Aevum.createFile / attachFile` |
| Managed heap (`ManagedHeap`) | a live object graph with concurrent compaction (defrag) | `ManagedHeap.create / attach` over an `Aevum` |

Use the managed heap when the data is a churning, pointer-linked graph (caches,
scene graphs, LLM context). Use the raw arena for large opaque blobs.

---

## 4. Managed-heap quick start

Describe each type once by the byte offsets of its reference fields and its
payload size. No reflection and no native code are involved:

```kotlin
// A graph node laid out as: [value: Long @0][next: ref @8][pad @16]  (32 bytes)
private const val NODE = 1
private const val SIZE = 32
private const val OFF_NEXT = 8

ManagedHeap.registerType(NODE, refFieldOffsets = intArrayOf(OFF_NEXT), sizeBytes = SIZE)

val arena = Aevum.createFile(File(cacheDir, "graph.aevum").absolutePath, 64L * 1024 * 1024)
val heap  = ManagedHeap.create(arena, spaceBytes = 24L * 1024 * 1024)

// allocate + initialize under a write scope (the write barrier)
val a = heap.alloc(NODE)
heap.write(a, SIZE) { buf -> buf.putLong(0, 42L) }

val b = heap.alloc(NODE)
heap.writeRef(a, OFF_NEXT, b)      // link a.next = b (self-healing edge)
heap.setRoot(0, a)                 // durable graph entry point

arena.commit()                     // crash-consistent checkpoint
```

After process death:

```kotlin
val arena = Aevum.attachFile(path)          // O(1)
val heap  = ManagedHeap.attach(arena)       // O(1)
val head  = heap.getRoot(0)                 // graph is live again, no deserialize
val next  = heap.readRef(head, OFF_NEXT)    // follow an edge (forwarding-safe)
val value = heap.read(head, SIZE).getLong(0)
```

The native side checks the offsets and sizes you pass here against the registered
layout and the object's size, and rejects bad ones with an `AevumException`
(`readRef` returns `MRef.NULL`; see [`SECURITY.md`](SECURITY.md), §3). Still keep
them as constants that match the registered layout.

---

## 5. Lifecycle: bind to `ViewModel` / `onCleared`

Own the arena in a `ViewModel` and close it in `onCleared()`. Closing flushes,
unmaps and releases the fd (RAII on the native side).

```kotlin
class GraphViewModel(app: Application) : AndroidViewModel(app) {
    private val arena: Aevum
    private val heap: ManagedHeap

    init {
        val path = File(app.cacheDir, "graph.aevum").absolutePath
        val exists = File(path).exists()
        arena = if (exists) Aevum.attachFile(path)
                else Aevum.createFile(path, 64L * 1024 * 1024)
        heap = if (exists && arena.getRoot(0).handle != 0L) ManagedHeap.attach(arena)
               else ManagedHeap.create(arena, 24L * 1024 * 1024)
    }

    fun compactInBackground() = viewModelScope.launch(Dispatchers.IO) { heap.compact() }

    override fun onCleared() {
        heap.close()      // close the heap first, then its backing arena
        arena.close()
        super.onCleared()
    }
}
```

Do not close the arena on the process-death path. The region is designed to
survive a kill, so close it only on a *graceful* teardown (`onCleared`, an explicit
sign-out). On an LMKD kill the OS reclaims the mapping, the file or `memfd`
persists, and the next launch reattaches. In an `Activity`, gate closing on
`isFinishing`:

```kotlin
override fun onDestroy() {
    if (isFinishing) store.close()   // graceful exit only; never on background kill
    super.onDestroy()
}
```

`close()` is idempotent and `@Synchronized`. A closed handle throws on further use.

---

## 6. The write barrier: `WriteScope` / `write { }` best practices

The managed heap uses the same barrier contract as every moving GC, made explicit
in the API. Follow three rules:

1. Read with `read` / `readRef`, and write only inside `write { }`. `write` takes
   the write barrier: during a compaction it evacuates on write, and it registers
   the write as in flight so a cycle cannot straddle it.

   ```kotlin
   // GOOD: the mutation is scoped by the barrier
   heap.write(ref, SIZE) { buf -> buf.putLong(0, buf.getLong(0) + 1) }
   ```

2. Never cache a `ByteBuffer` or a bare `MRef` across an `alloc()` or `compact()`.
   Compaction moves objects. A buffer aliases one specific address and a local
   `MRef` is not a root, so both go stale. Re-derive them from a root or a traced
   field for each logical operation.

   ```kotlin
   // BAD: buf/ref may point at abandoned space after a concurrent compact()
   val buf = heap.read(ref, SIZE); heavyWorkThatMayCompact(); buf.getLong(0)   // wrong

   // GOOD: re-resolve after work that could compact
   heavyWorkThatMayCompact()
   val v = heap.read(ref, SIZE).getLong(0)                                      // right (ref via a root)
   ```

3. Store cross-object edges with `writeRef` and follow them with `readRef`. These
   go through the self-healing pointer and forwarding. A handle written into a
   payload with a raw `putLong` will NOT be relocated by the collector.

Keep write scopes short. The compactor's end-of-phase handshake waits for
outstanding writes. It never blocks mutators, but a scope held for a long time
delays the background compactor.

---

## 7. Threading model

* `alloc`, `free`, `read`, `write`, `readRef`, `writeRef`, `setRoot`, `getRoot` and
  `commit` are safe to call concurrently on one arena/heap (lock-free).
* `create`, `attach` and `close` must not race other calls on the same instance.
  Serialize construction and teardown.
* Run `compact()` on a background thread (for example `Dispatchers.IO`). It pins
  itself to the LITTLE cluster, and mutators are not paused while it runs.
* Any thread that writes managed memory registers a small per-thread context on
  first use. Contexts are never freed, so use a bounded thread pool instead of
  spawning an unbounded number of short-lived writer threads.

---

## 8. Durability & compaction cadence

* Call `commit()` at meaningful checkpoints (after a batch, before going to the
  background). A crash loses only the mutations made since the last `commit()`,
  and the region never ends up corrupted (double-buffered CRC).
* Optionally call `arena.enableIncrementalCheckpoint()` so that `commit()` flushes
  only dirtied pages. This helps most for large arenas that are committed often
  with a small working set.
* Trigger `compact()` when `heap.bytesInUse` grows well past the live set
  (churn-heavy workloads). It reclaims dead objects concurrently, and
  `CompactResult.reclaimedBytes` reports how much came back.

---

## 9. Observability

Build and run with system tracing to see Aevum's phases in Perfetto: the sections
`Aevum.compact / mark / safepoint / evacuate / fixup / commit` and the counters
`Aevum.live_bytes / Aevum.reclaimed_bytes`. When no trace is active, the
instrumentation costs one predictable branch and no syscall. See
`scripts/field_proof_harness.sh` + `scripts/parse_perfetto.py` for a one-command
on-device capture that also shows that ART GC does no work on the arena.

---

## 10. Checklist for production

- [ ] `abiFilters` limited to `arm64-v8a` (+ `armeabi-v7a` if you still ship 32-bit).
- [ ] Arena owned by a lifecycle scope; `close()` only on graceful exit.
- [ ] All writes inside `write { }`; no cached buffers/refs across `alloc`/`compact`.
- [ ] `compact()` off the UI thread; `commit()` at checkpoints.
- [ ] Secrets in an arena you export off-device are wrapped in app-level AEAD
      (see [`SECURITY.md`](SECURITY.md)).
- [ ] Field-proof harness run once on a target device to record the C1/C2/C3 numbers.
