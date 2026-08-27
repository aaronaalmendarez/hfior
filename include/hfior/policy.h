#ifndef HFIOR_POLICY_H
#define HFIOR_POLICY_H
#include <hfior/ring.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct hfior_cursor {
    const struct hfior_ring_header *ring;
    int control_socket_fd;
    uint64_t generation;
    uint64_t local_tail;
    uint64_t acknowledged_tail;
};

int hfior_cursor_init(struct hfior_cursor *cursor,
                      const struct hfior_ring_header *ring,
                      int control_socket_fd, uint64_t generation,
                      bool begin_at_current_head);
ssize_t hfior_cursor_drain(struct hfior_cursor *cursor,
                           struct hfior_record *out, size_t max_records);
int hfior_cursor_ack(struct hfior_cursor *cursor);
/* Bounded final latch. It performs at most max_passes snapshots and returns all
 * newly committed records in order. It never acknowledges implicitly. */
ssize_t hfior_cursor_stable_latch(struct hfior_cursor *cursor,
                                  struct hfior_record *out,
                                  size_t max_records,
                                  unsigned max_passes);
/* O(1) newest-state query for visual/camera diagnostics. Full-history consumers
 * must still drain every record; this function is not a replacement for it. */
bool hfior_cursor_latest_committed(const struct hfior_cursor *cursor,
                                   struct hfior_record *out);
#endif
