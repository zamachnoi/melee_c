#define _POSIX_C_SOURCE 200809L

/*
 * SLP debug viewer: HTTP server for the WebGL2 replay frontend.
 *
 * Endpoints:
 *   GET /                     WebGL2 frontend
 *   GET /api/replays          JSON list with immutable replay ids
 *   POST /api/replays         upload raw .slp, returns immutable replay id
 *   GET /api/replays/{id}/manifest replay-scoped metadata and asset URLs
 *   GET /api/replays/{id}/timeline completed replay state snapshot
 *   GET /assets/...        allowlisted, schema-versioned assets (no-cache)
 *
 * Assets are versioned by schema in the URL (/assets/v5/...) and served with
 * ETag revalidation but no `immutable`, so a schema bump always reaches the
 * browser instead of being stuck in a stale edge/disk cache.
 *
 * Environment:
 *   PORT      HTTP port (default 8080)
 *   SLP_DIR   directory to scan for .slp files (default ./replays)
 *   ASSET_DIR extracted DAT cache (default ./cache, shared via fixtures/cache)
 *   HOST      bind address (default 0.0.0.0)
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <zlib.h>

#include "asset.h"
#include "parser.h"
#include "protocol.h"
#include "sha256.h"
#include "timeline.h"

#define FB_W 960
#define FB_H 720

#ifndef ASSET_DIR
#define ASSET_DIR "cache"
#endif

/* Immutable asset URL prefix.  Bumping ASSET_SCHEMA_VERSION must bump this
   too, so the previous schema's files are never reused from any cache.
   Assets themselves are served with `no-cache` + ETag (see send_immutable). */
#define ASSET_URL_PREFIX "/assets/v5/"
#define ASSET_URL_PREFIX_LEN (sizeof(ASSET_URL_PREFIX) - 1)

/* Safari ignores no-store on some cached GETs and 404s; be explicit. */
#define HTTP_NO_STORE \
    "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n" \
    "Pragma: no-cache\r\n" \
    "Expires: 0\r\n"

typedef struct {
    double scale;
    double cx, cy; /* world center mapped to screen center */
} cam_t;

typedef struct {
    slp_replay_t replay;
    cam_t cam;
    cam_t *frame_cams;
    size_t frame_cam_count;
    int32_t start_frame;
    int32_t last_frame;
} active_t;

static char g_dir[1024] = "./replays";
static char g_web_dir[1024] = "./web";

static char *read_whole_file(const char *path, size_t *len);

static const char *asset_dir(void) {
    const char *dir = getenv("ASSET_DIR");
    return dir && dir[0] ? dir : ASSET_DIR;
}

/* ------------------------------------------------------------------ */
/* Camera / world mapping                                              */
/* ------------------------------------------------------------------ */

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

/* Map Slippi stage id to the extracted stage asset basename (the lowercased
   DAT file name).  The WebGL2 manifest emits this so the browser can fetch
   /assets/vN/stages/<basename>.stage.  Unknown stages return NULL so callers
   degrade gracefully (the frontend warns when a stage asset is missing). */
static const char *stage_asset_slug(uint16_t stage) {
    switch (stage) {
        case 2:  return "griz";    /* Fountain of Dreams   */
        case 3:  return "grps";    /* Pokemon Stadium      */
        case 4:  return "grcs";    /* Princess Peach's Castle */
        case 5:  return "grkg";    /* Kongo Jungle         */
        case 6:  return "grze";    /* Brinstar             */
        case 7:  return "grcn";    /* Corneria             */
        case 8:  return "grst";    /* Yoshi's Story        */
        case 9:  return "grot";    /* Onett                */
        case 10: return "grmc";    /* Mute City            */
        case 11: return "grrc";    /* Rainbow Cruise       */
        case 12: return "grgd";    /* Jungle Japes         */
        case 13: return "grgb";    /* Great Bay            */
        case 14: return "grhr";    /* Hyrule Temple        */
        case 15: return "grkr";    /* Brinstar Depths      */
        case 16: return "gryt";    /* Yoshi's Island       */
        case 17: return "grgr";    /* Green Greens         */
        case 18: return "grfs";    /* Fourside             */
        case 19: return "gri1";    /* Mushroom Kingdom I   */
        case 20: return "gri2";    /* Mushroom Kingdom II  */
        case 22: return "grve";    /* Venom                */
        case 23: return "grnpo";   /* Poke Floats          */
        case 24: return "grbb";    /* Big Blue             */
        case 25: return "grim";    /* Icicle Mountain      */
        case 27: return "grfz";    /* Flat Zone            */
        case 28: return "grop";    /* Dream Land 64        */
        case 29: return "groy";    /* Yoshi's Island N64   */
        case 30: return "grok";    /* Kongo Jungle N64     */
        case 31: return "grnba";   /* Battlefield          */
        case 32: return "grnla";   /* Final Destination    */
        default: return NULL;
    }
}

/* Load the stage asset for a replay's stage id, if present on disk. */
static asset_stage_t *load_replay_stage(uint16_t stage) {
    const char *slug = stage_asset_slug(stage);
    if (!slug) return NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.stage", asset_dir(), slug);
    return asset_stage_load(path);
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

static void send_response_headers(int cfd, int status, const char *reason,
                                  const char *ctype, const char *headers,
                                  const void *body, size_t len) {
    char head[2048];
    int hl = snprintf(head, sizeof head,
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "X-Content-Type-Options: nosniff\r\n"
                      "%s"
                      "\r\n",
                      status, reason, ctype, len, headers ? headers : "");
    send_all(cfd, head, (size_t)hl);
    if (body && len) send_all(cfd, body, len);
}

static void send_response(int cfd, const char *ctype, const void *body,
                          size_t len) {
    send_response_headers(cfd, 200, "OK", ctype, HTTP_NO_STORE, body, len);
}

static void send_error(int cfd, int status, const char *reason,
                       const char *message) {
    send_response_headers(cfd, status, reason, "text/plain; charset=utf-8",
                          HTTP_NO_STORE, message, strlen(message));
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

static int parse_replay_path(const char *path, slp_replay_t *replay) {
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

    slp_error_t e = slp_parse(data, (size_t)sz, replay);
    free(data);
    if (e != SLP_OK) return -1;
    return 0;
}

static int valid_replay_id(const char *id) {
    if (strlen(id) != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((id[i] >= '0' && id[i] <= '9') ||
              (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    return 1;
}

static int replay_file_id(const char *path, char id[65]) {
    uint8_t digest[32];
    if (sha256_file(path, digest) != 0) return -1;
    sha256_hex(digest, id);
    return 0;
}

static int replay_display_name_valid(const char *name) {
    size_t len = strlen(name);
    if (len < 5 || len >= 256 || strcasecmp(name + len - 4, ".slp") != 0)
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') return 0;
    return 1;
}

static void replay_display_name(const char *id, const char *fallback,
                                char *out, size_t cap) {
    char sidecar[1400];
    snprintf(sidecar, sizeof sidecar, "%s/%s.name", g_dir, id);
    size_t len = 0;
    char *saved = read_whole_file(sidecar, &len);
    if (saved && replay_display_name_valid(saved))
        snprintf(out, cap, "%s", saved);
    else
        snprintf(out, cap, "%s", fallback);
    free(saved);
}

static int write_replay_display_name(const char *id, const char *name) {
    if (!replay_display_name_valid(name)) return -1;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.name", g_dir, id);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(name);
    size_t written = fwrite(name, 1, len, f);
    int closed = fclose(f);
    int ok = written == len && closed == 0;
    return ok ? 0 : -1;
}

/* Resolve by content, not by process-global selection.  Files uploaded through
   POST /api/replays are named by id; legacy named files are hashed on lookup. */
static int find_replay_by_id(const char *id, char *path, size_t path_cap,
                             char *name, size_t name_cap) {
    if (!valid_replay_id(id)) return -1;
    char direct[1400];
    snprintf(direct, sizeof direct, "%s/%s.slp", g_dir, id);
    struct stat st;
    if (stat(direct, &st) == 0 && S_ISREG(st.st_mode)) {
        char direct_id[65];
        if (replay_file_id(direct, direct_id) == 0 && strcmp(direct_id, id) == 0) {
            snprintf(path, path_cap, "%s", direct);
            char fallback[80]; snprintf(fallback, sizeof fallback, "%s.slp", id);
            replay_display_name(id, fallback, name, name_cap);
            return 0;
        }
    }
    DIR *dir = opendir(g_dir);
    if (!dir) return -1;
    int found = -1;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        size_t len = strlen(ent->d_name);
        if (len <= 4 || strcmp(ent->d_name + len - 4, ".slp") != 0) continue;
        char candidate[1400], candidate_id[65];
        snprintf(candidate, sizeof candidate, "%s/%s", g_dir, ent->d_name);
        if (replay_file_id(candidate, candidate_id) == 0 &&
            strcmp(candidate_id, id) == 0) {
            snprintf(path, path_cap, "%s", candidate);
            replay_display_name(id, ent->d_name, name, name_cap);
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
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
    bool emitted = false;
    char emitted_ids[256][65];
    size_t emitted_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (o + 1200 >= cap) break;
        char path[1400], id[65];
        snprintf(path, sizeof path, "%s/%s", g_dir, names[i]);
        if (replay_file_id(path, id) != 0) continue;
        bool duplicate = false;
        for (size_t j = 0; j < emitted_count; j++)
            if (strcmp(emitted_ids[j], id) == 0) { duplicate = true; break; }
        if (duplicate) continue;
        snprintf(emitted_ids[emitted_count++], 65, "%s", id);
        if (emitted) o += (size_t)snprintf(buf + o, cap - o, ",");
        char display[256], esc[600], file_esc[600];
        replay_display_name(id, names[i], display, sizeof display);
        json_escape(display, esc, sizeof esc);
        json_escape(names[i], file_esc, sizeof file_esc);
        o += (size_t)snprintf(buf + o, cap - o,
                              "{\"id\":\"%s\",\"name\":\"%s\","
                              "\"file\":\"%s\"}", id, esc, file_esc);
        emitted = true;
    }
    o += (size_t)snprintf(buf + o, cap - o, "]");
    return (int)o;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* Stateless replay and immutable asset API                            */
/* ------------------------------------------------------------------ */

static int32_t replay_end_frame(const slp_replay_t *r) {
    if (r->last_frame != INT32_MIN) return r->last_frame;
    return (int32_t)((int64_t)r->frame_count - SLP_FRAME_BASE - 1);
}

static const slp_frame_t *first_exact_frame(const slp_replay_t *r,
                                             unsigned slot, int32_t start,
                                             int32_t end) {
    for (int32_t fn = start; fn <= end; fn++) {
        const slp_frame_t *f = slp_frame_at(r, slot / 2, slot & 1, fn);
        if (f && f->frame_number == fn) return f;
        if (fn == INT32_MAX) break;
    }
    return NULL;
}

static timeline_camera_t *make_timeline_cameras(const slp_replay_t *r,
                                                 int32_t start, int32_t end,
                                                 size_t *count_out) {
    size_t count = (size_t)((int64_t)end - start + 1);
    active_t temp;
    memset(&temp, 0, sizeof temp);
    temp.replay = *r; /* borrowed: only the camera arrays below are owned */
    temp.start_frame = start;
    temp.last_frame = end;
    compute_camera(&temp.replay, &temp.cam);
    asset_stage_t *stage = load_replay_stage(r->game_start.stage_id);
    if (stage)
        compute_stage_camera(stage, &temp.cam);
    build_gameplay_cameras(&temp);
    timeline_camera_t *samples = malloc(count * sizeof(*samples));
    if (!samples) {
        free(temp.frame_cams);
        asset_stage_free(stage);
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        cam_t cam = temp.frame_cams ? temp.frame_cams[i]
                                   : gameplay_camera_target(&temp, start + (int32_t)i);
        samples[i].x = (float)cam.cx;
        samples[i].y = (float)cam.cy;
        samples[i].zoom = (float)cam.scale;
    }
    free(temp.frame_cams);
    asset_stage_free(stage);
    *count_out = count;
    return samples;
}

static int request_etag_matches(const char *req, const char *etag) {
    char value[256];
    return header_value(req, "If-None-Match", value, sizeof value) == 0 &&
           strcmp(value, etag) == 0;
}

/* Serves a schema-versioned asset with ETag revalidation but WITHOUT the
   `immutable` directive.  `immutable` made browsers never re-fetch for a year,
   so after a schema bump they kept serving stale schema-4 bytes even across
   hard refreshes.  `no-cache` forces the browser to revalidate via If-None-Match
   on every load: the server answers 304 when the bytes are unchanged and 200
   with the current bytes when the schema/cache changed. */
static void send_immutable(int cfd, const char *req, const char *ctype,
                           const char *etag, const void *body, size_t len) {
    char headers[512];
    snprintf(headers, sizeof headers,
             "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
             "ETag: %s\r\n", etag);
    if (request_etag_matches(req, etag)) {
        send_response_headers(cfd, 304, "Not Modified", ctype, headers,
                              NULL, 0);
        return;
    }
    send_response_headers(cfd, 200, "OK", ctype, headers, body, len);
}

static void handle_replay_manifest(int cfd, const char *req, const char *id) {
    char path[1400], name[256];
    if (find_replay_by_id(id, path, sizeof path, name, sizeof name) != 0) {
        send_error(cfd, 404, "Not Found", "unknown replay id");
        return;
    }
    slp_replay_t replay;
    if (parse_replay_path(path, &replay) != 0) {
        send_error(cfd, 422, "Unprocessable Content", "invalid replay");
        return;
    }
    int32_t start = -SLP_FRAME_BASE, end = replay_end_frame(&replay);
    char json[16384], escaped_name[600];
    json_escape(name, escaped_name, sizeof escaped_name);
    size_t o = (size_t)snprintf(json, sizeof json,
        "{\"id\":\"%s\",\"name\":\"%s\",\"completed\":true,"
        "\"assetSchema\":%u,\"timelineSchema\":%u,\"liveProtocol\":%u,"
        "\"startFrame\":%d,\"endFrame\":%d,\"stageId\":%u,"
        "\"stageName\":\"%s\",\"timelineUrl\":\"/api/replays/%s/timeline\","
        "\"players\":[",
        id, escaped_name, ASSET_SCHEMA_VERSION, TIMELINE_SCHEMA_VERSION,
        LIVE_PROTOCOL_VERSION, start, end, replay.game_start.stage_id,
        slp_stage_name(replay.game_start.stage_id), id);
    bool first = true;
    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!replay.game_start.has_player[port]) continue;
        char player_name[96];
        json_escape(replay.game_start.name[port], player_name, sizeof player_name);
        o += (size_t)snprintf(json + o, sizeof json - o,
            "%s{\"port\":%u,\"name\":\"%s\",\"characterId\":%u,"
            "\"costume\":%u,\"stocks\":%u}", first ? "" : ",", port,
            player_name, replay.game_start.external_char_id[port],
            replay.game_start.costume_index[port], replay.game_start.stock_count[port]);
        first = false;
    }
    o += (size_t)snprintf(json + o, sizeof json - o, "],\"assets\":[");
    first = true;
    for (unsigned slot = 0; slot < SLP_SLOT_COUNT; slot++) {
        if (!replay.slots[slot].active) continue;
        const slp_frame_t *sample = first_exact_frame(&replay, slot, start, end);
        if (!sample) continue;
        char slug[64]; character_slug(sample->character_id, slug, sizeof slug);
        const char *anim_slug = sample->character_id == 11 ? "popo" : slug;
        unsigned costume = replay.game_start.costume_index[slot / 2];
        o += (size_t)snprintf(json + o, sizeof json - o,
            "%s{\"slot\":%u,\"model\":\"" ASSET_URL_PREFIX "characters/%s/%s-%u.model\","
            "\"animations\":\"" ASSET_URL_PREFIX "characters/%s/%s-%u.anims\"}",
            first ? "" : ",", slot, slug, slug, costume, anim_slug, anim_slug, costume);
        first = false;
    }
    const char *stage_slug = stage_asset_slug(replay.game_start.stage_id);
    if (stage_slug)
        o += (size_t)snprintf(json + o, sizeof json - o,
                             "%s{\"stage\":\"" ASSET_URL_PREFIX "stages/%s.stage\","
                             "\"animations\":\"" ASSET_URL_PREFIX "stages/%s.anims?s=1\"}",
                             first ? "" : ",", stage_slug, stage_slug);
    if (replay.game_start.stage_id == 3) {
        static const struct { const char *slug; int type; } variants[] = {
            { "grps1", 3 }, { "grps2", 4 }, { "grps4", 6 }, { "grps3", 9 },
        };
        for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
            char stage_path[512];
            snprintf(stage_path, sizeof stage_path, "%s/%s.stage",
                     asset_dir(), variants[i].slug);
            if (access(stage_path, R_OK) != 0) continue;
            o += (size_t)snprintf(json + o, sizeof json - o,
                "%s{\"stage\":\"" ASSET_URL_PREFIX "stages/%s.stage\",\"stadiumType\":%d}",
                o && json[o - 1] != '[' ? "," : "",
                variants[i].slug, variants[i].type);
        }
    }
    o += (size_t)snprintf(json + o, sizeof json - o, "]}");
    slp_replay_free(&replay);
    if (o >= sizeof json) {
        send_error(cfd, 500, "Internal Server Error", "manifest overflow");
        return;
    }
    char etag[96]; snprintf(etag, sizeof etag, "\"%s:manifest-4\"", id);
    send_immutable(cfd, req, "application/json", etag, json, o);
}

static void handle_replay_timeline(int cfd, const char *req, const char *id) {
    char path[1400], name[256];
    if (find_replay_by_id(id, path, sizeof path, name, sizeof name) != 0) {
        send_error(cfd, 404, "Not Found", "unknown replay id");
        return;
    }
    slp_replay_t replay;
    if (parse_replay_path(path, &replay) != 0) {
        send_error(cfd, 422, "Unprocessable Content", "invalid replay");
        return;
    }
    int32_t start = -SLP_FRAME_BASE, end = replay_end_frame(&replay);
    size_t camera_count = 0;
    timeline_camera_t *cameras = make_timeline_cameras(&replay, start, end,
                                                        &camera_count);
    timeline_blob_t blob = {0};
    int result = cameras ? timeline_serialize(&replay, start, end, cameras,
                                               camera_count, &blob) : -1;
    free(cameras);
    slp_replay_free(&replay);
    if (result != 0) {
        send_error(cfd, 500, "Internal Server Error", "timeline serialization failed");
        return;
    }
    char etag[96];
    snprintf(etag, sizeof etag, "\"%s:timeline-%u\"", id,
             TIMELINE_SCHEMA_VERSION);
    send_immutable(cfd, req, "application/vnd.melee.timeline", etag,
                   blob.data, blob.len);
    timeline_blob_free(&blob);
}

static void handle_create_replay(int cfd, size_t content_len,
                                 const char *initial, size_t initial_len,
                                 const char *display_name) {
    if (display_name && display_name[0] &&
        !replay_display_name_valid(display_name)) {
        send_error(cfd, 400, "Bad Request", "invalid X-Replay-Name");
        return;
    }
    if (!content_len || content_len > 128u * 1024u * 1024u) {
        send_error(cfd, 413, "Content Too Large", "replay must be 1..134217728 bytes");
        return;
    }
    uint8_t *body = malloc(content_len);
    if (!body) { send_error(cfd, 500, "Internal Server Error", "oom"); return; }
    if (initial_len > content_len) initial_len = content_len;
    memcpy(body, initial, initial_len);
    if (read_body(cfd, (char *)body + initial_len, content_len - initial_len) != 0) {
        free(body); send_error(cfd, 400, "Bad Request", "truncated upload"); return;
    }
    slp_replay_t replay;
    slp_error_t error = slp_parse(body, content_len, &replay);
    if (error != SLP_OK) {
        free(body); send_error(cfd, 422, "Unprocessable Content", slp_error_string(error)); return;
    }
    slp_replay_free(&replay);
    uint8_t digest[32]; char id[65];
    sha256_bytes(body, content_len, digest); sha256_hex(digest, id);
    char path[1400]; snprintf(path, sizeof path, "%s/%s.slp", g_dir, id);
    struct stat st;
    if (stat(path, &st) != 0) {
        char temp[1500];
        snprintf(temp, sizeof temp, "%s/.upload-%s-%lu.tmp", g_dir, id,
                 (unsigned long)pthread_self());
        FILE *f = fopen(temp, "wb");
        int stored = 0;
        if (f) {
            size_t written = fwrite(body, 1, content_len, f);
            int flushed = fflush(f);
            int closed = fclose(f);
            if (written == content_len && flushed == 0 && closed == 0 &&
                rename(temp, path) == 0)
                stored = 1;
        }
        if (!stored) {
            free(body); send_error(cfd, 500, "Internal Server Error", "cannot store replay"); return;
        }
    }
    if (display_name && display_name[0] &&
        write_replay_display_name(id, display_name) != 0) {
        free(body);
        send_error(cfd, 400, "Bad Request", "invalid X-Replay-Name");
        return;
    }
    free(body);
    char json[768], escaped_name[600];
    json_escape(display_name && display_name[0] ? display_name : "",
                escaped_name, sizeof escaped_name);
    int len = snprintf(json, sizeof json,
                       "{\"id\":\"%s\",\"name\":\"%s\","
                       "\"manifestUrl\":\"/api/replays/%s/manifest\"}",
                       id, escaped_name, id);
    send_response_headers(cfd, 201, "Created", "application/json",
                          HTTP_NO_STORE, json, (size_t)len);
}

static int safe_asset_name(const char *name) {
    if (!*name || strstr(name, "..") || strchr(name, '/')) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!(islower(*p) || isdigit(*p) || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    return 1;
}

static void handle_asset(int cfd, const char *req, const char *path) {
    static const char prefix[] = ASSET_URL_PREFIX;
    const char *relative = path + sizeof(prefix) - 1;
    if (strcmp(relative, "effects.json") == 0) {
        char file_path[1400];
        snprintf(file_path, sizeof file_path, "%s/v5/effects.json", asset_dir());
        size_t len = 0; char *body = read_whole_file(file_path, &len);
        if (!body) { send_error(cfd, 404, "Not Found", "asset not found"); return; }
        if (len < 16 || body[0] != '{') {
            free(body);
            send_error(cfd, 422, "Unprocessable Content", "invalid effects catalog");
            return;
        }
        uint8_t digest[32]; char hex[65], etag[72];
        sha256_bytes(body, len, digest); sha256_hex(digest, hex);
        snprintf(etag, sizeof etag, "\"%.64s\"", hex);
        send_immutable(cfd, req, "application/json", etag, body, len);
        free(body);
        return;
    }
    /* relative is one of:
         characters/<char>/<name>
         stages/<name>
         effects/<name>
         icons/<name>           (served from top-level cache/icons)
       Build the on-disk subdirectory from the first path component, then
       validate the trailing basename against the allowlist. */
    const char *slash = strchr(relative, '/');
    if (!slash) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
    const char *name = slash + 1;
    const char *ctype = NULL, *suffix = NULL;
    int png_icon = 0;
    if (strncmp(relative, "characters/", 11) == 0) {
        /* <char> folder plus the file; character name is a second component. */
        const char *cslash = strchr(name, '/');
        if (!cslash) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
        name = cslash + 1;
        if (strchr(name, '/')) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
        char sub[512];
        size_t clen = (size_t)(cslash - (slash + 1));
        if (clen == 0 || clen >= sizeof sub) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
        memcpy(sub, slash + 1, clen); sub[clen] = '\0';
        if (!safe_asset_name(sub)) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
        char file_path[1400];
        size_t len = 0;
        if (strstr(name, ".model")) { suffix = ".model"; ctype = "application/vnd.melee.model";
            snprintf(file_path, sizeof file_path, "%s/v5/characters/%s/%s", asset_dir(), sub, name); }
        else if (strstr(name, ".anims")) { suffix = ".anims"; ctype = "application/vnd.melee.animations";
            snprintf(file_path, sizeof file_path, "%s/v5/characters/%s/%s", asset_dir(), sub, name); }
        else { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
        if (!safe_asset_name(name) || strlen(name) <= strlen(suffix) ||
            strcmp(name + strlen(name) - strlen(suffix), suffix) != 0) {
            send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return;
        }
        char *body = read_whole_file(file_path, &len);
        if (!body) { send_error(cfd, 404, "Not Found", "asset not found"); return; }
        if (len < 8 || (uint8_t)body[0] != 'M' || (uint8_t)body[1] != 'D' ||
            (uint8_t)body[2] != 'L' || body[3] != 0 ||
            (uint8_t)body[4] != 0 || (uint8_t)body[5] != 0 ||
            (uint8_t)body[6] != 0 || (uint8_t)body[7] != ASSET_SCHEMA_VERSION) {
            free(body); send_error(cfd, 422, "Unprocessable Content", "invalid asset header"); return;
        }
        uint8_t digest[32]; char hex[65], etag[72];
        sha256_bytes(body, len, digest); sha256_hex(digest, hex);
        snprintf(etag, sizeof etag, "\"%.64s\"", hex);
        send_immutable(cfd, req, ctype, etag, body, len);
        free(body);
        return;
    }
    if (strchr(name, '/')) { send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return; }
    if (strncmp(relative, "stages/", 7) == 0) {
        suffix = strstr(name, ".stage") ? ".stage" : ".anims";
        ctype = strstr(name, ".stage") ? "application/vnd.melee.stage" : "application/vnd.melee.animations";
    } else if (strncmp(relative, "effects/", 8) == 0) {
        suffix = ".model"; ctype = "application/vnd.melee.model";
    } else if (strncmp(relative, "icons/", 6) == 0) {
        suffix = ".png"; ctype = "image/png"; png_icon = 1;
    } else {
        send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return;
    }
    size_t name_len = name ? strlen(name) : 0, suffix_len = suffix ? strlen(suffix) : 0;
    if (!name || !safe_asset_name(name) || name_len <= suffix_len ||
        strcmp(name + name_len - suffix_len, suffix) != 0) {
        send_error(cfd, 404, "Not Found", "asset is not allowlisted"); return;
    }
    char file_path[1400];
    if (png_icon)
        snprintf(file_path, sizeof file_path, "%s/icons/%s", asset_dir(), name);
    else if (strncmp(relative, "stages/", 7) == 0)
        snprintf(file_path, sizeof file_path, "%s/v5/stages/%s", asset_dir(), name);
    else
        snprintf(file_path, sizeof file_path, "%s/v5/effects/%s", asset_dir(), name);
    size_t len = 0; char *body = read_whole_file(file_path, &len);
    if (!body && png_icon) {
        snprintf(file_path, sizeof file_path, "%s/%s", asset_dir(), name);
        body = read_whole_file(file_path, &len);
    }
    if (!body) { send_error(cfd, 404, "Not Found", "asset not found"); return; }
    if (png_icon) {
        static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (len < 8 || memcmp(body, sig, 8) != 0) {
            free(body); send_error(cfd, 422, "Unprocessable Content", "invalid icon png"); return;
        }
    } else if (len < 8 || (uint8_t)body[0] != 'M' || (uint8_t)body[1] != 'D' ||
        (uint8_t)body[2] != 'L' || body[3] != 0 ||
        (uint8_t)body[4] != 0 || (uint8_t)body[5] != 0 ||
        (uint8_t)body[6] != 0 || (uint8_t)body[7] != ASSET_SCHEMA_VERSION) {
        free(body); send_error(cfd, 422, "Unprocessable Content", "invalid asset header"); return;
    }
    uint8_t digest[32]; char hex[65], etag[72];
    sha256_bytes(body, len, digest); sha256_hex(digest, hex);
    snprintf(etag, sizeof etag, "\"%.64s\"", hex);
    send_immutable(cfd, req, ctype, etag, body, len);
    free(body);
}

/* ------------------------------------------------------------------ */
/* Frontend                                                            */
/* ------------------------------------------------------------------ */

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

/* Safari keeps a separate module cache for /main.js from the old
   type=module page. Serve the IIFE bundle under a new URL and stamp a
   content hash so a stale classic-script copy cannot be reused. */
static char *with_viewer_script_hash(char *html, size_t *len) {
    static const char needle[] = "src=\"/viewer.js\"";
    char *at = strstr(html, needle);
    if (!at) return html;
    char js_path[1400];
    snprintf(js_path, sizeof js_path, "%s/dist/main.js", g_web_dir);
    uint8_t digest[32];
    char hex[65];
    if (sha256_file(js_path, digest) != 0) return html;
    sha256_hex(digest, hex);
    char replacement[80];
    int n = snprintf(replacement, sizeof replacement,
                     "src=\"/viewer.js?v=%.12s\"", hex);
    if (n < 0) return html;
    size_t needle_len = sizeof needle - 1;
    size_t prefix = (size_t)(at - html);
    size_t suffix_off = prefix + needle_len;
    size_t suffix_len = *len - suffix_off;
    size_t new_len = prefix + (size_t)n + suffix_len;
    char *out = malloc(new_len + 1);
    if (!out) return html;
    memcpy(out, html, prefix);
    memcpy(out + prefix, replacement, (size_t)n);
    memcpy(out + prefix + (size_t)n, html + suffix_off, suffix_len);
    out[new_len] = '\0';
    *len = new_len;
    return out;
}

/* Serves the WebGL2 frontend.  This lets you edit the frontend and just
   refresh -- no rebuild needed. */
static void handle_root(int cfd) {
    char path[1400];
    snprintf(path, sizeof path, "%s/webgl.html", g_web_dir);
    size_t len = 0;
    char *body = read_whole_file(path, &len);
    if (body) {
        char *stamped = with_viewer_script_hash(body, &len);
        if (stamped != body) { free(body); body = stamped; }
        send_response_headers(cfd, 200, "OK", "text/html; charset=utf-8",
                              HTTP_NO_STORE, body, len);
        free(body);
    } else {
        send_error(cfd, 500, "Internal Server Error",
                   "web/webgl.html is missing");
    }
}

static int web_bundle_path(const char *path) {
    return strcmp(path, "/main.js") == 0 || strcmp(path, "/viewer.js") == 0;
}

static int web_module_path(const char *path) {
    if (!web_bundle_path(path) &&
        strncmp(path, "/assets/", 8) != 0 &&
        strncmp(path, "/animation/", 11) != 0 &&
        strncmp(path, "/replay/", 8) != 0 &&
        strncmp(path, "/renderer/", 10) != 0)
        return 0;
    if (strstr(path, "..") || !strstr(path, ".js")) return 0;
    size_t len = strlen(path);
    if (len < 3 || strcmp(path + len - 3, ".js") != 0) return 0;
    for (const unsigned char *p = (const unsigned char *)path + 1; *p; p++)
        if (!(isalnum(*p) || *p == '/' || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    return 1;
}

static void handle_web_module(int cfd, const char *path) {
    if (!web_module_path(path)) {
        send_error(cfd, 404, "Not Found", "module is not allowlisted");
        return;
    }
    const char *rel = web_bundle_path(path) ? "main.js" : path + 1;
    char file_path[1400];
    snprintf(file_path, sizeof file_path, "%s/dist/%s", g_web_dir, rel);
    size_t len = 0;
    char *body = read_whole_file(file_path, &len);
    if (!body) { send_error(cfd, 404, "Not Found", "module not found"); return; }
    send_response_headers(cfd, 200, "OK", "text/javascript; charset=utf-8",
                          HTTP_NO_STORE, body, len);
    free(body);
}

static void handle_scoped_replay(int cfd, const char *req, const char *path) {
    const char *tail = path + strlen("/api/replays/");
    const char *slash = strchr(tail, '/');
    if (!slash || (size_t)(slash - tail) != 64) {
        send_error(cfd, 404, "Not Found", "invalid replay route"); return;
    }
    char id[65]; memcpy(id, tail, 64); id[64] = '\0';
    if (!valid_replay_id(id)) {
        send_error(cfd, 404, "Not Found", "invalid replay id"); return;
    }
    if (strcmp(slash, "/manifest") == 0) handle_replay_manifest(cfd, req, id);
    else if (strcmp(slash, "/timeline") == 0) handle_replay_timeline(cfd, req, id);
    else send_error(cfd, 404, "Not Found", "unknown replay resource");
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
        else if (web_module_path(path))
            handle_web_module(cfd, path);
        else if (strcmp(path, "/api/replays") == 0) {
            if (strncmp(req, "POST ", 5) == 0) {
                char cl[32] = "0";
                header_value(req, "Content-Length", cl, sizeof cl);
                size_t clen = (size_t)strtoul(cl, NULL, 10);
                const char *body = strstr(req, "\r\n\r\n");
                body = body ? body + 4 : req + req_len;
                size_t have = (size_t)((req + req_len) - body);
                char display_name[256] = "";
                header_value(req, "X-Replay-Name", display_name,
                             sizeof display_name);
                handle_create_replay(cfd, clen, body, have, display_name);
            } else {
                char buf[65536];
                int len = replays_json(buf, sizeof buf);
                send_response(cfd, "application/json", buf, (size_t)len);
            }
        } else if (strncmp(path, "/api/replays/", 13) == 0)
            handle_scoped_replay(cfd, req, path);
        else if (strncmp(path, ASSET_URL_PREFIX, ASSET_URL_PREFIX_LEN) == 0)
            handle_asset(cfd, req, path);
        else
            send_error(cfd, 404, "Not Found", "not found");
    }
    close(cfd);
    return NULL;
}

/* Fresh worktrees have no replays/ dir (gitignored).  Point it at the shared
   fixtures symlink instead of copying .slp files into the worktree. */
static void ensure_replay_dir(void) {
    struct stat st;
    if (lstat(g_dir, &st) == 0) return;
    const char *fixtures = getenv("FIXTURES_DIR");
    if (!fixtures || !fixtures[0]) fixtures = "fixtures";
    if (symlink(fixtures, g_dir) != 0)
        mkdir(g_dir, 0755);
}

/* Same idea for the extracted DAT cache: default ./cache is gitignored, so
   new worktrees share fixtures/cache rather than extracting a private copy.
   Absolute ASSET_DIR (Docker, explicit env) is left alone. */
static void ensure_asset_dir(void) {
    const char *dir = asset_dir();
    struct stat st;
    if (lstat(dir, &st) == 0) return;
    if (strcmp(dir, "cache") != 0 && strcmp(dir, "./cache") != 0) return;
    const char *fixtures = getenv("FIXTURES_DIR");
    if (!fixtures || !fixtures[0]) fixtures = "fixtures";
    char target[1024];
    snprintf(target, sizeof target, "%s/cache", fixtures);
    mkdir(target, 0755);
    if (symlink(target, dir) != 0)
        mkdir(dir, 0755);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (argc >= 2)
        snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    else {
        const char *d = getenv("SLP_DIR");
        snprintf(g_dir, sizeof g_dir, "%s", d ? d : "./replays");
    }
    ensure_replay_dir();
    ensure_asset_dir();

    const char *wd = getenv("WEB_DIR");
    if (wd) snprintf(g_web_dir, sizeof g_web_dir, "%s", wd);

    const char *host = getenv("HOST");
    if (!host) host = "0.0.0.0";
    const char *port = getenv("PORT");
    long p = port ? strtol(port, NULL, 10) : 8080;

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

    printf("SLP debug viewer serving http://%s:%ld  (replays dir: %s, assets: %s)\n",
           host, p, g_dir, asset_dir());

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1024 * 1024);
        pthread_create(&t, &attr, conn_thread, (void *)(intptr_t)cfd);
        pthread_attr_destroy(&attr);
    }
    return 0;
}
