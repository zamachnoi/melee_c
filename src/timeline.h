#ifndef TIMELINE_H
#define TIMELINE_H

#include <stddef.h>
#include <stdint.h>

#include "parser.h"

typedef struct {
    float x;
    float y;
    float zoom;
} timeline_camera_t;

typedef struct {
    uint8_t *data;
    size_t len;
} timeline_blob_t;

/* Serialize one completed replay into timeline schema 1.  cameras must either
   be NULL or contain exactly end_frame-start_frame+1 samples. */
int timeline_serialize(const slp_replay_t *replay, int32_t start_frame,
                       int32_t end_frame, const timeline_camera_t *cameras,
                       size_t camera_count, timeline_blob_t *out);
void timeline_blob_free(timeline_blob_t *blob);

#endif
