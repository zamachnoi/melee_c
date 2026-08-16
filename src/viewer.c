#define _POSIX_C_SOURCE 200809L

/*
 * SLP debug viewer: software-renders replay frames and serves them over HTTP
 * as raw RGBA for the browser. No native windowing / SDL required.
 *
 * Endpoints:
 *   GET /                     HTML+JS frontend
 *   GET /api/replays          JSON list of .slp files in the data dir
 *   GET /api/set?f=<name>     load <name> as the active replay
 *   POST /api/upload?f=<name> upload an .slp file (raw body) and load it
 *   GET /api/info             JSON about the active replay
 *   GET /api/frame?n=<frame>  binary: [4B BE json_len][json][deflated RGBA]
 *
 * Environment:
 *   PORT   HTTP port (default 8080)
 *   SLP_DIR directory to scan for .slp files (default ./replays)
 *   HOST   bind address (default 0.0.0.0)
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "asset.h"
#include "parser.h"
#include "render.h"

#define FB_W 960
#define FB_H 720
#define FB_BYTES ((size_t)FB_W * FB_H * 4)

#ifndef ASSET_DIR
#define ASSET_DIR "cache"
#endif

typedef struct {
    uint8_t r, g, b, a;
} color_t;

typedef struct {
    double scale;
    double cx, cy; /* world center mapped to screen center */
} cam_t;

typedef struct {
    slp_replay_t replay;
    cam_t cam;
    asset_model_t *models[SLP_SLOT_COUNT];
    asset_anims_t *anims[SLP_SLOT_COUNT];
    asset_stage_t *stage;
    uint8_t *stage_fb;
    uint8_t *stage_sprite;
    int stage_sprite_w, stage_sprite_h;
    double stage_sprite_ppu, stage_sprite_min_x, stage_sprite_max_y;
    cam_t *frame_cams;
    size_t frame_cam_count;
    int32_t start_frame;
    int32_t last_frame;
    char name[256];
} active_t;

typedef struct {
    double clear_ms, stage_ms, items_ms, fighters_ms;
    double json_ms, deflate_ms, pack_ms;
    size_t compressed_bytes;
} frame_profile_t;

static double monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static active_t g_active;
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static _Thread_local uint8_t *g_fb;
static char g_dir[1024] = "./replays";
static char g_web_dir[1024] = "./web";

static const char *asset_dir(void) {
    const char *dir = getenv("ASSET_DIR");
    return dir && dir[0] ? dir : ASSET_DIR;
}

/* ------------------------------------------------------------------ */
/* Pixel helpers                                                       */
/* ------------------------------------------------------------------ */

static inline void plot(int x, int y, color_t c) {
    if (x < 0 || y < 0 || x >= FB_W || y >= FB_H) return;
    uint8_t *p = &g_fb[(y * FB_W + x) * 4];
    p[0] = (uint8_t)((c.r * c.a + p[0] * (255 - c.a)) / 255);
    p[1] = (uint8_t)((c.g * c.a + p[1] * (255 - c.a)) / 255);
    p[2] = (uint8_t)((c.b * c.a + p[2] * (255 - c.a)) / 255);
    p[3] = 255;
}

static void fill_rect(int x0, int y0, int x1, int y1, color_t c) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) plot(x, y, c);
}

static void fill_circle(int cx, int cy, int rad, color_t c) {
    int r2 = rad * rad;
    for (int y = cy - rad; y <= cy + rad; y++)
        for (int x = cx - rad; x <= cx + rad; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r2) plot(x, y, c);
        }
}

static void draw_line(int x0, int y0, int x1, int y1, color_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* ------------------------------------------------------------------ */
/* Camera / world mapping                                              */
/* ------------------------------------------------------------------ */

static void world_to_screen(const cam_t *cam, double wx, double wy, int *sx,
                            int *sy) {
    *sx = (int)lround((wx - cam->cx) * cam->scale + FB_W / 2.0);
    *sy = (int)lround(FB_H / 2.0 - (wy - cam->cy) * cam->scale);
}

static void compute_camera(slp_replay_t *r, cam_t *cam) {
    /* Final Destination's gameplay camera is intentionally stable.  A
       replay-wide fit includes off-screen deaths and makes the actual match
       tiny, so frame the legal play space instead. */
    if (r->game_start.stage_id == 32) {
        cam->scale = 3.45;
        cam->cx = 0.0;
        cam->cy = 32.0;
        return;
    }
    double minx = INFINITY, maxx = -INFINITY;
    double miny = INFINITY, maxy = -INFINITY;
    for (unsigned s = 0; s < SLP_SLOT_COUNT; s++) {
        slp_slot_t *slot = &r->slots[s];
        for (size_t i = 0; i < slot->count; i++) {
            const slp_frame_t *f = &slot->frames[i];
            if (f->frame_number != (int32_t)(i - SLP_FRAME_BASE)) continue;
            if (f->x < minx) minx = f->x;
            if (f->x > maxx) maxx = f->x;
            if (f->y < miny) miny = f->y;
            if (f->y > maxy) maxy = f->y;
        }
    }
    if (!isfinite(minx) || !isfinite(miny)) {
        minx = -250; maxx = 250; miny = -130; maxy = 180;
    }
    double sx = maxx - minx, sy = maxy - miny;
    if (sx < 1) sx = 1;
    if (sy < 1) sy = 1;
    cam->scale = fmin(FB_W / sx, FB_H / sy) * 0.9;
    cam->cx = (minx + maxx) / 2.0;
    cam->cy = (miny + maxy) / 2.0;
}

/* Reproduce the authored Melee stage camera.  HSD stores the camera position,
   vertical/horizontal aim angles, and vertical field of view. */
static void compute_stage_camera(const asset_stage_t *stage, cam_t *cam) {
    if (!stage || stage->cam_pos[2] <= 0.0f || stage->cam_fov <= 0.0f)
        return;
    const double radians = 3.14159265358979323846 / 180.0;
    double distance = stage->cam_pos[2];
    double fov = stage->cam_fov * radians;
    cam->cx = stage->cam_pos[0] + distance * tan(stage->cam_horiz * radians);
    cam->cy = stage->cam_pos[1] + distance * tan(stage->cam_vert * radians);
    cam->scale = FB_H / (2.0 * distance * tan(fov * 0.5));
}

static cam_t gameplay_camera_target(const active_t *a, int32_t fn) {
    cam_t cam = a->cam;
    double minx = INFINITY, maxx = -INFINITY;
    double miny = INFINITY, maxy = -INFINITY;
    const slp_frame_t *subjects[SLP_MAX_PORTS];
    unsigned count = 0;
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!a->replay.game_start.has_player[port]) continue;
        const slp_frame_t *f = slp_frame_at(&a->replay, port, false, fn);
        if (!f || !f->stocks_remaining || fabs(f->x) > 300 ||
            f->y < -120 || f->y > 240)
            continue;
        subjects[count++] = f;
    }
    if (!count) return cam;

    static const double subject_weight[] = {0.0, 1.5, 1.32, 1.16, 1.0};
    const double track = subject_weight[count] * 1.5;
    for (unsigned i = 0; i < count; i++) {
        const slp_frame_t *f = subjects[i];
        /* Melee tracks a character-specific CmSubject box.  Fox and Falco
           both use ftData.x3C = {10,22,-9, 16,-9,11.2}.  With two subjects,
           Camera_8002958C applies 1.32 * FD's 1.5 tracking ratio. */
        double x = fmax(-170.0, fmin(170.0, f->x));
        double y = fmax(-60.0, fmin(120.0, f->y + 10.0));
        double xl, xr;
        if (f->facing >= 0) {
            xl = x - 9.0 * track;
            xr = x + 22.0 * 1.5 * track;
        } else {
            xl = x - 22.0 * 1.5 * track;
            xr = x + 9.0 * track;
        }
        xl = fmax(-170.0, xl);
        xr = fmin(170.0, xr);
        double yb = fmax(-60.0, y - 9.0 * track);
        double yt = fmin(120.0, y + 16.0 * track);
        if (xl < minx) minx = xl;
        if (xr > maxx) maxx = xr;
        if (yb < miny) miny = yb;
        if (yt > maxy) maxy = yt;
    }

    /* Camera_8002958C adds bottom padding; Camera_80029CF8 then solves the
       asymmetric frustum around the box.  Constants are from the Melee
       decomp and FD's grGroundParam. */
    miny -= 10.0;
    const double rad = 3.14159265358979323846 / 180.0;
    const double half_fov = 19.0 * rad; /* gameplay FOV is 38 degrees */
    const double aspect = 1.2173333;    /* Melee's gameplay projection */
    double dx = maxx - minx, dy = maxy - miny;
    double spread = fmax(dx, dy);
    double bias = spread <= 60.0 ? 0.0 : spread >= 120.0 ? 0.0682
        : 0.0682 * (spread - 60.0) / 60.0;
    double base = (miny + maxy) * (0.5 - bias);
    double vang = -(base - 30.0) * 0.05;
    if (vang < -7.0) vang = -7.0;
    if (vang > 5.0) vang = 5.0;
    vang = (vang - 10.0) * rad; /* FD camera pan */
    double tan_up = tan(half_fov + vang);
    double tan_down = tan(half_fov - vang);
    double dist_y = dy / (tan_up + tan_down);
    double yoff = dist_y * tan(vang);
    cam.cy = yoff + (maxy - dist_y * tan_up);

    double xcenter = (minx + maxx) * 0.5;
    double hang = -xcenter * 0.05;
    if (hang < -17.5) hang = -17.5;
    if (hang > 17.5) hang = 17.5;
    hang *= rad;
    double tan_right = aspect * tan(half_fov - hang);
    double tan_left = aspect * tan(half_fov + hang);
    double dist_x = dx / (tan_right + tan_left);
    double xoff = aspect * dist_x * tan(hang);
    cam.cx = (maxx - dist_x * tan_right) - xoff;

    double distance = fmax(dist_x, dist_y);
    if (distance < 83.0) distance = 83.0;
    if (distance > 1000.0) distance = 1000.0;
    cam.scale = FB_H / (2.0 * distance * tan(half_fov));
    return cam;
}

static void build_gameplay_cameras(active_t *a) {
    free(a->frame_cams);
    a->frame_cams = NULL;
    a->frame_cam_count = 0;
    if (a->last_frame < a->start_frame) return;
    a->frame_cam_count = (size_t)(a->last_frame - a->start_frame + 1);
    a->frame_cams = malloc(a->frame_cam_count * sizeof(*a->frame_cams));
    if (!a->frame_cams) { a->frame_cam_count = 0; return; }

    cam_t current = a->cam;
    for (size_t i = 0; i < a->frame_cam_count; i++) {
        int32_t fn = a->start_frame + (int32_t)i;
        cam_t target = gameplay_camera_target(a, fn);
        /* Melee interpolates interest more gently than eye/depth.  FD's
           track-smooth is 1.8: roughly 9% and 27% per 60 Hz update. */
        current.cx += (target.cx - current.cx) * 0.09;
        current.cy += (target.cy - current.cy) * 0.09;
        double z = FB_H / (2.0 * current.scale * tan(19.0 *
                   3.14159265358979323846 / 180.0));
        double tz = FB_H / (2.0 * target.scale * tan(19.0 *
                    3.14159265358979323846 / 180.0));
        z += (tz - z) * 0.27;
        current.scale = FB_H / (2.0 * z * tan(19.0 *
                        3.14159265358979323846 / 180.0));
        a->frame_cams[i] = current;
    }
}

static cam_t gameplay_camera(const active_t *a, int32_t fn) {
    int64_t idx = (int64_t)fn - a->start_frame;
    if (a->frame_cams && idx >= 0 && (size_t)idx < a->frame_cam_count)
        return a->frame_cams[idx];
    return gameplay_camera_target(a, fn);
}

static void free_scene_assets(active_t *a) {
    for (unsigned i = 0; i < SLP_SLOT_COUNT; i++) {
        asset_model_free(a->models[i]);
        asset_anims_free(a->anims[i]);
        a->models[i] = NULL;
        a->anims[i] = NULL;
    }
    asset_stage_free(a->stage);
    a->stage = NULL;
    free(a->stage_fb);
    a->stage_fb = NULL;
    free(a->stage_sprite);
    a->stage_sprite = NULL;
    a->stage_sprite_w = a->stage_sprite_h = 0;
    free(a->frame_cams);
    a->frame_cams = NULL;
    a->frame_cam_count = 0;
}

static void character_slug(uint8_t id, char *out, size_t cap) {
    const char *name = slp_character_name(id);
    size_t n = 0;
    for (; *name && n + 1 < cap; name++) {
        unsigned char c = (unsigned char)*name;
        if (isalnum(c)) out[n++] = (char)tolower(c);
        else if (n && out[n - 1] != '_') out[n++] = '_';
    }
    while (n && out[n - 1] == '_') n--;
    out[n] = '\0';
}

static void load_scene_assets(active_t *a) {
    const slp_game_start_t *gs = &a->replay.game_start;
    for (unsigned slot = 0; slot < SLP_SLOT_COUNT; slot++) {
        unsigned port = slot / 2;
        slp_slot_t *frames = &a->replay.slots[slot];
        if (!gs->has_player[port] || !frames->active || !frames->count) continue;
        const slp_frame_t *sample = NULL;
        for (size_t i = 0; i < frames->count; i++) {
            if (frames->frames[i].frame_number != 0 || frames->frames[i].character_id) {
                sample = &frames->frames[i];
                break;
            }
        }
        if (!sample) continue;
        char slug[64], model_path[512], anim_path[512];
        character_slug(sample->character_id, slug, sizeof slug);
        unsigned costume = gs->costume_index[port];
        snprintf(model_path, sizeof model_path, "%s/%s-%u.model",
                 asset_dir(), slug, costume);
        snprintf(anim_path, sizeof anim_path, "%s/%s-%u.anims",
                 asset_dir(), slug, costume);
        a->models[slot] = asset_model_load(model_path);
        a->anims[slot] = asset_anims_load(anim_path);
    }
    if (gs->stage_id == 32) {
        char path[512];
        snprintf(path, sizeof path, "%s/fd.stage", asset_dir());
        a->stage = asset_stage_load(path);
    }
}

/* Stage half-widths so the ground bar spans roughly the right width. */
static double stage_half_width(uint16_t stage) {
    switch (stage) {
        case 32: return 248;  /* Final Destination */
        case 31: return 207;  /* Battlefield */
        case 28: return 220;  /* Dream Land */
        case 8:  return 173;  /* Yoshi's Story */
        case 3:  return 226;  /* Pokemon Stadium */
        default: return 200;
    }
}

static void draw_stage(const slp_game_start_t *gs, const cam_t *cam) {
    double hw = stage_half_width(gs->stage_id);
    int gx0, gy0, gx1, gy1;
    world_to_screen(cam, -hw, -6, &gx0, &gy0);
    world_to_screen(cam, hw, 6, &gx1, &gy1);
    fill_rect(gx0, gy0, gx1, gy1, (color_t){120, 130, 150, 255});

    color_t faint = {255, 255, 255, 28};
    int bx0, by0, bx1, by1;
    world_to_screen(cam, -hw, -130, &bx0, &by0);
    world_to_screen(cam, -hw, 190, &bx1, &by1);
    draw_line(bx0, by0, bx0, by1, faint); /* left blast line */
    world_to_screen(cam, hw, -130, &bx0, &by0);
    world_to_screen(cam, hw, 190, &bx1, &by1);
    draw_line(bx0, by0, bx0, by1, faint); /* right blast line */
    world_to_screen(cam, -hw, -130, &bx0, &by0);
    world_to_screen(cam, hw, -130, &bx1, &by1);
    draw_line(bx0, by0, bx1, by1, faint); /* bottom blast line */
    world_to_screen(cam, -hw, 190, &bx0, &by0);
    world_to_screen(cam, hw, 190, &bx1, &by1);
    draw_line(bx0, by0, bx1, by1, faint); /* top blast line */

    if (gs->stage_id == 31) { /* Battlefield side platforms */
        double plats[2] = {-80, 80};
        for (int i = 0; i < 2; i++) {
            int px0, py0, px1, py1;
            world_to_screen(cam, plats[i] - 28, 45, &px0, &py0);
            world_to_screen(cam, plats[i] + 28, 48, &px1, &py1);
            fill_rect(px0, py0, px1, py1, (color_t){140, 150, 170, 255});
        }
        int px0, py0, px1, py1;
        world_to_screen(cam, -30, 90, &px0, &py0);
        world_to_screen(cam, 30, 93, &px1, &py1);
        fill_rect(px0, py0, px1, py1, (color_t){140, 150, 170, 255});
    }
}

static void build_stage_frame(active_t *a) {
    free(a->stage_fb);
    a->stage_fb = malloc(FB_BYTES);
    if (!a->stage_fb) return;

    /* Space backdrop.  The extracted FD model supplies the platform and its
       textured scenery; this restrained gradient keeps transparent regions
       readable without competing with the match. */
    for (int y = 0; y < FB_H; y++) {
        float t = (float)y / (float)(FB_H - 1);
        uint8_t r = (uint8_t)(5 + 19 * t);
        uint8_t g = (uint8_t)(7 + 12 * t);
        uint8_t b = (uint8_t)(18 + 28 * t);
        for (int x = 0; x < FB_W; x++) {
            uint8_t *p = &a->stage_fb[(y * FB_W + x) * 4];
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
    uint32_t seed = 0x4D454C45u;
    for (int i = 0; i < 150; i++) {
        seed = seed * 1664525u + 1013904223u;
        int x = (int)(seed % FB_W);
        seed = seed * 1664525u + 1013904223u;
        int y = (int)(seed % (FB_H * 3 / 4));
        uint8_t *p = &a->stage_fb[(y * FB_W + x) * 4];
        uint8_t glow = (uint8_t)(105 + (seed >> 24) / 2);
        p[0] = glow; p[1] = glow; p[2] = (uint8_t)(glow + (255 - glow) / 2);
    }

}

static int stage_section_visible(const asset_model_t *section,
                                 float bounds[4]) {
    if (render_pose_bounds(section, NULL, UINT32_MAX, 0, bounds) != 0)
        return 0;
    /* The remaining sections are camera-facing 3D sky domes. */
    if (bounds[0] < -100.0f || bounds[2] > 100.0f || bounds[3] > 5.0f)
        return 0;
    /* Skip the two flat, overlapping top-surface material passes. */
    return bounds[3] - bounds[1] >= 1.0f;
}

/* Rasterize the static FD mesh once at a higher world-space resolution.
   Per-frame camera movement can then resample this sprite instead of skinning
   and filling thousands of stage triangles sixty times per second. */
static void build_stage_sprite(active_t *a) {
    free(a->stage_sprite);
    a->stage_sprite = NULL;
    a->stage_sprite_w = a->stage_sprite_h = 0;
    if (!a->stage) return;

    float minx = INFINITY, miny = INFINITY;
    float maxx = -INFINITY, maxy = -INFINITY;
    for (uint32_t i = 0; i < a->stage->section_count; i++) {
        float b[4];
        if (!stage_section_visible(&a->stage->sections[i], b)) continue;
        if (b[0] < minx) minx = b[0];
        if (b[1] < miny) miny = b[1];
        if (b[2] > maxx) maxx = b[2];
        if (b[3] > maxy) maxy = b[3];
    }
    if (!isfinite(minx) || !isfinite(miny)) return;

    const double ppu = 12.0;
    const double pad = 2.0 * a->stage->scale;
    a->stage_sprite_ppu = ppu;
    a->stage_sprite_min_x = minx * a->stage->scale - pad;
    a->stage_sprite_max_y = maxy * a->stage->scale + pad;
    double world_max_x = maxx * a->stage->scale + pad;
    double world_min_y = miny * a->stage->scale - pad;
    a->stage_sprite_w = (int)ceil((world_max_x - a->stage_sprite_min_x) * ppu) + 1;
    a->stage_sprite_h = (int)ceil((a->stage_sprite_max_y - world_min_y) * ppu) + 1;
    size_t bytes = (size_t)a->stage_sprite_w * a->stage_sprite_h * 4;
    a->stage_sprite = calloc(1, bytes);
    if (!a->stage_sprite) {
        a->stage_sprite_w = a->stage_sprite_h = 0;
        return;
    }

    float scale = (float)(ppu * a->stage->scale);
    float tx = (float)(-a->stage_sprite_min_x * ppu);
    float ty = (float)(a->stage_sprite_max_y * ppu);
    for (uint32_t i = 0; i < a->stage->section_count; i++) {
        float b[4];
        if (!stage_section_visible(&a->stage->sections[i], b)) continue;
        render_pose_tilted(&a->stage->sections[i], NULL, UINT32_MAX, 0, 1,
                           scale, tx, ty, 0.0f, 0.0f, a->stage_sprite,
                           a->stage_sprite_w, a->stage_sprite_h);
    }
}

static void render_stage_model(const active_t *a, const cam_t *cam,
                               uint8_t *fb) {
    if (!a->stage_sprite || !a->stage_sprite_w || !a->stage_sprite_h) return;
    double max_x = a->stage_sprite_min_x +
                   (a->stage_sprite_w - 1) / a->stage_sprite_ppu;
    double min_y = a->stage_sprite_max_y -
                   (a->stage_sprite_h - 1) / a->stage_sprite_ppu;
    int x0, y0, x1, y1;
    world_to_screen(cam, a->stage_sprite_min_x, a->stage_sprite_max_y,
                    &x0, &y0);
    world_to_screen(cam, max_x, min_y, &x1, &y1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= FB_W) x1 = FB_W - 1;
    if (y1 >= FB_H) y1 = FB_H - 1;
    if (x0 > x1 || y0 > y1) return;

    for (int y = y0; y <= y1; y++) {
        double wy = cam->cy - ((y + 0.5) - FB_H / 2.0) / cam->scale;
        int v = (int)((a->stage_sprite_max_y - wy) * a->stage_sprite_ppu);
        if (v < 0 || v >= a->stage_sprite_h) continue;
        for (int x = x0; x <= x1; x++) {
            double wx = ((x + 0.5) - FB_W / 2.0) / cam->scale + cam->cx;
            int u = (int)((wx - a->stage_sprite_min_x) * a->stage_sprite_ppu);
            if (u < 0 || u >= a->stage_sprite_w) continue;
            const uint8_t *src = &a->stage_sprite[
                ((size_t)v * a->stage_sprite_w + u) * 4];
            if (!src[3]) continue;
            uint8_t *dst = &fb[((size_t)y * FB_W + x) * 4];
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        }
    }
}

/* Frame rendering
 * ------------------------------------------------------------------ */

static const color_t port_colors[4] = {
    {255, 90, 90, 255},  {90, 150, 255, 255},
    {255, 220, 80, 255}, {90, 255, 150, 255},
};

/* Renders the full frame: stage (redrawn every frame for moving stages),
   items, and players. */
static void render_frame(const active_t *a, int32_t fn, uint8_t *fb,
                         frame_profile_t *profile) {
    double mark = monotonic_ms();
    g_fb = fb;
    if (a->stage_fb) memcpy(g_fb, a->stage_fb, FB_BYTES);
    else {
        for (int i = 0; i < FB_W * FB_H; i++) {
            g_fb[i * 4] = 20; g_fb[i * 4 + 1] = 22;
            g_fb[i * 4 + 2] = 32; g_fb[i * 4 + 3] = 255;
        }
    }
    if (profile) profile->clear_ms = monotonic_ms() - mark;

    mark = monotonic_ms();
    const slp_game_start_t *gs = &a->replay.game_start;
    cam_t cam = gameplay_camera(a, fn);
    if (a->stage) render_stage_model(a, &cam, g_fb);
    else draw_stage(gs, &cam);
    if (profile) profile->stage_ms = monotonic_ms() - mark;

    mark = monotonic_ms();
    const slp_item_list_t *items = slp_items_at(&a->replay, fn);
    if (items) {
        for (size_t i = 0; i < items->count; i++) {
            const slp_item_t *it = &items->items[i];
            int sx, sy;
            world_to_screen(&cam, it->x, it->y, &sx, &sy);
            if (it->type_id == 0x36 || it->type_id == 0x37) {
                color_t glow = it->type_id == 0x36
                    ? (color_t){255, 76, 64, 235}
                    : (color_t){80, 185, 255, 235};
                int tail = it->x_vel >= 0 ? -14 : 14;
                draw_line(sx + tail, sy, sx, sy, glow);
                draw_line(sx + tail / 2, sy - 1, sx, sy - 1,
                          (color_t){235, 245, 255, 180});
            } else {
                fill_circle(sx, sy, 4, (color_t){255, 240, 170, 220});
            }
        }
    }
    if (profile) profile->items_ms = monotonic_ms() - mark;

    mark = monotonic_ms();
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!gs->has_player[port]) continue;
        color_t col = port_colors[port];
        color_t dark = {col.r / 2, col.g / 2, col.b / 2, 255};

        for (int pass = 0; pass < 2; pass++) {
            const slp_frame_t *f = slp_frame_at(&a->replay, port, pass != 0, fn);
            if (!f || f->frame_number != fn) continue;
            int sx, sy;
            world_to_screen(&cam, f->x, f->y, &sx, &sy);

            unsigned slot = port * 2u + (unsigned)(pass != 0);
            const asset_model_t *model = a->models[slot];
            const asset_anims_t *anims = a->anims[slot];
            uint32_t action = UINT32_MAX;
            if (anims && f->animation_index < anims->action_count &&
                anims->actions[f->animation_index].joint_count)
                action = f->animation_index;
            else if (anims)
                action = render_find_action(anims, "Wait1");

            if (model) {
                render_pose_profile(model, anims, action, f->anim_frame,
                                    f->facing < 0 ? -1 : 1,
                                    (float)cam.scale, (float)sx, (float)sy,
                                    g_fb, FB_W, FB_H);
                if (f->action_state >= 178 && f->action_state <= 182 &&
                    f->shield_size > 0 && pass == 0) {
                    double rad = f->shield_size * 0.25 * cam.scale;
                    fill_circle(sx, sy, (int)rad,
                                (color_t){120, 200, 255, 85});
                }
                continue;
            }

            double half_w = 9, half_h = 17;
            if (pass == 1) { half_w = 7; half_h = 13; }
            double px = half_w * cam.scale, py = half_h * cam.scale;
            int rx = (int)px, ry = (int)py;

            /* body */
            fill_rect(sx - rx, sy - ry, sx + rx, sy + ry, dark);
            fill_rect(sx - rx + 2, sy - ry + 2, sx + rx - 2, sy + ry - 2, col);

            /* shield */
            if (f->action_state >= 178 && f->action_state <= 182 &&
                f->shield_size > 0 && pass == 0) {
                double rad = f->shield_size * 0.25 * cam.scale;
                fill_circle(sx, sy, (int)rad,
                            (color_t){120, 200, 255, 110});
            }

            /* facing notch */
            int fx = sx + (f->facing >= 0 ? rx : -rx);
            fill_rect(fx - 2, sy - ry - 6, fx + 2, sy - ry + 6,
                      (color_t){255, 255, 255, 255});
        }
    }
    if (profile) profile->fighters_ms = monotonic_ms() - mark;
}

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

/* Copies a string into JSON, escaping quotes and non-ASCII bytes. */
static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 32 && c < 127) {
            out[o++] = (char)c;
        } else {
            out[o++] = '?';
        }
    }
    out[o] = '\0';
}

static const char *frame_json(const active_t *a, int32_t fn, char *buf,
                              size_t cap) {
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o,
                          "{\"frame\":%d,\"players\":[", fn);

    const slp_game_start_t *gs = &a->replay.game_start;
    bool first = true;
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!gs->has_player[port]) continue;
        for (int pass = 0; pass < 2; pass++) {
            const slp_frame_t *f = slp_frame_at(&a->replay, port, pass != 0, fn);
            if (!f || f->frame_number != fn) continue;
            if (!first) o += (size_t)snprintf(buf + o, cap - o, ",");
            first = false;
            char cname[32], name[64];
            const char *cn = slp_character_name(f->character_id);
            snprintf(cname, sizeof cname, "%s%s", cn,
                     pass ? " (Nana)" : "");
            json_escape(cname, cname, sizeof cname);
            json_escape(gs->name[port], name, sizeof name);
            o += (size_t)snprintf(buf + o, cap - o,
                                  "{\"port\":%u,\"name\":\"%s\",\"char\":\"%s\","
                                  "\"percent\":%.1f,\"stocks\":%u,"
                                  "\"state\":\"0x%04X\",\"follower\":%d}",
                                  port + 1, name, cname, f->percent,
                                  f->stocks_remaining, f->action_state, pass);
        }
    }
    o += (size_t)snprintf(buf + o, cap - o, "]}");
    if (o >= cap) o = cap - 1;
    buf[o] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/* HTTP server                                                         */
/* ------------------------------------------------------------------ */

static void send_all(int cfd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(cfd, p, len, 0);
        if (n <= 0) return;
        p += n;
        len -= (size_t)n;
    }
}

static void send_response(int cfd, const char *ctype, const void *body,
                          size_t len) {
    char head[512];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "\r\n",
                      ctype, len);
    send_all(cfd, head, (size_t)hl);
    send_all(cfd, body, len);
}

/* Reads request headers into buf. Returns 0 on success. */
static ssize_t read_request(int cfd, char *buf, size_t cap) {
    size_t used = 0;
    while (used < cap - 1) {
        ssize_t n = recv(cfd, buf + used, cap - 1 - used, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) return (ssize_t)used;
    }
    return -1;
}

static void parse_path_query(const char *line, char *path, size_t pcap,
                             char *query, size_t qcap) {
    /* line = "GET /path?query HTTP/1.1\r\n..." -> scope to first line */
    const char *eol = strstr(line, "\r\n");
    if (!eol) eol = strchr(line, '\n');
    if (!eol) return;
    const char *a = strchr(line, ' ');
    if (!a || a >= eol) return;
    a++;
    const char *end = (const char *)memchr(a, ' ', (size_t)(eol - a));
    if (!end) return;
    const char *q = strchr(a, '?');
    if (q && q >= end) q = NULL;
    size_t plen = (size_t)((q ? q : end) - a);
    if (plen >= pcap) plen = pcap - 1;
    memcpy(path, a, plen);
    path[plen] = '\0';
    if (q) {
        size_t qlen = (size_t)(end - (q + 1));
        if (qlen >= qcap) qlen = qcap - 1;
        memcpy(query, q + 1, qlen);
        query[qlen] = '\0';
    } else {
        query[0] = '\0';
    }
}

static int query_value(const char *query, const char *key, char *out,
                       size_t cap) {
    size_t klen = strlen(key);
    const char *q = query;
    while (*q) {
        const char *eq = strchr(q, '=');
        const char *amp = strchr(q, '&');
        if (!eq) break;
        size_t key_len = (size_t)(eq - q);
        if (key_len == klen && strncmp(q, key, klen) == 0) {
            const char *val_end = amp ? amp : q + strlen(q);
            size_t vlen = (size_t)(val_end - (eq + 1));
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return 0;
        }
        if (!amp) break;
        q = amp + 1;
    }
    return -1;
}

/* Finds "Name: value" in request headers (case-insensitive name). */
static int header_value(const char *req, const char *name, char *out,
                        size_t cap) {
    const char *p = req;
    size_t nlen = strlen(name);
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        if (!line_end) return -1;
        const char *colon = (const char *)memchr(p, ':', (size_t)(line_end - p));
        if (colon) {
            size_t klen = (size_t)(colon - p);
            int match = klen == nlen;
            for (size_t i = 0; match && i < nlen; i++)
                if (tolower((unsigned char)p[i]) !=
                    tolower((unsigned char)name[i]))
                    match = 0;
            if (match) {
                const char *v = colon + 1;
                while (v < line_end && *v == ' ') v++;
                size_t vlen = (size_t)(line_end - v);
                if (vlen >= cap) vlen = cap - 1;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return 0;
            }
        }
        p = line_end + 2;
    }
    return -1;
}

/* Reads exactly `len` bytes of request body from the socket. */
static int read_body(int cfd, char *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(cfd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Replay management                                                   */
/* ------------------------------------------------------------------ */

static int load_replay(const char *dir, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, name);

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return -1; }
    unsigned char *data = malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        free(data); fclose(f); return -1;
    }
    fclose(f);

    slp_replay_t replay;
    slp_error_t e = slp_parse(data, (size_t)sz, &replay);
    free(data);
    if (e != SLP_OK) return -1;

    pthread_rwlock_wrlock(&g_lock);
    free_scene_assets(&g_active);
    slp_replay_free(&g_active.replay);
    g_active.replay = replay;
    snprintf(g_active.name, sizeof g_active.name, "%s", name);
    compute_camera(&g_active.replay, &g_active.cam);
    g_active.start_frame = -SLP_FRAME_BASE;
    g_active.last_frame = replay.last_frame;
    if (g_active.last_frame == INT32_MIN)
        g_active.last_frame =
            (int32_t)((int64_t)replay.frame_count - SLP_FRAME_BASE - 1);
    load_scene_assets(&g_active);
    build_stage_sprite(&g_active);
    compute_stage_camera(g_active.stage, &g_active.cam);
    build_gameplay_cameras(&g_active);
    build_stage_frame(&g_active);
    pthread_rwlock_unlock(&g_lock);
    return 0;
}

static int sanitize_name(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return -1;
        out[i] = (char)c;
    }
    out[i] = '\0';
    return (i >= 4 && strcmp(out + i - 4, ".slp") == 0) ? 0 : -1;
}

static int replays_json(char *buf, size_t cap) {
    DIR *d = opendir(g_dir);
    if (!d) return snprintf(buf, cap, "[]");
    char names[256][256];
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcmp(ent->d_name + len - 4, ".slp") == 0 &&
            count < 256)
            snprintf(names[count++], 256, "%s", ent->d_name);
    }
    closedir(d);
    /* simple sort */
    for (size_t i = 0; i + 1 < count; i++)
        for (size_t j = i + 1; j < count; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[256];
                snprintf(t, 256, "%s", names[i]);
                snprintf(names[i], 256, "%s", names[j]);
                snprintf(names[j], 256, "%s", t);
            }
    size_t o = 0;
    o += (size_t)snprintf(buf + o, cap - o, "[");
    for (size_t i = 0; i < count; i++) {
        if (i) o += (size_t)snprintf(buf + o, cap - o, ",");
        char esc[600];
        json_escape(names[i], esc, sizeof esc);
        o += (size_t)snprintf(buf + o, cap - o, "{\"name\":\"%s\"}", esc);
    }
    o += (size_t)snprintf(buf + o, cap - o, "]");
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* PNG encoding                                                        */
/* ------------------------------------------------------------------ */

static void png_chunk(uint8_t *dst, const char *type, const uint8_t *data,
                      size_t len) {
    dst[0] = (uint8_t)(len >> 24);
    dst[1] = (uint8_t)(len >> 16);
    dst[2] = (uint8_t)(len >> 8);
    dst[3] = (uint8_t)len;
    memcpy(dst + 4, type, 4);
    if (len) memcpy(dst + 8, data, len);
    uint32_t c = crc32(0L, Z_NULL, 0);
    c = crc32(c, (const Bytef *)type, 4);
    c = crc32(c, data, (uInt)len);
    dst[8 + len] = (uint8_t)(c >> 24);
    dst[8 + len + 1] = (uint8_t)(c >> 16);
    dst[8 + len + 2] = (uint8_t)(c >> 8);
    dst[8 + len + 3] = (uint8_t)c;
}

/* Encodes an RGBA framebuffer as an 8-bit RGB PNG. Returns size, 0 on
   error. Caller frees *out. */
static size_t png_encode(const uint8_t *rgba, int w, int h, uint8_t **out) {
    size_t row_bytes = (size_t)w * 3;
    size_t raw_len = (row_bytes + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0; /* filter: none */
        for (int x = 0; x < w; x++) {
            const uint8_t *p = &rgba[(y * w + x) * 4];
            raw[o++] = p[0];
            raw[o++] = p[1];
            raw[o++] = p[2];
        }
    }
    uLongf clen = compressBound(raw_len);
    uint8_t *z = malloc(clen);
    if (!z) { free(raw); return 0; }
    /* Frames are transient playback data; prioritize keeping the producer
       ahead of 60 fps over archival PNG size. */
    if (compress2(z, &clen, raw, raw_len, Z_BEST_SPEED) != Z_OK) {
        free(raw); free(z); return 0;
    }
    free(raw);

    uint8_t *png = malloc(8 + 25 + (12 + clen) + 12);
    if (!png) { free(z); return 0; }
    size_t p = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(png, sig, 8);
    p = 8;

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 2; /* color type: RGB */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(png + p, "IHDR", ihdr, 13);
    p += 25;
    png_chunk(png + p, "IDAT", z, clen);
    p += 12 + clen;
    png_chunk(png + p, "IEND", NULL, 0);
    p += 12;

    free(z);
    *out = png;
    return p;
}

/* Gameplay frames use deflated RGBA rather than PNG.  The browser can put
   the resulting ImageData directly on the canvas without its comparatively
   expensive PNG-to-ImageBitmap path. */
static size_t rgba_deflate(const uint8_t *rgba, size_t len, uint8_t **out) {
    uLongf compressed_len = compressBound(len);
    uint8_t *compressed = malloc(compressed_len);
    if (!compressed) return 0;
    if (compress2(compressed, &compressed_len, rgba, len,
                  Z_BEST_SPEED) != Z_OK) {
        free(compressed);
        return 0;
    }
    *out = compressed;
    return (size_t)compressed_len;
}

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

/* Caller holds g_lock for reading so replay/assets cannot change mid-frame. */
static int build_frame_body_locked(int32_t fn, uint8_t **out, size_t *out_len,
                                   frame_profile_t *profile) {
    if (profile) memset(profile, 0, sizeof(*profile));
    uint8_t *fb = malloc(FB_BYTES);
    if (!fb) return -1;
    render_frame(&g_active, fn, fb, profile);
    double mark = monotonic_ms();
    char json[16384];
    frame_json(&g_active, fn, json, sizeof json);
    if (profile) profile->json_ms = monotonic_ms() - mark;
    mark = monotonic_ms();
    uint8_t *pixels;
    size_t pixels_len = rgba_deflate(fb, FB_BYTES, &pixels);
    if (profile) {
        profile->deflate_ms = monotonic_ms() - mark;
        profile->compressed_bytes = pixels_len;
    }
    free(fb);
    if (!pixels_len) return -1;
    size_t jlen = strlen(json);
    size_t body_len = 4 + jlen + pixels_len;
    mark = monotonic_ms();
    uint8_t *body = malloc(body_len);
    if (!body) {
        free(pixels);
        return -1;
    }
    body[0] = (uint8_t)(jlen >> 24);
    body[1] = (uint8_t)(jlen >> 16);
    body[2] = (uint8_t)(jlen >> 8);
    body[3] = (uint8_t)jlen;
    memcpy(body + 4, json, jlen);
    memcpy(body + 4 + jlen, pixels, pixels_len);
    if (profile) profile->pack_ms = monotonic_ms() - mark;
    free(pixels);
    *out = body;
    *out_len = body_len;
    return 0;
}

static void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void handle_frame(int cfd, const char *query) {
    char nbuf[32];
    if (query_value(query, "n", nbuf, sizeof nbuf) != 0) {
        send_response(cfd, "text/plain", "missing n", 8);
        return;
    }
    int32_t fn = (int32_t)strtol(nbuf, NULL, 10);
    uint8_t *body = NULL;
    size_t body_len = 0;
    pthread_rwlock_rdlock(&g_lock);
    int result = build_frame_body_locked(fn, &body, &body_len, NULL);
    pthread_rwlock_unlock(&g_lock);
    if (result != 0) {
        send_response(cfd, "text/plain", "render error", 12);
        return;
    }
    send_response(cfd, "application/octet-stream", body, body_len);
    free(body);
}

/* Batch format: u32 count, then repeated i32 frame + u32 body_len + the
   ordinary /api/frame body.  Batching amortizes reverse-proxy latency while
   keeping each frame independently seekable and decodable in the browser. */
static void handle_frames(int cfd, const char *query) {
    char fbuf[32], cbuf[16] = "30";
    if (query_value(query, "from", fbuf, sizeof fbuf) != 0) {
        send_response(cfd, "text/plain", "missing from", 12);
        return;
    }
    query_value(query, "count", cbuf, sizeof cbuf);
    int32_t first = (int32_t)strtol(fbuf, NULL, 10);
    int count = atoi(cbuf);
    if (count < 1) count = 1;
    if (count > 60) count = 60;

    size_t used = 4, cap = 4 + (size_t)count * 70000;
    uint8_t *batch = malloc(cap);
    if (!batch) { send_response(cfd, "text/plain", "oom", 3); return; }
    int built = 0;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < count; i++) {
        uint8_t *body = NULL;
        size_t body_len = 0;
        if (build_frame_body_locked(first + i, &body, &body_len, NULL) != 0) break;
        size_t need = used + 8 + body_len;
        if (need > cap) {
            size_t next = cap * 2;
            if (next < need) next = need;
            uint8_t *grown = realloc(batch, next);
            if (!grown) { free(body); break; }
            batch = grown; cap = next;
        }
        put_u32be(batch + used, (uint32_t)(first + i));
        put_u32be(batch + used + 4, (uint32_t)body_len);
        memcpy(batch + used + 8, body, body_len);
        used += 8 + body_len;
        built++;
        free(body);
    }
    pthread_rwlock_unlock(&g_lock);
    put_u32be(batch, (uint32_t)built);
    send_response(cfd, "application/octet-stream", batch, used);
    free(batch);
}

/* Diagnostic endpoint: measures each server-side phase without transferring
   frame payloads, so rendering/compression time is separated from network
   latency. */
static void handle_profile(int cfd, const char *query) {
    char fbuf[32] = "0", cbuf[16] = "30";
    query_value(query, "from", fbuf, sizeof fbuf);
    query_value(query, "count", cbuf, sizeof cbuf);
    int32_t first = (int32_t)strtol(fbuf, NULL, 10);
    int count = atoi(cbuf);
    if (count < 1) count = 1;
    if (count > 60) count = 60;

    size_t cap = 32768, used = 0;
    char *json = malloc(cap);
    if (!json) { send_response(cfd, "text/plain", "oom", 3); return; }
    used += (size_t)snprintf(json + used, cap - used,
                            "{\"from\":%d,\"frames\":[", first);
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < count; i++) {
        uint8_t *body = NULL;
        size_t body_len = 0;
        frame_profile_t p;
        double start = monotonic_ms();
        if (build_frame_body_locked(first + i, &body, &body_len, &p) != 0)
            break;
        double total = monotonic_ms() - start;
        free(body);
        used += (size_t)snprintf(
            json + used, cap - used,
            "%s{\"n\":%d,\"clear\":%.3f,\"stage\":%.3f,"
            "\"items\":%.3f,\"fighters\":%.3f,\"json\":%.3f,"
            "\"deflate\":%.3f,\"pack\":%.3f,\"total\":%.3f,"
            "\"bytes\":%zu}",
            i ? "," : "", first + i, p.clear_ms, p.stage_ms, p.items_ms,
            p.fighters_ms, p.json_ms, p.deflate_ms, p.pack_ms, total,
            p.compressed_bytes);
        if (used >= cap - 512) break;
    }
    pthread_rwlock_unlock(&g_lock);
    used += (size_t)snprintf(json + used, cap - used, "]}");
    send_response(cfd, "application/json", json, used);
    free(json);
}

/* GET /api/pose?char=falco&color=0&action=Wait1&frame=30&facing=1
   Renders the extracted model (bind pose if action omitted) to a PNG. */
static void handle_pose(int cfd, const char *query) {
    char cbuf[64] = "falco", abuf[64] = "", fbuf[32] = "0", xbuf[16] = "1", cidx[16] = "0";
    query_value(query, "char", cbuf, sizeof cbuf);
    query_value(query, "action", abuf, sizeof abuf);
    query_value(query, "frame", fbuf, sizeof fbuf);
    query_value(query, "facing", xbuf, sizeof xbuf);
    query_value(query, "color", cidx, sizeof cidx);
    float frame = (float)atof(fbuf);
    int facing = atoi(xbuf), color = atoi(cidx);

    char mpath[512], apath[512];
    snprintf(mpath, sizeof mpath, "%s/%s-%d.model", asset_dir(), cbuf, color);
    snprintf(apath, sizeof apath, "%s/%s-%d.anims", asset_dir(), cbuf, color);

    asset_model_t *m = asset_model_load(mpath);
    asset_anims_t *a = asset_anims_load(apath);
    if (!m) {
        send_response(cfd, "text/plain", "model not cached; run make cache", 33);
        return;
    }

    uint32_t aidx = abuf[0] && a ? render_find_action(a, abuf) : UINT32_MAX;
    if (abuf[0] && aidx == UINT32_MAX) {
        asset_model_free(m); if (a) asset_anims_free(a);
        send_response(cfd, "text/plain", "action not found", 16);
        return;
    }

    /* Fit the camera to the transformed pose, not the JOBJ-local vertices. */
    float bounds[4];
    if (render_pose_bounds(m, a, aidx, frame, bounds) != 0) {
        asset_model_free(m); if (a) asset_anims_free(a);
        send_response(cfd, "text/plain", "empty model", 11);
        return;
    }
    float minx = bounds[0], miny = bounds[1];
    float maxx = bounds[2], maxy = bounds[3];
    float w = maxx - minx, h = maxy - miny;
    if (w < 1e-3f) w = 1e-3f;
    if (h < 1e-3f) h = 1e-3f;
    float scale = fminf((FB_W * 0.85f) / w, (FB_H * 0.85f) / h);
    float cx = (minx + maxx) / 2.0f;
    if (facing < 0) cx = -cx;
    float tx = FB_W / 2.0f - cx * scale;
    float ty = FB_H / 2.0f + (miny + maxy) / 2.0f * scale;

    uint8_t *fb = malloc(FB_BYTES);
    if (!fb) {
        asset_model_free(m); if (a) asset_anims_free(a);
        send_response(cfd, "text/plain", "oom", 3);
        return;
    }
    memset(fb, 40, FB_BYTES);
    for (int i = 0; i < FB_W * FB_H; i++) {
        fb[i*4] = 30; fb[i*4+1] = 30; fb[i*4+2] = 40; fb[i*4+3] = 255;
    }
    render_pose(m, a, aidx, frame, facing, scale, tx, ty, fb, FB_W, FB_H);

    uint8_t *png;
    size_t plen = png_encode(fb, FB_W, FB_H, &png);
    free(fb);
    if (m) asset_model_free(m);
    if (a) asset_anims_free(a);
    if (!plen) { send_response(cfd, "text/plain", "encode error", 12); return; }
    send_response(cfd, "image/png", png, plen);
    free(png);
}

static void handle_info(int cfd) {
    pthread_rwlock_rdlock(&g_lock);
    char buf[4096];
    size_t o = 0;
    const active_t *a = &g_active;
    const slp_game_start_t *gs = &a->replay.game_start;
    o += (size_t)snprintf(buf + o, sizeof buf - o,
                          "{\"name\":\"%s\",\"w\":%d,\"h\":%d,"
                          "\"start\":%d,\"last\":%d,\"frames\":%zu,"
                          "\"stage\":%u,\"stageName\":\"%s\","
                          "\"players\":[",
                          a->name, FB_W, FB_H, a->start_frame, a->last_frame,
                          a->replay.frame_count, gs->stage_id,
                          slp_stage_name(gs->stage_id));
    bool first = true;
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!gs->has_player[port]) continue;
        if (!first) o += (size_t)snprintf(buf + o, sizeof buf - o, ",");
        first = false;
        char name[64], cname[32];
        json_escape(gs->name[port], name, sizeof name);
        const char *cn =
            slp_character_name(slp_external_to_internal(gs->external_char_id[port]));
        snprintf(cname, sizeof cname, "%s", cn);
        o += (size_t)snprintf(buf + o, sizeof buf - o,
                              "{\"port\":%u,\"name\":\"%s\",\"char\":\"%s\","
                              "\"costume\":%u,\"stocks\":%u}",
                              port + 1, name, cname, gs->costume_index[port],
                              gs->stock_count[port]);
    }
    o += (size_t)snprintf(buf + o, sizeof buf - o, "]}");
    pthread_rwlock_unlock(&g_lock);
    send_response(cfd, "application/json", buf, o);
}

static void handle_set(int cfd, const char *query) {
    char fname[256];
    if (query_value(query, "f", fname, sizeof fname) != 0) {
        send_response(cfd, "text/plain", "missing f", 9);
        return;
    }
    if (load_replay(g_dir, fname) != 0) {
        send_response(cfd, "text/plain", "cannot load", 11);
        return;
    }
    handle_info(cfd);
}

static void handle_upload(int cfd, const char *query, size_t content_len,
                          const char *initial, size_t initial_len) {
    char fname[256];
    if (query_value(query, "f", fname, sizeof fname) != 0) {
        send_response(cfd, "text/plain", "missing f", 9);
        return;
    }
    char clean[256];
    if (sanitize_name(fname, clean, sizeof clean) != 0) {
        send_response(cfd, "text/plain", "bad name", 8);
        return;
    }
    char *body = malloc(content_len ? content_len : 1);
    if (!body) {
        send_response(cfd, "text/plain", "oom", 3);
        return;
    }
    if (initial_len > content_len) initial_len = content_len;
    if (initial_len) memcpy(body, initial, initial_len);
    if (read_body(cfd, body + initial_len, content_len - initial_len) != 0) {
        free(body);
        send_response(cfd, "text/plain", "bad body", 8);
        return;
    }

    char path[1400];
    snprintf(path, sizeof path, "%s/%s", g_dir, clean);
    FILE *out = fopen(path, "wb");
    if (!out) {
        free(body);
        send_response(cfd, "text/plain", "cannot write", 12);
        return;
    }
    fwrite(body, 1, content_len, out);
    fclose(out);
    free(body);

    if (load_replay(g_dir, clean) != 0) {
        send_response(cfd, "text/plain", "invalid slp", 11);
        return;
    }
    handle_info(cfd);
}

/* ------------------------------------------------------------------ */
/* Frontend                                                            */
/* ------------------------------------------------------------------ */

static const char *html_doc =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<title>Melee 2D Replay</title><style>"
    "body{background:#15171c;color:#ddd;font-family:ui-monospace,monospace;"
    "margin:0;padding:12px;text-align:center}"
    "h1{font-size:16px;font-weight:600;margin:0 0 8px;color:#9ab}"
    "#wrap{display:inline-block;position:relative}"
    "canvas{background:#000;border:1px solid #333;border-radius:4px;"
    "width:960px;height:auto;max-width:100%}"
    "#hud{position:absolute;left:8px;top:8px;text-align:left;font-size:12px;"
    "color:#fff;text-shadow:1px 1px 2px #000;pointer-events:none;line-height:1.4}"
    "#hud b{display:inline-block;width:10px;height:10px;border-radius:2px;"
    "margin-right:5px;vertical-align:-1px}"
    "#bar{display:flex;gap:8px;align-items:center;justify-content:center;"
    "margin:10px auto;max-width:980px;flex-wrap:wrap}"
    "select,input[type=file],button{background:#24272e;color:#ddd;border:1px "
    "solid #333;border-radius:4px;padding:5px 8px;font:inherit;font-size:13px}"
    "input[type=range]{flex:1;min-width:200px}"
    "#frameLabel{font-size:13px;min-width:180px;text-align:left}"
    "#help{color:#666;font-size:11px;margin-top:6px}"
    "</style></head><body>"
    "<h1>Melee 2D Replay</h1>"
    "<div id=wrap><canvas id=cv width=960 height=720></canvas>"
    "<div id=hud></div></div>"
    "<div id=bar>"
    "<select id=sel></select>"
    "<button id=playBtn>Play</button>"
    "<input type=range id=slider min=0 max=1 value=0>"
    "<span id=frameLabel></span>"
    "<label>Load replay: <input type=file id=file accept=.slp></label>"
    "</div>"
    "<div id=help>Space play/pause &middot; &larr;/&rarr; step &middot; "
    "Home/End first/last &middot; drag canvas to scrub</div>"
    "<script>"
    "const cv=document.getElementById('cv'),ctx=cv.getContext('2d');"
    "const sel=document.getElementById('sel'),sl=document.getElementById('slider');"
    "const playBtn=document.getElementById('playBtn'),file=document.getElementById('file');"
    "const fl=document.getElementById('frameLabel'),hud=document.getElementById('hud');"
    "let info=null,playing=false,cur=0,raf=0,lastT=0,acc=0,seq=0,displaying=false;"
    "let rendered=0,fps=0,fpsT=performance.now();"
    "const pngCache=new Map(),pending=new Map();"
    "let nextFill=0,inflight=0;"
    "const MAX_BATCH=2,BATCH=30,PREFETCH=120,START_BUFFER=30,STEP=1000/60;"
    "const W=960,H=720;"
    "const COL=['hsl(0 90% 60%)','hsl(240 85% 60%)','hsl(60 90% 60%)','hsl(120 85% 55%)'];"
    "async function fetchReplays(){"
    "const r=await fetch('/api/replays');const list=await r.json();"
    "sel.innerHTML=list.map(r=>`<option>${r.name}</option>`).join('');"
    "if(!list.length)return;"
    "if(!info||!list.some(r=>r.name===info.name)){await load(list[0].name);}"
    "}"
    "function requestFrame(n){"
    "if(pngCache.has(n))return Promise.resolve(pngCache.get(n));"
    "if(pending.has(n))return pending.get(n);"
    "const p=fetch('/api/frame?n='+n).then(r=>r.arrayBuffer()).then(ab=>{"
    "pngCache.set(n,ab);"
    "if(pngCache.size>2500){pngCache.delete(pngCache.keys().next().value);}"
    "pending.delete(n);return ab;"
    "}).catch(e=>{pending.delete(n);throw e;});"
    "pending.set(n,p);return p;"
    "}"
    "function cacheFrame(n,ab){"
    "pngCache.set(n,ab);"
    "while(pngCache.size>2500)pngCache.delete(pngCache.keys().next().value);"
    "}"
    "async function requestBatch(from,count){"
    "const ab=await fetch('/api/frames?from='+from+'&count='+count).then(r=>r.arrayBuffer());"
    "const dv=new DataView(ab);let off=4,total=dv.getUint32(0,false);"
    "for(let i=0;i<total;i++){"
    "const n=dv.getInt32(off,false),len=dv.getUint32(off+4,false);off+=8;"
    "cacheFrame(n,ab.slice(off,off+len));off+=len;"
    "}"
    "}"
    "function fillCache(){"
    "if(!info)return;"
    "const hi=Math.min(info.last,cur+PREFETCH);"
    "if(cur>nextFill)nextFill=cur+1;"
    "while(inflight<MAX_BATCH&&nextFill<=hi){"
    "while(nextFill<=hi&&pngCache.has(nextFill))nextFill++;"
    "if(nextFill>hi)break;"
    "const from=nextFill,count=Math.min(BATCH,hi-from+1);nextFill+=count;"
    "inflight++;"
    "requestBatch(from,count).finally(()=>{inflight--;fillCache();});"
    "}"
    "}"
    "async function load(name){"
    "const r=await fetch('/api/set?f='+encodeURIComponent(name));info=await r.json();"
    "pngCache.clear();pending.clear();inflight=0;displaying=false;seq++;"
    "sl.min=info.start;sl.max=info.last;cur=info.start;sl.value=cur;"
    "nextFill=cur+1;"
    "document.title='Melee 2D Replay - '+info.name;"
    "await show(cur);"
    "}"
    "async function show(n){"
    "if(displaying)return;displaying=true;"
    "const my=++seq;"
    "try{"
    "const ab=await requestFrame(n);"
    "if(my!==seq)return;"
    "const dv=new DataView(ab);"
    "const jlen=dv.getUint32(0,false);"
    "const st=JSON.parse(new TextDecoder().decode(new Uint8Array(ab,4,jlen)));"
    "const packed=new Blob([ab.slice(4+jlen)]).stream();"
    "const raw=await new Response(packed.pipeThrough(new DecompressionStream('deflate'))).arrayBuffer();"
    "if(my!==seq)return;"
    "ctx.putImageData(new ImageData(new Uint8ClampedArray(raw),W,H),0,0);"
    "rendered++;const now=performance.now();"
    "if(now-fpsT>=1000){fps=rendered*1000/(now-fpsT);rendered=0;fpsT=now;}"
    "fl.textContent=st.frame+' / '+info.last+(playing?' · '+fps.toFixed(0)+' fps':'');"
    "hud.innerHTML=st.players.map(p=>{"
    "const c=COL[(p.port-1)%4];"
    "return`<b style=\"background:${c}\"></b>`+"
    "`${p.name||('P'+p.port)} ${p.char} ${p.percent.toFixed(1)}% x${p.stocks}`+"
    "`<span style=\"color:#888\"> ${p.state}</span><br>`;"
    "}).join('');"
    "fillCache();"
    "}catch(e){}finally{displaying=false;}"
    "}"
    "async function toggle(){"
    "if(playing){playing=false;playBtn.textContent='Play';cancelAnimationFrame(raf);return;}"
    "playBtn.disabled=true;playBtn.textContent='Buffering…';fillCache();"
    "const target=Math.min(info.last,cur+START_BUFFER);let tries=0;"
    "while(!pngCache.has(target)&&tries++<100)await new Promise(r=>setTimeout(r,20));"
    "playBtn.disabled=false;playing=true;playBtn.textContent='Pause';acc=0;"
    "lastT=performance.now();raf=requestAnimationFrame(loop);"
    "}"
    "function loop(t){"
    "if(!playing)return;"
    "acc+=t-lastT;lastT=t;"
    "while(acc>=STEP&&playing){"
    "acc-=STEP;"
    "if(cur<info.last){cur++;sl.value=cur;show(cur);}"
    "else{toggle();return;}"
    "}"
    "raf=requestAnimationFrame(loop);"
    "}"
    "playBtn.onclick=toggle;"
    "sl.oninput=()=>{cur=+sl.value;show(cur);};"
    "sel.onchange=()=>load(sel.value);"
    "file.onchange=async()=>{const f=file.files[0];"
    "const r=await fetch('/api/upload?f='+encodeURIComponent(f.name),"
    "{method:'POST',body:f});"
    "if(r.ok){info=await r.json();await fetchReplays();sel.value=info.name;"
    "document.title='SLP Debug Viewer - '+info.name;sl.min=info.start;"
    "sl.max=info.last;cur=info.start;sl.value=cur;show(cur);}else{alert('upload failed');}"
    "file.value='';};"
    "addEventListener('keydown',e=>{"
    "if(e.code==='Space'){e.preventDefault();toggle();}"
    "else if(e.code==='ArrowRight'){cur=Math.min(cur+1,info.last);sl.value=cur;show(cur);}"
    "else if(e.code==='ArrowLeft'){cur=Math.max(cur-1,info.start);sl.value=cur;show(cur);}"
    "else if(e.code==='Home'){cur=info.start;sl.value=cur;show(cur);}"
    "else if(e.code==='End'){cur=info.last;sl.value=cur;show(cur);}});"
    "let scrubbing=false;"
    "cv.addEventListener('mousedown',e=>{scrubbing=true;scrub(e);});"
    "addEventListener('mousemove',e=>{if(scrubbing)scrub(e);});"
    "addEventListener('mouseup',()=>scrubbing=false);"
    "function scrub(e){const r=cv.getBoundingClientRect();"
    "const frac=(e.clientX-r.left)/r.width;"
    "cur=Math.round(info.start+frac*(info.last-info.start));"
    "sl.value=cur;show(cur);}"
    "fetchReplays();"
    "</script></body></html>";

/* Reads an entire file into a malloc'd buffer. Returns NULL on error. */
static char *read_whole_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    buf[sz] = '\0';
    *len = (size_t)sz;
    return buf;
}

/* Serves web/index.html from disk if present, else the embedded copy.
   This lets you edit the frontend and just refresh -- no rebuild needed. */
static void handle_root(int cfd) {
    char path[1400];
    snprintf(path, sizeof path, "%s/index.html", g_web_dir);
    size_t len = 0;
    char *body = read_whole_file(path, &len);
    if (body) {
        send_response(cfd, "text/html; charset=utf-8", body, len);
        free(body);
    } else {
        send_response(cfd, "text/html; charset=utf-8", html_doc,
                      strlen(html_doc));
    }
}

/* ------------------------------------------------------------------ */

static void *conn_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    struct timeval tv = {30, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[8192];
    ssize_t req_len = read_request(cfd, req, sizeof req);
    if (req_len >= 0) {
        char path[256] = "", query[256] = "";
        parse_path_query(req, path, sizeof path, query, sizeof query);
        if (strcmp(path, "/") == 0)
            handle_root(cfd);
        else if (strcmp(path, "/api/info") == 0)
            handle_info(cfd);
        else if (strcmp(path, "/api/replays") == 0) {
            char buf[4096];
            int len = replays_json(buf, sizeof buf);
            send_response(cfd, "application/json", buf, (size_t)len);
        } else if (strcmp(path, "/api/frame") == 0)
            handle_frame(cfd, query);
        else if (strcmp(path, "/api/frames") == 0)
            handle_frames(cfd, query);
        else if (strcmp(path, "/api/profile") == 0)
            handle_profile(cfd, query);
        else if (strcmp(path, "/api/pose") == 0)
            handle_pose(cfd, query);
        else if (strcmp(path, "/api/set") == 0)
            handle_set(cfd, query);
        else if (strcmp(path, "/api/upload") == 0) {
            char cl[32] = "0";
            header_value(req, "Content-Length", cl, sizeof cl);
            size_t clen = (size_t)strtoul(cl, NULL, 10);
            const char *body = strstr(req, "\r\n\r\n");
            body = body ? body + 4 : req + req_len;
            size_t have = (size_t)((req + req_len) - body);
            handle_upload(cfd, query, clen, body, have);
        } else
            send_response(cfd, "text/plain", "not found", 9);
    }
    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2)
        snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    else {
        const char *d = getenv("SLP_DIR");
        snprintf(g_dir, sizeof g_dir, "%s", d ? d : "./replays");
    }
    mkdir(g_dir, 0755);

    const char *wd = getenv("WEB_DIR");
    if (wd) snprintf(g_web_dir, sizeof g_web_dir, "%s", wd);

    const char *host = getenv("HOST");
    if (!host) host = "0.0.0.0";
    const char *port = getenv("PORT");
    long p = port ? strtol(port, NULL, 10) : 8080;

    memset(&g_active, 0, sizeof g_active);
    if (load_replay(g_dir, "vertical.slp") != 0) {
        DIR *d = opendir(g_dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                size_t l = strlen(e->d_name);
                if (l > 4 && strcmp(e->d_name + l - 4, ".slp") == 0 &&
                    load_replay(g_dir, e->d_name) == 0)
                    break;
            }
            closedir(d);
        }
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)p);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen");
        return 1;
    }

    printf("SLP debug viewer serving http://%s:%ld  (replays dir: %s)\n",
           host, p, g_dir);
    if (g_active.replay.frame_count > 0)
        printf("active replay: %s  frames=%d..%d\n", g_active.name,
               g_active.start_frame, g_active.last_frame);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, conn_thread, (void *)(intptr_t)cfd);
        pthread_attr_destroy(&attr);
    }
    return 0;
}
