// Testy SpscRing (core/spsc_ring.hpp): jednovláknová korektnost a
// dvouvláknový stres test producer/consumer.
#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "core/spsc_ring.hpp"
#include "doctest/doctest.h"

using namespace am;

TEST_CASE("SpscRing push/pop základní round-trip") {
    SpscRing<int> ring(8);
    CHECK(ring.capacity() == 8);
    CHECK(ring.sizeApprox() == 0);

    const std::vector<int> in{1, 2, 3, 4};
    CHECK(ring.push(in) == 4);
    CHECK(ring.sizeApprox() == 4);

    std::vector<int> out(4, 0);
    CHECK(ring.pop(out) == 4);
    CHECK(out == in);
    CHECK(ring.sizeApprox() == 0);
}

TEST_CASE("SpscRing push vrátí částečný počet, když je plno") {
    SpscRing<int> ring(4);
    const std::vector<int> in{1, 2, 3, 4, 5, 6};
    // kapacita 4 -> zapíše se jen 4 prvky
    CHECK(ring.push(in) == 4);
    CHECK(ring.sizeApprox() == 4);

    // buffer je plný, další push nezapíše nic
    const std::vector<int> more{7, 8};
    CHECK(ring.push(more) == 0);
}

TEST_CASE("SpscRing pop vrátí částečný počet, když je málo dat") {
    SpscRing<int> ring(8);
    const std::vector<int> in{42};
    CHECK(ring.push(in) == 1);

    std::vector<int> out(4, -1);
    CHECK(ring.pop(out) == 1);
    CHECK(out[0] == 42);
}

TEST_CASE("SpscRing korektně obtéká (wrap) přes hranici kapacity") {
    SpscRing<int> ring(4);

    // Naplníme a vyprázdníme buffer vícekrát, aby čítače přešly přes
    // hranici kapacity a index se musel zalomit (index = counter & mask).
    for (int round = 0; round < 5; ++round) {
        std::vector<int> in{round * 10 + 1, round * 10 + 2, round * 10 + 3};
        CHECK(ring.push(in) == 3);

        std::vector<int> out(3, 0);
        CHECK(ring.pop(out) == 3);
        CHECK(out == in);
    }
}

TEST_CASE("SpscRing wrap se zapisem/ctenim ve dvou segmentech") {
    SpscRing<int> ring(4);

    // Posuneme head_/tail_ tak, aby další push musel zapisovat na konec
    // i začátek storage (dva memcpy segmenty).
    std::vector<int> tmp{0, 0, 0};
    CHECK(ring.push(tmp) == 3);
    std::vector<int> drain(3, 0);
    CHECK(ring.pop(drain) == 3);
    // teď je head_ == tail_ == 3, další zápis 4 prvků obtéká přes index 0

    std::vector<int> in{100, 101, 102, 103};
    CHECK(ring.push(in) == 4);

    std::vector<int> out(4, 0);
    CHECK(ring.pop(out) == 4);
    CHECK(out == in);
}

TEST_CASE("SpscRing dvouvláknový stres test producer/consumer") {
    constexpr size_t kTotal = 1'000'000;
    SpscRing<int> ring(1024); // menší než kTotal, nutí producer/consumer se prolínat

    std::atomic<bool> mismatch{false};

    std::thread producer([&] {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<size_t> chunk_size_dist(1, 200);

        size_t next = 0;
        std::vector<int> chunk;
        while (next < kTotal) {
            const size_t n = std::min(chunk_size_dist(rng), kTotal - next);
            chunk.resize(n);
            for (size_t i = 0; i < n; ++i) chunk[i] = int(next + i);

            size_t written = 0;
            while (written < n) {
                written += ring.push(std::span<const int>(chunk).subspan(written));
                // ring může být plný, jednoduše to zkusíme znovu (busy-wait,
                // je to jen test, ne produkční kód)
            }
            next += n;
        }
    });

    std::thread consumer([&] {
        std::mt19937 rng(54321);
        std::uniform_int_distribution<size_t> chunk_size_dist(1, 150);

        size_t expected = 0;
        std::vector<int> buf;
        while (expected < kTotal) {
            const size_t n = chunk_size_dist(rng);
            buf.resize(n);
            const size_t got = ring.pop(buf);
            for (size_t i = 0; i < got; ++i) {
                if (buf[i] != int(expected + i)) {
                    mismatch.store(true, std::memory_order_relaxed);
                }
            }
            expected += got;
        }
    });

    producer.join();
    consumer.join();

    CHECK_FALSE(mismatch.load());
}
