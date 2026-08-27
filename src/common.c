#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t hfior_now_ns(int clock_id) {
    struct timespec ts;
    if (clock_gettime((clockid_t)clock_id, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

int hfior_sleep_until_ns(int clock_id, uint64_t deadline_ns) {
    struct timespec ts = {
        .tv_sec = (time_t)(deadline_ns / UINT64_C(1000000000)),
        .tv_nsec = (long)(deadline_ns % UINT64_C(1000000000)),
    };
    int rc;
    do {
        rc = clock_nanosleep((clockid_t)clock_id, TIMER_ABSTIME, &ts, NULL);
    } while (rc == EINTR);
    return rc == 0 ? 0 : -rc;
}

void hfior_vec_push(struct hfior_u64_vec *vec, uint64_t value) {
    if (vec->length == vec->capacity) {
        size_t new_capacity = vec->capacity ? vec->capacity * 2u : 4096u;
        void *new_data = realloc(vec->data, new_capacity * sizeof(*vec->data));
        if (!new_data) {
            fprintf(stderr, "fatal: out of memory growing metric vector\n");
            abort();
        }
        vec->data = new_data;
        vec->capacity = new_capacity;
    }
    vec->data[vec->length++] = value;
}

void hfior_vec_free(struct hfior_u64_vec *vec) {
    free(vec->data);
    *vec = (struct hfior_u64_vec){0};
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

double hfior_percentile_ns(struct hfior_u64_vec *vec, double percentile) {
    if (!vec->length)
        return 0.0;
    /* Public callers use conventional 0..100 percentile notation. */
    if (percentile < 0.0)
        percentile = 0.0;
    if (percentile > 100.0)
        percentile = 100.0;
    percentile /= 100.0;
    qsort(vec->data, vec->length, sizeof(*vec->data), compare_u64);
    const double rank = percentile * (double)(vec->length - 1u);
    const size_t lo = (size_t)floor(rank);
    const size_t hi = (size_t)ceil(rank);
    const double fraction = rank - (double)lo;
    return (double)vec->data[lo] * (1.0 - fraction) +
           (double)vec->data[hi] * fraction;
}

double hfior_worst_fraction_mean_ns(struct hfior_u64_vec *vec, double fraction) {
    if (!vec->length)
        return 0.0;
    if (fraction <= 0.0)
        fraction = 1.0 / (double)vec->length;
    if (fraction > 1.0)
        fraction = 1.0;
    qsort(vec->data, vec->length, sizeof(*vec->data), compare_u64);
    size_t count = (size_t)ceil(fraction * (double)vec->length);
    if (!count)
        count = 1u;
    const size_t start = vec->length - count;
    long double sum = 0.0L;
    for (size_t i = start; i < vec->length; ++i)
        sum += (long double)vec->data[i];
    return (double)(sum / (long double)count);
}

double hfior_mean_ns(const struct hfior_u64_vec *vec) {
    if (!vec->length)
        return 0.0;
    long double sum = 0.0L;
    for (size_t i = 0; i < vec->length; ++i)
        sum += (long double)vec->data[i];
    return (double)(sum / (long double)vec->length);
}

uint64_t hfior_max_ns(const struct hfior_u64_vec *vec) {
    uint64_t maximum = 0;
    for (size_t i = 0; i < vec->length; ++i)
        if (vec->data[i] > maximum)
            maximum = vec->data[i];
    return maximum;
}

bool hfior_is_power_of_two_u32(uint32_t value) {
    return value != 0u && (value & (value - 1u)) == 0u;
}

size_t hfior_mapping_size(uint32_t capacity) {
    return sizeof(struct hfior_ring_header) +
           (size_t)capacity * sizeof(struct hfior_record);
}

int hfior_initialize_mapping(struct hfior_ring_header *header, size_t mapping_size,
                             uint32_t capacity, uint64_t features,
                             const char *device_name,
                             uint16_t bustype, uint16_t vendor,
                             uint16_t product, uint16_t version,
                             uint64_t generation) {
    if (!header || !hfior_is_power_of_two_u32(capacity) ||
        mapping_size < hfior_mapping_size(capacity)) {
        errno = EINVAL;
        return -1;
    }
    memset(header, 0, mapping_size);
    header->metadata.magic = HFIOR_MAGIC;
    header->metadata.abi_version = HFIOR_ABI_VERSION;
    header->metadata.header_size = (uint32_t)sizeof(*header);
    header->metadata.record_size = (uint32_t)sizeof(struct hfior_record);
    header->metadata.capacity = capacity;
    header->metadata.features = features;
    header->metadata.source_clock_id = (uint32_t)CLOCK_MONOTONIC;
    header->metadata.input_bustype = bustype;
    header->metadata.input_vendor = vendor;
    header->metadata.input_product = product;
    header->metadata.input_version = version;
    if (device_name)
        snprintf(header->metadata.device_name,
                 sizeof(header->metadata.device_name), "%s", device_name);
    atomic_init(&header->producer.head, 0u);
    atomic_init(&header->producer.next_sequence, 1u);
    atomic_init(&header->producer.dropped_newest, 0u);
    atomic_init(&header->producer.eager_send_drops, 0u);
    atomic_init(&header->producer.generation, generation);
    atomic_init(&header->producer.heartbeat_ns,
                hfior_now_ns(CLOCK_MONOTONIC));
    atomic_init(&header->producer.urgent_writes, 0u);
    atomic_init(&header->consumer.tail, 0u);
    atomic_init(&header->consumer.deadline_ns, 0u);
    atomic_init(&header->consumer.heartbeat_ns, 0u);
    atomic_init(&header->consumer.drains, 0u);
    atomic_init(&header->consumer.records_consumed, 0u);
    atomic_init(&header->consumer.generation_seen, generation);
    atomic_init(&header->stats.records_published, 0u);
    atomic_init(&header->stats.motion_frames, 0u);
    atomic_init(&header->stats.button_frames, 0u);
    atomic_init(&header->stats.wheel_frames, 0u);
    atomic_init(&header->stats.syn_dropped_frames, 0u);
    atomic_init(&header->stats.device_reconnects, 0u);
    atomic_init(&header->stats.eager_packets_sent, 0u);
    return 0;
}

bool hfior_ring_publish(struct hfior_ring_header *header,
                        const struct hfior_record *record) {
    const uint64_t head =
        atomic_load_explicit(&header->producer.head, memory_order_relaxed);
    const uint64_t tail =
        atomic_load_explicit(&header->consumer.tail, memory_order_acquire);
    const uint32_t capacity = header->metadata.capacity;
    if (head - tail >= (uint64_t)capacity) {
        atomic_fetch_add_explicit(&header->producer.dropped_newest, 1u,
                                  memory_order_relaxed);
        return false;
    }
    struct hfior_record *records = hfior_records(header);
    records[head & (uint64_t)(capacity - 1u)] = *record;
    atomic_store_explicit(&header->producer.head, head + 1u,
                          memory_order_release);
    atomic_fetch_add_explicit(&header->stats.records_published, 1u,
                              memory_order_relaxed);
    if (record->flags & HFIOR_RECORD_MOTION)
        atomic_fetch_add_explicit(&header->stats.motion_frames, 1u,
                                  memory_order_relaxed);
    if (record->flags & HFIOR_RECORD_BUTTON)
        atomic_fetch_add_explicit(&header->stats.button_frames, 1u,
                                  memory_order_relaxed);
    if (record->flags & HFIOR_RECORD_WHEEL)
        atomic_fetch_add_explicit(&header->stats.wheel_frames, 1u,
                                  memory_order_relaxed);
    if (record->flags & HFIOR_RECORD_SYN_DROPPED)
        atomic_fetch_add_explicit(&header->stats.syn_dropped_frames, 1u,
                                  memory_order_relaxed);
    return true;
}

size_t hfior_ring_drain(struct hfior_ring_header *header,
                        struct hfior_record *out,
                        size_t max_records) {
    uint64_t tail =
        atomic_load_explicit(&header->consumer.tail, memory_order_relaxed);
    const uint64_t head =
        atomic_load_explicit(&header->producer.head, memory_order_acquire);
    uint64_t available = head - tail;
    if (available > max_records)
        available = max_records;
    const struct hfior_record *records = hfior_records_const(header);
    const uint64_t mask = (uint64_t)(header->metadata.capacity - 1u);
    for (uint64_t i = 0; i < available; ++i)
        out[i] = records[(tail + i) & mask];
    if (available) {
        tail += available;
        atomic_store_explicit(&header->consumer.tail, tail,
                              memory_order_release);
        atomic_fetch_add_explicit(&header->consumer.drains, 1u,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&header->consumer.records_consumed,
                                  available, memory_order_relaxed);
        atomic_store_explicit(&header->consumer.heartbeat_ns,
                              hfior_now_ns(CLOCK_MONOTONIC),
                              memory_order_relaxed);
    }
    return (size_t)available;
}

ssize_t hfior_ring_read_snapshot(const struct hfior_ring_header *header,
                                 uint64_t *local_tail,
                                 struct hfior_record *out,
                                 size_t max_records) {
    if (!header || !local_tail || (!out && max_records)) {
        errno = EINVAL;
        return -1;
    }
    const uint64_t head =
        atomic_load_explicit(&header->producer.head, memory_order_acquire);
    const uint32_t capacity = header->metadata.capacity;
    if (*local_tail > head || head - *local_tail > (uint64_t)capacity) {
        errno = EOVERFLOW;
        return -1;
    }
    uint64_t available = head - *local_tail;
    if (available > max_records)
        available = max_records;
    const struct hfior_record *records = hfior_records_const(header);
    const uint64_t mask = (uint64_t)(capacity - 1u);
    for (uint64_t i = 0; i < available; ++i)
        out[i] = records[(*local_tail + i) & mask];
    *local_tail += available;
    return (ssize_t)available;
}

int hfior_send_fds(int socket_fd, const void *message, size_t message_size,
                   const int *fds, size_t fd_count, int flags) {
    if (fd_count > 8u) {
        errno = E2BIG;
        return -1;
    }
    struct iovec iov = {.iov_base = (void *)message, .iov_len = message_size};
    char control[CMSG_SPACE(sizeof(int) * 8u)] = {0};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1u};
    if (fd_count) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    ssize_t written;
    do {
        written = sendmsg(socket_fd, &msg, flags | MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
        return -1;
    if ((size_t)written != message_size) {
        errno = EIO;
        return -1;
    }
    return 0;
}

ssize_t hfior_recv_fds(int socket_fd, void *message, size_t message_capacity,
                       int *fds, size_t fd_capacity, size_t *fd_count,
                       int flags) {
    if (fd_count)
        *fd_count = 0u;
    struct iovec iov = {.iov_base = message, .iov_len = message_capacity};
    char control[CMSG_SPACE(sizeof(int) * 8u)] = {0};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1u,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    ssize_t received;
    do {
        received = recvmsg(socket_fd, &msg, flags);
    } while (received < 0 && errno == EINTR);
    if (received <= 0)
        return received;
    if (msg.msg_flags & MSG_CTRUNC) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t count = 0u;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0u);
        const size_t found = bytes / sizeof(int);
        const int *received_fds = (const int *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < found; ++i) {
            if (count < fd_capacity) {
                fds[count++] = received_fds[i];
            } else {
                close(received_fds[i]);
            }
        }
    }
    if (fd_count)
        *fd_count = count;
    return received;
}

int hfior_set_nonblocking(int fd, bool enabled) {
    const int old_flags = fcntl(fd, F_GETFL);
    if (old_flags < 0)
        return -1;
    int new_flags = old_flags;
    if (enabled)
        new_flags |= O_NONBLOCK;
    else
        new_flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, new_flags);
}

int hfior_make_runtime_socket_path(char *out, size_t out_size, const char *leaf) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!runtime || !*runtime) {
        snprintf(fallback, sizeof(fallback), "/tmp/hfior-%lu",
                 (unsigned long)getuid());
        if (mkdir(fallback, 0700) != 0 && errno != EEXIST)
            return -1;
        runtime = fallback;
    }
    const int needed = snprintf(out, out_size, "%s/%s", runtime, leaf);
    if (needed < 0 || (size_t)needed >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int hfior_verify_same_uid_peer(int socket_fd, uid_t expected_uid, pid_t *peer_pid) {
    struct ucred credentials = {0};
    socklen_t length = sizeof(credentials);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        return -1;
    if (credentials.uid != expected_uid) {
        errno = EACCES;
        return -1;
    }
    if (peer_pid)
        *peer_pid = credentials.pid;
    return 0;
}

uint16_t hfior_button_bit_from_linux_code(uint16_t code) {
    switch (code) {
    case BTN_LEFT: return HFIOR_BUTTON_LEFT;
    case BTN_RIGHT: return HFIOR_BUTTON_RIGHT;
    case BTN_MIDDLE: return HFIOR_BUTTON_MIDDLE;
    case BTN_SIDE: return HFIOR_BUTTON_SIDE;
    case BTN_EXTRA: return HFIOR_BUTTON_EXTRA;
    case BTN_FORWARD: return HFIOR_BUTTON_FORWARD;
    case BTN_BACK: return HFIOR_BUTTON_BACK;
    case BTN_TASK: return HFIOR_BUTTON_TASK;
#ifdef BTN_8
    case BTN_8: return HFIOR_BUTTON_8;
#endif
#ifdef BTN_9
    case BTN_9: return HFIOR_BUTTON_9;
#endif
    default: return 0u;
    }
}

const char *hfior_mode_name(uint32_t mode) {
    switch (mode) {
    case HFIOR_MODE_HFIOR: return "hfior";
    case HFIOR_MODE_EAGER_THREAD: return "eager-thread";
    case HFIOR_MODE_EAGER_FRAME: return "eager-frame";
    default: return "unknown";
    }
}
