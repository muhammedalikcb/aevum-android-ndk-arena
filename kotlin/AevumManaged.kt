// SPDX-License-Identifier: Apache-2.0
package dev.aevum

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Aevum-Defrag managed heap - a GC-invisible object heap with concurrent, no
 * stop-the-world compaction, exposed to Kotlin.
 *
 * You describe each object type once by the byte offsets of its reference fields
 * (via [registerType]); the native engine walks those offsets to trace, evacuate
 * and heal the graph, so no per-type native code is needed. Objects are addressed
 * by [MRef], a durable handle that survives process death. Read data through the
 * zero-copy [read] buffer; follow/store reference fields with [readRef]/[writeRef];
 * mutate under a [write] scope (the write barrier - never cache a buffer across an
 * [alloc] or [compact]).
 */
class ManagedHeap private constructor(
    @Volatile private var ptr: Long,
    private val arena: Aevum,
) : AutoCloseable {

    /** Durable handle to a managed object. Survives process death. */
    @JvmInline
    value class MRef internal constructor(val handle: Long) {
        val isNull: Boolean get() = handle == 0L
        companion object { val NULL = MRef(0L) }
    }

    data class CompactResult(val liveObjects: Long, val reclaimedBytes: Long)

    fun alloc(typeId: Int): MRef = MRef(AevumNative.mhAlloc(p(), typeId))

    /**
     * Zero-copy read view of the object's payload (native byte order). Throws if
     * [ref] is not in the heap or [sizeBytes] is negative or past the object.
     */
    fun read(ref: MRef, sizeBytes: Int): ByteBuffer =
        (AevumNative.mhRead(p(), ref.handle, sizeBytes)
            ?: throw AevumException(AEV_INVALID_ARG, "read($sizeBytes) of a null/invalid MRef or past the object"))
            .order(ByteOrder.nativeOrder())

    /**
     * Run [block] with a writable, canonical view of the object. The write is
     * registered in-flight so the compactor's ragged safepoint cannot let a
     * compaction straddle it. Equivalent to native `WriteScope`.
     */
    fun <T> write(ref: MRef, sizeBytes: Int, block: (ByteBuffer) -> T): T {
        val buf = AevumNative.mhBeginWrite(p(), ref.handle, sizeBytes)
            ?: run { AevumNative.mhEndWrite(p()); throw AevumException(AEV_INVALID_ARG, "write($sizeBytes) of a null/invalid MRef or past the object") }
        try {
            return block(buf.order(ByteOrder.nativeOrder()))
        } finally {
            AevumNative.mhEndWrite(p())
        }
    }

    /**
     * Follow the reference field at [fieldOffset] to the child (or [MRef.NULL]).
     * Also returns [MRef.NULL] if [fieldOffset] is not a registered reference field.
     */
    fun readRef(ref: MRef, fieldOffset: Int): MRef = MRef(AevumNative.mhReadRef(p(), ref.handle, fieldOffset))

    /**
     * Set the reference field at [fieldOffset]. Goes through the write barrier.
     * Throws if [fieldOffset] is not a registered reference field of [ref].
     */
    fun writeRef(ref: MRef, fieldOffset: Int, child: MRef) {
        val s = AevumNative.mhWriteRef(p(), ref.handle, fieldOffset, child.handle)
        if (s != AEV_OK) throw AevumException(s, "writeRef(+$fieldOffset): ${statusName(s)}")
    }

    fun setRoot(index: Int, ref: MRef) {
        val s = AevumNative.mhSetRoot(p(), index, ref.handle)
        if (s != AEV_OK) throw AevumException(s, "setRoot($index): ${statusName(s)}")
    }
    fun getRoot(index: Int): MRef = MRef(AevumNative.mhGetRoot(p(), index))

    /** Trigger one concurrent compaction cycle (usually run on a background thread). */
    fun compact(): CompactResult {
        val r = AevumNative.mhCompact(p())
        return CompactResult(r[1], r[2])
    }

    val bytesInUse: Long get() = AevumNative.mhBytesInUse(p())

    private fun p(): Long {
        val v = ptr
        check(v != 0L) { "ManagedHeap is closed" }
        return v
    }

    @Synchronized
    override fun close() {
        val v = ptr
        if (v != 0L) { ptr = 0L; AevumNative.mhClose(v) }
    }

    companion object {
        /**
         * Register a managed type. [refFieldOffsets] are the byte offsets, within
         * the object payload, of its self-healing reference fields; [sizeBytes] is
         * the payload size. Register the same [typeId] deterministically across
         * runs (it is persisted in each object's header).
         */
        fun registerType(typeId: Int, refFieldOffsets: IntArray, sizeBytes: Int) {
            val s = AevumNative.mhRegisterLayout(typeId, refFieldOffsets, sizeBytes)
            if (s != AEV_OK) throw AevumException(s, "registerType($typeId): ${statusName(s)}")
        }

        /** Carve two semispaces of [spaceBytes] each from [arena] for the heap. */
        fun create(arena: Aevum, spaceBytes: Long): ManagedHeap {
            val r = AevumNative.mhCreate(arena.nativeHandle(), spaceBytes)
            if (r[0] == 0L) throw AevumException(r[1].toInt(), "ManagedHeap.create: ${statusName(r[1].toInt())}")
            return ManagedHeap(r[0], arena)
        }

        /** Reattach to a managed heap previously created in [arena] (O(1)). */
        fun attach(arena: Aevum): ManagedHeap {
            val r = AevumNative.mhAttach(arena.nativeHandle())
            if (r[0] == 0L) throw AevumException(r[1].toInt(), "ManagedHeap.attach: ${statusName(r[1].toInt())}")
            return ManagedHeap(r[0], arena)
        }
    }
}
