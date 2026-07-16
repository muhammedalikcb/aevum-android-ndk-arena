# Aevum: AI iş gücü yönetilerek geliştirilen deneysel, GC'ye görünmez bir Android NDK nesne arenası

*[English version](README.md)*

> *aevum* (skolastik Latince): zaman ile ebediyet arasındaki varoluş kipi;
> etrafındaki dünya doğup ölürken değişmeden kalan kalıcılık.

Aevum, bir Android uygulamasının canlı, işaretçilerle birbirine bağlı bir nesne
grafiğini bellekte tutmasını sağlayan native bir motor. Bu grafiğin iki özelliği
var:

1. **ART çöp toplayıcısı onu hiç görmez.** Concurrent Mark-Compact (CMC)
   toplayıcısı grafiğin tek baytını bile işaretlemez, taramaz ya da taşımaz. Bu
   yüzden grafik, boyutu ne olursa olsun GC kök taramasına da compaction
   demand-fault'larına da sıfır yük bindirir.
2. **Süreç ölümünü O(1)'de atlatır.** İşletim sistemi süreci öldürüp yeniden
   başlattığında grafik yeni (ASLR ile rastgele seçilmiş) bir adrese tekrar
   map'lenir. Deserialization yok, işaretçi düzeltme yok, çünkü her iç bağ
   kendine göreli bir offset olarak saklanıyor.

Sadece ayrıcalıksız bir uygulamanın stok Android 14/15'te erişebildiği
syscall'ları kullanır (`memfd_create`, `mmap`, `mprotect`, `msync`, `sigaction`,
`sched_setaffinity`, bir de LSE atomikleri). Root yok, platform imzası yok,
engellenmiş syscall yok.

### Kapsam: kimler kullanmamalı

Bu, keşif amaçlı bir NDK araştırmasından çıkmış bir kavram kanıtı (PoC). Android
uygulamalarının kabaca %95'i için gereğinden fazla. Durumunuz bir `ViewModel`'e,
bir Room veritabanına ya da birkaç megabaytlık JSON'a sığıyorsa platformun kendi
araçlarını kullanın. Aevum size, olmayan bir problemi çözmek için bir mutator
sözleşmesine (§4.3) ve bir native bağımlılığa mal olur.

Bu karmaşıklık ancak GC duraklama artefaktlarının ve durum kurtarma maliyetinin iş
yükünü belirlediği yerlerde kendini amorti eder: cihaz üstü LLM bağlam
önbellekleme, yüksek frekanslı telemetri, büyük sahne grafikleri, ağır durum
kurtarma. README'nin geri kalanı bu tür iş yükleri için yazıldı ve ödünleşimler
ilgili yerlerde açıkça yazıyor.

### Metodoloji ve AI şeffaflığı

Aevum, AI iş gücünün yönetilmesiyle yürütülen bir sistem mühendisliği deneyi.
Yöntem de sonucun bir parçası, o yüzden okuyucunun tahminine bırakmak yerine
burada anlatıyoruz.

İnsan mimar problem çerçevesini ve kısıtları belirledi, cevabı ise bilerek
vermedi. İlk direktif, hiçbir ipucu olmadan çözülmemiş bir Android/NDK darboğazı
bulmaktı. Mimar oradan sonra:

- tasarımın sağlaması gereken özellikleri sabitledi: stop-the-world yok, hiçbir
  mutator 1 µs bile bloklanmayacak, kilitsiz (lock-free) bir ziyaretçi,
  big.LITTLE genelinde cache line izolasyonu ve bir LMKD öldürmesine karşı
  kalıcılık;
- mühendislik standartlarını sabitledi: mock yok, stub yok, TODO yok, uydurma sayı
  yok (bu depodaki her rakam gerçekten koşturulmuş bir ölçümden geliyor);
- elde ARM64 donanım olmadığı için cihazsız doğrulama lab'ını istedi;
- her aşamada çıktıyı inceledi, reddetti ve kapsamını yeniden çizdi.

Mimarın yönettiği AI (Claude Opus 4.8) bu çerçevenin içindeki işi yaptı: hedef
olarak CMC'nin dağıtılmış fault fırtınasını seçen alt sistem taraması, ilk
tasarımı eleyen red-team analizi, bellek yerleşim hesapları, C++20
implementasyonu, eşzamanlı sıkıştırma algoritması, TSan/ASan/UBSan turları,
cihazsız kanıt lab'ı ve bu dokümantasyon.

Deney tam olarak bu iş bölümü. Bu, sürecin pürüzsüz geçtiği anlamına gelmiyor:

- İlk tasarım (render thread'inin önünden sayfaları önceden taşıyan, uygulama
  tarafında çalışan bir scheduler), implementasyonu savunulduktan sonra kendi
  red-team'i tarafından elendi (§0).
- Sıkıştırıcıdaki üç data race tasarım incelemesinden geçti ve sadece
  ThreadSanitizer ile yakalandı. Onları kodu gerekçelendiren argüman değil, kodu
  çalıştırmak buldu. §4.4'teki temiz sanitizer sonucu bu düzeltmelerden *sonraki*
  durum.
- Otomatik bir dokümantasyon geçişi, benzersiz olamayacak kadar kısa bir arama
  çıpası yüzünden bu README'yi bozdu. Yapısal bir parite kontrolü bunu yakaladı,
  düzenlemeyi yapan model yakalamadı.

Motor bu noktaya üretim adımı güvenilir olduğu için değil, her iddia çalıştırılıp
sınandığı için geldi. Ağırlığı koşturulmuş sonuçlara verin (aşağıda §3 ve §4.4,
bir de [whitepaper](docs/WHITEPAPER.md)'ın §6'sı), düzyazıya hiç vermeyin.

### Referans dokümanlar

| Belge | Nedir |
|---|---|
| [**WHITEPAPER**](docs/WHITEPAPER.tr.md) | Biçimsel sistem makalesi: O(1)-reattach teoremi, eşzamanlı sıkıştırmanın doğruluğu, ragged safepoint'in Dekker ispatı, yanlışlanabilir iddialar, koşturulmuş sonuçlar |
| [**ADR-001…003**](docs/architecture/ADR-001-to-003.tr.md) | Neden `userfaultfd` yok, neden JVM reflection yok, neden CAS döngüleri yerine LSE |
| [**INTEGRATION_GUIDE**](INTEGRATION_GUIDE.tr.md) | AAR'ı projeye ekleme, yaşam döngüsüne bağlama, `WriteScope` için iyi pratikler |
| [**SECURITY**](SECURITY.tr.md) | Tehdit modeli: sandbox izolasyonu, bellek güvenliği bildirimleri, diskte duran veri |
| [`scripts/`](scripts/) | `field_proof_harness.sh` + `parse_perfetto.py`: tek komutla cihaz üstü kanıt |
| [**`./run_headless_lab.sh`**](run_headless_lab.sh) | Cihaz gerekmez. [`test_lab/`](test_lab/)'ı derleyip çalıştırır: 100k düğümlü grafik → sıkıştırmanın ortasında gerçek bir `SIGKILL` → yeni bir PID yeni bir ASLR tabanında tekrar bağlanır → her düğüm doğrulanır ve bir Perfetto trace'i yazılır |

### Android cihaz olmadan iddiayı doğrulamak

```bash
./run_headless_lab.sh --sweep     # ~10sn; her sayı ölçülür, iddia edilmez
```

Android'in çekirdeği Linux ve iddiaların dayandığı primitifler (dosya destekli
`mmap` kalıcılığı, `fork`, yakalanamayan `SIGKILL`, ASLR) Linux ya da macOS bir
host'ta da aynı davranıyor. Lab'ın mock değil gerçek bir test sayılmasının sebebi
bu. Gereken tek şey `cmake` ve bir C++20 derleyicisi. Kökteki script
`test_lab/run_headless_lab.sh`'a yönlendirir; o da ayrıca `-o <outdir>`, `--asan`
(ASan + UBSan derlemesi) ve `--keep` (build dizinini silme) seçeneklerini alır.

Bununla ölçülenler:

| N (düğüm) | reattach + getRoot | tam O(N) gezinme | sıkıştırma ortası SIGKILL sonrası |
|----------:|-------------------:|-----------------:|:----------------------------------|
| 1.000 | ~42 µs | 7,6 µs | 1.000/1.000 · 0 yırtık okuma |
| 100.000 | ~50 µs | 743 µs | 100.000/100.000 · 0 yırtık okuma |
| **1.000.000** | **~43 µs** | 8.617 µs | 1.000.000/1.000.000 · 0 yırtık okuma |

Reattach süresi N'nin 1000 katlık aralığında yatay kalıyor, yani O(1). Aynı
ölçüm aracı tam gezinmenin doğrusal büyüdüğünü de gösteriyor. Kontrol grubu bu
gezinme: yatay eğrinin ölçümden değil sistemin kendisinden geldiğini gösteriyor.
Üretilen `test_lab/aevum_autonomous_proof.json` dosyasını
<https://ui.perfetto.dev> üzerine sürükleyin; SIGKILL'i ve ardından yeni bir PID'de,
farklı bir taban adreste gelen reattach'i görebilirsiniz.

---

## 0. Motivasyon: Aevum'un ele aldığı kör nokta

Altı Android alt sistemi üzerinde otonom bir tarama yapıldı; her aday prior-art
süzgecinden geçirilip değerlendirildi. Ortaya şu yapısal çıkmaz çıktı:

> **Android 14+'taki CMC toplayıcısı GC duraklamasını ortadan kaldırmadı. Onu
> ölçülmeyen, thread başına `SIGBUS` fault fırtınasına bölüp mutator'ların
> süresine yıktı.**

Her aracın raporladığı ~1,8 ms'lik "GC duraklaması" (Perfetto GC dilimleri, Studio
profiler, ANR izleri) sadece `MarkingPause` + `FlipThreadRoots` penceresi.
Sıkıştırmanın geri kalanı eşzamanlı çalışıyor. Henüz taşınmamış bir moving-space
sayfasına dokunan mutator bir `userfaultfd` demand-fault'u alıyor ve sayfa
kopyasını ve yerinde referans düzeltmesini kendi thread'inin süresinden yapıyor.
Bunların hiçbiri "duraklama" sayısında görünmüyor. Sıkıştırma başladığı anda büyük
bir nesne grafiğini gezmekte olan bir UI ya da render thread'i bunlardan art arda
yiyor. Maliyet ortadan kalkmadı, hiçbir trace'in göstermediği bir yere taşındı.

### Neden Aevum, neden CMC için bir "düzeltme" değil

Ardından bir red team akla ilk gelen çözümü kırmaya çalıştı: render thread'inin
önünden sayfaları önceden taşıyan, uygulama tarafında çalışan bir scheduler. Bu
fikir ayakta kalamadı ve Aevum bu sonucu olduğu gibi kabul ediyor:

- Bir uygulamadan ART'ın heap'ine dokunamazsınız. `userfaultfd` VMA başına tek
  sahiplidir, yani moving space üzerine ikinci bir `UFFDIO_REGISTER` `EEXIST`
  döndürür. Üstelik uygulama seccomp politikası `userfaultfd`'yi doğrudan
  engeller (`SIGSYS`) ve `vm.unprivileged_userfaultfd=0`. ART ona sadece
  ayrıcalıklı bir kurulum yolundan erişir.
- Sıkıştırma işi korunur. Her canlı sayfa tam olarak bir kez kopyalanır ve
  toplam işi sabit olan bir fazın işini hiçbir scheduler azaltamaz.

Red team'in kendi vardığı sonuç, bu tasarımın çıkış noktası: **korunan GC işinden
kaçmanın tek yolu nesneleri heap'ten tamamen çıkarmak.** Aevum tam olarak bunu
yapıyor. ART'ın sıkıştırmasını zamanlamaya çalışmıyor; ART'ın sıkıştıracağı
veriyi ART'ın gözünde *yok* hale getiriyor ve onu canlı, değiştirilebilir, kalıcı
bir nesne grafiği olarak tutmaya devam ediyor.

Bırakılan varsayım: *"bir Kotlin/Java nesnesinin ömrü, sürecinin ömrüyle ve ART
heap'inin değişmezleriyle sınırlıdır."* Aevum nesneleri süreçten uzun yaşar ve
heap'te hiç bulunmamıştır.

Prior art konusunda: kendine göreli işaretçiler (boost `offset_ptr`) ve page fault
anında işaretçi swizzling (Wilson, 1991) yeni fikirler değil. Tencent'in MMKV'si
piyasadaki en yakın Android örneği ama düz bir anahtar-değer deposu, işaretçilerle
bağlı canlı bir grafik değil. Burada yeni olan, parçaları Android'in tam
kısıtları altında bir araya getirmek: GC'ye görünmez, O(1)'de ortogonal kalıcı,
kendine göreli bir nesne grafiği; yanında big.LITTLE'ı gözeten kilitsiz bir
allocator ve mprotect fault'u ile çalışan artımlı checkpoint. Bu bileşimi sunan,
piyasada olan bir Android kütüphanesine rastlamadık.

---

## 1. Yanlışlanabilir iddia

> N düğümlü bir nesne grafiği bir Aevum arenasında tutulduğunda:
> **(a)** ART GC duraklama süresine ve CMC compaction demand-fault'larına katkısı,
> N'den bağımsız olarak tam **sıfırdır**; ve
> **(b)** süreç ölümünden sonra grafiği yeniden canlı kılma süresi N'de **O(1)**'dir
> (sabit bir `mmap` + başlık doğrulaması), eşdeğer bir Java nesne grafiğini
> deserialize etmenin O(N)'i değil.
>
> Bir ART GC izi arena belleğine herhangi bir mark/compaction maliyeti atfederse veya
> reattach gecikmesi grafik boyutuyla büyürse Aevum yanlışlanmış olur.

İkisi de tasarımın doğal sonucu. (a) Bölge ART heap'ine hiç kaydedilmiyor, yani
toplayıcının içeriye uzanan bir kökü yok. (b) Bütün bağlar konumdan bağımsız, yani
taşımak hiçbir şey yapmamakla aynı.

---

## 2. Mimari

### 2.1 Temel mekanizma

- Bir bölge ya bir `memfd_create` nesnesiyle (tmpfs; fd'yi tutan biri olduğu
  sürece süreç ölümünü atlatır) ya da özel bir dosyayla (`mmap`+`MAP_SHARED`;
  süreç ölümünü *ve* yeniden başlatmayı atlatır) desteklenir.
  `F_SEAL_SHRINK|GROW` bir memfd'nin boyutunu dondurur.
- Nesneler birbirini [`OffsetPtr<T>`](native/src/offset_ptr.h) üzerinden
  gösterir: `hedef − &alan` değerini tutan tek bir 64 bitlik kelime. Bölgenin
  tamamı taşındığında her alan ve hedefi aynı miktarda kayar, saklanan mesafe
  değişmez ve reattach hiçbir düzeltme gerektirmez. Null, offset `1` olarak
  kodlanır; 16'ya hizalı hiçbir hedef bu değeri üretemez. Grafiğe kalıcı giriş
  noktaları, tabana göreli [`Handle`](native/src/offset_ptr.h) ile adreslenen 64
  slotluk bir kök tablosunda durur.
- [Kilitsiz segregated-fit allocator](native/src/allocator.cpp), 16 B'tan 64 KB'a
  ~1,25x büyüyen geometrik bir merdivende 34 boyut sınıfına sahip. Her sınıf,
  başı `48-bit offset + 16-bit ABA etiketi` olarak paketlenen ve tek bir 64 bitlik
  CAS ile değiştirilen kilitsiz bir Treiber yığını. Boş sınıfların arkasında
  kilitsiz bir bump pointer var. Bump pointer, free-list başları dizisi, kök
  tablosu ve commit kilidi ayrı cache line'larda başlar. Dizinin içindeki sınıf
  başları ise 8 bayt arayla durduğu için her cache line'ı sekiz sınıf paylaşır.
  16 baytlık blok başlığı sınıf indeksini ve double-free'yi elden geldiğince
  yakalamak için bir alloc/free magic değerini tutar.
- [`commit()`](native/src/arena.cpp) çökmeye karşı tutarlıdır. Canlı kontrol
  bloğunun ve köklerin anlık görüntüsünü çift tamponlu iki checkpoint slotundan
  *aktif olmayana* yazar, CRC32C ile mühürler, `msync` çağırır ve aktif slotu
  atomik olarak değiştirir. Yazmanın ortasındaki bir çökme hiçbir şeyi bozamaz,
  çünkü attach CRC'si tutan en yeni slotu seçer. Commit edilmiş bir grafik sadece
  son commit'ten sonraki değişiklikleri kaybeder.

### 2.2 İsteğe bağlı, fault ile çalışan hızlandırıcı (aynı fikir, uffd gerekmez)

[`DirtyTracker`](native/src/dirty_tracker.cpp) isteğe bağlı (opt-in) bir artımlı
checkpoint. Canlı veri sayfalarını yazmaya karşı korur ve zincirlenmiş bir
`SIGSEGV`/`SIGBUS` handler'ı kurar. Her epoch'ta bir sayfaya yapılan ilk yazma
fault verir. Async-signal-safe handler (atomikler ve tek bir `mprotect`, libc yok)
sayfayı kirli olarak işaretleyip korumayı kaldırır ve yazma fark edilmeden yeniden
denenir. Sonraki `commit()` sadece kirlenen sayfaları flush eder: `O(bölge)`
yerine `O(çalışma kümesi)`.

Bu, CMC'nin kullandığı fault tabanlı sayfa servisleme yaklaşımının aynısı; sadece
`mprotect` + `sigaction` üzerine yeniden kurulmuş. Bu ikisine, sandbox'ın
engellediği `userfaultfd`'nin aksine, uygulamadan erişilebiliyor.

### 2.3 Kademeli geri çekilme: platform varsayılanından asla kötü değil

| Kademe | Gerektirir | Yoksa → |
|------|----------|------------------|
| Artımlı checkpoint | `mprotect`+`sigaction` kabul edilir | `enableIncrementalCheckpoint()` false döner; `commit()` tam flush yapar. Doğru, yalnızca daha ağır. |
| memfd desteği | `memfd_create` (Linux/Android) | `Aevum.createInMemory` `AevumException(UNSUPPORTED)` fırlatır; `createFile` kullanın (ayrıca daha kalıcı). |
| Donanım CRC32 / LSE | ARMv8.1 (hedefte evrensel) | çalışma zamanında yok: ikisi de derlemeye gömülü (`-march=armv8-a+crc+lse`), bu yüzden arm64 build ARMv8.1 ister. |
| 16 KB sayfa | Android 15 büyük sayfa cihazları | sayfa boyutu `sysconf` ile çalışma zamanında okunur; eşlemeler sayfadan bağımsızdır. |

Derleme anında seçilen CRC32 ve LSE dışında hiçbir şeyin var olduğu varsayılmıyor.
Diğer her yetenek çalışma zamanında yoklanıyor ve her başarısızlıkta daha yavaş ama yine doğru bir yola düşülüyor. Her şeyin
dayandığı kendine göreli arena sadece `memfd_create`/`mmap` istiyor ve ikisinin de
izinli olduğu doğrulandı.

### 2.4 Dil ve teknoloji

Bunu Kotlin/Java ile ifade etmek mümkün değil. Nesne yerleşimi ve kimliği JVM'in
elinde, GC nesneleri taşıyabiliyor ve bir Java nesne grafiğini yönetilmeyen bir
bellek eşlemesine koymanın ya da nesneler arası çıplak referansları yeniden
`mmap` sonrasında sabit tutmanın yolu yok. Motor C++20 ile yazıldı
(`-fno-exceptions -fno-rtti`): deterministik yerleşim, ARM'ın zayıf bellek
modelinde açık bellek sıralaması ve doğrudan syscall'lar için. Kotlin ise üstte
ince ve yanlış kullanılması zor bir katman. Bir [C ABI](native/include/aevum/aevum.h)
de motoru NDK/C++ tarafındaki kullanıcılara açıyor.

### 2.5 Genel API (Kotlin)

```kotlin
Aevum.createFile("/data/.../graph.aevum", 64L * 1024 * 1024).use { arena ->
    val node = arena.alloc(64)                 // Ref: durable bir handle
    arena.view(node).apply {                   // zero-copy ByteBuffer, native sıra
        putLong(0, 42); putLong(8, child.handle)
    }
    arena.setRoot(0, node)                      // durable grafik giriş noktası
    arena.enableIncrementalCheckpoint()         // opt-in; desteklenmezse false
    arena.commit()                              // çökme tutarlı checkpoint
}
// süreç ölümünden sonra:
Aevum.attachFile("/data/.../graph.aevum").use { arena ->
    val root = arena.getRoot(0)                 // grafik yeniden canlı, O(1)
}
```

`Ref` bir `@JvmInline value class`. Çalışma zamanında maliyeti yok ve handle
beklenen yere çıplak bir `Long` geçmenizi engelliyor. Okuma ve yazmalar payload'u
doğrudan gösteren bir `ByteBuffer` üzerinden yapılıyor; sıcak yolda buffer'ı almak
için tek bir JNI çağrısı var, sonrasında hiç yok. Kapatılmış bir arena üzerindeki
çağrılar exception fırlatır, `close()` ise idempotent.

### 2.6 Thread ve yaşam döngüsü sözleşmesi

`alloc`/`free`/`view`/`setRoot`/`getRoot`/`commit` aynı arena üzerinde eşzamanlı
çağrılabilir (allocator kilitsiz). Oluşturma ve `close()` ise aynı örnek
üzerindeki diğer çağrılarla yarışmamalı. Native tarafın kendi başına çalıştırdığı
tek kod `DirtyTracker` fault handler'ı ve o JVM'i hiç çağırmıyor. Yani yanlış
yapılabilecek bir `AttachCurrentThread`/`Detach` düzeni ya da sızabilecek bir
`JNIEnv` yok. Arenayı `~Arena` unmap edip flush ediyor (RAII). `close()` unmap
etmeden önce yazma korumasını kaldırıyor, böylece handler serbest bırakılmış
belleğe hiç dokunmuyor.

---

## 3. Doğrulama

Saf algoritmik çekirdek bir host üzerinde derlenip gerçekten çalıştırıldı, sadece
iddia edilmedi. Platforma özgü yollar gerçek NDK sysroot'una karşı çapraz
derlendi.

- Konum bağımsızlığı ve O(1) reattach: 65.535 düğümlü bir grafik kurulur, commit
  edilir, sonra aynı anda 64 MB arayla farklı tabanlardaki iki örneğe yeniden
  map'lenir. İkisi de hiçbir düzeltme yapmadan aynı checksum'a ulaşır.
- Allocator: 40.000 rastgele alloc/free işlemi. Çakışan payload yok, double-free
  reddediliyor ve geri dönüşüm high-water mark'ı sınırlıyor.
- Eşzamanlılık: 8 thread'de 1.600.000 işlem, thread'ler arası sıfır bozulma.
  Treiber intrusive-link erişimleri `__atomic` relaxed'a taşındıktan sonra
  ThreadSanitizer temiz.
- UB ya da bellek hatası yok: Address ve UndefinedBehavior sanitizer'ları,
  artımlı write-protect fault handler'ı da dahil olmak üzere temiz.
- Artımlı checkpoint: grafik tamamen yazma koruması altında kurulur (her yazma
  fault → handler → `mprotect` → yeniden dene), commit edilir, değiştirilir,
  tekrar commit edilir ve yeniden bağlanır. Diske yazılan görüntü doğru.
- Derleme: tüm çeviri birimleri `aarch64-linux-android` için çapraz derleniyor.
  `libaevum.so`, `-Wl,--no-undefined` ile linkleniyor, `JNI_OnLoad` + C ABI'yi
  export ediyor ve sadece `libc/libm/libdl/liblog/libandroid/libc++_shared`'a
  bağımlı. Arena API'sindeki 15 Kotlin `external` tanımı JNI `RegisterNatives`
  tablosuyla bayt bayt eşleşiyor.

---

## 4. Aevum-Defrag: uygulamanın kendi eşzamanlı sıkıştırması

Yukarıdaki arena nesneleri ART'ın GC'sinden saklıyor ama kendine ait bir
sıkıştırması yok. Sürekli tahsis edip bırakan uzun ömürlü bir grafik zamanla
parçalanır. Aevum-Defrag, yönetilen bir nesne heap'i için uygulamanın kendisine
ait, stop-the-world yapmayan, kopyalayan (mark-compact) bir toplayıcı. Aevum
kendi alanını kendi takviminde, ART'ın kullandığı türden bir mekanizmayla ve
tamamen kullanıcı alanında geri kazanıyor.

### 4.1 Model

Yönetilen heap, bir arena bölgesinden ayrılmış iki semispace'ten oluşuyor.
Nesneler aktif alana bump ile tahsis ediliyor. Nesne başına free yok; geri kazanım
sıkıştırmanın *kendisi*. Tipler referans alanlarını gezen O(1) bir
[`TraceFn`](native/src/type_registry.h) kaydediyor (reflection yok) ve her nesne
bir forwarding kelimesi, bir mark epoch'u ve bir `type_id` içeren 16 baytlık bir
[`ObjectHeader`](native/src/type_registry.h) taşıyor. Referanslar
[`SelfHealingOffsetPtr`](native/src/self_healing_ptr.h) tipinde: `OffsetPtr`
gibi kendine göreli, üstüne bariyerler eklenmiş.

`compact()` bir arka plan thread'inde çalışır ve `cpu_capacity` okunarak LITTLE
cluster'a sabitlenir. Üç fazı var ve baştan sona mutator'larla eşzamanlı çalışır:

1. **Eşzamanlı mark (SATB).** Kalıcı köklerden başlayıp canlı grafiği `TraceFn`
   ile gezer ve bir mark epoch'u damgalar. Referans yazmalarındaki
   snapshot-at-the-beginning silme bariyeri, başka thread'ler bağları koparırken
   canlılık bilgisini doğru tutar. Mark sabit noktaya ulaşana kadar sürer; SATB
   taşması ya da yakınsamama durumunda tüm döngüyü hiçbir şeyi değiştirmeden
   temiz şekilde iptal eder.
2. **Evacuate + forward.** İşaretlenmiş her from-space nesnesini to-space'e
   kopyalar, kendine göreli alanlarını yeni tabana göre ayarlar, sonra eski
   başlığa kazananın her şeyi aldığı bir `CAS` (LSE) ile forwarding kurar. Yazması
   gereken mutator'lar aynı protokolle yazarken tahliye ederek (evacuate-on-write)
   işbirliği yapar, böylece yazmalar her zaman kanonik kopyaya düşer.
3. **Proaktif fixup + geri kazanım.** Tahliye edilen nesneleri ve kökleri gezer,
   forward edilmiş her referansı to-space kopyasını gösterecek şekilde yeniden
   yazar, sonra kalıcı commit'i ilerletir ve from-space'i bırakır.

### 4.2 Dört zor kısıt nasıl karşılanıyor

- Stop-the-world yok. Bir mutator yolu hiçbir zaman mutex ya da spinlock almaz,
  `sleep` veya `yield` çağırmaz. Koordinasyon bir forwarding read bariyeri
  (zinciri takip et, salt okunur), bir evacuate-on-write bariyeri ve kilitsiz
  `CAS`/atomik store'lar ile sağlanır. Spin yapan tek yer sıkıştırıcının faz sonu
  el sıkışması; mutator asla.
- Kilitsiz ziyaretçi. Read bariyeri kesinlikle salt okunur. Bu yüzden from-space
  döngü boyunca bayt bayt salt okunur kalır ve tahliyeyi yapan `memcpy` hiçbir
  mutator yazmasıyla yarışamaz. Bir mutator'ın dokunduğu tek paylaşımlı döngü
  durumu tek bir atomik `phase` kelimesi.
- big.LITTLE izolasyonu. Thread'ler arası sıcak kelimeler (paketlenmiş tahsis
  imleci, her thread'in `in_write` bayrağı, SATB head/tail, kök tablosu) kendi
  cache line'larında `alignas(64)` olarak duruyor. Böylece E-core'daki sıkıştırıcı
  ile P-core'da 120 Hz çalışan bir mutator aynı cache line'ı birbirine
  sektirmiyor.
- LMKD / süreç ölümüne karşı kalıcılık. Kalıcı checkpoint (çift tamponlu, CRC ile
  mühürlü, [`ManagedHeader`](native/src/compactor.h) içinde) Faz 3 tamamen
  bitene kadar *from-space*'i göstermeye devam ediyor ve sıkıştırıcı from-space'in
  üzerine hiç yazmıyor. Sıkıştırmanın ortasında gelen bir kill from-space'i ve
  commit edilmiş köklerini sağlam bırakıyor; `attach()` sıkıştırma öncesindeki
  grafiği aynen geri getiriyor ve yarım kalmış to-space yok sayılıyor.

### 4.3 Mutator sözleşmesi

Tamamen genel, bariyersiz eşzamanlı sıkıştırma mümkün değil. Production'daki her
eşzamanlı toplayıcı (ART CC, Shenandoah, ZGC) bariyer kullanıyor, bu da öyle. Fark
şu: burada bariyerler derleyicinin eklediği bir şey değil, API'de açıkça
görünüyor.

- `resolveRead(ref)` / `SelfHealingOffsetPtr::get()` ile okuyun.
- Kapsamlı bir [`WriteScope`](native/src/compactor.h) ile yazın. Bu,
  evacuate-on-write yapar ve yazmayı "devam ediyor" olarak kaydeder; böylece yazma
  bir döngü sınırını aşamaz.
- Bir `alloc()` ya da `compact()` boyunca çıplak bir payload işaretçisini elde
  tutmayın, köklerden yeniden türetin. Yerel işaretçiler kök değil, bu yüzden
  toplayıcı onları düzeltemez. Stack taraması olmayan her yönetilen çalışma
  ortamında durum aynı.

Bu sınırın ilk sürümünün bir throughput bedeli vardı. Sıkıştırıcı global bir
in-flight sayacının sıfıra inmesini bekliyordu; *doygun ve sürekli* yazma yükü
altında bu yüzden throughput düşüyordu (boş bir an için daha uzun bekliyordu).
Hiçbir mutator'ı bloklamadı, hiçbir şeyi bozmadı. §5.1 bu global beklemeyi ragged
safepoint ve thread başına el sıkışma ile değiştiriyor.

### 4.4 Defrag doğrulaması (koşturuldu)

- Eşzamanlı bütünlük: 4.096 düğümlü bir omurga 4→24 eşzamanlı mutator thread'i,
  çöp üreten thread'ler ve canlı bir sıkıştırıcı altında çalıştırıldı. Sonunda
  omurga sağlam ve her düğümde `checksum == hash(id, counter)` tutuyor (yırtık ya
  da kayıp yazma yok); ~1.000 sıkıştırma döngüsü ve yüz binlerce değişiklik
  boyunca.
- Geri kazanım: ~200 bin çöp nesne üretilip bırakıldıktan sonra canlı bayt sayısı
  tam olarak omurga boyutuna dönüyor. Çöpü sıkıştırma topluyor.
- Çökmeye dayanıklı reattach: eşzamanlı sıkıştırmadan sonra kapatıp yeniden
  bağlanınca 4.096 düğümün hepsi kalıcı commit'ten geri geliyor.
- ThreadSanitizer temiz (içerik yazan mutator'lar + çöp üreticiler +
  sıkıştırıcı), Address ve UndefinedBehavior sanitizer'ları da temiz.
- `aarch64-linux-android` için çapraz derleniyor ve `libaevum.so`'nun içine
  giriyor.

---

## 5. Çekirdek ölçekleme, saha kanıtı, ürün yüzeyi

### 5.1 Ragged safepoint'ler + thread'e özel SATB (`safepoint.{h,cpp}`)

Mutator ile sıkıştırıcı arasındaki eski sınır, paylaşımlı tek bir MPSC SATB
tamponu (her yazma paylaşımlı bir baş üzerinde CAS yapıyordu) ve sıkıştırıcının
sıfıra inmesini beklediği global bir in-flight sayacı kullanıyordu. Doygun yazma
yükü altında bu sayaç neredeyse hiç sıfıra inmediği için ~16 thread'in üstünde
sıkıştırma throughput'u çöküyordu. İkisi de değiştirildi:

- Thread'e özel SATB. Her mutator thread'inin kendi cache line'ında izole bir SPSC
  ring'i var ([`ThreadLocalSatbBuffer`](native/src/safepoint.h)) ve write bariyeri
  thread'ler arası CAS olmadan tek üreticili bir ekleme yapıyor. Sıkıştırıcı her
  thread'in ring'ini sırayla boşaltıyor.
- Ragged safepoint. From-space'i tahliye için salt okunur hale getirirken
  sıkıştırıcı artık global bir sıfır beklemiyor. Mutator kayıt listesini gezip her
  thread'le ayrı ayrı el sıkışıyor: thread'in bir `in_write == 0` boşluğundan
  geçtiğini görüyor ve sıradakine geçiyor. Thread'ler birbirini hiç beklemiyor,
  her biri bir write scope içinde bir boşlukta yakalanıyor. Doğruluk, `beginWrite`
  ile el sıkışma arasındaki store-sonra-`seq_cst`-load (Dekker) eşleşmesine ve
  *alanlar yer değiştirmeden önce* yapılan ikinci bir el sıkışmaya dayanıyor.
  İkincisi sayesinde hiçbir işbirlikçi tahliye bir döngü sınırını aşıp yanlış
  alana düşemiyor. Bu son durum gerçek bir hataydı; bu bölümün testleri yakaladı
  ve düzeltildi.

Ölçülen ölçekleme (host, 64 düğümlü omurga, thread başına değişiklik + canlı
sıkıştırıcı, sabit bir pencerede tamamlanan sıkıştırma döngüsü sayısı; yüksek
olan iyi):

| mutator thread | global drain (reddedildi) | ragged safepoint |
|---:|---:|---:|
| 16 | ~120 | **~1.600** |
| 32 | raporlanmadı (daha kötü) | **~500** |
| 64 | raporlanmadı | **doğru, ~50** |

8→64 thread'te doğru (`bad=0`) ve ThreadSanitizer, ASan ve UBSan altında temiz.
Global drain tasarımının getirdiği throughput bedeli böylece ortadan kalkıyor.

### 5.2 Saha kanıtı için enstrümantasyon ve benchmark (`profiler.{h,cpp}`, `benchmark/`)

- Perfetto / atrace. [`profiler.h`](native/src/profiler.h); `compact`, `mark`,
  `safepoint`, `evacuate`, `fixup` ve `commit` adımlarını `android/trace.h`
  üzerinden sistem trace bölümleri ve canlı sayaçlar olarak dışa veriyor. Bir
  trace kapsamı önce `ATrace_isEnabled()`'a bakıyor; kayıt alınmıyorsa sıcak yolun
  maliyeti tek bir dallanma, syscall yok. Android dışında hiçbir koda derlenmiyor.
- Burada gerçekten çalıştırılanlar (host üzerinde, cihaz olmadan). Bir host
  benchmark'ı gerçek motoru sürüyor:
  - O(1) reattach: reattach + `getRoot`, grafik 1 k, 10 k, 100 k ya da 1.000.000
    düğüm olsun ~30 µs'de sabit kalıyor (29,4 / 31,0 / 32,8 / 31,5 µs). 1 M
    düğümlü bir nesne grafiğini deserialize etmek ise O(N), yani yüzlerce ms.
  - Stop-the-world yok: 100 k düğümlü bir grafikte arka arkaya 200 `compact()`
    döngüsü boyunca 8 mutator thread'i 95 milyon değişiklik yaptı. Hiç
    durdurulmadılar.
- Hâlâ cihaz gerektirenler. [`AevumBenchmark.kt`](benchmark/AevumBenchmark.kt)
  cihaz üstü sürücü: `reattachVsDeserialize()` (Aevum mmap ile O(N) Java yeniden
  inşası karşılaştırması) ve `gcPressure()` (`Debug.getRuntimeStat`'tan ART GC
  duraklama/sayısı; grafik ART heap'indeyken ve Aevum arenasındayken). Her şeyi
  gerçek çalışma zamanı sayaçlarından hesaplıyor. ART ile Aevum'un GC
  karşılaştırması ve 120 Hz jank ölçümü için bunun bir telefonda çalıştırılması
  gerekiyor. Bu henüz yapılmadı ve bunun için herhangi bir sayı verilmiyor.

### 5.3 Ürün yüzeyi: AAR + süreç ölümünü atlatan LLM showcase'i

- Kotlin'den yönetilen heap. Tipler referans alanlarının bayt offset'leriyle
  kaydediliyor ([`TypeRegistry::registerLayout`](native/src/type_registry.h) ve
  offset'leri gezen genel bir tracer), yani tip başına native kod gerekmiyor. Bir C
  ABI (`aev_mh_*`), JNI ve [`AevumManaged.kt`](kotlin/AevumManaged.kt) içindeki
  `ManagedHeap` sınıfı; alloc, kapsamlı bir `write` (bariyer), grafik kenarları için
  `readRef`/`writeRef`, kökler ve `compact` işlemlerini sunuyor. 29 Kotlin
  `external` tanımının tamamı (15 arena, 14 yönetilen heap) JNI tablosuyla bayt
  bayt eşleşiyor ve her çeviri birimi `libaevum.so` içine çapraz derleniyor.
- AAR. [`build.gradle.kts`](build.gradle.kts), `libaevum.so`'yu (`arm64-v8a` +
  `armeabi-v7a`, 16 KB sayfaya hizalı) ve Kotlin API'sini tek bir `.aar` içine
  paketliyor; C ABI başlıklarını da CMake Prefab ile dışa açıyor.
- Showcase (Jetpack Compose).
  [`ShowcaseApp.kt`](showcase/src/main/kotlin/dev/aevum/showcase/ShowcaseApp.kt),
  bir LLM konuşmasını Aevum yönetilen heap'inde bir token zinciri olarak tutuyor.
  "Trigger LMKD Process Death" butonu süreci sert biçimde öldürüyor; `onCreate`
  grafiği O(1)'de yeniden bağlıyor ve deserialize etmeden tam kalınan token'dan
  devam ediyor. Canlı bir HUD, 120 Hz Choreographer kare aralığını `compact()`
  durumunun yanında çiziyor; GC'ye görünmezlik iddiasını gözünüzle görebiliyorsunuz.
  JNI ve arena ömürleri RAII/`AutoCloseable` ile yönetiliyor ve arena kill yolunda
  bilerek *kapatılmıyor*, çünkü demonun bütün amacı o kill'i atlatmak.

Derlenmeyen ve çalıştırılmayanlar: Gradle/AGP derlemesi ve Compose uygulaması bu
ortamda derlenip çalıştırılamadı (cihaz yok, Gradle/AGP yok). Kotlin API katmanı
(`Aevum.kt`, `AevumManaged.kt`) burada derlendi ve imzaları kontrol edildi.
`build.gradle.kts`, manifest, Compose arayüzü ve benchmark gerçek bir Android
derlemesi için yazılmış eksiksiz kod. Burada çalıştırılarak doğrulanmadılar ve
hiçbirinde stub ya da sahte kod yok. Depoda Gradle wrapper ya da
`settings.gradle.kts` da yok; modülü kendi Gradle projenize eklemeniz gerekiyor.

## 6. Dosya yapısı

```
native/include/aevum/aevum.h    C ABI (kararlı, export edilir)
native/src/offset_ptr.h         kendine-göreli işaretçi + Handle
native/src/region_layout.h      bölge ikili yerleşimi (başlık, sınıflar, checkpoint'ler)
native/src/platform.h           syscall'lar, atomikler, CNTVCT zamanlama, CRC32C
native/src/allocator.{h,cpp}    kilitlenmesiz segregated-fit tahsisleyici
native/src/arena.{h,cpp}        RAII bölge sahibi, create/attach/commit
native/src/dirty_tracker.{h,cpp} mprotect-fault artımlı checkpoint
native/src/type_registry.h      nesne tipleri (TraceFn + offset layout), ObjectHeader
native/src/self_healing_ptr.h   forwarding read bariyeri + SATB write bariyeri
native/src/safepoint.{h,cpp}    thread-yerel SATB + ragged-safepoint kayıt defteri
native/src/compactor.{h,cpp}    eşzamanlı kopyalayan sıkıştırıcı + WriteScope
native/src/profiler.{h,cpp}     Perfetto/atrace enstrümantasyonu
native/src/aevum_c.cpp          Arena + yönetilen heap üzerinde C ABI
native/jni/aevum_jni.cpp        JNI köprüsü (RegisterNatives)
native/CMakeLists.txt           libaevum.so derlemesi (arm64-v8a + armeabi-v7a)
kotlin/Aevum.kt                 genel Kotlin arena API'si
kotlin/AevumManaged.kt          genel Kotlin yönetilen-heap API'si
build.gradle.kts                AAR + Prefab paketleme
showcase/…/ShowcaseApp.kt       süreç ölümünü atlatan LLM cache Compose uygulaması
benchmark/AevumBenchmark.kt     cihaz üstü ART-Aevum sürücüsü
test_lab/lmkd_simulator.cpp     çok-süreçli SIGKILL / O(1) reattach simülatörü
test_lab/trace_emitter.{h,cpp}  kilitlenmesiz artımlı NDJSON iz üreticisi
test_lab/run_headless_lab.sh    tek tuşlu otonom kanıt konsolu
scripts/field_proof_harness.sh  ADB tabanlı cihaz üstü denetim harness'i
scripts/parse_perfetto.py       Perfetto iz analizörü (Aevum vs ART GC)
```

---

## 7. Bu motor ne işe yarar

Bir yapılacaklar listesi uygulaması için fazlasıyla abartılı. Hedefi, mobil
yazılımın platform varsayılanlarının iyi idare edemediği uç durumları:

- Cihaz üstü çıkarım (LLM / ML). Modelin K-V cache'ini ve düşünce zinciri
  durumunu arenada tutun. Uygulama arka planda öldürülebilir; kullanıcı geri
  döndüğünde model, cache'i baştan yüklemek yerine host benchmark'ında ~30 µs
  ölçülen bir reattach (§5.2) ile tam kaldığı token'dan devam eder.
- Oyun motorları ve ağır grafik hatları. Sahne grafiğinin tamamını arenada tutun.
  ART bu nesneleri hiç görmediği için 120 Hz'de GC kaynaklı kare atlamasına yol
  açamazlar.
- Çökme ve durum dökümleri. Uygulama durumunu kilitsiz ve kopyasız şekilde arenaya
  yazın, sonraki açılışta veritabanı sorgusu ve deserialization olmadan geri
  yükleyin.
