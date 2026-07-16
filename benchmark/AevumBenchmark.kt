// SPDX-License-Identifier: Apache-2.0
package dev.aevum.benchmark

import android.os.Debug
import android.os.SystemClock
import dev.aevum.Aevum
import dev.aevum.ManagedHeap
import java.io.File

/**
 * On-device comparison harness for the two Aevum claims. Run each method from an
 * instrumented test / a benchmark Activity on a real device - the numbers below
 * come from the hardware, not from here. This file is the driver; it fabricates
 * nothing.
 *
 * WHAT IT MEASURES
 *  1) reattachVsDeserialize(): time to make a 500k-node graph live again after a
 *     cold start - Aevum mmap+getRoot (O(1)) vs rebuilding the object graph
 *     (O(N)), the honest apples-to-apples for LMKD recovery.
 *  2) gcPressure(): ART GC pause time + count accumulated while churning a large
 *     graph on the ART heap vs. the SAME workload held in an Aevum arena (where
 *     ART sees nothing). Read from Debug.getRuntimeStat, which is the platform's
 *     own GC accounting - not our estimate.
 */
object AevumBenchmark {

    private const val TYPE = 1
    private const val SIZE = 32                 // [value: long @0][next: MRef @8][pad @16]
    private const val OFF_NEXT = 8

    // ---------------------------------------------------------------------
    // Java baseline: an ordinary linked object graph on the ART heap.
    // ---------------------------------------------------------------------
    private class JavaNode(var value: Long) { var next: JavaNode? = null }

    fun buildJavaGraph(n: Int): JavaNode {
        var head: JavaNode? = null
        var prev: JavaNode? = null
        for (i in 0 until n) {
            val node = JavaNode(i.toLong())
            if (head == null) head = node else prev!!.next = node
            prev = node
        }
        return head!!
    }

    fun serializeJava(head: JavaNode, file: File) {
        file.outputStream().buffered().use { out ->
            val buf = ByteArray(8)
            var cur: JavaNode? = head
            while (cur != null) {
                var v = cur.value
                for (i in 0 until 8) { buf[i] = (v and 0xFF).toByte(); v = v shr 8 }
                out.write(buf); cur = cur.next
            }
        }
    }

    /** O(N): reconstruct the whole object graph from disk (the LMKD status quo). */
    fun deserializeJava(file: File): JavaNode {
        var head: JavaNode? = null
        var prev: JavaNode? = null
        file.inputStream().buffered().use { ins ->
            val buf = ByteArray(8)
            while (ins.read(buf) == 8) {
                var v = 0L
                for (i in 7 downTo 0) v = (v shl 8) or (buf[i].toLong() and 0xFF)
                val node = JavaNode(v)
                if (head == null) head = node else prev!!.next = node
                prev = node
            }
        }
        return head!!
    }

    // ---------------------------------------------------------------------
    // Aevum: the SAME graph, GC-invisible and position-independent.
    // ---------------------------------------------------------------------
    fun buildAevumGraph(dir: File, n: Int) {
        ManagedHeap.registerType(TYPE, intArrayOf(OFF_NEXT), SIZE)
        val path = File(dir, "bench.aevum").absolutePath
        File(path).delete()
        Aevum.createFile(path, 512L * 1024 * 1024).use { arena ->
            val heap = ManagedHeap.create(arena, 200L * 1024 * 1024)
            var prev = ManagedHeap.MRef.NULL
            for (i in 0 until n) {
                val node = heap.alloc(TYPE)
                heap.write(node, SIZE) { it.putLong(0, i.toLong()) }
                if (prev.isNull) heap.setRoot(0, node) else heap.writeRef(prev, OFF_NEXT, node)
                prev = node
            }
            arena.commit()
            heap.close()
        }
    }

    data class ReattachResult(val aevumMicros: Long, val javaMicros: Long, val nodes: Int)

    /** Compare cold-start recovery of a 500k-node graph. Returns real microseconds. */
    fun reattachVsDeserialize(dir: File, n: Int = 500_000): ReattachResult {
        buildAevumGraph(dir, n)
        val javaFile = File(dir, "bench.javabin")
        serializeJava(buildJavaGraph(n), javaFile)

        // Aevum: mmap + attach + getRoot - independent of N.
        val aStart = SystemClock.elapsedRealtimeNanos()
        val arena = Aevum.attachFile(File(dir, "bench.aevum").absolutePath)
        val heap = ManagedHeap.attach(arena)
        val root = heap.getRoot(0)
        val firstVal = heap.read(root, SIZE).getLong(0)     // touch the head
        val aEnd = SystemClock.elapsedRealtimeNanos()
        heap.close(); arena.close()

        // Java: rebuild every node from disk - O(N).
        val jStart = SystemClock.elapsedRealtimeNanos()
        val head = deserializeJava(javaFile)
        val jEnd = SystemClock.elapsedRealtimeNanos()

        check(firstVal == 0L && head.value == 0L)
        return ReattachResult((aEnd - aStart) / 1000, (jEnd - jStart) / 1000, n)
    }

    data class GcResult(val label: String, val gcCount: Long, val gcPauseMs: Long, val blockingGcMs: Long)

    private fun gcStat(name: String): Long =
        runCatching { Debug.getRuntimeStat(name)?.toLong() ?: 0L }.getOrDefault(0L)

    private fun snapshotGc(label: String): GcResult = GcResult(
        label,
        gcStat("art.gc.gc-count"),
        gcStat("art.gc.gc-time"),
        gcStat("art.gc.blocking-gc-time"),
    )

    /**
     * Churn a large graph while allocating garbage, and read ART's own GC
     * accounting before/after. Run once with `useAevum=false` (graph on the ART
     * heap) and once with `useAevum=true` (graph in the arena, ART sees nothing).
     * The delta is what ART actually spent on GC for that graph.
     */
    fun gcPressure(dir: File, useAevum: Boolean, n: Int = 200_000, churnIters: Int = 2_000_000): GcResult {
        val before = snapshotGc(if (useAevum) "aevum" else "art")
        if (useAevum) {
            ManagedHeap.registerType(TYPE, intArrayOf(OFF_NEXT), SIZE)
            val path = File(dir, "gc.aevum").absolutePath
            File(path).delete()
            Aevum.createFile(path, 512L * 1024 * 1024).use { arena ->
                val heap = ManagedHeap.create(arena, 200L * 1024 * 1024)
                var prev = ManagedHeap.MRef.NULL
                for (i in 0 until n) {
                    val node = heap.alloc(TYPE); heap.write(node, SIZE) { it.putLong(0, i.toLong()) }
                    if (prev.isNull) heap.setRoot(0, node) else heap.writeRef(prev, OFF_NEXT, node); prev = node
                }
                // churn: allocate garbage nodes; compaction (not ART) reclaims them.
                repeat(churnIters) { heap.alloc(TYPE) }
                heap.compact(); heap.close()
            }
        } else {
            val graph = buildJavaGraph(n)                       // pins N nodes in the ART heap
            var sink: JavaNode? = null
            repeat(churnIters) { sink = JavaNode(it.toLong()) } // garbage -> forces ART GC
            check(graph.value == 0L && sink != null)
        }
        val after = snapshotGc(before.label)
        return GcResult(before.label,
            after.gcCount - before.gcCount,
            after.gcPauseMs - before.gcPauseMs,
            after.blockingGcMs - before.blockingGcMs)
    }
}
