#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* These protocols deliberately version independently.  Changing one wire
   layout must not silently reinterpret either of the others. */
#define TIMELINE_SCHEMA_VERSION 1u
#define LIVE_PROTOCOL_VERSION 1u
#define TIMELINE_MAGIC 0x52504C32u /* "RPL2" */

enum {
    TIMELINE_FLAG_COMPLETED = 1u << 0,
    TIMELINE_FLAG_CAMERA = 1u << 1,
};

enum {
    TIMELINE_SLOT_AIRBORNE = 1u << 0,
};

enum {
    TIMELINE_STAGE_FOD = 1,
    TIMELINE_STAGE_WHISPY = 2,
    TIMELINE_STAGE_STADIUM = 3,
};

#endif
