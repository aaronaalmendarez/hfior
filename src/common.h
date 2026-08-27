#ifndef HFIOR_COMMON_H
#define HFIOR_COMMON_H

#include <hfior/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <hfior/ring.h>

struct hfior_u64_vec {
    uint64_t *data;
    size_t length;
    size_t capacity;
};

uint64_t hfior_now_ns(int clock_id);
int hfior_sleep_until_ns(int clock_id, uint64_t deadline_ns);
void hfior_vec_push(struct hfior_u64_vec *vec, uint64_t value);
void hfior_vec_free(struct hfior_u64_vec *vec);
double hfior_percentile_ns(struct hfior_u64_vec *vec, double percentile);
double hfior_mean_ns(const struct hfior_u64_vec *vec);
double hfior_worst_fraction_mean_ns(struct hfior_u64_vec *vec, double fraction);
uint64_t hfior_max_ns(const struct hfior_u64_vec *vec);

bool hfior_is_power_of_two_u32(uint32_t value);
size_t hfior_mapping_size(uint32_t capacity);
int hfior_initialize_mapping(struct hfior_ring_header *header, size_t mapping_size,
                             uint32_t capacity, uint64_t features,
                             const char *device_name,
                             uint16_t bustype, uint16_t vendor,
                             uint16_t product, uint16_t version,
                             uint64_t generation);

/* Producer: release-publish one record. Returns false if ring is full. */
bool hfior_ring_publish(struct hfior_ring_header *header,
                        const struct hfior_record *record);

/* Consumer: copy up to max_records and release-advance tail. */
size_t hfior_ring_drain(struct hfior_ring_header *header,
                        struct hfior_record *out,
                        size_t max_records);

/* Read-only client drain. local_tail is caller-owned; returns -1 on a
 * corrupted/impossible producer view. Progress becomes reusable only after
 * the client sends a validated HFIOR_MSG_ACK to the producer. */
ssize_t hfior_ring_read_snapshot(const struct hfior_ring_header *header,
                                 uint64_t *local_tail,
                                 struct hfior_record *out,
                                 size_t max_records);

int hfior_send_fds(int socket_fd, const void *message, size_t message_size,
                   const int *fds, size_t fd_count, int flags);
ssize_t hfior_recv_fds(int socket_fd, void *message, size_t message_capacity,
                       int *fds, size_t fd_capacity, size_t *fd_count,
                       int flags);
int hfior_set_nonblocking(int fd, bool enabled);
int hfior_make_runtime_socket_path(char *out, size_t out_size, const char *leaf);
int hfior_verify_same_uid_peer(int socket_fd, uid_t expected_uid, pid_t *peer_pid);
uint16_t hfior_button_bit_from_linux_code(uint16_t code);
const char *hfior_mode_name(uint32_t mode);

#endif
