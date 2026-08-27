#include <hfior/hfior.h>

#include <assert.h>
#include <inttypes.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const uint32_t capacity = 8u;
    const size_t bytes = hfior_mapping_size(capacity);
    void *memory = aligned_alloc(HFIOR_CACHELINE,
                                 (bytes + HFIOR_CACHELINE - 1u) &
                                     ~(HFIOR_CACHELINE - 1u));
    assert(memory);
    struct hfior_ring_header *ring = memory;
    assert(hfior_initialize_mapping(ring, bytes, capacity,
                                    HFIOR_FEAT_MONOTONIC_CLOCK,
                                    "example", 0, 0, 0, 0, 1u) == 0);
    const struct hfior_record input = {
        .sequence = 1u, .timestamp_ns = 100u,
        .dx = 3, .dy = -1, .flags = HFIOR_RECORD_MOTION,
    };
    assert(hfior_ring_publish(ring, &input));

    uint64_t cursor = 0u;
    struct hfior_record output;
    assert(hfior_ring_read_snapshot(ring, &cursor, &output, 1u) == 1);
    printf("sequence=%" PRIu64 " timestamp=%" PRIu64 " dx=%d dy=%d\n",
           output.sequence, output.timestamp_ns, output.dx, output.dy);
    free(memory);
    return 0;
}
