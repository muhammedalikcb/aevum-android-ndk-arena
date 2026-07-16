// SPDX-License-Identifier: Apache-2.0
package dev.aevum.showcase

import android.os.Bundle
import android.os.Process
import android.os.SystemClock
import android.util.Log
import android.view.Choreographer
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dev.aevum.Aevum
import dev.aevum.ManagedHeap
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.ByteBuffer

/**
 * The persistent LLM context cache. A conversation is a chain of token nodes in
 * an Aevum managed heap: root 0 = head, root 1 = tail. Because the heap is
 * GC-invisible and position-independent, it survives process death and reattaches
 * in O(1) - the model resumes from the exact token with no deserialization.
 */
class ContextStore(cacheDir: File) : AutoCloseable {

    // TokenNode layout (64 bytes): [seq: long @0][next: MRef @8][text: 48 bytes @16]
    private companion object {
        const val TYPE = 1
        const val SIZE = 64
        const val OFF_SEQ = 0
        const val OFF_NEXT = 8          // the only reference field
        const val OFF_TEXT = 16
        const val TEXT_CAP = 48
        const val ROOT_HEAD = 0
        const val ROOT_TAIL = 1
    }

    data class Token(val seq: Long, val text: String)

    private val arena: Aevum
    private val heap: ManagedHeap

    init {
        ManagedHeap.registerType(TYPE, intArrayOf(OFF_NEXT), SIZE)
        val path = File(cacheDir, "llm_context.aevum").absolutePath
        val reattaching = File(path).exists()
        // O(1) reattach if the region already exists (process was killed before);
        // otherwise create a fresh 64 MB region with a 24 MB managed heap. Time the
        // reattach so the field-proof harness can read it from logcat.
        val t0 = SystemClock.elapsedRealtimeNanos()
        arena = if (reattaching) Aevum.attachFile(path)
                else Aevum.createFile(path, 64L * 1024 * 1024)
        heap = if (reattaching && arena.getRoot(0).handle != 0L)
                   runCatching { ManagedHeap.attach(arena) }.getOrElse { ManagedHeap.create(arena, 24L * 1024 * 1024) }
               else ManagedHeap.create(arena, 24L * 1024 * 1024)
        val root = heap.getRoot(ROOT_HEAD)
        val us = (SystemClock.elapsedRealtimeNanos() - t0) / 1000
        if (reattaching) {
            // The whole surviving LLM context is live again; report the O(1) cost.
            Log.i("AEVUM_PROOF", "reattach_us=$us nodes=${conversation().size} root=${root.handle}")
        }
    }

    /** Append one generated token, O(1), and durably checkpoint it. */
    @Synchronized
    fun append(text: String) {
        val node = heap.alloc(TYPE)
        val seq = (readTail()?.seq ?: -1L) + 1
        heap.write(node, SIZE) { buf ->
            buf.putLong(OFF_SEQ, seq)
            val bytes = text.encodeToByteArray()
            val n = minOf(bytes.size, TEXT_CAP - 1)
            for (i in 0 until n) buf.put(OFF_TEXT + i, bytes[i])
            buf.put(OFF_TEXT + n, 0)
        }
        val tail = heap.getRoot(ROOT_TAIL)
        if (tail.isNull) {
            heap.setRoot(ROOT_HEAD, node)
        } else {
            heap.writeRef(tail, OFF_NEXT, node)
        }
        heap.setRoot(ROOT_TAIL, node)
    }

    /** The full conversation, head → tail. O(tokens), used only to paint the UI. */
    @Synchronized
    fun conversation(): List<Token> {
        val out = ArrayList<Token>()
        var ref = heap.getRoot(ROOT_HEAD)
        while (!ref.isNull) {
            val buf = heap.read(ref, SIZE)
            out.add(Token(buf.getLong(OFF_SEQ), readText(buf)))
            ref = heap.readRef(ref, OFF_NEXT)
        }
        return out
    }

    private fun readTail(): Token? {
        val ref = heap.getRoot(ROOT_TAIL)
        if (ref.isNull) return null
        val buf = heap.read(ref, SIZE)
        return Token(buf.getLong(OFF_SEQ), readText(buf))
    }

    private fun readText(buf: ByteBuffer): String {
        val bytes = ByteArray(TEXT_CAP)
        for (i in 0 until TEXT_CAP) bytes[i] = buf.get(OFF_TEXT + i)
        val end = bytes.indexOf(0).let { if (it < 0) TEXT_CAP else it }
        return String(bytes, 0, end, Charsets.UTF_8)
    }

    /** One concurrent compaction cycle - reclaims dropped speculative tokens. */
    @Synchronized
    fun compact(): ManagedHeap.CompactResult = heap.compact()

    fun tokenCount(): Int = conversation().size

    /** Durably commit so a kill right now recovers everything appended so far. */
    fun checkpoint() { arena.commit() }

    override fun close() { heap.close(); arena.close() }
}

class ChatViewModel(private val store: ContextStore) : ViewModel() {
    val tokens = mutableStateListOf<ContextStore.Token>()
    var compacting by mutableStateOf(false); private set
    var lastReclaimBytes by mutableStateOf(0L); private set

    init { tokens.addAll(store.conversation()) }   // resume the conversation on (re)start

    private val corpus = ("Aevum keeps the model's chain of thought alive across " +
        "process death so the assistant never forgets where it was and resumes " +
        "instantly without reloading a single token from disk").split(" ")

    /** Simulate a streaming LLM: emit a token, persist it, keep the UI in sync. */
    fun streamNextToken() {
        viewModelScope.launch {
            val word = corpus[(store.tokenCount()) % corpus.size]
            withContext(Dispatchers.IO) { store.append(word); store.checkpoint() }
            tokens.add(store.conversation().last())
        }
    }

    /** Run compaction off the UI thread (E-core-pinned inside the engine). */
    fun compactInBackground() {
        viewModelScope.launch {
            compacting = true
            val r = withContext(Dispatchers.IO) { store.compact() }
            lastReclaimBytes = r.reclaimedBytes
            compacting = false
        }
    }
}

class ShowcaseActivity : ComponentActivity() {
    private lateinit var store: ContextStore
    private lateinit var vm: ChatViewModel

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        store = ContextStore(cacheDir)
        vm = ChatViewModel(store)
        setContent { ShowcaseScreen(vm, onKill = ::triggerProcessDeath) }
    }

    /** Simulate an LMKD kill: commit, then hard-kill our own process. Android
     *  relaunches us and onCreate reattaches the graph in O(1). */
    private fun triggerProcessDeath() {
        store.checkpoint()
        Process.killProcess(Process.myPid())
    }

    override fun onDestroy() {
        // Note: not closed on kill (that's the point). Closed only on graceful exit.
        if (isFinishing) store.close()
        super.onDestroy()
    }
}

@Composable
fun ShowcaseScreen(vm: ChatViewModel, onKill: () -> Unit) {
    val listState = rememberLazyListState()
    LaunchedEffect(vm.tokens.size) { if (vm.tokens.isNotEmpty()) listState.animateScrollToItem(vm.tokens.size - 1) }

    Column(Modifier.fillMaxSize().padding(12.dp)) {
        GcIsolationHud(compacting = vm.compacting, reclaimed = vm.lastReclaimBytes)
        Spacer(Modifier.height(8.dp))
        Text("Persistent LLM Context Cache", fontWeight = FontWeight.Bold, fontSize = 20.sp)
        Text("${vm.tokens.size} tokens survive process death in the Aevum arena",
            color = Color.Gray, fontSize = 12.sp)
        Spacer(Modifier.height(8.dp))

        LazyColumn(Modifier.weight(1f), state = listState) {
            items(vm.tokens, key = { it.seq }) { t ->
                Text("#${t.seq}  ${t.text}", fontFamily = FontFamily.Monospace, fontSize = 14.sp,
                    modifier = Modifier.padding(vertical = 2.dp))
            }
        }

        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { vm.streamNextToken() }) { Text("Generate token") }
            OutlinedButton(onClick = { vm.compactInBackground() }) { Text("Compact") }
        }
        Spacer(Modifier.height(6.dp))
        Button(
            onClick = onKill,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFFB00020)),
            modifier = Modifier.fillMaxWidth()
        ) { Text("☠  Trigger LMKD Process Death") }
        Text("After the kill, the app relaunches and resumes from token #${vm.tokens.lastOrNull()?.seq ?: 0} instantly (O(1) reattach).",
            color = Color.Gray, fontSize = 11.sp, modifier = Modifier.padding(top = 4.dp))
    }
}

/** Live GC-isolation HUD: 120 Hz frame interval + Aevum compaction status.
 *  Shows frame intervals staying flat while compact() runs. */
@Composable
fun GcIsolationHud(compacting: Boolean, reclaimed: Long) {
    var frameMs by remember { mutableStateOf(0.0) }
    DisposableEffect(Unit) {
        var last = 0L
        val cb = object : Choreographer.FrameCallback {
            override fun doFrame(frameTimeNanos: Long) {
                if (last != 0L) frameMs = (frameTimeNanos - last) / 1_000_000.0
                last = frameTimeNanos
                Choreographer.getInstance().postFrameCallback(this)
            }
        }
        Choreographer.getInstance().postFrameCallback(cb)
        onDispose { Choreographer.getInstance().removeFrameCallback(cb) }
    }
    val smooth = frameMs <= 9.0   // ~120 Hz budget
    Surface(color = Color(0xFF101418), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(10.dp), verticalAlignment = Alignment.CenterVertically) {
            Text("frame: ${"%.1f".format(frameMs)} ms",
                color = if (smooth) Color(0xFF00E676) else Color(0xFFFF5252),
                fontFamily = FontFamily.Monospace, fontSize = 13.sp)
            Spacer(Modifier.width(16.dp))
            Text(if (compacting) "compact(): RUNNING (E-core, no STW)" else "compact(): idle",
                color = if (compacting) Color(0xFFFFC107) else Color.Gray,
                fontFamily = FontFamily.Monospace, fontSize = 13.sp)
            Spacer(Modifier.weight(1f))
            Text("GC-invisible ✓", color = Color(0xFF00E676), fontSize = 12.sp)
        }
    }
}
