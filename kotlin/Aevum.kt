// SPDX-License-Identifier: Apache-2.0
package dev.aevum

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Aevum - a GC-invisible, process-death-surviving object arena.
 *
 * An [Aevum] arena is a region of memory that lives entirely outside the ART
 * heap. The Concurrent Mark-Compact collector never scans, marks, or relocates
 * any object you place here, so a graph of any size costs zero GC root-scan time
 * and produces zero compaction demand-faults - the source of tail jank that CMC
 * moved off every profiler trace. Because every internal link is stored as a
 * self-relative offset, the region is position-independent: after your process
 * is killed and restarted, [attachFile] re-maps it at a fresh address in O(1),
 * with no deserialization and no object-graph reconstruction.
 *
 * Objects are addressed by [Ref] - a durable, base-relative handle that stays
 * valid across process death. Read and write an object's bytes through the
 * zero-copy [view] buffer; store cross-references between objects as the raw
 * `long` of a child [Ref], or with self-relative pointer fields in native code.
 *
 * Thread-safety: [alloc], [free], [view], [setRoot], [getRoot] and [commit] are
 * safe to call concurrently on one arena (the native allocator is lock-free).
 * Construction and [close] must not race other calls on the same instance.
 */
class Aevum private constructor(@Volatile private var ptr: Long) : AutoCloseable {

    /** A durable handle to an object in the arena. Survives process death. */
    @JvmInline
    value class Ref internal constructor(val handle: Long) {
        val isNull: Boolean get() = handle == 0L
        companion object { val NULL = Ref(0L) }
    }

    // ---- allocation ------------------------------------------------------

    /** Allocate [size] bytes; throws [AevumException] if the arena is full. */
    fun alloc(size: Long): Ref {
        val h = AevumNative.alloc(ptrChecked(), size)
        if (h == 0L) throw AevumException(AEV_NO_MEMORY, "alloc($size) exhausted the arena")
        return Ref(h)
    }

    /** Allocate [size] bytes, or return [Ref.NULL] if the arena is full. */
    fun tryAlloc(size: Long): Ref = Ref(AevumNative.alloc(ptrChecked(), size))

    /** Return a block to its size-class free list. False on double-free. */
    fun free(ref: Ref): Boolean = AevumNative.free(ptrChecked(), ref.handle) == AEV_OK

    /**
     * A direct [ByteBuffer] aliasing the object's payload in place (zero-copy,
     * native byte order). Valid until this arena is [close]d. Reads and writes
     * hit the mapped region directly with no per-access JNI cost.
     */
    fun view(ref: Ref): ByteBuffer =
        (AevumNative.resolve(ptrChecked(), ref.handle)
            ?: throw AevumException(AEV_INVALID_ARG, "resolve of a null/invalid Ref"))
            .order(ByteOrder.nativeOrder())

    /** Usable payload capacity (the block's size class), in bytes. */
    fun capacity(ref: Ref): Long = AevumNative.capacity(ptrChecked(), ref.handle)

    // ---- durable graph roots (0..63) -------------------------------------

    fun setRoot(index: Int, ref: Ref) {
        val s = AevumNative.setRoot(ptrChecked(), index, ref.handle)
        if (s != AEV_OK) throw AevumException(s, "setRoot($index): ${statusName(s)}")
    }

    /** The root at [index], or [Ref.NULL] if unset. */
    fun getRoot(index: Int): Ref = Ref(AevumNative.getRoot(ptrChecked(), index))

    // ---- durability ------------------------------------------------------

    /** Take a crash-consistent checkpoint. Torn-write-safe (double-buffered). */
    fun commit() {
        val s = AevumNative.commit(ptrChecked())
        if (s != AEV_OK) throw AevumException(s, "commit: ${statusName(s)}")
    }

    /**
     * Opt in to incremental checkpointing: later [commit]s flush only pages
     * mutated since the last commit. Returns false (and keeps full-flush) if the
     * platform refuses the write-protect fault handler. Best for large arenas
     * committed often with a small working set.
     */
    fun enableIncrementalCheckpoint(): Boolean =
        AevumNative.enableIncremental(ptrChecked()) == AEV_OK

    /** Bytes handed out by the bump allocator (high-water mark). */
    val bytesInUse: Long get() = AevumNative.bytesInUse(ptrChecked())

    /** The backing memfd for SCM_RIGHTS handoff, or -1 for a file-backed arena. */
    val fd: Int get() = AevumNative.fd(ptrChecked())

    @Synchronized
    override fun close() {
        val v = ptr
        if (v != 0L) { ptr = 0L; AevumNative.close(v) }  // idempotent; native flushes + unmaps
    }

    private fun ptrChecked(): Long {
        val v = ptr
        check(v != 0L) { "Aevum arena is closed" }
        return v
    }

    /** The native arena pointer, for the managed-heap layer. Not public API. */
    internal fun nativeHandle(): Long = ptrChecked()

    companion object {
        /** Durable across process death AND device reboot. */
        fun createFile(path: String, sizeBytes: Long): Aevum =
            build(AevumNative.createFile(path, sizeBytes), "createFile")

        /** Re-map an existing file-backed arena in O(1). */
        fun attachFile(path: String): Aevum =
            build(AevumNative.attachFile(path), "attachFile")

        /**
         * memfd/tmpfs-backed: survives process death only while some process
         * holds [fd] (hand it off via SCM_RIGHTS or inherit across fork). Faster
         * than file-backed; no reboot durability.
         */
        fun createInMemory(name: String, sizeBytes: Long): Aevum =
            build(AevumNative.createMemfd(name, sizeBytes), "createInMemory")

        /** Reattach a memfd handed over by a peer/holder process. */
        fun attachFd(fd: Int): Aevum = build(AevumNative.attachFd(fd), "attachFd")

        private fun build(result: LongArray, op: String): Aevum {
            val ptr = result[0]
            val status = result[1].toInt()
            if (ptr == 0L) throw AevumException(status, "$op failed: ${statusName(status)}")
            return Aevum(ptr)
        }
    }
}

/** Thrown when a native Aevum operation fails; [status] is an `AEV_*` code. */
class AevumException(val status: Int, message: String) : RuntimeException(message)

// Status codes mirror the native C ABI (aevum.h).
const val AEV_OK = 0
const val AEV_NO_MEMORY = -1
const val AEV_INVALID_ARG = -2
const val AEV_UNSUPPORTED = -3
const val AEV_CORRUPT = -4
const val AEV_ALREADY = -5
const val AEV_IO_ERROR = -6
const val AEV_PROBE_FAILED = -7

internal fun statusName(s: Int): String = when (s) {
    AEV_OK -> "OK"
    AEV_NO_MEMORY -> "NO_MEMORY"
    AEV_INVALID_ARG -> "INVALID_ARG"
    AEV_UNSUPPORTED -> "UNSUPPORTED"
    AEV_CORRUPT -> "CORRUPT"
    AEV_ALREADY -> "ALREADY_ATTACHED"
    AEV_IO_ERROR -> "IO_ERROR"
    AEV_PROBE_FAILED -> "PROBE_FAILED"
    else -> "UNKNOWN($s)"
}

/**
 * The raw JNI surface. Bound via RegisterNatives in JNI_OnLoad; each `external`
 * here matches one entry in the native method table. Not part of the public API.
 */
internal object AevumNative {
    init { System.loadLibrary("aevum") }

    external fun createFile(path: String, size: Long): LongArray   // [ptr, status]
    external fun attachFile(path: String): LongArray
    external fun createMemfd(name: String, size: Long): LongArray
    external fun attachFd(fd: Int): LongArray
    external fun close(ptr: Long)
    external fun fd(ptr: Long): Int
    external fun alloc(ptr: Long, size: Long): Long
    external fun free(ptr: Long, handle: Long): Int
    external fun resolve(ptr: Long, handle: Long): ByteBuffer?
    external fun capacity(ptr: Long, handle: Long): Long
    external fun setRoot(ptr: Long, index: Int, handle: Long): Int
    external fun getRoot(ptr: Long, index: Int): Long
    external fun commit(ptr: Long): Int
    external fun enableIncremental(ptr: Long): Int
    external fun bytesInUse(ptr: Long): Long

    // --- managed heap (Aevum-Defrag) ---
    external fun mhRegisterLayout(typeId: Int, offsets: IntArray, size: Int): Int
    external fun mhCreate(arenaPtr: Long, spaceBytes: Long): LongArray   // [ptr, status]
    external fun mhAttach(arenaPtr: Long): LongArray
    external fun mhClose(ptr: Long)
    external fun mhAlloc(ptr: Long, typeId: Int): Long
    external fun mhRead(ptr: Long, ref: Long, size: Int): ByteBuffer?
    external fun mhBeginWrite(ptr: Long, ref: Long, size: Int): ByteBuffer?
    external fun mhEndWrite(ptr: Long)
    external fun mhSetRoot(ptr: Long, index: Int, ref: Long): Int
    external fun mhGetRoot(ptr: Long, index: Int): Long
    external fun mhReadRef(ptr: Long, ref: Long, fieldOffset: Int): Long
    external fun mhWriteRef(ptr: Long, ref: Long, fieldOffset: Int, child: Long): Int
    external fun mhCompact(ptr: Long): LongArray   // [status, liveObjects, reclaimedBytes]
    external fun mhBytesInUse(ptr: Long): Long
}
