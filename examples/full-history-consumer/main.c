#include <hfior/hfior.h>

#include <assert.h>
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
    assert(hfior_initialize_mapping(ring, bytes, capacity, 0u, "example",
                                    0, 0, 0, 0, 1u) == 0);
    const struct hfior_record path[] = {
        {.sequence = 1u, .timestamp_ns = 100u, .dx = 3, .flags = HFIOR_RECORD_MOTION},
        {.sequence = 2u, .timestamp_ns = 200u, .dx = -3, .flags = HFIOR_RECORD_MOTION},
    };
    assert(hfior_ring_publish(ring, &path[0]));
    assert(hfior_ring_publish(ring, &path[1]));

    uint64_t cursor = 0u;
    struct hfior_record history[8];
    const ssize_t count = hfior_ring_read_snapshot(ring, &cursor, history, 8u);
    assert(count == 2);
    int integrated_x = 0;
    for (ssize_t i = 0; i < count; ++i) {
        integrated_x += history[i].dx;
        printf("t=%llu dx=%d integrated=%d\n",
               (unsigned long long)history[i].timestamp_ns,
               history[i].dx, integrated_x);
    }
    free(memory);
    return 0;
}
