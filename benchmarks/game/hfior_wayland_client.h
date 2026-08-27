#ifndef HFIOR_BENCHMARKS_GAME_WAYLAND_CLIENT_H
#define HFIOR_BENCHMARKS_GAME_WAYLAND_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hfior_game_client;

struct hfior_game_batch {
    double dx;
    double dy;
    uint64_t records;
    uint64_t newest_timestamp_ns;
    uint64_t producer_drops;
    uint64_t sequence_errors;
};

struct hfior_game_motion {
    double dx;
    double dy;
    uint64_t timestamp_ns;
};

struct hfior_game_client *hfior_game_client_create(void);
void hfior_game_client_destroy(struct hfior_game_client *client);

/* The display and surface remain owned by SDL. */
bool hfior_game_client_connect(struct hfior_game_client *client,
                               void *wayland_display,
                               void *wayland_surface);
void hfior_game_client_disconnect(struct hfior_game_client *client);

/* Dispatch lifecycle messages only during the first read in a frame. */
bool hfior_game_client_read(struct hfior_game_client *client,
                            bool dispatch_pending,
                            struct hfior_game_batch *batch,
                            struct hfior_game_motion *motions,
                            size_t motion_capacity);
bool hfior_game_client_acknowledge(struct hfior_game_client *client);

bool hfior_game_client_active(const struct hfior_game_client *client);
const char *hfior_game_client_error(const struct hfior_game_client *client);
const char *hfior_game_client_device(const struct hfior_game_client *client);

#ifdef __cplusplus
}
#endif

#endif
