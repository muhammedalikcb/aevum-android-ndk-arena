# Aevum: Entegrasyon Kılavuzu

*[English version](INTEGRATION_GUIDE.md)*

Aevum'u bir Jetpack Compose ya da NDK projesine nasıl ekleyeceğinizi, Android yaşam
döngüsüne nasıl doğru bağlayacağınızı ve write bariyerini nasıl güvenle
kullanacağınızı anlatır. Buradaki tüm kod parçaları yayımlanan API ile uyumludur
(`kotlin/Aevum.kt`, `kotlin/AevumManaged.kt`).

---

## 1. Bağımlılığı ekleyin

Aevum, `libaevum.so` (`arm64-v8a` + `armeabi-v7a`, 16 KB sayfa hizalı) ve Kotlin
API'sini içeren tek bir `.aar` olarak gelir. CMake Prefab kullandığı için NDK kodu
da C ABI'ye linklenebilir.

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories { google(); mavenCentral(); /* + dev.aevum:aevum için Maven'iniz */ }
}

// app/build.gradle.kts
android {
    defaultConfig {
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a") }
    }
    // 16 KB hizalı .so'nun sıkıştırılmadan ve sayfa hizalı paketlenmesi için önerilir:
    packaging { jniLibs.useLegacyPackaging = false }
    buildFeatures { prefab = true }        // yalnızca kendi NDK kodunuz C ABI'yi linkliyorsa
}

dependencies {
    implementation("dev.aevum:aevum:1.0.0")
}
```

AAR'daki `consumer-rules.pro` otomatik uygulanır, ProGuard kurallarını kopyalamanız
gerekmez. Agresif, global bir R8 yapılandırmanız varsa `dev.aevum.AevumNative`'i
`-repackageclasses` ile taşımadığından emin olun. JNI bu sınıfı tam adıyla bağlar
ve tüketici kuralları onu zaten korur.

### NDK tarafı (isteğe bağlı)

```cmake
find_package(aevum REQUIRED CONFIG)
target_link_libraries(your_native aevum::aevum)   # sonra #include <aevum/aevum.h>
```

---

## 2. Asgari gereksinimler

* minSdk 26 (`memfd_create` / ASharedMemory taban çizgisi). `memfd`'nin olmadığı
  yerde `Aevum.createInMemory`, `UNSUPPORTED` durumuyla bir `AevumException`
  fırlatır; bu durumda dosya desteği (`Aevum.createFile`) kullanın. Otomatik bir
  geçiş yoktur.
* ARMv8.1 destekli bir arm64 cihaz. `arm64-v8a` kütüphanesi LSE atomikleri ve
  donanım CRC'si açık olarak derlenir (`-march=armv8-a+crc+lse`). Yazılım CRC'si ve
  genel atomikler yalnızca bu uzantılar olmadan derlenen yapılarda, örneğin
  `armeabi-v7a`'da kullanılır. Motor Android 14/15 sınıfı donanımı hedefler.

---

## 3. İki katman

| Katman | Ne için | Giriş noktası |
|---|---|---|
| Arena (`Aevum`) | ham, GC'ye görünmez, süreç ölümünü atlatan bayt blokları ve durable kökler | `Aevum.createFile / attachFile` |
| Yönetilen heap (`ManagedHeap`) | eşzamanlı sıkıştırmalı (defrag) canlı bir nesne grafiği | bir `Aevum` üzerinde `ManagedHeap.create / attach` |

Veri sürekli değişen, işaretçilerle bağlı bir grafikse (cache'ler, sahne
grafikleri, LLM bağlamı) yönetilen heap'i kullanın. Büyük, opak blob'lar için ham
arenayı kullanın.

---

## 4. Yönetilen heap ile hızlı başlangıç

Her tipi bir kez, referans alanlarının bayt offset'leri ve payload boyutuyla
tanımlayın. Reflection ya da native kod gerekmez:

```kotlin
// Yerleşimi şöyle olan bir grafik düğümü: [value: Long @0][next: ref @8][pad @16]  (32 bayt)
private const val NODE = 1
private const val SIZE = 32
private const val OFF_NEXT = 8

ManagedHeap.registerType(NODE, refFieldOffsets = intArrayOf(OFF_NEXT), sizeBytes = SIZE)

val arena = Aevum.createFile(File(cacheDir, "graph.aevum").absolutePath, 64L * 1024 * 1024)
val heap  = ManagedHeap.create(arena, spaceBytes = 24L * 1024 * 1024)

// write kapsamı (write bariyeri) altında tahsis + başlat
val a = heap.alloc(NODE)
heap.write(a, SIZE) { buf -> buf.putLong(0, 42L) }

val b = heap.alloc(NODE)
heap.writeRef(a, OFF_NEXT, b)      // a.next = b bağla (kendini iyileştiren kenar)
heap.setRoot(0, a)                 // durable grafik giriş noktası

arena.commit()                     // çökme tutarlı checkpoint
```

Süreç ölümünden sonra:

```kotlin
val arena = Aevum.attachFile(path)          // O(1)
val heap  = ManagedHeap.attach(arena)       // O(1)
val head  = heap.getRoot(0)                 // grafik yeniden canlı, deserialize yok
val next  = heap.readRef(head, OFF_NEXT)    // bir kenarı takip et (forwarding güvenli)
val value = heap.read(head, SIZE).getLong(0)
```

Burada verdiğiniz offset'ler ve boyutlar v1.0'da native tarafta sınır kontrolünden
geçmez (bkz. [`SECURITY.tr.md`](SECURITY.tr.md), §3). Onları kayıtlı yerleşimle
uyumlu sabitler olarak tutun.

---

## 5. Yaşam döngüsü: `ViewModel` / `onCleared`'a bağlayın

Arenayı bir `ViewModel`'in sahipliğine verin ve `onCleared()`'da kapatın. Kapatma
işlemi flush eder, unmap eder ve fd'yi serbest bırakır (native tarafta RAII).

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
        heap.close()      // önce heap'i, sonra onu destekleyen arenayı kapat
        arena.close()
        super.onCleared()
    }
}
```

Süreç ölümü yolunda arenayı kapatmayın. Bölge zaten bir kill'i atlatmak için
tasarlandı; onu yalnızca *düzgün* bir kapanışta kapatın (`onCleared`, kullanıcının
açıkça oturumu kapatması gibi). Bir LMKD kill'inde işletim sistemi eşlemeyi geri
alır, dosya ya da `memfd` yerinde kalır ve bir sonraki açılışta uygulama yeniden
bağlanır. Bir `Activity`'de kapatmayı `isFinishing`'e bağlayın:

```kotlin
override fun onDestroy() {
    if (isFinishing) store.close()   // yalnızca düzgün çıkışta; arka plan kill'inde asla
    super.onDestroy()
}
```

`close()` idempotent ve `@Synchronized`'dır. Kapatılmış bir handle sonraki
kullanımda exception fırlatır.

---

## 6. Write bariyeri: `WriteScope` / `write { }` için iyi pratikler

Yönetilen heap, taşıyan her GC'deki bariyer sözleşmesinin aynısını kullanır; tek
fark, burada API'de açıkça görünmesidir. Üç kurala uyun:

1. `read` / `readRef` ile okuyun, yalnızca `write { }` içinde yazın. `write` write
   bariyerini devreye sokar: sıkıştırma sırasında evacuate-on-write yapar ve
   yazmayı devam eden yazma olarak kaydeder, böylece bir döngü yazmanın ortasına
   denk gelemez.

   ```kotlin
   // İYİ: mutasyon bariyerin kapsamında
   heap.write(ref, SIZE) { buf -> buf.putLong(0, buf.getLong(0) + 1) }
   ```

2. Bir `ByteBuffer`'ı ya da çıplak bir `MRef`'i `alloc()` veya `compact()`
   çağrısının ötesinde saklamayın. Sıkıştırma nesneleri taşır. Buffer belirli bir
   adresi gösterir, yerel bir `MRef` de kök değildir; ikisi de bayatlar. Her
   mantıksal işlemde onları bir kökten ya da izlenen bir alandan yeniden türetin.

   ```kotlin
   // KÖTÜ: buf/ref, eşzamanlı bir compact() sonrası bırakılmış alanı gösterebilir
   val buf = heap.read(ref, SIZE); compactTetikleyebilecekAgirIs(); buf.getLong(0)   // yanlış

   // İYİ: sıkıştırma tetikleyebilecek işten sonra yeniden çözümle
   compactTetikleyebilecekAgirIs()
   val v = heap.read(ref, SIZE).getLong(0)                                            // doğru (ref bir kökten)
   ```

3. Nesneler arası kenarları `writeRef` ile yazın, `readRef` ile takip edin. Bunlar
   kendini iyileştiren işaretçi ve forwarding üzerinden geçer. Bir handle'ı
   payload'a düz bir `putLong` ile yazarsanız toplayıcı onu taşımaz.

Write kapsamlarını kısa tutun. Sıkıştırıcının faz sonu el sıkışması devam eden
yazmaların bitmesini bekler. Bu hiçbir zaman mutator'ları bloklamaz, ama uzun süre
açık kalan bir kapsam arka plandaki sıkıştırıcıyı geciktirir.

---

## 7. Thread modeli

* `alloc`, `free`, `read`, `write`, `readRef`, `writeRef`, `setRoot`, `getRoot` ve
  `commit` aynı arena/heap üzerinde eşzamanlı çağrılabilir (kilitsiz).
* `create`, `attach` ve `close` aynı nesne üzerindeki diğer çağrılarla
  yarışmamalıdır. Oluşturma ve kapatmayı sıraya koyun.
* `compact()`'i bir arka plan thread'inde çalıştırın (ör. `Dispatchers.IO`).
  Kendini LITTLE cluster'a sabitler ve çalışırken mutator'lar duraklatılmaz.
* Yönetilen belleğe yazan her thread ilk kullanımda küçük, thread'e özel bir
  bağlam kaydeder. Bu bağlamlar hiç serbest bırakılmadığı için sınırsız sayıda
  kısa ömürlü yazıcı thread açmak yerine sınırlı bir thread havuzu kullanın.

---

## 8. Kalıcılık ve sıkıştırma sıklığı

* `commit()`'i anlamlı kontrol noktalarında çağırın (bir toplu işten sonra, arka
  plana geçmeden önce). Bir çökmede yalnızca son `commit()`'ten sonraki mutasyonlar
  kaybolur ve bölge hiçbir zaman bozuk kalmaz (çift tamponlu CRC).
* İsterseniz `arena.enableIncrementalCheckpoint()` çağırın; böylece `commit()`
  yalnızca kirlenen sayfaları flush eder. Bu en çok, küçük bir çalışma kümesiyle
  sık commit edilen büyük arenalarda işe yarar.
* `heap.bytesInUse` canlı kümenin belirgin şekilde üstüne çıktığında `compact()`
  tetikleyin (çok churn olan iş yükleri). Ölü nesneleri eşzamanlı olarak geri
  kazanır ve `CompactResult.reclaimedBytes` ne kadar yer açıldığını bildirir.

---

## 9. Gözlemlenebilirlik

Aevum'un fazlarını Perfetto'da görmek için sistem izlemesi açıkken derleyip
çalıştırın: `Aevum.compact / mark / safepoint / evacuate / fixup / commit`
bölümleri ve `Aevum.live_bytes / Aevum.reclaimed_bytes` sayaçları görünür. Aktif bir
trace yokken enstrümantasyonun maliyeti tek bir tahmin edilebilir dallanmadır,
syscall yapılmaz. Cihaz üstünde tek komutla yakalama (ART GC'nin arenada iş
yapmadığını da gösterir) için `scripts/field_proof_harness.sh` +
`scripts/parse_perfetto.py`'ye bakın.

---

## 10. Canlıya çıkış kontrol listesi

- [ ] `abiFilters` `arm64-v8a` ile sınırlı (hâlâ 32-bit dağıtıyorsanız `armeabi-v7a` da).
- [ ] Arena bir yaşam döngüsü kapsamına ait; `close()` yalnızca düzgün çıkışta.
- [ ] Tüm yazmalar `write { }` içinde; `alloc`/`compact` boyunca saklanan buffer/ref yok.
- [ ] `compact()` UI thread'i dışında; `commit()` kontrol noktalarında.
- [ ] Cihaz dışına çıkardığınız bir arenadaki sırlar uygulama seviyesinde AEAD ile
      sarılmış (bkz. [`SECURITY.tr.md`](SECURITY.tr.md)).
- [ ] Saha kanıtı harness'i hedef bir cihazda en az bir kez çalıştırılmış ve
      C1/C2/C3 sayıları kaydedilmiş.
