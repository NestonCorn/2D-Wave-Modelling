# 2D Wave Modelling — Seismic FDTD Solver

![Seismic FDTD Solver](cpp/assets/thumbnail.png)

Gerçek zamanlı 2B akustik dalga modellemesi ile deniz sismiği veri toplama simülatörü. Kademeli ızgara (staggered grid) üzerinde sonlu farklar zaman alanı (FDTD) çözücüsü, OpenGL 4.3 compute shader'larıyla GPU'da ya da AVX2 vektörleştirilmiş çok iş parçacıklı CPU motorunda çalışır. Arayüz Dear ImGui ile yazılmıştır ve Türkçedir.

## Özellikler

**Fizik ve sayısal yöntem**
- Basınç–parçacık hızı (P, Vx, Vz) kademeli ızgara akustik FDTD, 2. – 10. derece uzaysal türevler
- CPML soğurucu sınırlar (Komatitsch & Martin, 2007)
- Deniz yüzeyi yansıması (Dirichlet serbest yüzey) açılıp kapatılabilir; kapalıyken üstte de CPML
- Eikonal tabanlı "expanding box": yalnızca dalganın ulaştığı bölge hesaplanır
- CFL kararlılık ve ızgara dağılımı (dalga boyu başına nokta) göstergeleri
- Jeoloji modelleri: düz tabaka, fay, tuz domu

**Kaynak dalgacıkları**
- Ricker (sıfır faz), minimum faz, Gauss, Gauss'un 1. türevi
- Kaynak, 2B'de yayılan darbenin seçilen dalgacıkla aynı şekilde olacağı biçimde (yarı-integral düzeltmesiyle) enjekte edilir

**Veri toplama**
- Geleneksel, SLO ve CLO atış modları; hareketli gemi, streamer ve dip sismometreleri (OBS)
- Streamer ve OBS sismogram panelleri: wiggle ya da gri tonlu variable density, kübik interpolasyon, sürükleyerek kaydırma ve yakınlaştırma
- Alıcılar ızgaraya yuvarlanmaz: yatayda 12 noktalı Kaiser pencereli sinc, düşeyde doğrusal interpolasyonla okunur
- Her iz 4096 örneğe kadar saklanır (4000 ms kayıtta 1 ms örnek aralığı)
- SEG-Y Rev1 dışa aktarımı, parametrelerin JSON olarak kaydedilip yüklenmesi

## Performans

AMD Ryzen 5 3600 ve Radeon RX 5700 XT üzerinde, 10. derece türevle, `fdtd_tests --bench` ile ölçülmüştür (adım/s; GPU sonuçları ölçümden ölçüme %10–20 oynayabilir):

| Izgara | Hücre | CPU (zaman bloklamalı, AVX2) | GPU (OpenGL compute) |
|---|---|---|---|
| 1200 × 480 m, 4 m | 301 × 121 | ~22 000 | ~67 000 |
| 6000 × 2400 m, 2 m | 3001 × 1201 | ~380 | ~2 400 |
| 25000 × 12000 m, 4 m | 6251 × 3001 | ~47 | ~400–480 |

- **GPU:** alan tamponları `glBufferStorage` ile GPU belleğinde tutulur, CPML yardımcı alanları yalnızca soğurucu katmanda okunur/yazılır, serbest yüzey basınç çekirdeğinde uygulanır.
- **CPU:** AVX2/FMA ile 8 hücre birlikte işlenir; her satır sol CPML / iç bölge / sağ CPML olarak bölütlenir; iş parçacıkları dalga cephesi boru hattıyla (zaman bloklaması) adım başına bariyer olmadan çalışır ve sonuç bariyerli şemayla bit bit aynıdır.

## Derleme

Gereksinimler: Windows, Visual Studio 2022 (MSVC, C++20), CMake 3.24 veya üzeri, internet bağlantısı (GLFW, Dear ImGui, nlohmann/json ve portable-file-dialogs ilk yapılandırmada indirilir). GPU motoru için OpenGL 4.3 destekli bir ekran kartı gerekir; yoksa program CPU motoruyla çalışır.

```bash
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64
cmake --build cpp/build --config Release
```

Program: `cpp/build/Release/SeismicFDTD.exe`

## Testler

```bash
cpp/build/Release/fdtd_tests.exe
```

Başsız testler şunları denetler: CPU–GPU eşliği (alan ve sismogram), kararsızlık algılama, eikonal kutusu, dalgacık şekilleri ve yayılan darbe, zaman bloklamalı CPU şemasının bariyerli şemayla birebir aynılığı, eikonal kutusu ve serbest yüzey açık/kapalıyken kaynak dalgacığı, alıcı interpolasyonu (izden ize varış zamanı ve genlik düzgünlüğü), SEG-Y düzeni ve JSON gidiş-dönüşü. `--bench` ile hız ölçümü de çalışır.

Ortam değişkenleri: `FDTD_PIPELINE=0` CPU zaman bloklamasını kapatır, `FDTD_TILE=64x4` GPU iş grubu boyutunu değiştirir.

## Proje yapısı

```
cpp/
  src/core/   FDTD çekirdeği: CPU ve GPU çözücüleri, CPML, eikonal, jeoloji, dalgacıklar, SEG-Y, parametre G/Ç
  src/ui/     Dear ImGui arayüzü: sahne görünümü, sismogram panelleri, tema
  tests/      başsız testler ve hız ölçümü
  third_party/glad/   OpenGL yükleyicisi
  assets/     simge ve küçük resim
```
