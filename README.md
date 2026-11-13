# lockfree-spsc-queue

A header-only, dependency-free, **lock-free single-producer / single-consumer (SPSC) ring buffer** for modern C++17.

**Languages:** [English](#english) · [Türkçe](#türkçe)

---

## English

### Features

- **Header-only, zero dependencies** – one header, standard library only.
- **Lock-free and wait-free** for both sides: no mutexes, no syscalls, no allocation after construction.
- **False-sharing free** – producer state, consumer state and read-only configuration each live on their own 64-byte cache line.
- **Cached indices** – each side re-reads the other side's atomic index only when the queue *looks* full or empty.
- **Correct object lifetimes** – elements are constructed in place and destroyed exactly once; leftovers are destroyed by the queue's destructor.
- **Move-only payloads** (`std::unique_ptr`, …), `try_emplace`, and zero-copy `front()` / `pop()`.
- **Strong exception guarantee** – if a constructor or move assignment throws, the queue is unchanged.
- Clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` with `-Werror`.

### Installation

Copy [`include/lockfree_spsc/spsc_queue.hpp`](include/lockfree_spsc/spsc_queue.hpp) into your project, **or** use CMake:

```cmake
# Option 1: add as a subdirectory / FetchContent
add_subdirectory(lockfree-spsc-queue)
target_link_libraries(my_app PRIVATE lockfree_spsc::spsc_queue)

# Option 2: after `cmake --install`
find_package(lockfree_spsc_queue 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE lockfree_spsc::spsc_queue)
```

### Quick start

```cpp
#include <lockfree_spsc/spsc_queue.hpp>
#include <thread>

int main() {
    lockfree_spsc::spsc_queue<int> queue(1024);  // holds up to 1024 ints

    std::thread producer([&queue] {
        for (int i = 0; i < 100; ++i) {
            while (!queue.try_push(i)) {
                // Queue is full: spin, yield or do other work.
            }
        }
    });

    long long sum = 0;
    for (int received = 0; received < 100;) {
        int value = 0;
        if (queue.try_pop(value)) {
            sum += value;
            ++received;
        }
    }
    producer.join();  // sum == 4950
}
```

Move-only payloads and zero-copy consumption:

```cpp
lockfree_spsc::spsc_queue<std::unique_ptr<std::string>> messages(16);
bool pushed = messages.try_emplace(std::make_unique<std::string>("hello"));
std::optional<std::unique_ptr<std::string>> message = messages.try_pop();

lockfree_spsc::spsc_queue<std::string> lines(4);
pushed = lines.try_emplace(std::size_t{5}, 'x');
if (const std::string* line = lines.front()) {  // inspect in place
    // ... use *line ...
    lines.pop();                                  // then destroy it
}
```

The complete, compiling version lives in [`examples/quick_start.cpp`](examples/quick_start.cpp) and runs as part of the test suite.

### API

| Member | Thread | Description |
|---|---|---|
| `explicit spsc_queue(size_type capacity)` | – | Allocates storage for `capacity` elements. Throws `std::invalid_argument` for 0, `std::length_error` if too large. |
| `bool try_push(const T&)` / `bool try_push(T&&)` | producer | Copies / moves a value in. Returns `false` if full. |
| `bool try_emplace(Args&&...)` | producer | Constructs a value in place. Returns `false` if full. |
| `bool try_pop(T& out)` | consumer | Move-assigns the oldest element into `out`. Returns `false` if empty. |
| `std::optional<T> try_pop()` | consumer | Returns the oldest element, or `std::nullopt` if empty. |
| `T* front()` | consumer | Pointer to the oldest element, or `nullptr` if empty. |
| `void pop()` | consumer | Destroys the oldest element. Precondition: `front() != nullptr`. |
| `size_type capacity() const` | any | Maximum number of elements. |
| `size_type size() const` / `bool empty() const` | any | Snapshot; exact only when the other side is idle. |

Every status-returning operation is `[[nodiscard]]`. The queue is neither copyable nor movable.

### Rules and limitations

- **Exactly one producer thread and one consumer thread.** Calling producer operations from two threads at once (or consumer operations from two threads at once) is a data race. Use an MPMC queue if you need more.
- Construction and destruction must not overlap with any other operation.
- **Capacity is fixed** at construction. One extra slot is allocated internally to distinguish *full* from *empty*.
- Operations never block. If you need to wait, spin, yield or sleep yourself.
- `size()` and `empty()` are snapshots and may be stale as soon as they return when the other thread is active.
- `T` must be a non-const object type with a `noexcept` destructor.

### Memory ordering (user-level summary)

The producer constructs an element and then publishes it with a **release** store of `head`. The consumer reads `head` with an **acquire** load, so it always sees the fully constructed element. After consuming and destroying it, the consumer publishes `tail` with a **release** store, and the producer reads `tail` with **acquire** before reusing that slot. Each thread reads its *own* index with `relaxed` ordering, because nobody else writes it. No sequentially consistent operations or fences are needed.

### Cache-line layout

```
spsc_queue<T> object (alignas(64), 192 bytes)
offset   0 ┌──────────────────────────────────────────────────────┐
           │ producer_  : atomic head        | size_t cached_tail │  written by producer only
offset  64 ├──────────────────────────────────────────────────────┤
           │ consumer_  : atomic tail        | size_t cached_head │  written by consumer only
offset 128 ├──────────────────────────────────────────────────────┤
           │ config_    : size_t slot_count  | T* slots           │  read-only after construction
offset 192 └──────────────────────────────────────────────────────┘

Slot storage (separate heap allocation)
┌───────────┬────────┬────────┬─────┬────────┬───────────┐
│  padding  │ slot 0 │ slot 1 │ ... │ slot N │  padding  │   N = capacity
│ ≥ 64 bytes│        │        │     │        │ ≥ 64 bytes│
└───────────┴────────┴────────┴─────┴────────┴───────────┘
```

The padding keeps the first and last slots from sharing a cache line with unrelated heap data. `static_assert`s in the header verify that each state block is exactly one cache line.

### Building, testing and benchmarking

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/bench/bench_queue            # optional: [throughput_items] [latency_round_trips]
```

| CMake option | Default | Meaning |
|---|---|---|
| `SPSC_BUILD_TESTS` | `ON` when top-level | Unit, stress and example tests |
| `SPSC_BUILD_BENCHMARKS` | `OFF` | `bench_queue` executable |
| `SPSC_WARNINGS_AS_ERRORS` | `ON` when top-level | Adds `-Werror` / `/WX` to in-tree targets |

### Benchmark results

**Methodology.** One producer and one consumer thread, both queues bounded to 1024 × `uint64_t`, both sides busy-polling with `try_push` / `try_pop`. The baseline is a `std::queue` protected by a `std::mutex` with the same capacity limit.
- *Throughput:* 10,000,000 integers streamed producer → consumer.
- *Latency:* 1,000,000 ping-pong round trips over two queues; each sample is the mean of 100 consecutive round trips (above the timer resolution), from which p50/p99 are taken.
- Every figure is the median of 5 runs. Threads are not pinned.

**Environment:** Intel Core Ultra 7 255U (14 hardware threads), Windows 11 Pro, GCC 16.2.0 (MinGW-w64 UCRT), `Release` (`-O3`).

| Queue | Throughput (M items/s) | RTT mean (ns) | RTT p50 (ns) | RTT p99 (ns) |
|---|---:|---:|---:|---:|
| `lockfree_spsc::spsc_queue` | **119.4** | **493.2** | **519.0** | **1038.0** |
| `std::mutex` + `std::queue` | 11.4 | 2454.2 | 1797.0 | 8756.0 |

≈ **10.5× higher throughput** and ≈ **5× lower mean round-trip latency** on this machine. These are measurements for the environment above, not guarantees: results depend heavily on CPU topology (e.g. whether the two threads share a core or a cache), power management and OS scheduling. Run `bench_queue` on your own hardware.

### Contributing

Contributions are welcome! A good starting point is [issue #5](https://github.com/erencicekk25/lockfree-spsc-queue/issues/5): bulk operations (`try_push_bulk` / `try_pop_bulk`) or custom allocator support. Please keep the library header-only, dependency-free and C++17-compatible, add tests, and update both language sections of this README.

### License

[MIT](LICENSE)

---

## Türkçe

### Özellikler

- **Yalnızca başlık dosyası, sıfır bağımlılık** – tek bir header, sadece standart kütüphane.
- **Kilitlenmesiz (lock-free) ve bekleme-serbest (wait-free)**: mutex yok, sistem çağrısı yok, kurulumdan sonra bellek ayırma yok.
- **Sahte paylaşım (false sharing) yok** – üretici durumu, tüketici durumu ve salt-okunur yapılandırma ayrı 64 baytlık cache line'larda tutulur.
- **Önbelleklenmiş indeksler** – her taraf karşı tarafın atomik indeksini yalnızca kuyruk dolu ya da boş *göründüğünde* yeniden okur.
- **Doğru nesne ömrü yönetimi** – elemanlar yerinde oluşturulur ve tam olarak bir kez yok edilir; kuyrukta kalanlar yıkıcı tarafından temizlenir.
- **Yalnızca taşınabilir (move-only) tipler** (`std::unique_ptr` vb.), `try_emplace` ve kopyasız `front()` / `pop()`.
- **Güçlü istisna garantisi** – kurucu veya taşıma ataması istisna fırlatırsa kuyruk değişmez.
- `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast` ve `-Werror` ile uyarısız derlenir.

### Kurulum

[`include/lockfree_spsc/spsc_queue.hpp`](include/lockfree_spsc/spsc_queue.hpp) dosyasını projenize kopyalayın **ya da** CMake kullanın:

```cmake
# Seçenek 1: alt dizin / FetchContent olarak ekleyin
add_subdirectory(lockfree-spsc-queue)
target_link_libraries(my_app PRIVATE lockfree_spsc::spsc_queue)

# Seçenek 2: `cmake --install` sonrasında
find_package(lockfree_spsc_queue 1.0 REQUIRED)
target_link_libraries(my_app PRIVATE lockfree_spsc::spsc_queue)
```

### Hızlı başlangıç

```cpp
#include <lockfree_spsc/spsc_queue.hpp>
#include <thread>

int main() {
    lockfree_spsc::spsc_queue<int> queue(1024);  // en fazla 1024 int tutar

    std::thread producer([&queue] {
        for (int i = 0; i < 100; ++i) {
            while (!queue.try_push(i)) {
                // Kuyruk dolu: bekleyin, yield edin veya başka iş yapın.
            }
        }
    });

    long long sum = 0;
    for (int received = 0; received < 100;) {
        int value = 0;
        if (queue.try_pop(value)) {
            sum += value;
            ++received;
        }
    }
    producer.join();  // sum == 4950
}
```

Taşınabilir tipler ve kopyasız tüketim:

```cpp
lockfree_spsc::spsc_queue<std::unique_ptr<std::string>> messages(16);
bool pushed = messages.try_emplace(std::make_unique<std::string>("hello"));
std::optional<std::unique_ptr<std::string>> message = messages.try_pop();

lockfree_spsc::spsc_queue<std::string> lines(4);
pushed = lines.try_emplace(std::size_t{5}, 'x');
if (const std::string* line = lines.front()) {  // yerinde incele
    // ... *line kullan ...
    lines.pop();                                  // sonra yok et
}
```

Derlenen tam sürüm [`examples/quick_start.cpp`](examples/quick_start.cpp) dosyasındadır ve test paketinin bir parçası olarak çalıştırılır.

### API

| Üye | İş parçacığı | Açıklama |
|---|---|---|
| `explicit spsc_queue(size_type capacity)` | – | `capacity` eleman için bellek ayırır. 0 için `std::invalid_argument`, çok büyük değerler için `std::length_error` fırlatır. |
| `bool try_push(const T&)` / `bool try_push(T&&)` | üretici | Değeri kopyalar / taşır. Kuyruk doluysa `false` döner. |
| `bool try_emplace(Args&&...)` | üretici | Değeri yerinde oluşturur. Kuyruk doluysa `false` döner. |
| `bool try_pop(T& out)` | tüketici | En eski elemanı `out` içine taşır. Kuyruk boşsa `false` döner. |
| `std::optional<T> try_pop()` | tüketici | En eski elemanı döndürür; boşsa `std::nullopt`. |
| `T* front()` | tüketici | En eski elemana işaretçi; boşsa `nullptr`. |
| `void pop()` | tüketici | En eski elemanı yok eder. Ön koşul: `front() != nullptr`. |
| `size_type capacity() const` | herhangi | Azami eleman sayısı. |
| `size_type size() const` / `bool empty() const` | herhangi | Anlık görüntü; yalnızca karşı taraf boştayken kesindir. |

Durum döndüren tüm işlemler `[[nodiscard]]` olarak işaretlidir. Kuyruk kopyalanamaz ve taşınamaz.

### Kurallar ve sınırlamalar

- **Tam olarak bir üretici ve bir tüketici iş parçacığı.** Üretici işlemlerini (veya tüketici işlemlerini) aynı anda iki iş parçacığından çağırmak veri yarışıdır (data race). Daha fazlası gerekiyorsa MPMC kuyruğu kullanın.
- Kurulum ve yıkım başka hiçbir işlemle çakışmamalıdır.
- **Kapasite sabittir** ve kurulumda belirlenir. *Dolu* ile *boş* durumunu ayırt etmek için dahili olarak bir ek slot ayrılır.
- İşlemler asla bloklamaz. Beklemeniz gerekiyorsa spin, yield veya sleep'i kendiniz yapın.
- `size()` ve `empty()` anlık görüntüdür; karşı iş parçacığı aktifken döndükleri anda eskimiş olabilirler.
- `T`, `noexcept` yıkıcıya sahip, const olmayan bir nesne tipi olmalıdır.

### Bellek sıralaması (kullanıcı düzeyinde özet)

Üretici elemanı oluşturur ve ardından `head` indeksini **release** ile yayımlar. Tüketici `head`'i **acquire** ile okur; böylece elemanı her zaman tamamen oluşturulmuş hâliyle görür. Tüketici elemanı alıp yok ettikten sonra `tail`'i **release** ile yayımlar; üretici de o slotu yeniden kullanmadan önce `tail`'i **acquire** ile okur. Her iş parçacığı *kendi* indeksini `relaxed` okur, çünkü onu başka kimse yazmaz. Sıralı tutarlı (seq_cst) işlemlere veya fence'lere gerek yoktur.

### Cache line yerleşimi

```
spsc_queue<T> nesnesi (alignas(64), 192 bayt)
ofset   0 ┌──────────────────────────────────────────────────────┐
          │ producer_  : atomic head        | size_t cached_tail │  yalnızca üretici yazar
ofset  64 ├──────────────────────────────────────────────────────┤
          │ consumer_  : atomic tail        | size_t cached_head │  yalnızca tüketici yazar
ofset 128 ├──────────────────────────────────────────────────────┤
          │ config_    : size_t slot_count  | T* slots           │  kurulumdan sonra salt-okunur
ofset 192 └──────────────────────────────────────────────────────┘

Slot belleği (ayrı heap ayırması)
┌───────────┬────────┬────────┬─────┬────────┬───────────┐
│   dolgu   │ slot 0 │ slot 1 │ ... │ slot N │   dolgu   │   N = kapasite
│ ≥ 64 bayt │        │        │     │        │ ≥ 64 bayt │
└───────────┴────────┴────────┴─────┴────────┴───────────┘
```

Dolgu (padding), ilk ve son slotların ilgisiz heap verisiyle aynı cache line'ı paylaşmasını önler. Header içindeki `static_assert`'ler her durum bloğunun tam olarak bir cache line olduğunu doğrular.

### Derleme, test ve benchmark

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSPSC_BUILD_BENCHMARKS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/bench/bench_queue            # isteğe bağlı: [throughput_items] [latency_round_trips]
```

| CMake seçeneği | Varsayılan | Anlamı |
|---|---|---|
| `SPSC_BUILD_TESTS` | Üst düzey projede `ON` | Birim, stres ve örnek testleri |
| `SPSC_BUILD_BENCHMARKS` | `OFF` | `bench_queue` çalıştırılabilir dosyası |
| `SPSC_WARNINGS_AS_ERRORS` | Üst düzey projede `ON` | Proje hedeflerine `-Werror` / `/WX` ekler |

### Benchmark sonuçları

**Yöntem.** Bir üretici ve bir tüketici iş parçacığı; her iki kuyruk da 1024 × `uint64_t` ile sınırlı, iki taraf da `try_push` / `try_pop` ile meşgul-bekleme (busy polling) yapıyor. Karşılaştırma tabanı, aynı kapasite sınırına sahip ve `std::mutex` ile korunan bir `std::queue`'dur.
- *Verim (throughput):* 10.000.000 tamsayı üreticiden tüketiciye aktarılır.
- *Gecikme (latency):* İki kuyruk üzerinden 1.000.000 ping-pong gidiş-dönüşü; her örnek ardışık 100 gidiş-dönüşün ortalamasıdır (zamanlayıcı çözünürlüğünün üzerinde kalmak için), p50/p99 bu örneklerden hesaplanır.
- Her değer 5 çalıştırmanın medyanıdır. İş parçacıkları çekirdeğe sabitlenmemiştir.

**Ortam:** Intel Core Ultra 7 255U (14 donanım iş parçacığı), Windows 11 Pro, GCC 16.2.0 (MinGW-w64 UCRT), `Release` (`-O3`).

| Kuyruk | Verim (M eleman/s) | RTT ortalama (ns) | RTT p50 (ns) | RTT p99 (ns) |
|---|---:|---:|---:|---:|
| `lockfree_spsc::spsc_queue` | **119.4** | **493.2** | **519.0** | **1038.0** |
| `std::mutex` + `std::queue` | 11.4 | 2454.2 | 1797.0 | 8756.0 |

Bu makinede ≈ **10,5 kat daha yüksek verim** ve ≈ **5 kat daha düşük ortalama gidiş-dönüş gecikmesi**. Bunlar yukarıdaki ortama ait ölçümlerdir, garanti değildir: sonuçlar büyük ölçüde CPU topolojisine (ör. iki iş parçacığının aynı çekirdeği veya önbelleği paylaşıp paylaşmadığı), güç yönetimine ve işletim sistemi zamanlayıcısına bağlıdır. `bench_queue`'yu kendi donanımınızda çalıştırın.

### Katkıda bulunma

Katkılarınızı bekliyoruz! İyi bir başlangıç noktası [issue #5](https://github.com/erencicekk25/lockfree-spsc-queue/issues/5): toplu işlemler (`try_push_bulk` / `try_pop_bulk`) veya özel allocator desteği. Lütfen kütüphaneyi header-only, bağımlılıksız ve C++17 uyumlu tutun, test ekleyin ve bu README'nin her iki dil bölümünü de güncelleyin.

### Lisans

[MIT](LICENSE)
