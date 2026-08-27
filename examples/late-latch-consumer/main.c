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
    struct hfior_cursor cursor;
    assert(hfior_cursor_init(&cursor, ring, -1, 1u, false) == 0);

    const struct hfior_record normal = {
        .sequence = 1u, .timestamp_ns = 100u, .dx = 2,
        .flags = HFIOR_RECORD_MOTION,
    };
    assert(hfior_ring_publish(ring, &normal));
    struct hfior_record records[8];
    assert(hfior_cursor_drain(&cursor, records, 8u) == 1);
    int camera_x = records[0].dx;

    /* Normal camera work occurs here. A new record commits meanwhile. */
    const struct hfior_record suffix = {
        .sequence = 2u, .timestamp_ns = 200u, .dx = -1,
        .flags = HFIOR_RECORD_MOTION,
    };
    assert(hfior_ring_publish(ring, &suffix));

    const ssize_t late = hfior_cursor_stable_latch(&cursor, records, 8u, 2u);
    assert(late == 1);
    camera_x += records[0].dx;
    printf("camera_x=%d late_records=%zd\n", camera_x, late);
    /* A real authorized consumer sends hfior_cursor_ack() after use/frame. */
    free(memory);
    return 0;
}
