# Aevum: Güvenlik ve Tehdit Modeli

*[English version](SECURITY.md)*

Aevum, bir dosya ya da `memfd` bölgesini uygulamanın adres alanına map'leyen ve
orada canlı bir nesne grafiği tutan native bir bellek motoru. Düşük seviyeli
primitiflerle çalıştığı için (`mmap`, `memfd_create`, `mprotect`, sinyal
handler'ları, JNI, doğrudan bellek) bu belge, uygulamanın güvenlik duruşuna neyi
yapıp neyi yapmadığını net olarak anlatıyor. Aevum'u hassas ürünlere (finans,
cihaz üstü yapay zekâ, telemetri) entegre eden ve inceleyen kişiler için yazıldı.

Kısaca: Aevum tamamen uygulamanın mevcut sandbox'ı içinde, ayrıcalıksız ve aynı
uid'e bağlı primitiflerle çalışır. Süreçler arası yeni bir saldırı yüzeyi açmaz,
izin istemez ve veriyi asla kod olarak çalıştırmaz.

---

## 1. Güven sınırı ve onu neyin geçtiği

| Sınır | Aevum'un tutumu |
|---|---|
| Uygulama sandbox'ı (SELinux `untrusted_app`, uygulama uid'i) | Aevum bu sınırın içinde kalır. Yaptığı her syscall, ayrıcalıksız bir uygulamanın zaten yapabildiği bir çağrıdır (bkz. [ADR-001](docs/architecture/ADR-001-to-003.tr.md)). `su` yok, ayrıcalıklı yardımcı yok, platform imzası yok. |
| Süreç ↔ süreç | Varsayılan destek, `context.cacheDir`/`filesDir` altında özel bir dosyadır (uygulamaya ait, `0600` moduyla oluşturulur). Hiçbir bölge başka bir uid ile paylaşılmaz. İsteğe bağlı `memfd` yolu yalnızca uygulamanın kendisi fd'yi `SCM_RIGHTS` ile devrederse paylaşılır; bu varsayılan değil, bilinçli bir adımdır. |
| Kernel | Yalnızca izin listesindeki syscall'lar. `userfaultfd`, `io_uring`, `bpf` ve `perf_event_open` kullanılmaz (uygulamalar için zaten engelli). |
| Ağ | Yok. Aevum, map'lenen dosya ve `msync` dışında I/O yapmaz. |
| İzinler | Aevum hiçbir Android izni tanımlamaz ve gerektirmez. |

## 2. Sandbox izolasyonu zayıflamaz

* Yalnızca aynı uid. `mmap`/`memfd` bölgeleri uygulamanın uid'ine aittir. Aevum'un
  map'lediği hiçbir şeye başka bir uygulama erişemez; `process_vm_readv` ve benzeri
  çağrılar Aevum'dan bağımsız olarak kernel tarafından aynı uid ile sınırlı tutulur.
* memfd boyutları mühürlüdür. Oluşturulan bölgelere `F_SEAL_SHRINK | F_SEAL_GROW`
  uygulanır; fd'yi tutan bir süreç bölgeyi canlı bir eşlemenin altından
  küçültemez (truncation/`SIGBUS` riski) ya da beklenmedik şekilde büyütemez.
* Yeni IPC uç noktası yok. Aevum soket, Binder servisi ya da content provider
  oluşturmaz. Dışarı açabileceği tek fd, uygulamanın devretmeyi seçtiği fd'dir.
* Eşlemeler çalıştırılabilir değildir. Bölgeler `PROT_READ|PROT_WRITE` ile
  map'lenir, asla `PROT_EXEC` ile değil. Dirty tracker yalnızca `PROT_READ` ile
  `PROT_READ|PROT_WRITE` arasında geçiş yapar. Aevum veriyi hiçbir zaman kod
  olarak map'lemez ve W^X ihlali getirmez.

## 3. Bellek güvenliğiyle ilgili açıklamalar (native kod)

Aevum C++ ile yazıldı. Riskler ve önlemler şöyle:

* Sınırlar ve offset'ler, ham arena. Tahsisleyici sabit boyut sınıflarından 16
  bayt hizalı bloklar döndürür. Bir arena bloğunun Kotlin view'ının boyutu
  `capacityOf` ile belirlenir. `capacityOf` önce handle'ı `data_start`'a ve bloğun
  alloc magic değerine karşı kontrol eder; iki kontrolden biri başarısız olursa
  view döndürülmez.
* Sınırlar ve offset'ler, yönetilen heap. C ABI ve JNI üzerinden gelen değerler,
  belleğe dokunulmadan önce kontrol edilir:
  * `registerLayout`; 0 olan ya da `0x7FFFFFF0`'ı aşan boyutları, 32'den fazla
    referans alanını ve 8 bayta hizalı olmayan, beyan edilen boyutun içine tam
    bir 8 baytlık alan sığdıramayan ya da tekrar eden her offset'i reddeder.
    Reddedilen bir çağrı, o tip için önceki yerleşimi korur.
  * Bir `MRef` 16 bayta hizalı olmalı ve iki semispace'in içinde kalmalıdır;
    aksi halde `read`, `write` ve `setRoot` başarısız olur.
  * `read`/`write`'a verilen boyut negatif olamaz ve nesnenin ayrılmış
    payload'ından (kayıtlı boyutun 16'nın katına yuvarlanmış hali) büyük olamaz.
    Böylece view hem nesnenin hem de eşlemenin içinde kalır.
  * `readRef`/`writeRef` yalnızca tipin referans alanı olarak kaydettiği
    offset'leri kabul eder (native kodla trace edilen bir tipte nesnenin içindeki
    hizalı herhangi bir alanı). `writeRef` ayrıca heap dışındaki bir alt `MRef`'i
    reddeder.
  * `attach` sırasında bölge başlığında saklanan semispace yerleşimi eşlemeye
    göre kontrol edilir.

  Hatalı bir çağrı nesneye dokunmadan `AEV_INVALID_ARG` (Kotlin'de
  `AevumException`) ya da `readRef`'ten `MRef.NULL` döndürür. Her kontrol birkaç
  karşılaştırmadan ibarettir. Bu kontroller yanlış bir boyutu ya da offset'i
  yakalar, ama sınırların içinde kalıp yine de bir nesnenin başını göstermeyen
  sahte bir `MRef`'i yakalamaz; bu yüzden yalnızca heap'in size verdiği ref'leri
  kullanın.
* Eşzamanlılık. Sıkıştırma motoru, 8→64 thread'te tam eşzamanlı iş yükü altında
  ThreadSanitizer, AddressSanitizer ve UndefinedBehaviorSanitizer ile temizdir
  (bkz. [WHITEPAPER §6](docs/WHITEPAPER.tr.md)). Bellek sıralaması açıkça
  belirtilir (`acquire`/`release`/`seq_cst`) ve ragged safepoint el sıkışması bir
  Dekker argümanıyla ispatlanmıştır.
* Sinyal handler'ı (dirty tracker, opt-in). Handler async-signal-safe'tir: Aevum
  aralığındaki bir fault için yalnızca atomik güncellemeler ve tek bir `mprotect`
  yapar. `errno`'yu girişte kaydeder ve her çıkış yolunda geri yükler. Aevum'un
  aralıkları dışındaki fault'larda uygulamanın
  önceki `SIGSEGV`/`SIGBUS` handler'ına zincirlenir, yani gerçek bir çökmeyi asla
  yutmaz. Dirty tracker açılmazsa hiçbir handler kurulmaz.
* JNI. Native metotlar `RegisterNatives` ile bağlanır, dolayısıyla çözülmesi
  gereken mangled `Java_*` sembolleri yoktur. Hiçbir global referans tutulmaz.
  Fault handler JVM'e hiç çağrı yapmaz; bu yüzden sızabilecek bir `JNIEnv` ve
  yanlış yapılabilecek bir attach/detach yoktur. `DirectByteBuffer`'lar arena
  belleğini doğrudan gösterir ve yalnızca sahibi olan arena açıkken geçerlidir
  (bkz. `INTEGRATION_GUIDE.tr.md`).

## 4. Diskte duran veri

* Gizlilik. Dosya destekli bir bölge uygulamanın özel depolamasında durur ve
  Android'in Dosya Tabanlı Şifrelemesi'ni (FBE) devralır: diğer uygulama dosyaları
  gibi cihaz/kullanıcı anahtarıyla şifrelenir. Aevum kendi şifrelemesini eklemez ve
  FBE'nin ötesinde bir gizlilik iddiası taşımaz. Cihaz dışına da gönderdiğiniz bir
  arenaya, uygulama seviyesinde şifreleme olmadan sır koymayın.
* Bütünlük. Durable checkpoint'ler CRC32C ile mühürlenir ve çift tamponludur.
  Yırtık bir yazma ya da bit bozulması `attach` sırasında tespit edilir
  (`kCorrupt`), sessizce kabul edilmez. CRC bir bütünlük kontrolüdür, MAC değildir:
  kazara oluşan bozulmayı yakalar, CRC'yi de yeniden yazabilen kasıtlı bir
  saldırganı yakalamaz. Dosyaya yazma erişimi olan yerel bir saldırgana karşı
  kurcalama tespiti istiyorsanız bölgeyi uygulama seviyesinde bir AEAD ile sarın.
* Güvenilmeyen bölgeler. Güvenilmeyen bir kaynaktan gelen bölgeyi düşmanca girdi
  olarak ele alın. Başlıktaki magic, sürüm ve CRC doğrulanır, ama özel olarak
  hazırlanmış bir bölge yine de *uygulamanın* kendi mantığı için zararlı bir nesne
  grafiği içerebilir; yönetilen heap de nesnelerinin içinde saklanan göreli
  offset'leri doğrulamaz (§3 yalnızca çağıranın verdiği değerleri kapsar).
  Yalnızca kendi uygulamanızın oluşturduğu bölgelere bağlanın.

## 5. Hizmet reddi (DoS) sınırları

* Arena ve semispace boyutları oluşturulurken sabitlenir. Yer bittiğinde tahsis
  sınırsız büyümek yerine başarısız olur (null bir handle ya da `Aevum.alloc`'tan
  bir `AevumException`). Mark fazı SATB taşmasında ya da yakınsamadığında temiz
  biçimde iptal edilir, sonsuz döngüye giremez. Mutator kayıt defteri,
  uygulamanın kullandığı thread sayısıyla sınırlıdır. Aevum `mlock` çağırmaz,
  dolayısıyla `RLIMIT_MEMLOCK` tüketmez.

## 6. Bildirim

Şüphelendiğiniz güvenlik açıklarını kamuya duyurmadan önce bakımcılara özel olarak
bildirin. Platformu, ABI'yi, küçük bir yeniden üretme örneğini ve bir sanitizer'ın
hatayı işaretleyip işaretlemediğini ekleyin. Eşzamanlılık sorunlarında lütfen bir
TSan logu ekleyin.
