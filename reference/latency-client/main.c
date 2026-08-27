#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum hfior_policy {
    POLICY_EAGER_THREAD = 1,
    POLICY_HFIOR_CRITICAL_ACK,
    POLICY_HFIOR_LATE,
    POLICY_HFIOR_MULTI,
    POLICY_HFIOR_TWO_STAGE,
    POLICY_HFIOR_SPIN,
    POLICY_HFIOR_LATE_LATCH,
};

enum ack_placement {
    ACK_CRITICAL = 1,
    ACK_POST_USE,
    ACK_POST_FRAME,
};

enum receipt_reason {
    RECEIPT_URGENT = 1,
    RECEIPT_FRAME_RING,
    RECEIPT_FINAL_RING,
    RECEIPT_EAGER_THREAD,
    RECEIPT_SHUTDOWN,
};

struct client_config {
    const char *socket_path;
    const char *output_dir;
    enum hfior_policy policy;
    enum ack_placement ack_placement;
    uint32_t checks_per_frame;
    uint32_t final_rechecks;
    uint32_t frame_hz;
    double duration_s;
    double input_phase;
    uint64_t base_work_ns;
    uint64_t integration_work_ns;
    uint64_t callback_work_ns;
    uint64_t spin_ns;
    uint64_t tail_threshold_ns;
    uint32_t stall_after_ms;
    uint32_t stall_ms;
    int consumer_cpu;
    int ingress_cpu;
    bool record_trace;
    bool uncapped;
    bool quiet;
};

struct record_queue {
    pthread_mutex_t mutex;
    struct hfior_record *records;
    size_t length;
    size_t capacity;
};

struct receipt_entry {
    uint64_t receipt_time_ns;
    struct hfior_record record;
    uint32_t reason;
    uint32_t reserved;
};

struct receipt_vec {
    struct receipt_entry *data;
    size_t length;
    size_t capacity;
};

struct client_stats {
    uint64_t started_ns;
    uint64_t finished_ns;
    uint64_t frames;
    uint64_t overruns;
    uint64_t input_consumptions;
    uint64_t expensive_actions;
    uint64_t late_latch_actions;
    uint64_t ring_checks;
    uint64_t nonempty_ring_checks;
    uint64_t ring_records;
    uint64_t shadow_ring_records;
    uint64_t eager_messages;
    uint64_t eager_records;
    uint64_t control_messages;
    uint64_t acknowledgements_sent;
    uint64_t urgent_reads;
    uint64_t urgent_counter_total;
    uint64_t terminal_records;
    uint64_t sequence_gaps;
    uint64_t duplicate_or_reordered;
    uint64_t first_sequence;
    uint64_t last_sequence;
    uint64_t ingress_thread_cpu_ns;
    uint64_t consumer_process_cpu_ns;
    uint64_t voluntary_context_switches;
    uint64_t involuntary_context_switches;
    uint64_t generation_changes;
    uint64_t stall_count;
    uint64_t late_latch_records;
    uint64_t frames_with_late_latch;
    uint64_t spin_iterations;
};

struct frame_observation {
    uint64_t frame_index;
    uint64_t target_start_ns;
    uint64_t frame_start_ns;
    uint64_t start_lateness_ns;
    uint64_t legacy_measurement_ns;
    uint64_t latch_begin_ns;
    uint64_t latch_end_ns;
    uint64_t use_time_ns;
    uint64_t newest_timestamp_ns;
    uint64_t newest_sequence;
    uint64_t oldest_timestamp_ns;
    uint64_t actual_age_ns;
    uint64_t legacy_age_ns;
    uint64_t latch_cost_ns;
    uint64_t post_latch_to_use_ns;
    uint64_t head_at_frame_start;
    uint64_t head_at_initial_latch;
    uint64_t head_at_final_latch;
    uint64_t local_tail_after;
    uint64_t records;
    uint64_t late_records;
    uint64_t checks;
    uint64_t expensive_actions;
    uint64_t ring_depth_after;
};

struct client {
    struct client_config config;
    struct client_stats stats;
    int socket_fd;
    int memfd;
    int urgent_fd;
    struct hfior_ring_header *ring;
    size_t mapping_size;
    uint64_t generation;
    uint64_t local_tail;
    uint64_t last_acked_tail;
    uint64_t features;
    pid_t producer_pid;
    FILE *frames_csv;
    FILE *tail_csv;
    struct receipt_vec receipts;
    struct record_queue pending;
    pthread_mutex_t ring_drain_mutex;
    pthread_t ingress_thread;
    bool ingress_started;
    _Atomic bool stop;
    _Atomic bool terminal_seen;
    struct hfior_u64_vec frame_times;
    struct hfior_u64_vec sample_ages;
    struct hfior_u64_vec legacy_ages;
    struct hfior_u64_vec latch_costs;
    struct hfior_u64_vec start_lateness;
    struct hfior_u64_vec button_ages;
    struct hfior_u64_vec eager_ingress_ages;
};

static volatile sig_atomic_t signal_stop = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    signal_stop = 1;
}

static void die(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static uint32_t parse_u32(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint32_t)value;
}

static int parse_i32(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, 10);
    if (errno || !end || *end || value < INT32_MIN || value > INT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (int)value;
}

static double parse_double(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    const double value = strtod(text, &end);
    if (errno || !end || *end || !isfinite(value) || value < 0.0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static enum ack_placement parse_ack_placement(const char *text) {
    if (!strcmp(text, "critical")) return ACK_CRITICAL;
    if (!strcmp(text, "post-use")) return ACK_POST_USE;
    if (!strcmp(text, "post-frame")) return ACK_POST_FRAME;
    fprintf(stderr, "invalid ack placement: %s\n", text);
    exit(2);
}

static const char *ack_placement_name(enum ack_placement placement) {
    switch (placement) {
    case ACK_CRITICAL: return "critical";
    case ACK_POST_USE: return "post-use";
    case ACK_POST_FRAME: return "post-frame";
    }
    return "unknown";
}

static enum hfior_policy parse_policy(const char *text,
                                       uint32_t *checks_per_frame) {
    if (!strcmp(text, "eager-thread")) return POLICY_EAGER_THREAD;
    if (!strcmp(text, "hfior-critical-ack")) return POLICY_HFIOR_CRITICAL_ACK;
    if (!strcmp(text, "hfior-late")) return POLICY_HFIOR_LATE;
    if (!strcmp(text, "hfior-2")) {
        *checks_per_frame = 2u;
        return POLICY_HFIOR_MULTI;
    }
    if (!strcmp(text, "hfior-4")) {
        *checks_per_frame = 4u;
        return POLICY_HFIOR_MULTI;
    }
    if (!strcmp(text, "hfior-8")) {
        *checks_per_frame = 8u;
        return POLICY_HFIOR_MULTI;
    }
    if (!strcmp(text, "hfior-multi")) return POLICY_HFIOR_MULTI;
    if (!strcmp(text, "hfior-two-stage")) return POLICY_HFIOR_TWO_STAGE;
    if (!strcmp(text, "hfior-spin")) return POLICY_HFIOR_SPIN;
    if (!strcmp(text, "hfior-late-latch")) return POLICY_HFIOR_LATE_LATCH;
    if (!strcmp(text, "hfior-8-latch")) {
        *checks_per_frame = 8u;
        return POLICY_HFIOR_LATE_LATCH;
    }
    if (!strcmp(text, "hfior-stable-latch")) {
        *checks_per_frame = 1u;
        return POLICY_HFIOR_LATE_LATCH;
    }
    if (!strcmp(text, "hfior-8-stable-latch")) {
        *checks_per_frame = 8u;
        return POLICY_HFIOR_LATE_LATCH;
    }
    fprintf(stderr, "invalid policy: %s\n", text);
    exit(2);
}

static const char *policy_name(const struct client_config *config) {
    switch (config->policy) {
    case POLICY_EAGER_THREAD: return "eager-thread";
    case POLICY_HFIOR_CRITICAL_ACK: return "hfior-critical-ack";
    case POLICY_HFIOR_LATE: return "hfior-late";
    case POLICY_HFIOR_MULTI:
        if (config->checks_per_frame == 2u) return "hfior-2";
        if (config->checks_per_frame == 4u) return "hfior-4";
        if (config->checks_per_frame == 8u) return "hfior-8";
        return "hfior-multi";
    case POLICY_HFIOR_TWO_STAGE: return "hfior-two-stage";
    case POLICY_HFIOR_SPIN: return "hfior-spin";
    case POLICY_HFIOR_LATE_LATCH:
        if (config->final_rechecks)
            return config->checks_per_frame == 8u ? "hfior-8-stable-latch" :
                                                   "hfior-stable-latch";
        return config->checks_per_frame == 8u ? "hfior-8-latch" :
                                               "hfior-late-latch";
    }
    return "unknown";
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s --policy POLICY --output-dir DIR [options]\n"
            "Policies: eager-thread, hfior-critical-ack, hfior-late, hfior-2,\n"
            "          hfior-4, hfior-8, hfior-multi, hfior-two-stage,\n"
            "          hfior-spin, hfior-late-latch, hfior-8-latch,\n"
            "          hfior-stable-latch, hfior-8-stable-latch\n"
            "  --socket PATH              runtime socket\n"
            "  --duration SEC             default 5\n"
            "  --frame-hz HZ              default 240\n"
            "  --input-phase FRACTION     primary latch point, default 0.5\n"
            "  --base-work-us US          work before/around latch, default 1800\n"
            "  --integration-work-us US   work between initial and final latch\n"
            "  --callback-work-ns NS      eager per-report work\n"
            "  --checks-per-frame N       hfior-multi / late-latch prechecks\n"
            "  --final-rechecks N         bounded stability checks at use\n"
            "  --ack-placement critical|post-use|post-frame\n"
            "  --spin-us US               bounded spin experiment\n"
            "  --tail-threshold-us US     detailed tail-event cutoff\n"
            "  --consumer-cpu N --ingress-cpu N\n"
            "  --stall-after-ms N --stall-ms N\n"
            "  --record-trace             retain every record receipt\n"
            "  --uncapped --quiet\n",
            argv0);
}

static void mkdir_p(const char *path) {
    char buffer[4096];
    if (strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        die("output path");
    }
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *p = buffer + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
                die("mkdir");
            *p = '/';
        }
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST)
        die("mkdir");
}

static FILE *open_output(const char *directory, const char *name) {
    char path[4096];
    const int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        die("output file path");
    }
    FILE *file = fopen(path, "w");
    if (!file)
        die(path);
    setvbuf(file, NULL, _IOFBF, 1u << 20);
    return file;
}

static void pin_current_thread(int cpu) {
    if (cpu < 0)
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    const int result = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (result != 0) {
        errno = result;
        die("pthread_setaffinity_np");
    }
}

static void busy_for_ns(uint64_t duration_ns) {
    if (!duration_ns)
        return;
    const uint64_t end = hfior_now_ns(CLOCK_MONOTONIC) + duration_ns;
    volatile uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    do {
        for (unsigned i = 0; i < 64u; ++i) {
            state ^= state << 7;
            state ^= state >> 9;
            state *= UINT64_C(0xbf58476d1ce4e5b9);
        }
    } while (hfior_now_ns(CLOCK_MONOTONIC) < end);
    (void)state;
}

static void queue_init(struct record_queue *queue) {
    memset(queue, 0, sizeof(*queue));
    const int result = pthread_mutex_init(&queue->mutex, NULL);
    if (result != 0) {
        errno = result;
        die("queue mutex");
    }
}

static void queue_reserve_locked(struct record_queue *queue, size_t wanted) {
    if (wanted <= queue->capacity)
        return;
    size_t capacity = queue->capacity ? queue->capacity : 4096u;
    while (capacity < wanted) {
        if (capacity > SIZE_MAX / 2u) {
            errno = ENOMEM;
            die("queue capacity");
        }
        capacity *= 2u;
    }
    void *data = realloc(queue->records, capacity * sizeof(*queue->records));
    if (!data)
        die("queue realloc");
    queue->records = data;
    queue->capacity = capacity;
}

static void queue_push(struct record_queue *queue,
                       const struct hfior_record *records, size_t count) {
    if (!count)
        return;
    pthread_mutex_lock(&queue->mutex);
    queue_reserve_locked(queue, queue->length + count);
    memcpy(queue->records + queue->length, records,
           count * sizeof(*records));
    queue->length += count;
    pthread_mutex_unlock(&queue->mutex);
}

static struct hfior_record *queue_take_all(struct record_queue *queue,
                                           size_t *count) {
    pthread_mutex_lock(&queue->mutex);
    *count = queue->length;
    struct hfior_record *records = NULL;
    if (queue->length) {
        records = malloc(queue->length * sizeof(*records));
        if (!records)
            die("queue snapshot");
        memcpy(records, queue->records, queue->length * sizeof(*records));
    }
    queue->length = 0u;
    pthread_mutex_unlock(&queue->mutex);
    return records;
}

static void queue_destroy(struct record_queue *queue) {
    pthread_mutex_destroy(&queue->mutex);
    free(queue->records);
    memset(queue, 0, sizeof(*queue));
}

static int connect_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    for (unsigned attempt = 0; attempt < 500u; ++attempt) {
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
            return fd;
        if (errno != ENOENT && errno != ECONNREFUSED)
            break;
        struct timespec delay = {.tv_nsec = 2000000L};
        nanosleep(&delay, NULL);
    }
    const int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

static void receive_hello(struct client *client) {
    struct hfior_hello_message hello;
    int fds[4] = {-1, -1, -1, -1};
    size_t fd_count = 0u;
    const ssize_t bytes = hfior_recv_fds(client->socket_fd, &hello,
                                         sizeof(hello), fds, 4u,
                                         &fd_count, 0);
    if (bytes != (ssize_t)sizeof(hello) ||
        hello.header.type != HFIOR_MSG_HELLO ||
        hello.header.size != sizeof(hello) ||
        hello.abi_version != HFIOR_ABI_VERSION || fd_count != 2u) {
        errno = EPROTO;
        die("HFIOR hello");
    }
    client->memfd = fds[0];
    client->urgent_fd = fds[1];
    client->generation = hello.generation;
    client->features = hello.features;
    client->producer_pid = (pid_t)hello.producer_pid;
    client->mapping_size = hfior_mapping_size(hello.capacity);
    client->ring = mmap(NULL, client->mapping_size, PROT_READ, MAP_SHARED,
                        client->memfd, 0);
    if (client->ring == MAP_FAILED)
        die("mmap HFIOR ring");
    if (client->ring->metadata.magic != HFIOR_MAGIC ||
        client->ring->metadata.abi_version != HFIOR_ABI_VERSION ||
        client->ring->metadata.record_size != sizeof(struct hfior_record) ||
        client->ring->metadata.capacity != hello.capacity) {
        errno = EPROTO;
        die("HFIOR metadata");
    }
}

static void send_mode(struct client *client) {
    const uint32_t bridge_mode = client->config.policy == POLICY_EAGER_THREAD
                                     ? HFIOR_MODE_EAGER_THREAD
                                     : HFIOR_MODE_HFIOR;
    const struct hfior_set_mode_message message = {
        .header = {HFIOR_MSG_SET_MODE, sizeof(message)},
        .mode = bridge_mode,
        .consumer_hz = client->config.frame_hz,
        .requested_deadline_ns = 0u,
    };
    if (send(client->socket_fd, &message, sizeof(message), MSG_NOSIGNAL) !=
        (ssize_t)sizeof(message))
        die("send HFIOR mode");
}

static void synchronize_start(struct client *client) {
    const struct hfior_message_header start = {
        HFIOR_MSG_START, sizeof(start)
    };
    if (send(client->socket_fd, &start, sizeof(start), MSG_NOSIGNAL) !=
        (ssize_t)sizeof(start))
        die("send HFIOR start");
    for (;;) {
        struct hfior_message_header response;
        const ssize_t bytes = recv(client->socket_fd, &response,
                                   sizeof(response), 0);
        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            die("wait HFIOR start");
        }
        if (bytes == (ssize_t)sizeof(response) &&
            response.size == sizeof(response) &&
            response.type == HFIOR_MSG_STARTED)
            break;
    }
    client->local_tail = atomic_load_explicit(
        &client->ring->producer.head, memory_order_acquire);
    client->last_acked_tail = client->local_tail;
}

static void send_ack_if_needed(struct client *client) {
    if (client->local_tail == client->last_acked_tail)
        return;
    const struct hfior_ack_message message = {
        .header = {HFIOR_MSG_ACK, sizeof(message)},
        .generation = client->generation,
        .consumed_tail = client->local_tail,
    };
    if (send(client->socket_fd, &message, sizeof(message), MSG_NOSIGNAL) !=
        (ssize_t)sizeof(message))
        die("send HFIOR acknowledgement");
    client->last_acked_tail = client->local_tail;
    client->stats.acknowledgements_sent++;
}

static void send_stop(struct client *client) {
    const struct hfior_message_header message = {
        HFIOR_MSG_STOP, sizeof(message)
    };
    (void)send(client->socket_fd, &message, sizeof(message), MSG_NOSIGNAL);
}

static void account_sequences(struct client *client,
                              const struct hfior_record *records, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint64_t sequence = records[i].sequence;
        if (!sequence)
            continue;
        if (!client->stats.first_sequence)
            client->stats.first_sequence = sequence;
        if (client->stats.last_sequence) {
            if (sequence == client->stats.last_sequence + 1u) {
                /* expected */
            } else if (sequence > client->stats.last_sequence + 1u) {
                client->stats.sequence_gaps +=
                    sequence - client->stats.last_sequence - 1u;
            } else {
                client->stats.duplicate_or_reordered++;
            }
        }
        if (sequence > client->stats.last_sequence)
            client->stats.last_sequence = sequence;
        if (records[i].flags & HFIOR_RECORD_TERMINATE) {
            client->stats.terminal_records++;
            atomic_store_explicit(&client->terminal_seen, true,
                                  memory_order_relaxed);
        }
    }
}

static void receipt_push(struct receipt_vec *vec,
                         const struct receipt_entry *entry) {
    if (vec->length == vec->capacity) {
        size_t capacity = vec->capacity ? vec->capacity * 2u : 16384u;
        struct receipt_entry *data = realloc(vec->data,
                                             capacity * sizeof(*data));
        if (!data)
            die("receipt realloc");
        vec->data = data;
        vec->capacity = capacity;
    }
    vec->data[vec->length++] = *entry;
}

static void log_records(struct client *client, uint32_t reason,
                        const struct hfior_record *records, size_t count,
                        uint64_t receipt_time_ns) {
    if (!client->config.record_trace)
        return;
    for (size_t i = 0; i < count; ++i) {
        const struct receipt_entry entry = {
            .receipt_time_ns = receipt_time_ns,
            .record = records[i],
            .reason = reason,
        };
        receipt_push(&client->receipts, &entry);
    }
}

static size_t drain_ring(struct client *client, uint32_t reason,
                         bool shadow, bool acknowledge_now) {
    struct hfior_record records[4096];
    size_t total = 0u;
    pthread_mutex_lock(&client->ring_drain_mutex);
    client->stats.ring_checks++;
    for (;;) {
        const ssize_t result = hfior_ring_read_snapshot(
            client->ring, &client->local_tail, records,
            sizeof(records) / sizeof(records[0]));
        if (result < 0)
            die("read-only HFIOR drain");
        const size_t count = (size_t)result;
        if (!count)
            break;
        total += count;
        if (!shadow) {
            const uint64_t now_ns = hfior_now_ns(CLOCK_MONOTONIC);
            account_sequences(client, records, count);
            if (reason == RECEIPT_URGENT) {
                for (size_t i = 0; i < count; ++i) {
                    if (records[i].flags & HFIOR_RECORD_BUTTON) {
                        const uint64_t age = now_ns >= records[i].timestamp_ns
                                                 ? now_ns - records[i].timestamp_ns : 0u;
                        hfior_vec_push(&client->button_ages, age);
                    }
                }
            }
            log_records(client, reason, records, count, now_ns);
            queue_push(&client->pending, records, count);
            client->stats.ring_records += count;
        } else {
            client->stats.shadow_ring_records += count;
        }
        if (count < sizeof(records) / sizeof(records[0]))
            break;
    }
    if (total)
        client->stats.nonempty_ring_checks++;
    if (acknowledge_now)
        send_ack_if_needed(client);
    pthread_mutex_unlock(&client->ring_drain_mutex);
    return total;
}

static void process_eager_message(struct client *client,
                                  const struct hfior_eager_record_message *message) {
    const uint64_t now_ns = hfior_now_ns(CLOCK_MONOTONIC);
    if (client->config.callback_work_ns)
        busy_for_ns(client->config.callback_work_ns);
    client->stats.eager_messages++;
    client->stats.eager_records++;
    account_sequences(client, &message->record, 1u);
    if (now_ns >= message->record.timestamp_ns)
        hfior_vec_push(&client->eager_ingress_ages,
                       now_ns - message->record.timestamp_ns);
    if (message->record.flags & HFIOR_RECORD_BUTTON) {
        const uint64_t age = now_ns >= message->record.timestamp_ns
                                 ? now_ns - message->record.timestamp_ns : 0u;
        hfior_vec_push(&client->button_ages, age);
    }
    log_records(client, RECEIPT_EAGER_THREAD, &message->record, 1u, now_ns);
    queue_push(&client->pending, &message->record, 1u);
}

static size_t drain_control_socket_nonblocking(struct client *client,
                                               bool accept_eager) {
    size_t eager_count = 0u;
    for (;;) {
        uint8_t buffer[256];
        const ssize_t bytes = recv(client->socket_fd, buffer, sizeof(buffer),
                                   MSG_DONTWAIT);
        if (bytes == 0) {
            atomic_store_explicit(&client->terminal_seen, true,
                                  memory_order_relaxed);
            break;
        }
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            atomic_store_explicit(&client->terminal_seen, true,
                                  memory_order_relaxed);
            break;
        }
        if ((size_t)bytes < sizeof(struct hfior_message_header))
            continue;
        const struct hfior_message_header *header =
            (const struct hfior_message_header *)buffer;
        if (header->size != (uint32_t)bytes)
            continue;
        if (header->type == HFIOR_MSG_EAGER_RECORD &&
            (size_t)bytes == sizeof(struct hfior_eager_record_message) &&
            accept_eager) {
            process_eager_message(client,
                (const struct hfior_eager_record_message *)buffer);
            eager_count++;
        } else if (header->type == HFIOR_MSG_DEVICE_STATE &&
                   (size_t)bytes == sizeof(struct hfior_device_state_message)) {
            const struct hfior_device_state_message *state =
                (const struct hfior_device_state_message *)buffer;
            if (state->generation != client->generation) {
                client->generation = state->generation;
                client->stats.generation_changes++;
            }
            client->stats.control_messages++;
        } else {
            client->stats.control_messages++;
        }
    }
    return eager_count;
}

static void *ingress_thread_main(void *opaque) {
    struct client *client = opaque;
    pin_current_thread(client->config.ingress_cpu);
    const uint64_t cpu_start = hfior_now_ns(CLOCK_THREAD_CPUTIME_ID);
    if (client->config.policy == POLICY_EAGER_THREAD) {
        while (!atomic_load_explicit(&client->stop, memory_order_relaxed)) {
            struct pollfd fd = {.fd = client->socket_fd, .events = POLLIN};
            const int ready = poll(&fd, 1u, 100);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (ready > 0 && (fd.revents & POLLIN))
                (void)drain_control_socket_nonblocking(client, true);
            if (ready > 0 && (fd.revents & (POLLHUP | POLLERR | POLLNVAL)))
                break;
        }
    } else {
        while (!atomic_load_explicit(&client->stop, memory_order_relaxed)) {
            struct pollfd fd = {.fd = client->urgent_fd, .events = POLLIN};
            const int ready = poll(&fd, 1u, 100);
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (ready > 0 && (fd.revents & POLLIN)) {
                uint64_t counter = 0u;
                const ssize_t bytes = read(client->urgent_fd, &counter,
                                           sizeof(counter));
                if (bytes == (ssize_t)sizeof(counter)) {
                    client->stats.urgent_reads++;
                    client->stats.urgent_counter_total += counter;
                    drain_ring(client, RECEIPT_URGENT, false, true);
                }
            }
        }
    }
    client->stats.ingress_thread_cpu_ns =
        hfior_now_ns(CLOCK_THREAD_CPUTIME_ID) - cpu_start;
    return NULL;
}

static void start_ingress_thread(struct client *client) {
    const int result = pthread_create(&client->ingress_thread, NULL,
                                      ingress_thread_main, client);
    if (result != 0) {
        errno = result;
        die("pthread_create");
    }
    client->ingress_started = true;
}

static void summarize_records(const struct hfior_record *records, size_t count,
                              uint64_t *newest, uint64_t *newest_sequence,
                              uint64_t *oldest, uint64_t *button_count) {
    for (size_t i = 0; i < count; ++i) {
        if (records[i].timestamp_ns > *newest ||
            (records[i].timestamp_ns == *newest &&
             records[i].sequence > *newest_sequence)) {
            *newest = records[i].timestamp_ns;
            *newest_sequence = records[i].sequence;
        }
        if (records[i].timestamp_ns < *oldest)
            *oldest = records[i].timestamp_ns;
        if (records[i].flags & HFIOR_RECORD_BUTTON)
            (*button_count)++;
    }
}

static size_t take_pending(struct client *client, uint64_t *newest,
                           uint64_t *newest_sequence, uint64_t *oldest,
                           uint64_t *buttons) {
    size_t count = 0u;
    struct hfior_record *records = queue_take_all(&client->pending, &count);
    summarize_records(records, count, newest, newest_sequence, oldest, buttons);
    free(records);
    return count;
}

static bool ack_during_drain(const struct client *client) {
    return client->config.ack_placement == ACK_CRITICAL;
}

static void maybe_ack_post_use(struct client *client) {
    if (client->config.ack_placement == ACK_POST_USE) {
        pthread_mutex_lock(&client->ring_drain_mutex);
        send_ack_if_needed(client);
        pthread_mutex_unlock(&client->ring_drain_mutex);
    }
}

static void maybe_ack_post_frame(struct client *client) {
    if (client->config.ack_placement == ACK_POST_FRAME) {
        pthread_mutex_lock(&client->ring_drain_mutex);
        send_ack_if_needed(client);
        pthread_mutex_unlock(&client->ring_drain_mutex);
    }
}

static void perform_spin(struct client *client) {
    const uint64_t deadline = hfior_now_ns(CLOCK_MONOTONIC) + client->config.spin_ns;
    while (hfior_now_ns(CLOCK_MONOTONIC) < deadline) {
        (void)atomic_load_explicit(&client->ring->producer.head,
                                   memory_order_acquire);
        client->stats.spin_iterations++;
        __asm__ __volatile__("pause" ::: "memory");
    }
}

static struct frame_observation run_frame_policy(struct client *client,
                                                  uint64_t frame_index,
                                                  uint64_t target_start_ns,
                                                  uint64_t frame_start_ns) {
    struct frame_observation result = {
        .frame_index = frame_index,
        .target_start_ns = target_start_ns,
        .frame_start_ns = frame_start_ns,
        .start_lateness_ns = frame_start_ns > target_start_ns
                                 ? frame_start_ns - target_start_ns : 0u,
        .head_at_frame_start = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire),
        .oldest_timestamp_ns = UINT64_MAX,
    };
    const uint64_t checks_before = client->stats.ring_checks;
    const uint64_t actions_before = client->stats.expensive_actions;
    uint64_t newest = 0u;
    uint64_t newest_sequence = 0u;
    uint64_t oldest = UINT64_MAX;
    uint64_t buttons = 0u;
    size_t total_records = 0u;
    size_t late_records = 0u;

    if (client->config.policy == POLICY_EAGER_THREAD) {
        busy_for_ns(client->config.base_work_ns +
                    client->config.integration_work_ns);
        result.legacy_measurement_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.latch_begin_ns = result.legacy_measurement_ns;
        total_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.use_time_ns = result.latch_end_ns;
        if (total_records)
            client->stats.expensive_actions += total_records;
        /* Shadow-drain the exact same source for preservation validation. */
        (void)drain_ring(client, RECEIPT_FRAME_RING, true, true);
    } else if (client->config.policy == POLICY_HFIOR_CRITICAL_ACK) {
        const uint64_t before = (uint64_t)((double)client->config.base_work_ns *
                                            client->config.input_phase);
        const uint64_t after = client->config.base_work_ns - before;
        busy_for_ns(before);
        result.legacy_measurement_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.latch_begin_ns = result.legacy_measurement_ns;
        (void)drain_ring(client, RECEIPT_FRAME_RING, false,
                         ack_during_drain(client));
        result.head_at_initial_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
        total_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.use_time_ns = result.latch_end_ns;
        if (total_records)
            client->stats.expensive_actions++;
        maybe_ack_post_use(client);
        busy_for_ns(after + client->config.integration_work_ns);
    } else if (client->config.policy == POLICY_HFIOR_LATE ||
               client->config.policy == POLICY_HFIOR_SPIN) {
        busy_for_ns(client->config.base_work_ns);
        if (client->config.policy == POLICY_HFIOR_SPIN)
            perform_spin(client);
        result.legacy_measurement_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.latch_begin_ns = result.legacy_measurement_ns;
        (void)drain_ring(client, RECEIPT_FINAL_RING, false,
                         ack_during_drain(client));
        result.head_at_initial_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
        total_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        if (total_records)
            client->stats.expensive_actions++;
        maybe_ack_post_use(client);
        if (client->config.integration_work_ns)
            busy_for_ns(client->config.integration_work_ns);
        result.use_time_ns = hfior_now_ns(CLOCK_MONOTONIC);
    } else if (client->config.policy == POLICY_HFIOR_MULTI ||
               client->config.policy == POLICY_HFIOR_LATE_LATCH) {
        const uint32_t checks = client->config.checks_per_frame
                                    ? client->config.checks_per_frame : 1u;
        const uint64_t segment = checks ? client->config.base_work_ns / checks : 0u;
        uint64_t spent = 0u;
        uint64_t final_check_begin_ns = frame_start_ns;
        uint64_t final_check_end_ns = frame_start_ns;
        for (uint32_t i = 0; i < checks; ++i) {
            uint64_t work = segment;
            if (i + 1u == checks)
                work = client->config.base_work_ns - spent;
            busy_for_ns(work);
            spent += work;
            const uint64_t check_begin_ns = hfior_now_ns(CLOCK_MONOTONIC);
            (void)drain_ring(client, RECEIPT_FRAME_RING, false, false);
            const uint64_t check_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
            if (i + 1u == checks) {
                final_check_begin_ns = check_begin_ns;
                final_check_end_ns = check_end_ns;
            }
        }
        result.legacy_measurement_ns = final_check_begin_ns;
        result.latch_begin_ns = final_check_begin_ns;
        result.head_at_initial_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
        total_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        (void)final_check_end_ns;
        if (total_records)
            client->stats.expensive_actions++;

        if (client->config.integration_work_ns)
            busy_for_ns(client->config.integration_work_ns);

        if (client->config.policy == POLICY_HFIOR_LATE_LATCH) {
            const uint64_t final_latch_begin_ns = hfior_now_ns(CLOCK_MONOTONIC);
            (void)drain_ring(client, RECEIPT_FINAL_RING, false,
                             ack_during_drain(client));
            late_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
            for (uint32_t recheck = 0; recheck < client->config.final_rechecks; ++recheck) {
                (void)drain_ring(client, RECEIPT_FINAL_RING, false,
                                 recheck + 1u == client->config.final_rechecks &&
                                 ack_during_drain(client));
                const size_t extra = take_pending(client, &newest,
                                                  &newest_sequence, &oldest,
                                                  &buttons);
                late_records += extra;
            }
            result.latch_begin_ns = final_latch_begin_ns;
            result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
            if (late_records) {
                client->stats.late_latch_actions++;
                client->stats.frames_with_late_latch++;
                client->stats.late_latch_records += late_records;
            }
        } else if (ack_during_drain(client)) {
            pthread_mutex_lock(&client->ring_drain_mutex);
            send_ack_if_needed(client);
            pthread_mutex_unlock(&client->ring_drain_mutex);
        }
        result.head_at_final_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
        result.use_time_ns = hfior_now_ns(CLOCK_MONOTONIC);
        maybe_ack_post_use(client);
    } else if (client->config.policy == POLICY_HFIOR_TWO_STAGE) {
        const uint64_t first_work = client->config.base_work_ns / 2u;
        const uint64_t second_work = client->config.base_work_ns - first_work;
        busy_for_ns(first_work);
        result.legacy_measurement_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.latch_begin_ns = result.legacy_measurement_ns;
        (void)drain_ring(client, RECEIPT_FRAME_RING, false, false);
        size_t first_count = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        if (first_count)
            client->stats.expensive_actions++;
        total_records += first_count;
        busy_for_ns(second_work + client->config.integration_work_ns);
        (void)drain_ring(client, RECEIPT_FINAL_RING, false,
                         ack_during_drain(client));
        late_records = take_pending(client, &newest, &newest_sequence, &oldest, &buttons);
        if (late_records)
            client->stats.expensive_actions++;
        result.latch_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        result.head_at_final_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
        result.use_time_ns = result.latch_end_ns;
        maybe_ack_post_use(client);
    }

    if (!result.head_at_initial_latch)
        result.head_at_initial_latch = atomic_load_explicit(
            &client->ring->producer.head, memory_order_acquire);
    if (!result.head_at_final_latch)
        result.head_at_final_latch = result.head_at_initial_latch;
    if (!result.latch_end_ns)
        result.latch_end_ns = result.use_time_ns;
    if (!result.use_time_ns)
        result.use_time_ns = hfior_now_ns(CLOCK_MONOTONIC);

    result.newest_timestamp_ns = newest;
    result.newest_sequence = newest_sequence;
    result.oldest_timestamp_ns = oldest == UINT64_MAX ? 0u : oldest;
    result.records = total_records + late_records;
    result.late_records = late_records;
    if (newest && result.use_time_ns >= newest)
        result.actual_age_ns = result.use_time_ns - newest;
    if (newest && result.legacy_measurement_ns >= newest)
        result.legacy_age_ns = result.legacy_measurement_ns - newest;
    result.latch_cost_ns = result.latch_end_ns >= result.latch_begin_ns
                               ? result.latch_end_ns - result.latch_begin_ns : 0u;
    result.post_latch_to_use_ns = result.use_time_ns >= result.latch_end_ns
                                      ? result.use_time_ns - result.latch_end_ns : 0u;
    result.local_tail_after = client->local_tail;
    result.checks = client->stats.ring_checks - checks_before;
    result.expensive_actions = client->stats.expensive_actions - actions_before;
    result.ring_depth_after = hfior_ring_depth(client->ring);
    client->stats.input_consumptions++;
    if (result.records && newest) {
        hfior_vec_push(&client->sample_ages, result.actual_age_ns);
        hfior_vec_push(&client->legacy_ages, result.legacy_age_ns);
    }
    hfior_vec_push(&client->latch_costs, result.latch_cost_ns);
    hfior_vec_push(&client->start_lateness, result.start_lateness_ns);
    return result;
}

static void write_frame(struct client *client,
                        const struct frame_observation *frame) {
    fprintf(client->frames_csv,
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (unsigned long long)frame->frame_index,
            (unsigned long long)frame->target_start_ns,
            (unsigned long long)frame->frame_start_ns,
            (unsigned long long)frame->start_lateness_ns,
            (unsigned long long)frame->legacy_measurement_ns,
            (unsigned long long)frame->latch_begin_ns,
            (unsigned long long)frame->latch_end_ns,
            (unsigned long long)frame->use_time_ns,
            (unsigned long long)frame->newest_timestamp_ns,
            (unsigned long long)frame->newest_sequence,
            (unsigned long long)frame->oldest_timestamp_ns,
            (unsigned long long)frame->actual_age_ns,
            (unsigned long long)frame->legacy_age_ns,
            (unsigned long long)frame->latch_cost_ns,
            (unsigned long long)frame->post_latch_to_use_ns,
            (unsigned long long)frame->head_at_frame_start,
            (unsigned long long)frame->head_at_initial_latch,
            (unsigned long long)frame->head_at_final_latch,
            (unsigned long long)frame->local_tail_after,
            (unsigned long long)frame->records,
            (unsigned long long)frame->late_records,
            (unsigned long long)frame->checks,
            (unsigned long long)frame->expensive_actions,
            (unsigned long long)frame->ring_depth_after);
    if (frame->actual_age_ns >= client->config.tail_threshold_ns &&
        frame->newest_timestamp_ns) {
        const char *classification = "unknown";
        if (frame->start_lateness_ns > client->config.tail_threshold_ns / 2u)
            classification = "consumer-started-late";
        else if (frame->latch_cost_ns > client->config.tail_threshold_ns / 2u)
            classification = "latch-or-preemption";
        else if (frame->post_latch_to_use_ns > client->config.tail_threshold_ns / 2u)
            classification = "post-latch-work";
        else if (frame->head_at_final_latch > frame->local_tail_after)
            classification = "arrival-after-latch";
        fprintf(client->tail_csv,
                "%llu,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)frame->frame_index,
                (unsigned long long)frame->actual_age_ns,
                classification,
                (unsigned long long)frame->start_lateness_ns,
                (unsigned long long)frame->latch_cost_ns,
                (unsigned long long)frame->post_latch_to_use_ns,
                (unsigned long long)frame->head_at_frame_start,
                (unsigned long long)frame->head_at_initial_latch,
                (unsigned long long)frame->head_at_final_latch,
                (unsigned long long)frame->local_tail_after,
                (unsigned long long)frame->records);
    }
}

static double fps_from_ns(double ns) {
    return ns > 0.0 ? 1e9 / ns : 0.0;
}

static const char *receipt_reason_name(uint32_t reason) {
    switch (reason) {
    case RECEIPT_URGENT: return "urgent";
    case RECEIPT_FRAME_RING: return "frame-ring";
    case RECEIPT_FINAL_RING: return "final-ring";
    case RECEIPT_EAGER_THREAD: return "eager-thread";
    case RECEIPT_SHUTDOWN: return "shutdown";
    default: return "unknown";
    }
}

static void dump_receipts(struct client *client) {
    FILE *records = open_output(client->config.output_dir, "records.csv");
    fprintf(records,
            "receipt_time_ns,reason,sequence,timestamp_ns,dx,dy,wheel,hwheel,buttons,flags\n");
    for (size_t i = 0; i < client->receipts.length; ++i) {
        const struct receipt_entry *entry = &client->receipts.data[i];
        fprintf(records, "%llu,%s,%llu,%llu,%d,%d,%d,%d,%u,%u\n",
                (unsigned long long)entry->receipt_time_ns,
                receipt_reason_name(entry->reason),
                (unsigned long long)entry->record.sequence,
                (unsigned long long)entry->record.timestamp_ns,
                entry->record.dx, entry->record.dy,
                entry->record.wheel, entry->record.hwheel,
                entry->record.buttons, entry->record.flags);
    }
    fclose(records);
}

static void dump_frametimes(struct client *client) {
    FILE *file = open_output(client->config.output_dir, "frametimes.csv");
    fprintf(file, "frame,frametime_ns\n");
    for (size_t i = 0; i < client->frame_times.length; ++i)
        fprintf(file, "%zu,%llu\n", i,
                (unsigned long long)client->frame_times.data[i]);
    fclose(file);
}

static void write_summary(struct client *client) {
    struct rusage usage;
    memset(&usage, 0, sizeof(usage));
    (void)getrusage(RUSAGE_SELF, &usage);
    client->stats.voluntary_context_switches = (uint64_t)usage.ru_nvcsw;
    client->stats.involuntary_context_switches = (uint64_t)usage.ru_nivcsw;

    const double frame_mean = hfior_mean_ns(&client->frame_times);
    const double frame_p50 = hfior_percentile_ns(&client->frame_times, 50.0);
    const double frame_p95 = hfior_percentile_ns(&client->frame_times, 95.0);
    const double frame_p99 = hfior_percentile_ns(&client->frame_times, 99.0);
    const double frame_p999 = hfior_percentile_ns(&client->frame_times, 99.9);
    const double frame_worst_1pct = hfior_worst_fraction_mean_ns(
        &client->frame_times, 0.01);
    const double frame_worst_01pct = hfior_worst_fraction_mean_ns(
        &client->frame_times, 0.001);
    const uint64_t frame_max = hfior_max_ns(&client->frame_times);
    const double age_p50 = hfior_percentile_ns(&client->sample_ages, 50.0);
    const double age_p90 = hfior_percentile_ns(&client->sample_ages, 90.0);
    const double age_p95 = hfior_percentile_ns(&client->sample_ages, 95.0);
    const double age_p99 = hfior_percentile_ns(&client->sample_ages, 99.0);
    const double age_p999 = hfior_percentile_ns(&client->sample_ages, 99.9);
    const uint64_t age_max = hfior_max_ns(&client->sample_ages);
    const double legacy_p50 = hfior_percentile_ns(&client->legacy_ages, 50.0);
    const double legacy_p99 = hfior_percentile_ns(&client->legacy_ages, 99.0);
    const double latch_p50 = hfior_percentile_ns(&client->latch_costs, 50.0);
    const double latch_p99 = hfior_percentile_ns(&client->latch_costs, 99.0);
    const double timer_p50 = hfior_percentile_ns(&client->start_lateness, 50.0);
    const double timer_p99 = hfior_percentile_ns(&client->start_lateness, 99.0);
    const double ingress_p50 = hfior_percentile_ns(&client->eager_ingress_ages, 50.0);
    const double ingress_p99 = hfior_percentile_ns(&client->eager_ingress_ages, 99.0);
    const double button_p50 = hfior_percentile_ns(&client->button_ages, 50.0);
    const double button_p99 = hfior_percentile_ns(&client->button_ages, 99.0);
    const double wall_s = client->stats.finished_ns > client->stats.started_ns
                              ? (double)(client->stats.finished_ns -
                                         client->stats.started_ns) / 1e9 : 0.0;
    const uint64_t producer_drops = atomic_load_explicit(
        &client->ring->producer.dropped_newest, memory_order_relaxed);
    const uint64_t eager_drops = atomic_load_explicit(
        &client->ring->producer.eager_send_drops, memory_order_relaxed);
    const uint64_t planned_cpu_ns =
        (client->config.base_work_ns + client->config.integration_work_ns) *
        client->stats.frames;
    const uint64_t transport_cpu_ns =
        client->stats.consumer_process_cpu_ns > planned_cpu_ns
            ? client->stats.consumer_process_cpu_ns - planned_cpu_ns : 0u;
    const double transport_cpu_percent = wall_s > 0.0
        ? (double)transport_cpu_ns / (wall_s * 1e9) * 100.0 : 0.0;
    const double actions_per_s = wall_s > 0.0
        ? (double)client->stats.expensive_actions / wall_s : 0.0;
    const double context_switches_per_s = wall_s > 0.0
        ? (double)(client->stats.voluntary_context_switches +
                   client->stats.involuntary_context_switches) / wall_s : 0.0;

    FILE *json = open_output(client->config.output_dir, "summary.json");
    fprintf(json,
            "{\n"
            "  \"schema\": 3,\n"
            "  \"evidence_class\": \"%s\",\n"
            "  \"policy\": \"%s\",\n"
            "  \"ack_placement\": \"%s\",\n"
            "  \"device_name\": \"%s\",\n"
            "  \"producer_pid\": %ld,\n"
            "  \"wall_s\": %.9f,\n"
            "  \"frame_hz_target\": %u,\n"
            "  \"checks_per_frame\": %u,\n"
            "  \"final_rechecks\": %u,\n"
            "  \"base_work_ns_per_frame\": %llu,\n"
            "  \"integration_work_ns_per_frame\": %llu,\n"
            "  \"callback_work_ns_per_record\": %llu,\n"
            "  \"spin_ns\": %llu,\n"
            "  \"input_phase\": %.6f,\n"
            "  \"frames\": %llu,\n"
            "  \"overruns\": %llu,\n"
            "  \"average_fps\": %.6f,\n"
            "  \"one_percent_low_fps\": %.6f,\n"
            "  \"zero_point_one_percent_low_fps\": %.6f,\n"
            "  \"frametime_mean_ns\": %.3f,\n"
            "  \"frametime_p50_ns\": %.3f,\n"
            "  \"frametime_p95_ns\": %.3f,\n"
            "  \"frametime_p99_ns\": %.3f,\n"
            "  \"frametime_p99_9_ns\": %.3f,\n"
            "  \"frametime_max_ns\": %llu,\n"
            "  \"newest_sample_age_p50_ns\": %.3f,\n"
            "  \"newest_sample_age_p90_ns\": %.3f,\n"
            "  \"newest_sample_age_p95_ns\": %.3f,\n"
            "  \"newest_sample_age_p99_ns\": %.3f,\n"
            "  \"newest_sample_age_p99_9_ns\": %.3f,\n"
            "  \"newest_sample_age_max_ns\": %llu,\n"
            "  \"legacy_pre_latch_age_p50_ns\": %.3f,\n"
            "  \"legacy_pre_latch_age_p99_ns\": %.3f,\n"
            "  \"latch_cost_p50_ns\": %.3f,\n"
            "  \"latch_cost_p99_ns\": %.3f,\n"
            "  \"frame_start_lateness_p50_ns\": %.3f,\n"
            "  \"frame_start_lateness_p99_ns\": %.3f,\n"
            "  \"eager_ingress_age_p50_ns\": %.3f,\n"
            "  \"eager_ingress_age_p99_ns\": %.3f,\n"
            "  \"button_age_p50_ns\": %.3f,\n"
            "  \"button_age_p99_ns\": %.3f,\n"
            "  \"input_consumptions\": %llu,\n"
            "  \"expensive_actions\": %llu,\n"
            "  \"expensive_actions_per_s\": %.6f,\n"
            "  \"late_latch_actions\": %llu,\n"
            "  \"late_latch_records\": %llu,\n"
            "  \"frames_with_late_latch\": %llu,\n"
            "  \"ring_checks\": %llu,\n"
            "  \"nonempty_ring_checks\": %llu,\n"
            "  \"ring_records\": %llu,\n"
            "  \"shadow_ring_records\": %llu,\n"
            "  \"eager_messages\": %llu,\n"
            "  \"eager_records\": %llu,\n"
            "  \"acknowledgements_sent\": %llu,\n"
            "  \"urgent_reads\": %llu,\n"
            "  \"sequence_gaps\": %llu,\n"
            "  \"duplicate_or_reordered\": %llu,\n"
            "  \"first_sequence\": %llu,\n"
            "  \"last_sequence\": %llu,\n"
            "  \"producer_ring_drops\": %llu,\n"
            "  \"producer_eager_drops\": %llu,\n"
            "  \"planned_work_cpu_ns\": %llu,\n"
            "  \"estimated_transport_cpu_ns\": %llu,\n"
            "  \"estimated_transport_cpu_percent\": %.6f,\n"
            "  \"consumer_process_cpu_ns\": %llu,\n"
            "  \"ingress_thread_cpu_ns\": %llu,\n"
            "  \"voluntary_context_switches\": %llu,\n"
            "  \"involuntary_context_switches\": %llu,\n"
            "  \"context_switches_per_s\": %.6f,\n"
            "  \"generation_changes\": %llu,\n"
            "  \"stall_count\": %llu,\n"
            "  \"spin_iterations\": %llu\n"
            "}\n",
            (client->features & HFIOR_FEAT_SYNTHETIC_SOURCE)
                ? "synthetic-hfior-transport" : "physical-evdev-hfior",
            policy_name(&client->config),
            ack_placement_name(client->config.ack_placement),
            client->ring->metadata.device_name,
            (long)client->producer_pid,
            wall_s,
            client->config.frame_hz,
            client->config.checks_per_frame,
            client->config.final_rechecks,
            (unsigned long long)client->config.base_work_ns,
            (unsigned long long)client->config.integration_work_ns,
            (unsigned long long)client->config.callback_work_ns,
            (unsigned long long)client->config.spin_ns,
            client->config.input_phase,
            (unsigned long long)client->stats.frames,
            (unsigned long long)client->stats.overruns,
            fps_from_ns(frame_mean), fps_from_ns(frame_worst_1pct),
            fps_from_ns(frame_worst_01pct),
            frame_mean, frame_p50, frame_p95, frame_p99, frame_p999,
            (unsigned long long)frame_max,
            age_p50, age_p90, age_p95, age_p99, age_p999,
            (unsigned long long)age_max,
            legacy_p50, legacy_p99, latch_p50, latch_p99,
            timer_p50, timer_p99, ingress_p50, ingress_p99,
            button_p50, button_p99,
            (unsigned long long)client->stats.input_consumptions,
            (unsigned long long)client->stats.expensive_actions,
            actions_per_s,
            (unsigned long long)client->stats.late_latch_actions,
            (unsigned long long)client->stats.late_latch_records,
            (unsigned long long)client->stats.frames_with_late_latch,
            (unsigned long long)client->stats.ring_checks,
            (unsigned long long)client->stats.nonempty_ring_checks,
            (unsigned long long)client->stats.ring_records,
            (unsigned long long)client->stats.shadow_ring_records,
            (unsigned long long)client->stats.eager_messages,
            (unsigned long long)client->stats.eager_records,
            (unsigned long long)client->stats.acknowledgements_sent,
            (unsigned long long)client->stats.urgent_reads,
            (unsigned long long)client->stats.sequence_gaps,
            (unsigned long long)client->stats.duplicate_or_reordered,
            (unsigned long long)client->stats.first_sequence,
            (unsigned long long)client->stats.last_sequence,
            (unsigned long long)producer_drops,
            (unsigned long long)eager_drops,
            (unsigned long long)planned_cpu_ns,
            (unsigned long long)transport_cpu_ns,
            transport_cpu_percent,
            (unsigned long long)client->stats.consumer_process_cpu_ns,
            (unsigned long long)client->stats.ingress_thread_cpu_ns,
            (unsigned long long)client->stats.voluntary_context_switches,
            (unsigned long long)client->stats.involuntary_context_switches,
            context_switches_per_s,
            (unsigned long long)client->stats.generation_changes,
            (unsigned long long)client->stats.stall_count,
            (unsigned long long)client->stats.spin_iterations);
    fclose(json);
}

int main(int argc, char **argv) {
    char default_socket[HFIOR_MAX_SOCKET_PATH + 1u];
    if (hfior_make_runtime_socket_path(default_socket, sizeof(default_socket),
                                       "hfior.sock") != 0)
        die("runtime socket path");
    struct client client = {
        .config = {
            .socket_path = default_socket,
            .policy = 0,
            .ack_placement = ACK_POST_FRAME,
            .checks_per_frame = 1u,
            .frame_hz = 240u,
            .duration_s = 5.0,
            .input_phase = 0.5,
            .base_work_ns = UINT64_C(1800000),
            .tail_threshold_ns = UINT64_C(500000),
            .consumer_cpu = -1,
            .ingress_cpu = -1,
        },
        .socket_fd = -1,
        .memfd = -1,
        .urgent_fd = -1,
    };

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            client.config.socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--output-dir") && i + 1 < argc) {
            client.config.output_dir = argv[++i];
        } else if (!strcmp(argv[i], "--policy") && i + 1 < argc) {
            client.config.policy = parse_policy(argv[++i],
                                                &client.config.checks_per_frame);
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            client.config.duration_s = parse_double(argv[++i], "duration");
        } else if (!strcmp(argv[i], "--frame-hz") && i + 1 < argc) {
            client.config.frame_hz = parse_u32(argv[++i], "frame rate");
        } else if (!strcmp(argv[i], "--input-phase") && i + 1 < argc) {
            client.config.input_phase = parse_double(argv[++i], "input phase");
        } else if (!strcmp(argv[i], "--base-work-us") && i + 1 < argc) {
            client.config.base_work_ns =
                (uint64_t)(parse_double(argv[++i], "base work") * 1000.0);
        } else if (!strcmp(argv[i], "--integration-work-us") && i + 1 < argc) {
            client.config.integration_work_ns =
                (uint64_t)(parse_double(argv[++i], "integration work") * 1000.0);
        } else if (!strcmp(argv[i], "--callback-work-ns") && i + 1 < argc) {
            client.config.callback_work_ns =
                (uint64_t)parse_double(argv[++i], "callback work");
        } else if (!strcmp(argv[i], "--checks-per-frame") && i + 1 < argc) {
            client.config.checks_per_frame = parse_u32(argv[++i], "checks");
        } else if (!strcmp(argv[i], "--final-rechecks") && i + 1 < argc) {
            client.config.final_rechecks = parse_u32(argv[++i], "final rechecks");
        } else if (!strcmp(argv[i], "--ack-placement") && i + 1 < argc) {
            client.config.ack_placement = parse_ack_placement(argv[++i]);
        } else if (!strcmp(argv[i], "--spin-us") && i + 1 < argc) {
            client.config.spin_ns =
                (uint64_t)(parse_double(argv[++i], "spin") * 1000.0);
        } else if (!strcmp(argv[i], "--tail-threshold-us") && i + 1 < argc) {
            client.config.tail_threshold_ns =
                (uint64_t)(parse_double(argv[++i], "tail threshold") * 1000.0);
        } else if (!strcmp(argv[i], "--consumer-cpu") && i + 1 < argc) {
            client.config.consumer_cpu = parse_i32(argv[++i], "consumer cpu");
        } else if (!strcmp(argv[i], "--ingress-cpu") && i + 1 < argc) {
            client.config.ingress_cpu = parse_i32(argv[++i], "ingress cpu");
        } else if (!strcmp(argv[i], "--stall-after-ms") && i + 1 < argc) {
            client.config.stall_after_ms = parse_u32(argv[++i], "stall after");
        } else if (!strcmp(argv[i], "--stall-ms") && i + 1 < argc) {
            client.config.stall_ms = parse_u32(argv[++i], "stall duration");
        } else if (!strcmp(argv[i], "--record-trace")) {
            client.config.record_trace = true;
        } else if (!strcmp(argv[i], "--uncapped")) {
            client.config.uncapped = true;
        } else if (!strcmp(argv[i], "--quiet")) {
            client.config.quiet = true;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!client.config.policy || !client.config.output_dir ||
        !client.config.frame_hz || client.config.input_phase > 1.0 ||
        client.config.duration_s <= 0.0 ||
        client.config.checks_per_frame > 64u) {
        usage(argv[0]);
        return 2;
    }
    if (client.config.policy == POLICY_HFIOR_SPIN && !client.config.spin_ns)
        client.config.spin_ns = UINT64_C(20000);
    /* Stable-latch names are distinguished by their explicit default. */
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "hfior-stable-latch") ||
            !strcmp(argv[i], "hfior-8-stable-latch")) {
            if (!client.config.final_rechecks)
                client.config.final_rechecks = 2u;
        }
    }

    mkdir_p(client.config.output_dir);
    queue_init(&client.pending);
    if (pthread_mutex_init(&client.ring_drain_mutex, NULL) != 0)
        die("ring mutex");
    client.frames_csv = open_output(client.config.output_dir, "frames.csv");
    client.tail_csv = open_output(client.config.output_dir, "tail-events.csv");
    fprintf(client.frames_csv,
            "frame,target_start_ns,frame_start_ns,start_lateness_ns,legacy_measurement_ns,latch_begin_ns,latch_end_ns,use_time_ns,newest_timestamp_ns,newest_sequence,oldest_timestamp_ns,actual_age_ns,legacy_age_ns,latch_cost_ns,post_latch_to_use_ns,head_at_frame_start,head_at_initial_latch,head_at_final_latch,local_tail_after,records,late_records,checks,expensive_actions,ring_depth_after\n");
    fprintf(client.tail_csv,
            "frame,actual_age_ns,classification,start_lateness_ns,latch_cost_ns,post_latch_to_use_ns,head_at_frame_start,head_at_initial_latch,head_at_final_latch,local_tail_after,records\n");

    client.socket_fd = connect_socket(client.config.socket_path);
    if (client.socket_fd < 0)
        die("connect HFIOR bridge");
    struct ucred peer = {0};
    socklen_t peer_len = sizeof(peer);
    if (getsockopt(client.socket_fd, SOL_SOCKET, SO_PEERCRED,
                   &peer, &peer_len) != 0)
        die("SO_PEERCRED");
    if (peer.uid != getuid() && getuid() != 0u) {
        errno = EACCES;
        die("bridge UID mismatch");
    }
    receive_hello(&client);
    send_mode(&client);
    synchronize_start(&client);
    pin_current_thread(client.config.consumer_cpu);
    start_ingress_thread(&client);

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    const uint64_t process_cpu_start = hfior_now_ns(CLOCK_PROCESS_CPUTIME_ID);
    client.stats.started_ns = hfior_now_ns(CLOCK_MONOTONIC);
    const uint64_t finish_ns = client.stats.started_ns +
        (uint64_t)(client.config.duration_s * 1e9);
    const uint64_t frame_period_ns = UINT64_C(1000000000) /
                                     client.config.frame_hz;
    uint64_t target_start_ns = client.stats.started_ns;
    bool stalled = false;

    while (!signal_stop &&
           !atomic_load_explicit(&client.terminal_seen, memory_order_relaxed)) {
        const uint64_t now = hfior_now_ns(CLOCK_MONOTONIC);
        if (now >= finish_ns)
            break;
        if (!client.config.uncapped && now < target_start_ns)
            (void)hfior_sleep_until_ns(CLOCK_MONOTONIC, target_start_ns);
        const uint64_t frame_start_ns = hfior_now_ns(CLOCK_MONOTONIC);

        if (!stalled && client.config.stall_ms &&
            frame_start_ns - client.stats.started_ns >=
                (uint64_t)client.config.stall_after_ms * UINT64_C(1000000)) {
            struct timespec stall = {
                .tv_sec = client.config.stall_ms / 1000u,
                .tv_nsec = (long)(client.config.stall_ms % 1000u) * 1000000L,
            };
            nanosleep(&stall, NULL);
            stalled = true;
            client.stats.stall_count++;
        }

        struct frame_observation observation = run_frame_policy(
            &client, client.stats.frames, target_start_ns, frame_start_ns);
        maybe_ack_post_frame(&client);

        if (!client.config.uncapped) {
            const uint64_t target_end_ns = target_start_ns + frame_period_ns;
            const uint64_t before_sleep = hfior_now_ns(CLOCK_MONOTONIC);
            if (before_sleep < target_end_ns)
                (void)hfior_sleep_until_ns(CLOCK_MONOTONIC, target_end_ns);
            else
                client.stats.overruns++;
            target_start_ns = target_end_ns;
        }
        const uint64_t frame_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
        hfior_vec_push(&client.frame_times, frame_end_ns - frame_start_ns);
        write_frame(&client, &observation);
        client.stats.frames++;
    }

    send_stop(&client);
    const uint64_t shutdown_deadline = hfior_now_ns(CLOCK_MONOTONIC) +
                                       UINT64_C(100000000);
    while (!atomic_load_explicit(&client.terminal_seen, memory_order_relaxed) &&
           hfior_now_ns(CLOCK_MONOTONIC) < shutdown_deadline) {
        if (client.config.policy == POLICY_EAGER_THREAD) {
            (void)drain_control_socket_nonblocking(&client, true);
            (void)drain_ring(&client, RECEIPT_SHUTDOWN, true, true);
        } else {
            (void)drain_ring(&client, RECEIPT_SHUTDOWN, false, true);
        }
        struct timespec delay = {.tv_nsec = 1000000L};
        nanosleep(&delay, NULL);
    }
    atomic_store_explicit(&client.stop, true, memory_order_relaxed);
    if (client.urgent_fd >= 0) {
        const uint64_t one = 1u;
        (void)write(client.urgent_fd, &one, sizeof(one));
    }
    if (client.config.policy == POLICY_EAGER_THREAD)
        (void)shutdown(client.socket_fd, SHUT_RD);
    if (client.ingress_started)
        pthread_join(client.ingress_thread, NULL);
    if (client.config.policy == POLICY_EAGER_THREAD) {
        (void)drain_control_socket_nonblocking(&client, true);
        (void)drain_ring(&client, RECEIPT_SHUTDOWN, true, true);
    } else {
        (void)drain_ring(&client, RECEIPT_SHUTDOWN, false, true);
    }

    client.stats.finished_ns = hfior_now_ns(CLOCK_MONOTONIC);
    client.stats.consumer_process_cpu_ns =
        hfior_now_ns(CLOCK_PROCESS_CPUTIME_ID) - process_cpu_start;

    fclose(client.frames_csv);
    fclose(client.tail_csv);
    dump_receipts(&client);
    dump_frametimes(&client);
    write_summary(&client);

    if (!client.config.quiet) {
        fprintf(stderr,
                "policy=%s frames=%llu records=%llu eager=%llu actions=%llu gaps=%llu drops=%llu\n",
                policy_name(&client.config),
                (unsigned long long)client.stats.frames,
                (unsigned long long)client.stats.ring_records,
                (unsigned long long)client.stats.eager_records,
                (unsigned long long)client.stats.expensive_actions,
                (unsigned long long)client.stats.sequence_gaps,
                (unsigned long long)atomic_load_explicit(
                    &client.ring->producer.dropped_newest,
                    memory_order_relaxed));
    }

    if (client.ring && client.ring != MAP_FAILED)
        munmap(client.ring, client.mapping_size);
    if (client.memfd >= 0) close(client.memfd);
    if (client.urgent_fd >= 0) close(client.urgent_fd);
    if (client.socket_fd >= 0) close(client.socket_fd);
    pthread_mutex_destroy(&client.ring_drain_mutex);
    queue_destroy(&client.pending);
    hfior_vec_free(&client.frame_times);
    hfior_vec_free(&client.sample_ages);
    hfior_vec_free(&client.legacy_ages);
    hfior_vec_free(&client.latch_costs);
    hfior_vec_free(&client.start_lateness);
    hfior_vec_free(&client.button_ages);
    hfior_vec_free(&client.eager_ingress_ages);
    free(client.receipts.data);
    return 0;
}
