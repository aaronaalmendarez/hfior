#ifndef HFIOR_RING_H
#define HFIOR_RING_H

#include <hfior/abi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

bool hfior_is_power_of_two_u32(uint32_t value);
size_t hfior_mapping_size(uint32_t capacity);
int hfior_initialize_mapping(struct hfior_ring_header *header,
                             size_t mapping_size, uint32_t capacity,
                             uint64_t features, const char *device_name,
                             uint16_t bustype, uint16_t vendor,
                             uint16_t product, uint16_t version,
                             uint64_t generation);

/* Producer operation. The record becomes visible with release publication. */
bool hfior_ring_publish(struct hfior_ring_header *header,
                        const struct hfior_record *record);

/* Writable single-process/reference drain; advances the shared tail. */
size_t hfior_ring_drain(struct hfior_ring_header *header,
                        struct hfior_record *out, size_t max_records);

/* Read-only client drain. The caller owns local_tail and ACKs separately. */
ssize_t hfior_ring_read_snapshot(const struct hfior_ring_header *header,
                                 uint64_t *local_tail,
                                 struct hfior_record *out,
                                 size_t max_records);

#ifdef __cplusplus
}
#endif
#endif
