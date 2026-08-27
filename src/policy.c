#include <hfior/policy.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>

int hfior_cursor_init(struct hfior_cursor *cursor,
                      const struct hfior_ring_header *ring,
                      int control_socket_fd, uint64_t generation,
                      bool begin_at_current_head) {
    if (!cursor || !ring || ring->metadata.magic != HFIOR_MAGIC ||
        ring->metadata.abi_version != HFIOR_ABI_VERSION) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t head = atomic_load_explicit(&ring->producer.head,
                                               memory_order_acquire);
    const uint64_t tail = atomic_load_explicit(&ring->consumer.tail,
                                               memory_order_acquire);
    *cursor = (struct hfior_cursor){
        .ring = ring,
        .control_socket_fd = control_socket_fd,
        .generation = generation,
        .local_tail = begin_at_current_head ? head : tail,
        .acknowledged_tail = begin_at_current_head ? head : tail,
    };
    return 0;
}

ssize_t hfior_cursor_drain(struct hfior_cursor *cursor,
                           struct hfior_record *out, size_t max_records) {
    if (!cursor) {
        errno = EINVAL;
        return -1;
    }
    return hfior_ring_read_snapshot(cursor->ring, &cursor->local_tail,
                                    out, max_records);
}

int hfior_cursor_ack(struct hfior_cursor *cursor) {
    if (!cursor || cursor->control_socket_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (cursor->local_tail == cursor->acknowledged_tail)
        return 0;
    const struct hfior_ack_message message = {
        .header = {HFIOR_MSG_ACK, sizeof(message)},
        .generation = cursor->generation,
        .consumed_tail = cursor->local_tail,
    };
    const ssize_t sent = send(cursor->control_socket_fd, &message,
                              sizeof(message), MSG_NOSIGNAL);
    if (sent != (ssize_t)sizeof(message))
        return -1;
    cursor->acknowledged_tail = cursor->local_tail;
    return 0;
}

ssize_t hfior_cursor_stable_latch(struct hfior_cursor *cursor,
                                  struct hfior_record *out,
                                  size_t max_records,
                                  unsigned max_passes) {
    if (!cursor || (!out && max_records) || !max_passes) {
        errno = EINVAL;
        return -1;
    }
    size_t total = 0u;
    for (unsigned pass = 0; pass < max_passes && total < max_records; ++pass) {
        const ssize_t count = hfior_cursor_drain(cursor, out + total,
                                                 max_records - total);
        if (count < 0)
            return -1;
        total += (size_t)count;
        const uint64_t head = atomic_load_explicit(&cursor->ring->producer.head,
                                                   memory_order_acquire);
        if (head == cursor->local_tail)
            break;
    }
    return (ssize_t)total;
}

bool hfior_cursor_latest_committed(const struct hfior_cursor *cursor,
                                   struct hfior_record *out) {
    if (!cursor || !out)
        return false;
    const uint64_t head = atomic_load_explicit(&cursor->ring->producer.head,
                                               memory_order_acquire);
    if (!head)
        return false;
    const uint64_t tail = atomic_load_explicit(&cursor->ring->consumer.tail,
                                               memory_order_acquire);
    if (head < tail || head - tail > cursor->ring->metadata.capacity)
        return false;
    const struct hfior_record *records = hfior_records_const(cursor->ring);
    const uint64_t index = (head - 1u) &
        (uint64_t)(cursor->ring->metadata.capacity - 1u);
    *out = records[index];
    return out->sequence != 0u;
}
