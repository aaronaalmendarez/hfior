#include "common.h"

#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const uint32_t capacity = 8u;
    const size_t size = hfior_mapping_size(capacity);
    void *memory = aligned_alloc(HFIOR_CACHELINE,
                                 (size + HFIOR_CACHELINE - 1u) & ~(HFIOR_CACHELINE - 1u));
    assert(memory);
    struct hfior_ring_header *ring = memory;
    assert(hfior_initialize_mapping(ring, size, capacity,
                                    HFIOR_FEAT_MONOTONIC_CLOCK,
                                    "unit-test", 0, 0, 0, 0, 1u) == 0);
    assert((uintptr_t)&ring->producer % HFIOR_CACHELINE == 0u);
    assert((uintptr_t)&ring->consumer % HFIOR_CACHELINE == 0u);

    for (uint64_t i = 1; i <= capacity; ++i) {
        struct hfior_record record = {
            .sequence = i,
            .timestamp_ns = i * 100u,
            .dx = (int32_t)i,
            .dy = -(int32_t)i,
            .flags = HFIOR_RECORD_MOTION,
        };
        assert(hfior_ring_publish(ring, &record));
    }
    struct hfior_record overflow = {.sequence = 9u};
    assert(!hfior_ring_publish(ring, &overflow));
    assert(atomic_load(&ring->producer.dropped_newest) == 1u);

    struct hfior_record output[16] = {0};
    assert(hfior_ring_drain(ring, output, 3u) == 3u);
    for (uint64_t i = 0; i < 3u; ++i)
        assert(output[i].sequence == i + 1u);

    for (uint64_t i = 9; i <= 11; ++i) {
        struct hfior_record record = {
            .sequence = i,
            .timestamp_ns = i * 100u,
            .dx = (int32_t)i,
            .flags = HFIOR_RECORD_MOTION,
        };
        assert(hfior_ring_publish(ring, &record));
    }
    assert(hfior_ring_drain(ring, output, 16u) == 8u);
    const uint64_t expected[] = {4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        assert(output[i].sequence == expected[i]);
    assert(hfior_ring_depth(ring) == 0u);

    free(memory);
    puts("ring wrap/overflow/order: PASS");
    return 0;
}
