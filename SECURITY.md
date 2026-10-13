# Aevum: Security & Threat Model

*[Türkçe sürüm](SECURITY.tr.md)*

Aevum is a native memory engine that maps a file or `memfd` region into an app's
address space and keeps a live object graph there. It works with low-level
primitives (`mmap`, `memfd_create`, `mprotect`, signal handlers, JNI, direct
memory), so this document states exactly what it does and does not do to the app's
security posture. It is written for reviewers integrating Aevum into sensitive
products (finance, on-device AI, telemetry).

In short: Aevum runs entirely inside the app's existing sandbox using unprivileged,
same-uid primitives. It opens no new cross-process attack surface, requests no
permissions, and never executes data as code.

---

## 1. Trust boundary and what crosses it

| Boundary | Aevum's stance |
|---|---|
| App sandbox (SELinux `untrusted_app`, app uid) | Aevum stays inside it. Every syscall it makes is one an unprivileged app is already allowed to make (see [ADR-001](docs/architecture/ADR-001-to-003.md)). No `su`, no privileged helper, no platform signature. |
| Process ↔ process | The default backing is a private file under `context.cacheDir`/`filesDir` (app-owned, created with mode `0600`). No region is shared with another uid. The optional `memfd` path is shared only if the app itself hands the fd over `SCM_RIGHTS`, which is an explicit act and not a default. |
| Kernel | Only allowlisted syscalls. `userfaultfd`, `io_uring`, `bpf` and `perf_event_open` are not used (they are blocked for apps anyway). |
| Network | None. Aevum does no I/O beyond the mapped file and `msync`. |
| Permissions | Aevum declares and requires zero Android permissions. |

## 2. Sandbox isolation is not weakened

* Same uid only. `mmap`/`memfd` regions are owned by the app's uid. Nothing Aevum
  maps is reachable by another app, and `process_vm_readv` and similar calls stay
  gated on the same uid by the kernel, regardless of Aevum.
* memfd sizes are sealed. Created regions apply `F_SEAL_SHRINK | F_SEAL_GROW`, so
  a process holding the fd cannot shrink the region under a live mapping (a
  truncation/`SIGBUS` hazard) or grow it unexpectedly.
* No new IPC endpoint. Aevum creates no sockets, no Binder services and no content
  providers. The only fd it can expose is one the app chooses to pass.
* Mappings are not executable. Regions are mapped `PROT_READ|PROT_WRITE`, never
  `PROT_EXEC`. The dirty tracker only switches between `PROT_READ` and
  `PROT_READ|PROT_WRITE`. Aevum never maps data as code and introduces no W^X
  violation.

## 3. Memory-safety disclosures (native code)

Aevum is written in C++. The risks and mitigations are:

* Bounds and offsets, raw arena. The allocator returns 16-aligned blocks from
  fixed size classes. A Kotlin view of an arena block is sized by `capacityOf`,
  which first checks the handle against `data_start` and the block's alloc magic,
  and returns no view if either check fails.
* Bounds and offsets, managed heap. Values coming in through the C ABI and JNI
  are checked before any memory is touched:
  * `registerLayout` rejects a size of 0 or above `0x7FFFFFF0`, more than 32
    reference fields, and any offset that is not 8-aligned, does not fit a whole
    8-byte field inside the declared size, or is repeated. A rejected call keeps
    the earlier layout for that type.
  * An `MRef` must be 16-aligned and lie inside the two semispaces, or `read`,
    `write` and `setRoot` fail.
  * The size passed to `read`/`write` must not be negative or larger than the
    object's allocated payload (its registered size rounded up to 16), so the
    view stays inside the object and the mapping.
  * `readRef`/`writeRef` accept only an offset the type registered as a reference
    field (for a type traced by native code, any aligned field inside the
    object), and `writeRef` rejects a child `MRef` outside the heap.
  * On `attach`, the semispace layout stored in the region header is checked
    against the mapping.

  Failures return `AEV_INVALID_ARG` (an `AevumException` in Kotlin), or
  `MRef.NULL` from `readRef`, without touching the object. Each check is a few
  compares. They catch a wrong size or offset, but not a forged `MRef` that is in
  bounds and still does not point at the start of an object, so only use refs the
  heap gave you.
* Concurrency. The compaction engine is clean under ThreadSanitizer,
  AddressSanitizer and UndefinedBehaviorSanitizer on the full concurrent workload
  at 8→64 threads (see [WHITEPAPER §6](docs/WHITEPAPER.md)). Memory ordering is
  explicit (`acquire`/`release`/`seq_cst`), and the ragged-safepoint handshake is
  proven with a Dekker argument.
* Signal handler (dirty tracker, opt-in). The handler is async-signal-safe: for a
  fault inside an Aevum range it does only atomic updates and a single `mprotect`.
  It saves `errno` on entry and restores it on every exit path. For
  faults outside Aevum's ranges it chains to the app's previous `SIGSEGV`/`SIGBUS`
  handler, so it never swallows a genuine crash. If the dirty tracker is not
  enabled, no handler is installed.
* JNI. Natives are bound through `RegisterNatives`, so there are no mangled
  `Java_*` symbols to resolve. No global references are held. The fault handler
  never calls into the JVM, so no `JNIEnv` can leak and there is no attach/detach
  to get wrong. `DirectByteBuffer`s alias arena memory and are valid only while
  the owning arena is open (see `INTEGRATION_GUIDE.md`).

## 4. Data at rest

* Confidentiality. A file-backed region lives in the app's private storage and
  inherits Android File-Based Encryption: it is encrypted at rest with the
  device/user key like any other app file. Aevum adds no encryption of its own and
  makes no confidentiality claim beyond FBE. Do not put secrets in an arena that
  you also send off the device without app-level encryption.
* Integrity. Durable checkpoints are CRC32C-sealed and double-buffered. A torn
  write or bit rot is detected on `attach` (`kCorrupt`) instead of being silently
  trusted. CRC is an integrity check, not a MAC: it detects accidental corruption,
  not a deliberate attacker who can also rewrite the CRC. For tamper evidence
  against a local attacker with file-write access, wrap the region with an
  app-level AEAD.
* Untrusted regions. Treat a region from an untrusted source as hostile input. The
  header magic, version and CRC are validated, but a crafted region could still
  encode an object graph that is malicious for the *app's* own logic, and the
  managed heap does not validate the self-relative offsets stored inside its
  objects (§3 covers only values passed in by the caller). Only attach regions
  your app created.

## 5. Denial-of-service bounds

* Arena and semispace sizes are fixed at creation. Allocation fails on exhaustion
  (a null handle, or an `AevumException` from `Aevum.alloc`) instead of growing
  without bound. The mark phase aborts cleanly on SATB overflow or
  non-convergence, so it cannot loop forever. The mutator registry is bounded by
  the number of threads the app uses. Aevum does not call `mlock`, so it does not
  consume `RLIMIT_MEMLOCK`.

## 6. Reporting

Report suspected vulnerabilities privately to the maintainers before public
disclosure. Include the platform, the ABI, a minimal reproducer, and whether a
sanitizer flags it. For concurrency issues, please attach a TSan log.
