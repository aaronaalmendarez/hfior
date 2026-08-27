#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct bridge_config {
    const char *device_path;
    const char *socket_path;
    const char *stats_path;
    const char *raw_log_path;
    const char *trace_path;
    uint32_t capacity;
    uint32_t synthetic_rate_hz;
    uint32_t synthetic_button_every;
    uint32_t reconnect_ms;
    double duration_s;
    int socket_send_buffer;
    bool synthetic_publish_timestamps;
    bool quiet;
};

struct bridge_stats {
    uint64_t started_ns;
    uint64_t finished_ns;
    uint64_t input_events;
    uint64_t syn_reports;
    uint64_t records_attempted;
    uint64_t records_published;
    uint64_t ring_drops;
    uint64_t eager_packets_sent;
    uint64_t eager_send_drops;
    uint64_t urgent_writes;
    uint64_t button_transitions;
    uint64_t device_disconnects;
    uint64_t device_reconnects;
    uint64_t control_messages;
    uint64_t malformed_messages;
    uint64_t acknowledgements;
    uint64_t invalid_acknowledgements;
};

struct publication_trace_entry {
    uint64_t sequence;
    uint64_t source_timestamp_ns;
    uint64_t publish_begin_ns;
    uint64_t publish_end_ns;
    uint64_t head_after;
    uint32_t published;
    uint32_t reserved;
};

struct publication_trace_vec {
    struct publication_trace_entry *data;
    size_t length;
    size_t capacity;
};

struct frame_accumulator {
    int64_t dx;
    int64_t dy;
    int64_t wheel;
    int64_t hwheel;
    uint16_t buttons;
    uint16_t flags;
    bool wheel_hires_seen;
    bool hwheel_hires_seen;
    uint64_t timestamp_ns;
};

struct bridge {
    struct bridge_config config;
    struct bridge_stats stats;
    struct hfior_ring_header *ring;
    size_t mapping_size;
    int memfd;
    int readonly_memfd;
    int urgent_fd;
    int listen_fd;
    int client_fd;
    int input_fd;
    int synthetic_timer_fd;
    FILE *raw_log;
    uint32_t mode;
    uint64_t generation;
    uint64_t stop_deadline_ns;
    uid_t authorized_uid;
    pid_t peer_pid;
    struct input_id input_id;
    char device_name[HFIOR_MAX_DEVICE_NAME];
    struct frame_accumulator frame;
    bool source_ready;
    uint64_t synthetic_period_ns;
    uint64_t synthetic_next_timestamp_ns;
    struct publication_trace_vec publication_trace;
};

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void fatal(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static uint32_t parse_u32(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !end || *end || value > UINT32_MAX) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint32_t)value;
}

static double parse_double(const char *text, const char *name) {
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || !end || *end || value < 0.0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return value;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s (--device /dev/input/eventN | --synthetic-rate HZ) [options]\n"
            "  --socket PATH              default: $XDG_RUNTIME_DIR/hfior.sock\n"
            "  --capacity N               power of two, default 65536\n"
            "  --duration SEC             0 means until client stop/signal\n"
            "  --button-every N           synthetic transition interval\n"
            "  --reconnect-ms N           real-device reopen delay, default 250\n"
            "  --socket-send-buffer BYTES eager reference buffer, default 1048576\n"
            "  --stats FILE               write bridge key=value evidence\n"
            "  --raw-log FILE             retain raw evdev records as CSV\n"
            "  --trace FILE               retain producer publication timeline\n"
            "  --synthetic-timestamps scheduled|publish\n"
            "  --quiet\n",
            argv0);
}

static uid_t determine_authorized_uid(void) {
    const char *sudo_uid = getenv("SUDO_UID");
    if (sudo_uid && *sudo_uid) {
        char *end = NULL;
        errno = 0;
        unsigned long value = strtoul(sudo_uid, &end, 10);
        if (!errno && end && !*end && value <= UINT32_MAX)
            return (uid_t)value;
    }
    return getuid();
}

static int create_memfd(const char *name) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    return fd;
}

static int setup_listening_socket(struct bridge *bridge) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(bridge->config.socket_path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s",
             bridge->config.socket_path);
    unlink(address.sun_path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    if (chmod(address.sun_path, 0600) != 0 ||
        chown(address.sun_path, bridge->authorized_uid, (gid_t)-1) != 0) {
        const int saved = errno;
        unlink(address.sun_path);
        close(fd);
        errno = saved;
        return -1;
    }
    if (listen(fd, 1) != 0) {
        const int saved = errno;
        unlink(address.sun_path);
        close(fd);
        errno = saved;
        return -1;
    }
    bridge->listen_fd = fd;
    return 0;
}

static int open_input_device(struct bridge *bridge) {
    if (!bridge->config.device_path)
        return 0;
    int fd = open(bridge->config.device_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int clock_id = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clock_id) != 0) {
        fprintf(stderr, "warning: EVIOCSCLOCKID failed: %s; timestamps may use a different clock\n",
                strerror(errno));
    }
    memset(&bridge->input_id, 0, sizeof(bridge->input_id));
    (void)ioctl(fd, EVIOCGID, &bridge->input_id);
    memset(bridge->device_name, 0, sizeof(bridge->device_name));
    if (ioctl(fd, EVIOCGNAME(sizeof(bridge->device_name)), bridge->device_name) < 0)
        snprintf(bridge->device_name, sizeof(bridge->device_name), "%s",
                 bridge->config.device_path);
    bridge->input_fd = fd;
    bridge->source_ready = true;
    return 0;
}

static int setup_synthetic_source(struct bridge *bridge) {
    bridge->synthetic_timer_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (bridge->synthetic_timer_fd < 0)
        return -1;
    memset(&bridge->input_id, 0, sizeof(bridge->input_id));
    snprintf(bridge->device_name, sizeof(bridge->device_name),
             "synthetic-%uHz", bridge->config.synthetic_rate_hz);
    bridge->source_ready = false;
    return 0;
}

static int arm_synthetic_source(struct bridge *bridge) {
    const uint64_t period_ns =
        UINT64_C(1000000000) / bridge->config.synthetic_rate_hz;
    const uint64_t first_ns = hfior_now_ns(CLOCK_MONOTONIC) + UINT64_C(2000000);
    bridge->synthetic_period_ns = period_ns;
    bridge->synthetic_next_timestamp_ns = first_ns;
    const struct itimerspec timer = {
        .it_interval = {
            .tv_sec = (time_t)(period_ns / UINT64_C(1000000000)),
            .tv_nsec = (long)(period_ns % UINT64_C(1000000000)),
        },
        .it_value = {
            .tv_sec = (time_t)(first_ns / UINT64_C(1000000000)),
            .tv_nsec = (long)(first_ns % UINT64_C(1000000000)),
        },
    };
    if (timerfd_settime(bridge->synthetic_timer_fd, TFD_TIMER_ABSTIME,
                        &timer, NULL) != 0)
        return -1;
    bridge->source_ready = true;
    return 0;
}

static int reopen_memfd_read_only(int memfd) {
    char path[64];
    const int length = snprintf(path, sizeof(path), "/proc/self/fd/%d", memfd);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return open(path, O_RDONLY | O_CLOEXEC);
}

static uint64_t timeval_to_ns(const struct timeval *time) {
    if (time->tv_sec < 0 || time->tv_usec < 0)
        return 0;
    return (uint64_t)time->tv_sec * UINT64_C(1000000000) +
           (uint64_t)time->tv_usec * UINT64_C(1000);
}

static void write_urgent(struct bridge *bridge) {
    uint64_t one = 1u;
    ssize_t written;
    do {
        written = write(bridge->urgent_fd, &one, sizeof(one));
    } while (written < 0 && errno == EINTR);
    if (written == (ssize_t)sizeof(one)) {
        bridge->stats.urgent_writes++;
        atomic_fetch_add_explicit(&bridge->ring->producer.urgent_writes, 1u,
                                  memory_order_relaxed);
    } else if (written < 0 && errno != EAGAIN) {
        fprintf(stderr, "warning: urgent eventfd write failed: %s\n",
                strerror(errno));
    }
}

static void send_device_state(struct bridge *bridge, uint32_t flags) {
    if (bridge->client_fd < 0)
        return;
    const struct hfior_device_state_message message = {
        .header = {HFIOR_MSG_DEVICE_STATE, sizeof(message)},
        .generation = bridge->generation,
        .state_flags = flags,
    };
    if (hfior_send_fds(bridge->client_fd, &message, sizeof(message), NULL, 0u,
                       MSG_DONTWAIT) != 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
        fprintf(stderr, "warning: device-state send failed: %s\n",
                strerror(errno));
    }
    write_urgent(bridge);
}

static void send_eager_record(struct bridge *bridge,
                              const struct hfior_record *record) {
    if (bridge->client_fd < 0 || bridge->mode == HFIOR_MODE_HFIOR)
        return;
    const struct hfior_eager_record_message message = {
        .header = {HFIOR_MSG_EAGER_RECORD, sizeof(message)},
        .generation = bridge->generation,
        .record = *record,
    };
    if (hfior_send_fds(bridge->client_fd, &message, sizeof(message), NULL, 0u,
                       MSG_DONTWAIT) == 0) {
        bridge->stats.eager_packets_sent++;
        atomic_fetch_add_explicit(&bridge->ring->stats.eager_packets_sent, 1u,
                                  memory_order_relaxed);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
        bridge->stats.eager_send_drops++;
        atomic_fetch_add_explicit(&bridge->ring->producer.eager_send_drops, 1u,
                                  memory_order_relaxed);
        write_urgent(bridge);
    } else if (errno == EPIPE || errno == ECONNRESET) {
        stop_requested = 1;
    } else {
        fprintf(stderr, "warning: eager record send failed: %s\n",
                strerror(errno));
    }
}

static void account_record_flags(struct bridge *bridge,
                                 const struct hfior_record *record) {
    if (record->flags & HFIOR_RECORD_BUTTON)
        bridge->stats.button_transitions++;
}

static void trace_publication(struct bridge *bridge,
                              const struct publication_trace_entry *entry) {
    if (!bridge->config.trace_path)
        return;
    if (bridge->publication_trace.length == bridge->publication_trace.capacity) {
        size_t capacity = bridge->publication_trace.capacity
                              ? bridge->publication_trace.capacity * 2u : 65536u;
        void *data = realloc(bridge->publication_trace.data,
                             capacity * sizeof(*bridge->publication_trace.data));
        if (!data)
            fatal("publication trace realloc");
        bridge->publication_trace.data = data;
        bridge->publication_trace.capacity = capacity;
    }
    bridge->publication_trace.data[bridge->publication_trace.length++] = *entry;
}

static void dump_publication_trace(const struct bridge *bridge) {
    if (!bridge->config.trace_path)
        return;
    FILE *file = fopen(bridge->config.trace_path, "w");
    if (!file) {
        fprintf(stderr, "warning: cannot write publication trace %s: %s\n",
                bridge->config.trace_path, strerror(errno));
        return;
    }
    setvbuf(file, NULL, _IOFBF, 1u << 20);
    fprintf(file, "sequence,source_timestamp_ns,publish_begin_ns,publish_end_ns,head_after,published\n");
    for (size_t i = 0; i < bridge->publication_trace.length; ++i) {
        const struct publication_trace_entry *entry =
            &bridge->publication_trace.data[i];
        fprintf(file, "%llu,%llu,%llu,%llu,%llu,%u\n",
                (unsigned long long)entry->sequence,
                (unsigned long long)entry->source_timestamp_ns,
                (unsigned long long)entry->publish_begin_ns,
                (unsigned long long)entry->publish_end_ns,
                (unsigned long long)entry->head_after, entry->published);
    }
    fclose(file);
}

static void publish_record(struct bridge *bridge, struct hfior_record *record) {
    bridge->stats.records_attempted++;
    const uint64_t publish_begin_ns = hfior_now_ns(CLOCK_MONOTONIC);
    record->sequence = atomic_fetch_add_explicit(
        &bridge->ring->producer.next_sequence, 1u, memory_order_relaxed);
    atomic_store_explicit(&bridge->ring->producer.heartbeat_ns,
                          publish_begin_ns, memory_order_relaxed);
    const bool published = hfior_ring_publish(bridge->ring, record);
    const uint64_t publish_end_ns = hfior_now_ns(CLOCK_MONOTONIC);
    const uint64_t head_after = atomic_load_explicit(
        &bridge->ring->producer.head, memory_order_acquire);
    const struct publication_trace_entry trace = {
        .sequence = record->sequence,
        .source_timestamp_ns = record->timestamp_ns,
        .publish_begin_ns = publish_begin_ns,
        .publish_end_ns = publish_end_ns,
        .head_after = head_after,
        .published = published ? 1u : 0u,
    };
    trace_publication(bridge, &trace);
    if (published) {
        bridge->stats.records_published++;
        account_record_flags(bridge, record);
        send_eager_record(bridge, record);
        if (record->flags & (HFIOR_RECORD_BUTTON | HFIOR_RECORD_SYN_DROPPED |
                             HFIOR_RECORD_DEVICE_GONE | HFIOR_RECORD_OVERFLOW))
            write_urgent(bridge);
    } else {
        bridge->stats.ring_drops++;
        write_urgent(bridge);
    }
}

static int64_t clamp_i64(int64_t value, int64_t minimum, int64_t maximum) {
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static void complete_frame(struct bridge *bridge, uint64_t timestamp_ns) {
    struct hfior_record record = {
        .timestamp_ns = timestamp_ns ? timestamp_ns : hfior_now_ns(CLOCK_MONOTONIC),
        .dx = (int32_t)clamp_i64(bridge->frame.dx, INT32_MIN, INT32_MAX),
        .dy = (int32_t)clamp_i64(bridge->frame.dy, INT32_MIN, INT32_MAX),
        .wheel = (int16_t)clamp_i64(bridge->frame.wheel, INT16_MIN, INT16_MAX),
        .hwheel = (int16_t)clamp_i64(bridge->frame.hwheel, INT16_MIN, INT16_MAX),
        .buttons = bridge->frame.buttons,
        .flags = bridge->frame.flags,
    };
    if (record.dx || record.dy)
        record.flags |= HFIOR_RECORD_MOTION;
    if (record.wheel || record.hwheel)
        record.flags |= HFIOR_RECORD_WHEEL;
    publish_record(bridge, &record);
    bridge->stats.syn_reports++;
    const uint16_t buttons = bridge->frame.buttons;
    memset(&bridge->frame, 0, sizeof(bridge->frame));
    bridge->frame.buttons = buttons;
}

static void raw_log_event(struct bridge *bridge, const struct input_event *event) {
    if (!bridge->raw_log)
        return;
    fprintf(bridge->raw_log, "%llu,%u,%u,%d\n",
            (unsigned long long)timeval_to_ns(&event->time),
            event->type, event->code, event->value);
}

static void process_input_event(struct bridge *bridge,
                                const struct input_event *event) {
    bridge->stats.input_events++;
    raw_log_event(bridge, event);
    const uint64_t timestamp_ns = timeval_to_ns(&event->time);
    if (timestamp_ns)
        bridge->frame.timestamp_ns = timestamp_ns;
    if (event->type == EV_REL) {
        switch (event->code) {
        case REL_X:
            bridge->frame.dx += event->value;
            break;
        case REL_Y:
            bridge->frame.dy += event->value;
            break;
        case REL_WHEEL:
            if (!bridge->frame.wheel_hires_seen)
                bridge->frame.wheel += event->value;
            break;
#ifdef REL_WHEEL_HI_RES
        case REL_WHEEL_HI_RES:
            if (!bridge->frame.wheel_hires_seen)
                bridge->frame.wheel = 0;
            bridge->frame.wheel_hires_seen = true;
            bridge->frame.wheel += event->value;
            break;
#endif
        case REL_HWHEEL:
            if (!bridge->frame.hwheel_hires_seen)
                bridge->frame.hwheel += event->value;
            break;
#ifdef REL_HWHEEL_HI_RES
        case REL_HWHEEL_HI_RES:
            if (!bridge->frame.hwheel_hires_seen)
                bridge->frame.hwheel = 0;
            bridge->frame.hwheel_hires_seen = true;
            bridge->frame.hwheel += event->value;
            break;
#endif
        default:
            break;
        }
    } else if (event->type == EV_KEY) {
        const uint16_t bit = hfior_button_bit_from_linux_code(event->code);
        if (bit) {
            const uint16_t old_buttons = bridge->frame.buttons;
            if (event->value)
                bridge->frame.buttons |= bit;
            else
                bridge->frame.buttons &= (uint16_t)~bit;
            if (bridge->frame.buttons != old_buttons)
                bridge->frame.flags |= HFIOR_RECORD_BUTTON;
        }
    } else if (event->type == EV_SYN) {
        if (event->code == SYN_DROPPED) {
            bridge->frame.flags |= HFIOR_RECORD_SYN_DROPPED;
        } else if (event->code == SYN_REPORT) {
            complete_frame(bridge, timestamp_ns);
        }
    }
}

static int drain_input_device(struct bridge *bridge) {
    struct input_event events[256];
    for (;;) {
        const ssize_t bytes = read(bridge->input_fd, events, sizeof(events));
        if (bytes > 0) {
            if ((size_t)bytes % sizeof(events[0]) != 0u) {
                errno = EPROTO;
                return -1;
            }
            const size_t count = (size_t)bytes / sizeof(events[0]);
            for (size_t i = 0; i < count; ++i)
                process_input_event(bridge, &events[i]);
            continue;
        }
        if (bytes == 0) {
            errno = ENODEV;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
}

static void emit_synthetic(struct bridge *bridge, uint64_t expirations) {
    static uint64_t synthetic_index = 0u;
    if (expirations > UINT64_C(1000000))
        expirations = UINT64_C(1000000);
    for (uint64_t i = 0; i < expirations; ++i) {
        synthetic_index++;
        bridge->frame.dx = (synthetic_index & 1u) ? 3 : -3;
        bridge->frame.dy = (synthetic_index % 3u) ? -1 : 1;
        if (bridge->config.synthetic_button_every &&
            synthetic_index % bridge->config.synthetic_button_every == 0u) {
            bridge->frame.buttons ^= HFIOR_BUTTON_LEFT;
            bridge->frame.flags |= HFIOR_RECORD_BUTTON;
        }
        uint64_t timestamp_ns;
        if (bridge->config.synthetic_publish_timestamps) {
            timestamp_ns = hfior_now_ns(CLOCK_MONOTONIC);
        } else {
            timestamp_ns = bridge->synthetic_next_timestamp_ns;
        }
        bridge->synthetic_next_timestamp_ns += bridge->synthetic_period_ns;
        complete_frame(bridge, timestamp_ns);
    }
}

static bool key_bit_is_set(const unsigned long *bits, unsigned code) {
    const unsigned bits_per_word = (unsigned)(sizeof(unsigned long) * 8u);
    return (bits[code / bits_per_word] >> (code % bits_per_word)) & 1u;
}

static void refresh_button_state(struct bridge *bridge) {
    enum { WORDS = (KEY_MAX + 1u + sizeof(unsigned long) * 8u - 1u) /
                   (sizeof(unsigned long) * 8u) };
    unsigned long keys[WORDS];
    memset(keys, 0, sizeof(keys));
    if (bridge->input_fd < 0 ||
        ioctl(bridge->input_fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
        return;
    bridge->frame.buttons = 0u;
    const unsigned codes[] = {
        BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
        BTN_FORWARD, BTN_BACK, BTN_TASK,
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        if (key_bit_is_set(keys, codes[i]))
            bridge->frame.buttons |= hfior_button_bit_from_linux_code(
                (uint16_t)codes[i]);
    }
}

static int discard_pending_input(struct bridge *bridge) {
    if (bridge->input_fd < 0)
        return 0;
    struct input_event events[256];
    for (;;) {
        const ssize_t bytes = read(bridge->input_fd, events, sizeof(events));
        if (bytes > 0)
            continue;
        if (bytes == 0) {
            errno = ENODEV;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        return -1;
    }
    memset(&bridge->frame, 0, sizeof(bridge->frame));
    refresh_button_state(bridge);
    return 0;
}

static int wait_for_start_barrier(struct bridge *bridge) {
    bool mode_received = false;
    while (!stop_requested) {
        struct pollfd fd = {.fd = bridge->client_fd,
                            .events = POLLIN | POLLERR | POLLHUP};
        const int ready = poll(&fd, 1u, 100);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!ready)
            continue;
        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECONNRESET;
            return -1;
        }
        uint8_t buffer[256];
        const ssize_t bytes = recv(bridge->client_fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0)
            return -1;
        if ((size_t)bytes < sizeof(struct hfior_message_header)) {
            bridge->stats.malformed_messages++;
            continue;
        }
        const struct hfior_message_header *header =
            (const struct hfior_message_header *)buffer;
        if (header->size != (uint32_t)bytes) {
            bridge->stats.malformed_messages++;
            continue;
        }
        bridge->stats.control_messages++;
        if (header->type == HFIOR_MSG_SET_MODE &&
            (size_t)bytes == sizeof(struct hfior_set_mode_message)) {
            const struct hfior_set_mode_message *set_mode =
                (const struct hfior_set_mode_message *)buffer;
            if (set_mode->mode < HFIOR_MODE_HFIOR ||
                set_mode->mode > HFIOR_MODE_EAGER_FRAME) {
                errno = EPROTO;
                return -1;
            }
            bridge->mode = set_mode->mode;
            mode_received = true;
        } else if (header->type == HFIOR_MSG_START) {
            if (!mode_received) {
                errno = EPROTO;
                return -1;
            }
            if (bridge->config.synthetic_rate_hz) {
                if (arm_synthetic_source(bridge) != 0)
                    return -1;
            } else if (discard_pending_input(bridge) != 0) {
                return -1;
            }
            bridge->stats.started_ns = hfior_now_ns(CLOCK_MONOTONIC);
            if (bridge->config.duration_s > 0.0)
                bridge->stop_deadline_ns = bridge->stats.started_ns +
                    (uint64_t)(bridge->config.duration_s * 1e9);
            const struct hfior_message_header started = {
                HFIOR_MSG_STARTED, sizeof(started)
            };
            if (send(bridge->client_fd, &started, sizeof(started),
                     MSG_NOSIGNAL) != (ssize_t)sizeof(started))
                return -1;
            return 0;
        } else if (header->type == HFIOR_MSG_STOP) {
            stop_requested = 1;
            return 0;
        }
    }
    return 0;
}

static int handle_control_message(struct bridge *bridge) {
    uint8_t buffer[256];
    const ssize_t bytes = recv(bridge->client_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (bytes == 0) {
        stop_requested = 1;
        return 0;
    }
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }
    bridge->stats.control_messages++;
    if ((size_t)bytes < sizeof(struct hfior_message_header)) {
        bridge->stats.malformed_messages++;
        return 0;
    }
    const struct hfior_message_header *header =
        (const struct hfior_message_header *)buffer;
    if (header->size != (uint32_t)bytes) {
        bridge->stats.malformed_messages++;
        return 0;
    }
    if (header->type == HFIOR_MSG_SET_MODE &&
        (size_t)bytes == sizeof(struct hfior_set_mode_message)) {
        const struct hfior_set_mode_message *set_mode =
            (const struct hfior_set_mode_message *)buffer;
        if (set_mode->mode >= HFIOR_MODE_HFIOR &&
            set_mode->mode <= HFIOR_MODE_EAGER_FRAME) {
            bridge->mode = set_mode->mode;
            atomic_store_explicit(&bridge->ring->consumer.deadline_ns,
                                  set_mode->requested_deadline_ns,
                                  memory_order_relaxed);
        }
    } else if (header->type == HFIOR_MSG_ACK &&
               (size_t)bytes == sizeof(struct hfior_ack_message)) {
        const struct hfior_ack_message *ack =
            (const struct hfior_ack_message *)buffer;
        const uint64_t head = atomic_load_explicit(
            &bridge->ring->producer.head, memory_order_acquire);
        const uint64_t old_tail = atomic_load_explicit(
            &bridge->ring->consumer.tail, memory_order_relaxed);
        if (ack->generation != bridge->generation ||
            ack->consumed_tail < old_tail || ack->consumed_tail > head ||
            head - ack->consumed_tail > bridge->ring->metadata.capacity) {
            bridge->stats.invalid_acknowledgements++;
            write_urgent(bridge);
        } else {
            const uint64_t consumed = ack->consumed_tail - old_tail;
            atomic_store_explicit(&bridge->ring->consumer.tail,
                                  ack->consumed_tail, memory_order_release);
            atomic_fetch_add_explicit(&bridge->ring->consumer.drains, 1u,
                                      memory_order_relaxed);
            atomic_fetch_add_explicit(&bridge->ring->consumer.records_consumed,
                                      consumed, memory_order_relaxed);
            atomic_store_explicit(&bridge->ring->consumer.heartbeat_ns,
                                  hfior_now_ns(CLOCK_MONOTONIC),
                                  memory_order_relaxed);
            bridge->stats.acknowledgements++;
        }
    } else if (header->type == HFIOR_MSG_STOP) {
        stop_requested = 1;
    } else if (header->type == HFIOR_MSG_PING) {
        const struct hfior_message_header pong = {
            HFIOR_MSG_PONG, sizeof(pong)
        };
        (void)send(bridge->client_fd, &pong, sizeof(pong), MSG_NOSIGNAL);
    }
    return 0;
}

static int accept_client_and_send_hello(struct bridge *bridge) {
    bridge->client_fd = accept4(bridge->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (bridge->client_fd < 0)
        return -1;
    if (hfior_verify_same_uid_peer(bridge->client_fd, bridge->authorized_uid,
                                   &bridge->peer_pid) != 0) {
        close(bridge->client_fd);
        bridge->client_fd = -1;
        return -1;
    }
    if (bridge->config.socket_send_buffer > 0)
        (void)setsockopt(bridge->client_fd, SOL_SOCKET, SO_SNDBUF,
                         &bridge->config.socket_send_buffer,
                         sizeof(bridge->config.socket_send_buffer));
    if (hfior_set_nonblocking(bridge->client_fd, true) != 0)
        return -1;
    const struct hfior_hello_message hello = {
        .header = {HFIOR_MSG_HELLO, sizeof(hello)},
        .abi_version = HFIOR_ABI_VERSION,
        .capacity = bridge->config.capacity,
        .generation = bridge->generation,
        .features = bridge->ring->metadata.features,
        .producer_pid = (uint32_t)getpid(),
    };
    const int fds[2] = {bridge->readonly_memfd, bridge->urgent_fd};
    if (hfior_send_fds(bridge->client_fd, &hello, sizeof(hello), fds, 2u, 0) != 0)
        return -1;
    return 0;
}

static void mark_device_gone(struct bridge *bridge) {
    if (bridge->input_fd >= 0) {
        close(bridge->input_fd);
        bridge->input_fd = -1;
    }
    bridge->source_ready = false;
    bridge->stats.device_disconnects++;
    send_device_state(bridge, HFIOR_RECORD_DEVICE_GONE);
}

static int try_reconnect_device(struct bridge *bridge) {
    if (!bridge->config.device_path)
        return 0;
    if (open_input_device(bridge) != 0)
        return -1;
    bridge->generation++;
    atomic_store_explicit(&bridge->ring->producer.generation,
                          bridge->generation, memory_order_release);
    atomic_fetch_add_explicit(&bridge->ring->stats.device_reconnects, 1u,
                              memory_order_relaxed);
    bridge->stats.device_reconnects++;
    send_device_state(bridge, HFIOR_RECORD_DEVICE_READY);
    return 0;
}

static void wait_for_terminal_ack(struct bridge *bridge) {
    const uint64_t deadline = hfior_now_ns(CLOCK_MONOTONIC) + UINT64_C(100000000);
    for (;;) {
        const uint64_t head = atomic_load_explicit(
            &bridge->ring->producer.head, memory_order_acquire);
        const uint64_t tail = atomic_load_explicit(
            &bridge->ring->consumer.tail, memory_order_acquire);
        if (tail >= head || hfior_now_ns(CLOCK_MONOTONIC) >= deadline)
            return;
        struct pollfd fd = {.fd = bridge->client_fd,
                            .events = POLLIN | POLLERR | POLLHUP};
        const int ready = poll(&fd, 1u, 5);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (ready > 0 && (fd.revents & POLLIN))
            (void)handle_control_message(bridge);
        if (ready > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            return;
    }
}

static void write_stats(const struct bridge *bridge) {
    FILE *output = stdout;
    bool close_output = false;
    if (bridge->config.stats_path) {
        FILE *opened = fopen(bridge->config.stats_path, "w");
        if (!opened) {
            fprintf(stderr, "warning: cannot write stats %s: %s\n",
                    bridge->config.stats_path, strerror(errno));
        } else {
            output = opened;
            close_output = true;
        }
    }
    const double wall_s = bridge->stats.finished_ns > bridge->stats.started_ns
                              ? (double)(bridge->stats.finished_ns - bridge->stats.started_ns) / 1e9
                              : 0.0;
    fprintf(output,
            "evidence_class=%s\n"
            "source=%s\n"
            "device_name=%s\n"
            "authorized_uid=%lu\n"
            "peer_pid=%ld\n"
            "mode=%s\n"
            "wall_s=%.9f\n"
            "input_events=%llu\n"
            "syn_reports=%llu\n"
            "records_attempted=%llu\n"
            "records_published=%llu\n"
            "ring_drops=%llu\n"
            "eager_packets_sent=%llu\n"
            "eager_send_drops=%llu\n"
            "urgent_writes=%llu\n"
            "button_transitions=%llu\n"
            "device_disconnects=%llu\n"
            "device_reconnects=%llu\n"
            "control_messages=%llu\n"
            "malformed_messages=%llu\n"
            "acknowledgements=%llu\n"
            "invalid_acknowledgements=%llu\n"
            "generation=%llu\n"
            "ring_head=%llu\n"
            "ring_tail=%llu\n"
            "ring_depth=%llu\n"
            "ring_dropped_counter=%llu\n"
            "eager_drop_counter=%llu\n",
            bridge->config.synthetic_rate_hz ? "synthetic-transport-validation" : "physical-evdev",
            bridge->config.synthetic_rate_hz ? "synthetic" : bridge->config.device_path,
            bridge->device_name,
            (unsigned long)bridge->authorized_uid,
            (long)bridge->peer_pid,
            hfior_mode_name(bridge->mode),
            wall_s,
            (unsigned long long)bridge->stats.input_events,
            (unsigned long long)bridge->stats.syn_reports,
            (unsigned long long)bridge->stats.records_attempted,
            (unsigned long long)bridge->stats.records_published,
            (unsigned long long)bridge->stats.ring_drops,
            (unsigned long long)bridge->stats.eager_packets_sent,
            (unsigned long long)bridge->stats.eager_send_drops,
            (unsigned long long)bridge->stats.urgent_writes,
            (unsigned long long)bridge->stats.button_transitions,
            (unsigned long long)bridge->stats.device_disconnects,
            (unsigned long long)bridge->stats.device_reconnects,
            (unsigned long long)bridge->stats.control_messages,
            (unsigned long long)bridge->stats.malformed_messages,
            (unsigned long long)bridge->stats.acknowledgements,
            (unsigned long long)bridge->stats.invalid_acknowledgements,
            (unsigned long long)bridge->generation,
            (unsigned long long)atomic_load_explicit(&bridge->ring->producer.head, memory_order_acquire),
            (unsigned long long)atomic_load_explicit(&bridge->ring->consumer.tail, memory_order_acquire),
            (unsigned long long)hfior_ring_depth(bridge->ring),
            (unsigned long long)atomic_load_explicit(&bridge->ring->producer.dropped_newest,
                                                     memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&bridge->ring->producer.eager_send_drops,
                                                     memory_order_relaxed));
    if (close_output)
        fclose(output);
}

int main(int argc, char **argv) {
    char default_socket[HFIOR_MAX_SOCKET_PATH + 1u];
    if (hfior_make_runtime_socket_path(default_socket, sizeof(default_socket),
                                       "hfior.sock") != 0)
        fatal("runtime socket path");

    struct bridge bridge = {
        .config = {
            .socket_path = default_socket,
            .capacity = 65536u,
            .reconnect_ms = 250u,
            .socket_send_buffer = 1048576,
        },
        .memfd = -1,
        .readonly_memfd = -1,
        .urgent_fd = -1,
        .listen_fd = -1,
        .client_fd = -1,
        .input_fd = -1,
        .synthetic_timer_fd = -1,
        .mode = HFIOR_MODE_HFIOR,
        .generation = 1u,
    };

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) {
            bridge.config.device_path = argv[++i];
        } else if (!strcmp(argv[i], "--synthetic-rate") && i + 1 < argc) {
            bridge.config.synthetic_rate_hz = parse_u32(argv[++i], "synthetic rate");
        } else if (!strcmp(argv[i], "--button-every") && i + 1 < argc) {
            bridge.config.synthetic_button_every = parse_u32(argv[++i], "button interval");
        } else if (!strcmp(argv[i], "--socket") && i + 1 < argc) {
            bridge.config.socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--capacity") && i + 1 < argc) {
            bridge.config.capacity = parse_u32(argv[++i], "capacity");
        } else if (!strcmp(argv[i], "--duration") && i + 1 < argc) {
            bridge.config.duration_s = parse_double(argv[++i], "duration");
        } else if (!strcmp(argv[i], "--reconnect-ms") && i + 1 < argc) {
            bridge.config.reconnect_ms = parse_u32(argv[++i], "reconnect delay");
        } else if (!strcmp(argv[i], "--socket-send-buffer") && i + 1 < argc) {
            bridge.config.socket_send_buffer = (int)parse_u32(argv[++i], "socket send buffer");
        } else if (!strcmp(argv[i], "--stats") && i + 1 < argc) {
            bridge.config.stats_path = argv[++i];
        } else if (!strcmp(argv[i], "--raw-log") && i + 1 < argc) {
            bridge.config.raw_log_path = argv[++i];
        } else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            bridge.config.trace_path = argv[++i];
        } else if (!strcmp(argv[i], "--synthetic-timestamps") && i + 1 < argc) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "publish"))
                bridge.config.synthetic_publish_timestamps = true;
            else if (!strcmp(mode, "scheduled"))
                bridge.config.synthetic_publish_timestamps = false;
            else {
                fprintf(stderr, "invalid synthetic timestamp mode: %s\n", mode);
                return 2;
            }
        } else if (!strcmp(argv[i], "--quiet")) {
            bridge.config.quiet = true;
        } else if (!strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!!bridge.config.device_path == !!bridge.config.synthetic_rate_hz) {
        fprintf(stderr, "choose exactly one of --device or --synthetic-rate\n");
        return 2;
    }
    if (!hfior_is_power_of_two_u32(bridge.config.capacity) ||
        bridge.config.capacity < 64u) {
        fprintf(stderr, "capacity must be a power of two and at least 64\n");
        return 2;
    }
    if (bridge.config.synthetic_rate_hz > 1000000u) {
        fprintf(stderr, "synthetic rate is unreasonably high\n");
        return 2;
    }

    bridge.authorized_uid = determine_authorized_uid();
    bridge.mapping_size = hfior_mapping_size(bridge.config.capacity);
    bridge.memfd = create_memfd("hfior-ring");
    if (bridge.memfd < 0)
        fatal("memfd_create");
    if (ftruncate(bridge.memfd, (off_t)bridge.mapping_size) != 0)
        fatal("ftruncate memfd");
    bridge.ring = mmap(NULL, bridge.mapping_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, bridge.memfd, 0);
    if (bridge.ring == MAP_FAILED)
        fatal("mmap ring");
    bridge.urgent_fd = eventfd(0u, EFD_CLOEXEC | EFD_NONBLOCK);
    if (bridge.urgent_fd < 0)
        fatal("eventfd");

    if (bridge.config.device_path) {
        if (open_input_device(&bridge) != 0)
            fatal("open input device");
    } else if (setup_synthetic_source(&bridge) != 0) {
        fatal("setup synthetic source");
    }

    uint64_t features = HFIOR_FEAT_MONOTONIC_CLOCK |
                        HFIOR_FEAT_URGENT_EVENTFD |
                        HFIOR_FEAT_EAGER_REFERENCE |
                        HFIOR_FEAT_DEVICE_GENERATION |
                        HFIOR_FEAT_DROP_ACCOUNTING |
                        HFIOR_FEAT_READ_ONLY_RING |
                        HFIOR_FEAT_ACK_PROGRESS |
                        HFIOR_FEAT_START_BARRIER;
    if (bridge.config.device_path)
        features |= HFIOR_FEAT_KERNEL_EVENT_TIME;
    else
        features |= HFIOR_FEAT_SYNTHETIC_SOURCE;
    if (hfior_initialize_mapping(bridge.ring, bridge.mapping_size,
                                 bridge.config.capacity, features,
                                 bridge.device_name,
                                 bridge.input_id.bustype,
                                 bridge.input_id.vendor,
                                 bridge.input_id.product,
                                 bridge.input_id.version,
                                 bridge.generation) != 0)
        fatal("initialize ring");

    /* Prevent size changes while permitting the shared mapping to remain writable. */
    if (fcntl(bridge.memfd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0)
        fatal("seal memfd");
    bridge.readonly_memfd = reopen_memfd_read_only(bridge.memfd);
    if (bridge.readonly_memfd < 0)
        fatal("reopen memfd read-only");

    if (bridge.config.raw_log_path) {
        bridge.raw_log = fopen(bridge.config.raw_log_path, "w");
        if (!bridge.raw_log)
            fatal("open raw log");
        setvbuf(bridge.raw_log, NULL, _IOFBF, 1u << 20);
        fprintf(bridge.raw_log, "timestamp_ns,type,code,value\n");
    }

    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    if (setup_listening_socket(&bridge) != 0)
        fatal("listen socket");
    if (!bridge.config.quiet)
        fprintf(stderr, "HFIOR bridge ready at %s; source=%s\n",
                bridge.config.socket_path,
                bridge.config.device_path ? bridge.config.device_path : bridge.device_name);
    if (accept_client_and_send_hello(&bridge) != 0)
        fatal("accept/hello");
    if (wait_for_start_barrier(&bridge) != 0)
        fatal("start barrier");

    uint64_t next_reconnect_ns = 0u;
    while (!stop_requested) {
        const uint64_t now_ns = hfior_now_ns(CLOCK_MONOTONIC);
        if (bridge.stop_deadline_ns && now_ns >= bridge.stop_deadline_ns)
            break;

        if (bridge.config.device_path && bridge.input_fd < 0 &&
            now_ns >= next_reconnect_ns) {
            if (try_reconnect_device(&bridge) == 0) {
                if (!bridge.config.quiet)
                    fprintf(stderr, "input device reconnected, generation=%llu\n",
                            (unsigned long long)bridge.generation);
            } else {
                next_reconnect_ns = now_ns +
                    (uint64_t)bridge.config.reconnect_ms * UINT64_C(1000000);
            }
        }

        struct pollfd fds[2];
        nfds_t count = 0u;
        const int source_fd = bridge.config.synthetic_rate_hz
                                  ? bridge.synthetic_timer_fd
                                  : bridge.input_fd;
        if (source_fd >= 0)
            fds[count++] = (struct pollfd){.fd = source_fd, .events = POLLIN | POLLERR | POLLHUP};
        fds[count++] = (struct pollfd){.fd = bridge.client_fd, .events = POLLIN | POLLERR | POLLHUP};

        int timeout_ms = 100;
        if (bridge.stop_deadline_ns) {
            const uint64_t remaining = bridge.stop_deadline_ns > now_ns
                                           ? bridge.stop_deadline_ns - now_ns : 0u;
            const int remaining_ms = (int)(remaining / UINT64_C(1000000));
            if (remaining_ms < timeout_ms)
                timeout_ms = remaining_ms > 0 ? remaining_ms : 0;
        }
        const int ready = poll(fds, count, timeout_ms);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            fatal("poll");
        }
        nfds_t index = 0u;
        if (source_fd >= 0) {
            const short revents = fds[index++].revents;
            if (revents & POLLIN) {
                if (bridge.config.synthetic_rate_hz) {
                    uint64_t expirations = 0u;
                    const ssize_t bytes = read(bridge.synthetic_timer_fd,
                                               &expirations, sizeof(expirations));
                    if (bytes == (ssize_t)sizeof(expirations))
                        emit_synthetic(&bridge, expirations);
                    else if (bytes < 0 && errno != EAGAIN && errno != EINTR)
                        fatal("read timerfd");
                } else if (drain_input_device(&bridge) != 0) {
                    if (!bridge.config.quiet)
                        fprintf(stderr, "input device unavailable: %s\n", strerror(errno));
                    mark_device_gone(&bridge);
                    next_reconnect_ns = hfior_now_ns(CLOCK_MONOTONIC) +
                        (uint64_t)bridge.config.reconnect_ms * UINT64_C(1000000);
                }
            }
            if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
                if (bridge.config.device_path && bridge.input_fd >= 0) {
                    mark_device_gone(&bridge);
                    next_reconnect_ns = hfior_now_ns(CLOCK_MONOTONIC) +
                        (uint64_t)bridge.config.reconnect_ms * UINT64_C(1000000);
                } else if (bridge.config.synthetic_rate_hz) {
                    stop_requested = 1;
                }
            }
        }
        const short client_events = fds[index].revents;
        if (client_events & POLLIN) {
            if (handle_control_message(&bridge) != 0) {
                fprintf(stderr, "control socket failed: %s\n", strerror(errno));
                break;
            }
        }
        if (client_events & (POLLERR | POLLHUP | POLLNVAL))
            break;
    }

    /* Publish an explicit terminal marker if capacity remains. */
    struct hfior_record terminal = {
        .timestamp_ns = hfior_now_ns(CLOCK_MONOTONIC),
        .buttons = bridge.frame.buttons,
        .flags = HFIOR_RECORD_TERMINATE,
    };
    publish_record(&bridge, &terminal);
    write_urgent(&bridge);
    wait_for_terminal_ack(&bridge);
    bridge.stats.finished_ns = hfior_now_ns(CLOCK_MONOTONIC);
    write_stats(&bridge);
    dump_publication_trace(&bridge);

    if (bridge.raw_log)
        fclose(bridge.raw_log);
    if (bridge.client_fd >= 0)
        close(bridge.client_fd);
    if (bridge.listen_fd >= 0)
        close(bridge.listen_fd);
    if (bridge.input_fd >= 0)
        close(bridge.input_fd);
    if (bridge.synthetic_timer_fd >= 0)
        close(bridge.synthetic_timer_fd);
    if (bridge.urgent_fd >= 0)
        close(bridge.urgent_fd);
    if (bridge.ring && bridge.ring != MAP_FAILED)
        munmap(bridge.ring, bridge.mapping_size);
    if (bridge.readonly_memfd >= 0)
        close(bridge.readonly_memfd);
    if (bridge.memfd >= 0)
        close(bridge.memfd);
    free(bridge.publication_trace.data);
    unlink(bridge.config.socket_path);
    return 0;
}
