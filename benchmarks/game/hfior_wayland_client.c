#define _POSIX_C_SOURCE 200809L

#include "hfior_wayland_client.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client.h>

#include <hfior/abi.h>

#include "hyprland-hf-input-v1-client-protocol.h"

#define HFIOR_FEAT_WL_FIXED_DELTA (UINT64_C(1) << 10)

struct hfior_game_client {
    struct wl_display *display;
    struct wl_event_queue *queue;
    struct wl_registry *registry;
    struct hyprland_hf_input_manager_v1 *manager;
    struct hyprland_hf_input_stream_v1 *stream;
    struct wl_seat *seat;
    struct wl_surface *surface;
    const struct hfior_ring_header *header;
    size_t mapping_size;
    uint32_t capacity;
    uint64_t generation;
    uint64_t local_tail;
    uint64_t acknowledged_tail;
    uint64_t expected_sequence;
    uint64_t sequence_errors;
    int notify_fd;
    bool active;
    bool faulted;
    char error[160];
    char device[HFIOR_MAX_DEVICE_NAME + 1];
};

static void set_error(struct hfior_game_client *client, const char *message) {
    client->active = false;
    client->faulted = true;
    snprintf(client->error, sizeof(client->error), "%s",
             message ? message : "unknown HFIOR error");
}

void hfior_game_client_disconnect(struct hfior_game_client *client) {
    if (!client)
        return;
    if (client->stream)
        hyprland_hf_input_stream_v1_destroy(client->stream);
    if (client->manager)
        hyprland_hf_input_manager_v1_destroy(client->manager);
    if (client->seat)
        wl_seat_destroy(client->seat);
    if (client->registry)
        wl_registry_destroy(client->registry);
    if (client->display)
        (void)wl_display_flush(client->display);
    if (client->header)
        munmap((void *)client->header, client->mapping_size);
    if (client->notify_fd >= 0)
        close(client->notify_fd);
    if (client->queue)
        wl_event_queue_destroy(client->queue);

    client->display = NULL;
    client->queue = NULL;
    client->registry = NULL;
    client->manager = NULL;
    client->stream = NULL;
    client->seat = NULL;
    client->surface = NULL;
    client->header = NULL;
    client->mapping_size = 0;
    client->capacity = 0;
    client->generation = 0;
    client->local_tail = 0;
    client->acknowledged_tail = 0;
    client->expected_sequence = 0;
    client->notify_fd = -1;
    client->active = false;
}

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    struct hfior_game_client *client = data;
    if (strcmp(interface, hyprland_hf_input_manager_v1_interface.name) == 0 &&
        !client->manager) {
        const uint32_t bind_version = version < 1 ? version : 1;
        client->manager = wl_registry_bind(
            registry, name, &hyprland_hf_input_manager_v1_interface,
            bind_version);
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               !client->seat) {
        const uint32_t bind_version = version < 9 ? version : 9;
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                        bind_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void stream_active(void *data,
                          struct hyprland_hf_input_stream_v1 *stream,
                          int32_t ring_fd, int32_t notify_fd,
                          uint32_t mapping_size, uint32_t capacity,
                          uint32_t sample_stride, uint32_t clock_id,
                          uint32_t features, uint32_t generation_hi,
                          uint32_t generation_lo) {
    struct hfior_game_client *client = data;
    (void)stream;

    const bool capacity_valid =
        capacity != 0 && (capacity & (capacity - 1)) == 0;
    const size_t required_size =
        capacity_valid
            ? sizeof(struct hfior_ring_header) +
                  (size_t)capacity * sizeof(struct hfior_record)
            : SIZE_MAX;
    struct stat status = {0};
    if (ring_fd < 0 || notify_fd < 0 || !capacity_valid ||
        sample_stride != sizeof(struct hfior_record) ||
        mapping_size < required_size || fstat(ring_fd, &status) != 0 ||
        status.st_size < 0 || (uint64_t)status.st_size < mapping_size) {
        if (ring_fd >= 0)
            close(ring_fd);
        if (notify_fd >= 0)
            close(notify_fd);
        set_error(client, "compositor supplied invalid HFIOR ring bounds");
        return;
    }

    const struct hfior_ring_header *header =
        mmap(NULL, mapping_size, PROT_READ, MAP_SHARED, ring_fd, 0);
    close(ring_fd);
    if (header == MAP_FAILED) {
        close(notify_fd);
        set_error(client, "could not map the HFIOR ring read-only");
        return;
    }

    const uint64_t required_features =
        HFIOR_FEAT_READ_ONLY_RING | HFIOR_FEAT_ACK_PROGRESS |
        HFIOR_FEAT_WL_FIXED_DELTA;
    const bool valid =
        header->metadata.magic == HFIOR_MAGIC &&
        header->metadata.abi_version == HFIOR_ABI_VERSION &&
        header->metadata.header_size == sizeof(struct hfior_ring_header) &&
        header->metadata.record_size == sizeof(struct hfior_record) &&
        header->metadata.capacity == capacity &&
        (header->metadata.features & required_features) == required_features &&
        (features & HYPRLAND_HF_INPUT_STREAM_V1_FEATURE_HISTORY) != 0 &&
        clock_id == HYPRLAND_HF_INPUT_STREAM_V1_CLOCK_MONOTONIC;
    if (!valid) {
        munmap((void *)header, mapping_size);
        close(notify_fd);
        set_error(client, "compositor and benchmark HFIOR ABIs do not match");
        return;
    }

    client->header = header;
    client->mapping_size = mapping_size;
    client->capacity = capacity;
    client->notify_fd = notify_fd;
    client->generation = ((uint64_t)generation_hi << 32) | generation_lo;
    client->local_tail = atomic_load_explicit(&header->consumer.tail,
                                               memory_order_acquire);
    client->acknowledged_tail = client->local_tail;
    client->expected_sequence = 0;
    client->active = true;
    client->faulted = false;
    client->error[0] = '\0';
    memcpy(client->device, header->metadata.device_name,
           HFIOR_MAX_DEVICE_NAME);
    client->device[HFIOR_MAX_DEVICE_NAME] = '\0';
}

static void stream_notify(void *data,
                          struct hyprland_hf_input_stream_v1 *stream,
                          uint32_t reason, uint32_t latest_head_hi,
                          uint32_t latest_head_lo) {
    struct hfior_game_client *client = data;
    (void)stream;
    (void)latest_head_hi;
    (void)latest_head_lo;
    if (reason == HYPRLAND_HF_INPUT_STREAM_V1_NOTIFY_REASON_OVERFLOW)
        set_error(client, "HFIOR reported explicit ring overflow");
    else if (reason == HYPRLAND_HF_INPUT_STREAM_V1_NOTIFY_REASON_REVOKED)
        set_error(client, "HFIOR authorization was revoked");
}

static void stream_denied(void *data,
                          struct hyprland_hf_input_stream_v1 *stream,
                          uint32_t reason) {
    struct hfior_game_client *client = data;
    (void)stream;
    client->active = false;
    client->faulted = true;
    snprintf(client->error, sizeof(client->error),
             "HFIOR authorization denied (reason %u)", reason);
}

static void stream_revoked(void *data,
                           struct hyprland_hf_input_stream_v1 *stream,
                           uint32_t reason, uint32_t final_head_hi,
                           uint32_t final_head_lo, uint32_t generation_hi,
                           uint32_t generation_lo) {
    struct hfior_game_client *client = data;
    (void)stream;
    (void)reason;
    (void)final_head_hi;
    (void)final_head_lo;
    (void)generation_hi;
    (void)generation_lo;
    set_error(client, "HFIOR stream was revoked");
}

static const struct hyprland_hf_input_stream_v1_listener stream_listener = {
    .active = stream_active,
    .notify = stream_notify,
    .denied = stream_denied,
    .revoked = stream_revoked,
};

struct hfior_game_client *hfior_game_client_create(void) {
    struct hfior_game_client *client = calloc(1, sizeof(*client));
    if (client)
        client->notify_fd = -1;
    return client;
}

void hfior_game_client_destroy(struct hfior_game_client *client) {
    if (!client)
        return;
    hfior_game_client_disconnect(client);
    free(client);
}

bool hfior_game_client_connect(struct hfior_game_client *client,
                               void *wayland_display,
                               void *wayland_surface) {
    if (!client || !wayland_display || !wayland_surface)
        return false;
    hfior_game_client_disconnect(client);
    client->faulted = false;
    client->error[0] = '\0';
    client->display = wayland_display;
    client->surface = wayland_surface;
    client->queue = wl_display_create_queue(client->display);
    if (!client->queue) {
        set_error(client, "could not create a private Wayland event queue");
        return false;
    }

    struct wl_proxy *display_wrapper =
        wl_proxy_create_wrapper(client->display);
    if (!display_wrapper) {
        set_error(client, "could not wrap the SDL Wayland display");
        return false;
    }
    wl_proxy_set_queue(display_wrapper, client->queue);
    client->registry =
        wl_display_get_registry((struct wl_display *)display_wrapper);
    wl_proxy_wrapper_destroy(display_wrapper);
    if (!client->registry) {
        set_error(client, "could not obtain the Wayland registry");
        return false;
    }

    wl_registry_add_listener(client->registry, &registry_listener, client);
    if (wl_display_roundtrip_queue(client->display, client->queue) < 0) {
        set_error(client, "Wayland registry roundtrip failed");
        return false;
    }
    if (!client->manager) {
        set_error(client, "Hyprland does not advertise the HFIOR protocol");
        return false;
    }
    if (!client->seat) {
        set_error(client, "Wayland seat is unavailable");
        return false;
    }

    client->stream = hyprland_hf_input_manager_v1_get_stream(
        client->manager, client->seat, client->surface,
        HYPRLAND_HF_INPUT_STREAM_V1_FEATURE_HISTORY);
    if (!client->stream) {
        set_error(client, "could not allocate an HFIOR stream");
        return false;
    }
    hyprland_hf_input_stream_v1_add_listener(client->stream,
                                              &stream_listener, client);
    if (wl_display_roundtrip_queue(client->display, client->queue) < 0) {
        set_error(client, "HFIOR activation roundtrip failed");
        return false;
    }
    return client->active && !client->faulted;
}

bool hfior_game_client_read(struct hfior_game_client *client,
                            bool dispatch_pending,
                            struct hfior_game_batch *batch,
                            struct hfior_game_motion *motions,
                            size_t motion_capacity) {
    if (!client || !batch)
        return false;
    memset(batch, 0, sizeof(*batch));
    if (!client->active || !client->header)
        return false;
    if (dispatch_pending &&
        wl_display_dispatch_queue_pending(client->display, client->queue) < 0) {
        set_error(client, "Wayland HFIOR dispatch failed");
        return false;
    }
    if (!client->active || client->faulted)
        return false;

    const uint64_t head = atomic_load_explicit(
        &client->header->producer.head, memory_order_acquire);
    const uint64_t drops = atomic_load_explicit(
        &client->header->producer.dropped_newest, memory_order_acquire);
    batch->producer_drops = drops;
    if (drops != 0) {
        set_error(client, "HFIOR producer drop counter is nonzero");
        return false;
    }
    if (head < client->local_tail ||
        head - client->local_tail > client->capacity) {
        set_error(client, "HFIOR ring bounds are invalid");
        return false;
    }
    const uint64_t available = head - client->local_tail;
    if (motions && available > motion_capacity) {
        set_error(client, "benchmark motion buffer is smaller than ring depth");
        return false;
    }

    const struct hfior_record *records =
        hfior_records_const(client->header);
    int64_t dx = 0;
    int64_t dy = 0;
    uint64_t expected = client->expected_sequence;
    uint64_t newest = 0;
    size_t output_index = 0;
    for (uint64_t position = client->local_tail; position < head; ++position) {
        struct hfior_record record = {0};
        memcpy(&record, &records[position & (client->capacity - 1)],
               sizeof(record));
        if (record.sequence == 0 ||
            (expected != 0 && record.sequence != expected)) {
            ++client->sequence_errors;
            batch->sequence_errors = client->sequence_errors;
            set_error(client, "HFIOR sequence gap or reordering detected");
            return false;
        }
        expected = record.sequence + 1;
        dx += record.dx;
        dy += record.dy;
        newest = record.timestamp_ns;
        if (motions) {
            motions[output_index].dx = (double)record.dx / 256.0;
            motions[output_index].dy = (double)record.dy / 256.0;
            motions[output_index].timestamp_ns = record.timestamp_ns;
            ++output_index;
        }
    }

    batch->dx = (double)dx / 256.0;
    batch->dy = (double)dy / 256.0;
    batch->records = available;
    batch->newest_timestamp_ns = newest;
    batch->sequence_errors = client->sequence_errors;
    client->local_tail = head;
    client->expected_sequence = expected;
    return true;
}

bool hfior_game_client_acknowledge(struct hfior_game_client *client) {
    if (!client || !client->active || !client->stream)
        return false;
    if (client->local_tail == client->acknowledged_tail)
        return true;
    hyprland_hf_input_stream_v1_acknowledge(
        client->stream, (uint32_t)(client->local_tail >> 32),
        (uint32_t)client->local_tail, (uint32_t)(client->generation >> 32),
        (uint32_t)client->generation);
    client->acknowledged_tail = client->local_tail;
    if (wl_display_flush(client->display) < 0 && errno != EAGAIN) {
        set_error(client, "Wayland HFIOR acknowledgement failed");
        return false;
    }
    return true;
}

bool hfior_game_client_active(const struct hfior_game_client *client) {
    return client && client->active && !client->faulted;
}

const char *hfior_game_client_error(const struct hfior_game_client *client) {
    return client && client->error[0] ? client->error : "none";
}

const char *hfior_game_client_device(const struct hfior_game_client *client) {
    return client && client->device[0] ? client->device : "unknown";
}
