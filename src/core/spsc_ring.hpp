#pragma once
// Bezzámkový single-producer/single-consumer kruhový buffer.
//
// Použití: audio vlákno (producer nebo consumer podle směru TX/RX) nesmí
// nikdy blokovat na mutexu — jeden atomický pár (head_, tail_) s
// acquire/release synchronizací stačí, protože existuje přesně jeden
// zapisující a jeden čtoucí thread. Čítače head_/tail_ monotónně rostou
// (nepřetáčí se na kapacitu) a index do storage_ se počítá modulo
// kapacita bitovou maskou — proto je kapacita vynucena jako mocnina 2.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace am {

template <typename T>
class SpscRing {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing používá memcpy, T musí být trivially copyable");

    explicit SpscRing(size_t pow2_capacity)
        : capacity_(pow2_capacity), mask_(pow2_capacity - 1), storage_(pow2_capacity) {
        // Kontrola musí platit i v Release buildu (NDEBUG vypíná assert) —
        // špatná kapacita by jinak potichu rozbila indexování bitovou maskou.
        if (pow2_capacity == 0 || (pow2_capacity & (pow2_capacity - 1)) != 0) {
            throw std::invalid_argument("SpscRing: kapacita musí být mocnina dvou");
        }
    }

    // Zapíše co nejvíce z `in` do bufferu, nikdy neblokuje. Vrací počet
    // skutečně zapsaných prvků (méně, než in.size(), pokud je málo místa).
    size_t push(std::span<const T> in) {
        const size_t tail = tail_.load(std::memory_order_acquire);
        const size_t head = head_.load(std::memory_order_relaxed); // vlastní hodnota, jen producer píše
        const size_t free_space = capacity_ - (head - tail);
        const size_t n = std::min(in.size(), free_space);
        if (n == 0) return 0;

        const size_t start = head & mask_;
        const size_t first = std::min(n, capacity_ - start);
        std::memcpy(storage_.data() + start, in.data(), first * sizeof(T));
        if (n > first) {
            std::memcpy(storage_.data(), in.data() + first, (n - first) * sizeof(T));
        }

        head_.store(head + n, std::memory_order_release);
        return n;
    }

    // Přečte a odstraní nejvýše out.size() prvků. Vrací počet skutečně
    // přečtených prvků (méně, pokud je v bufferu méně dat).
    size_t pop(std::span<T> out) {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_relaxed); // vlastní hodnota, jen consumer píše
        const size_t available = head - tail;
        const size_t n = std::min(out.size(), available);
        if (n == 0) return 0;

        const size_t start = tail & mask_;
        const size_t first = std::min(n, capacity_ - start);
        std::memcpy(out.data(), storage_.data() + start, first * sizeof(T));
        if (n > first) {
            std::memcpy(out.data() + first, storage_.data(), (n - first) * sizeof(T));
        }

        tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    // Přibližný počet prvků čekajících ke čtení — přesný jen z pohledu
    // vlákna, které ho volá (druhá strana může mezitím dat přibýt/ubýt).
    size_t sizeApprox() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    size_t capacity() const { return capacity_; }

private:
    size_t capacity_;
    size_t mask_;
    std::vector<T> storage_;

    // Monotónně rostoucí čítače (nepřetáčí se na capacity_, jen na size_t).
    std::atomic<size_t> head_{0}; // píše producer (push)
    std::atomic<size_t> tail_{0}; // píše consumer (pop)
};

} // namespace am
