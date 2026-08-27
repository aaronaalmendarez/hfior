#ifndef HFIOR_ABI_H
#define HFIOR_ABI_H

#include <stdalign.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HFIOR_ABI_VERSION 2u
#define HFIOR_MAGIC UINT64_C(0x4846494f52563031) /* "HFIORV01" */
#define HFIOR_MAX_DEVICE_NAME 96u
#define HFIOR_MAX_SOCKET_PATH 107u
#define HFIOR_CACHELINE 64u

/* Feature bits negotiated in hfior_ring_header::features. */
enum hfior_feature {
    HFIOR_FEAT_KERNEL_EVENT_TIME = UINT64_C(1) << 0,
    HFIOR_FEAT_MONOTONIC_CLOCK   = UINT64_C(1) << 1,
    HFIOR_FEAT_URGENT_EVENTFD    = UINT64_C(1) << 2,
    HFIOR_FEAT_EAGER_REFERENCE   = UINT64_C(1) << 3,
    HFIOR_FEAT_SYNTHETIC_SOURCE  = UINT64_C(1) << 4,
    HFIOR_FEAT_DEVICE_GENERATION = UINT64_C(1) << 5,
    HFIOR_FEAT_DROP_ACCOUNTING   = UINT64_C(1) << 6,
    HFIOR_FEAT_READ_ONLY_RING    = UINT64_C(1) << 7,
    HFIOR_FEAT_ACK_PROGRESS      = UINT64_C(1) << 8,
    HFIOR_FEAT_START_BARRIER     = UINT64_C(1) << 9,
};

enum hfior_record_flag {
    HFIOR_RECORD_MOTION        = 1u << 0,
    HFIOR_RECORD_BUTTON        = 1u << 1,
    HFIOR_RECORD_WHEEL         = 1u << 2,
    HFIOR_RECORD_SYN_DROPPED   = 1u << 3,
    HFIOR_RECORD_DEVICE_GONE   = 1u << 4,
    HFIOR_RECORD_DEVICE_READY  = 1u << 5,
    HFIOR_RECORD_OVERFLOW      = 1u << 6,
    HFIOR_RECORD_TERMINATE     = 1u << 7,
};

/* Common gaming-button bitmap. Unknown buttons remain visible in raw evdev logs. */
enum hfior_button_bit {
    HFIOR_BUTTON_LEFT    = 1u << 0,
    HFIOR_BUTTON_RIGHT   = 1u << 1,
    HFIOR_BUTTON_MIDDLE  = 1u << 2,
    HFIOR_BUTTON_SIDE    = 1u << 3,
    HFIOR_BUTTON_EXTRA   = 1u << 4,
    HFIOR_BUTTON_FORWARD = 1u << 5,
    HFIOR_BUTTON_BACK    = 1u << 6,
    HFIOR_BUTTON_TASK    = 1u << 7,
    HFIOR_BUTTON_8       = 1u << 8,
    HFIOR_BUTTON_9       = 1u << 9,
    HFIOR_BUTTON_10      = 1u << 10,
    HFIOR_BUTTON_11      = 1u << 11,
    HFIOR_BUTTON_12      = 1u << 12,
    HFIOR_BUTTON_13      = 1u << 13,
    HFIOR_BUTTON_14      = 1u << 14,
    HFIOR_BUTTON_15      = 1u << 15,
};

/*
 * One preserved evdev SYN_REPORT frame. The ABI is intentionally 32 bytes:
 * two records per 64-byte cache line, with sequence/timestamp first.
 */
struct hfior_record {
    uint64_t sequence;
    uint64_t timestamp_ns;
    int32_t  dx;
    int32_t  dy;
    int16_t  wheel;
    int16_t  hwheel;
    uint16_t buttons;
    uint16_t flags;
};
_Static_assert(sizeof(struct hfior_record) == 32u, "HFIOR record ABI must be 32 bytes");

/* Immutable after publication. */
struct hfior_ring_metadata {
    uint64_t magic;
    uint32_t abi_version;
    uint32_t header_size;
    uint32_t record_size;
    uint32_t capacity;
    uint64_t features;
    uint32_t source_clock_id;
    uint16_t input_bustype;
    uint16_t input_vendor;
    uint16_t input_product;
    uint16_t input_version;
    uint32_t reserved0;
    char     device_name[HFIOR_MAX_DEVICE_NAME];
};
_Static_assert(sizeof(struct hfior_ring_metadata) == 144u, "metadata ABI drift");

/* Producer-owned hot cache line. Monotonic counters avoid wrap ambiguity. */
struct hfior_producer_line {
    _Atomic uint64_t head;
    _Atomic uint64_t next_sequence;
    _Atomic uint64_t dropped_newest;
    _Atomic uint64_t eager_send_drops;
    _Atomic uint64_t generation;
    _Atomic uint64_t heartbeat_ns;
    _Atomic uint64_t urgent_writes;
    _Atomic uint64_t reserved;
};
_Static_assert(sizeof(struct hfior_producer_line) == HFIOR_CACHELINE, "producer line ABI drift");

/* Consumer-owned hot cache line. */
struct hfior_consumer_line {
    _Atomic uint64_t tail;
    _Atomic uint64_t deadline_ns;
    _Atomic uint64_t heartbeat_ns;
    _Atomic uint64_t drains;
    _Atomic uint64_t records_consumed;
    _Atomic uint64_t generation_seen;
    _Atomic uint64_t reserved0;
    _Atomic uint64_t reserved1;
};
_Static_assert(sizeof(struct hfior_consumer_line) == HFIOR_CACHELINE, "consumer line ABI drift");

/* Read-mostly counters placed away from head/tail. */
struct hfior_stats_line {
    _Atomic uint64_t records_published;
    _Atomic uint64_t motion_frames;
    _Atomic uint64_t button_frames;
    _Atomic uint64_t wheel_frames;
    _Atomic uint64_t syn_dropped_frames;
    _Atomic uint64_t device_reconnects;
    _Atomic uint64_t eager_packets_sent;
    _Atomic uint64_t reserved;
};
_Static_assert(sizeof(struct hfior_stats_line) == HFIOR_CACHELINE, "stats line ABI drift");

struct hfior_ring_header {
    struct hfior_ring_metadata metadata;
    uint8_t metadata_padding[48]; /* producer line begins at 192 */
    alignas(HFIOR_CACHELINE) struct hfior_producer_line producer;
    alignas(HFIOR_CACHELINE) struct hfior_consumer_line consumer;
    alignas(HFIOR_CACHELINE) struct hfior_stats_line stats;
};
_Static_assert(sizeof(struct hfior_ring_header) == 384u, "ring header ABI drift");
_Static_assert(offsetof(struct hfior_ring_header, producer) % HFIOR_CACHELINE == 0u, "producer misaligned");
_Static_assert(offsetof(struct hfior_ring_header, consumer) % HFIOR_CACHELINE == 0u, "consumer misaligned");
_Static_assert(offsetof(struct hfior_ring_header, stats) % HFIOR_CACHELINE == 0u, "stats misaligned");

static inline struct hfior_record *hfior_records(struct hfior_ring_header *header) {
    return (struct hfior_record *)((uint8_t *)header + header->metadata.header_size);
}

static inline const struct hfior_record *hfior_records_const(const struct hfior_ring_header *header) {
    return (const struct hfior_record *)((const uint8_t *)header + header->metadata.header_size);
}

static inline uint64_t hfior_ring_depth(const struct hfior_ring_header *header) {
    const uint64_t head = atomic_load_explicit(&header->producer.head, memory_order_acquire);
    const uint64_t tail = atomic_load_explicit(&header->consumer.tail, memory_order_acquire);
    return head - tail;
}

enum hfior_message_type {
    HFIOR_MSG_HELLO = 1,
    HFIOR_MSG_SET_MODE = 2,
    HFIOR_MSG_EAGER_RECORD = 3,
    HFIOR_MSG_DEVICE_STATE = 4,
    HFIOR_MSG_STOP = 5,
    HFIOR_MSG_PING = 6,
    HFIOR_MSG_PONG = 7,
    HFIOR_MSG_ACK = 8,
    HFIOR_MSG_START = 9,
    HFIOR_MSG_STARTED = 10,
};

enum hfior_client_mode {
    HFIOR_MODE_HFIOR = 1,
    HFIOR_MODE_EAGER_THREAD = 2,
    HFIOR_MODE_EAGER_FRAME = 3,
};

struct hfior_message_header {
    uint32_t type;
    uint32_t size;
};

struct hfior_hello_message {
    struct hfior_message_header header;
    uint32_t abi_version;
    uint32_t capacity;
    uint64_t generation;
    uint64_t features;
    uint32_t producer_pid;
    uint32_t reserved;
};

struct hfior_set_mode_message {
    struct hfior_message_header header;
    uint32_t mode;
    uint32_t consumer_hz;
    uint64_t requested_deadline_ns;
};

struct hfior_eager_record_message {
    struct hfior_message_header header;
    uint64_t generation;
    struct hfior_record record;
};


struct hfior_ack_message {
    struct hfior_message_header header;
    uint64_t generation;
    uint64_t consumed_tail;
};

struct hfior_device_state_message {
    struct hfior_message_header header;
    uint64_t generation;
    uint32_t state_flags;
    uint32_t reserved;
};

#ifdef __cplusplus
}
#endif
#endif
