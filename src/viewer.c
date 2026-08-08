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
 *   GET /api/frame?n=<frame>  binary: [4B BE json_len][json][W*H*4 RGBA]
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
#include <unistd.h>

#include <zlib.h>

#include "parser.h"

#define FB_W 960
#define FB_H 540

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
    int32_t start_frame;
    int32_t last_frame;
    char name[256];
} active_t;

static active_t g_active;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_fb[FB_W * FB_H * 4];
static char g_dir[1024] = "./replays";

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

/* Frame rendering
 * ------------------------------------------------------------------ */

static const color_t port_colors[4] = {
    {255, 90, 90, 255},  {90, 150, 255, 255},
    {255, 220, 80, 255}, {90, 255, 150, 255},
};

/* Renders the full frame: stage (redrawn every frame for moving stages),
   items, and players. */
static void render_frame(const active_t *a, int32_t fn) {
    memset(g_fb, 40, sizeof g_fb); /* dark background */

    const slp_game_start_t *gs = &a->replay.game_start;
    draw_stage(gs, &a->cam);

    const slp_item_list_t *items = slp_items_at(&a->replay, fn);
    if (items) {
        for (size_t i = 0; i < items->count; i++) {
            const slp_item_t *it = &items->items[i];
            int sx, sy;
            world_to_screen(&a->cam, it->x, it->y, &sx, &sy);
            fill_rect(sx - 4, sy - 4, sx + 4, sy + 4,
                      (color_t){255, 255, 180, 220});
        }
    }

    for (unsigned port = 0; port < SLP_MAX_PORTS; port++) {
        if (!gs->has_player[port]) continue;
        color_t col = port_colors[port];
        color_t dark = {col.r / 2, col.g / 2, col.b / 2, 255};

        for (int pass = 0; pass < 2; pass++) {
            const slp_frame_t *f = slp_frame_at(&a->replay, port, pass != 0, fn);
            if (!f || f->frame_number != fn) continue;
            int sx, sy;
            world_to_screen(&a->cam, f->x, f->y, &sx, &sy);

            double half_w = 9, half_h = 17;
            if (pass == 1) { half_w = 7; half_h = 13; }
            double px = half_w * a->cam.scale, py = half_h * a->cam.scale;
            int rx = (int)px, ry = (int)py;

            /* body */
            fill_rect(sx - rx, sy - ry, sx + rx, sy + ry, dark);
            fill_rect(sx - rx + 2, sy - ry + 2, sx + rx - 2, sy + ry - 2, col);

            /* shield */
            if (f->shield_size > 0 && pass == 0) {
                double rad = f->shield_size * 0.8 * a->cam.scale;
                fill_circle(sx, sy, (int)rad,
                            (color_t){120, 200, 255, 110});
            }

            /* facing notch */
            int fx = sx + (f->facing >= 0 ? rx : -rx);
            fill_rect(fx - 2, sy - ry - 6, fx + 2, sy - ry + 6,
                      (color_t){255, 255, 255, 255});
        }
    }
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
static int read_request(int cfd, char *buf, size_t cap) {
    size_t used = 0;
    while (used < cap - 1) {
        ssize_t n = recv(cfd, buf + used, cap - 1 - used, 0);
        if (n <= 0) return -1;
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) return 0;
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

    pthread_mutex_lock(&g_lock);
    slp_replay_free(&g_active.replay);
    g_active.replay = replay;
    snprintf(g_active.name, sizeof g_active.name, "%s", name);
    compute_camera(&g_active.replay, &g_active.cam);
    g_active.start_frame = -SLP_FRAME_BASE;
    g_active.last_frame = replay.last_frame;
    if (g_active.last_frame == INT32_MIN)
        g_active.last_frame =
            (int32_t)((int64_t)replay.frame_count - SLP_FRAME_BASE - 1);
    pthread_mutex_unlock(&g_lock);
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
    memcpy(dst + 8, data, len);
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
    if (compress2(z, &clen, raw, raw_len, 6) != Z_OK) {
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

/* ------------------------------------------------------------------ */
/* Request handlers                                                    */
/* ------------------------------------------------------------------ */

static void handle_frame(int cfd, const char *query) {
    char nbuf[32];
    if (query_value(query, "n", nbuf, sizeof nbuf) != 0) {
        send_response(cfd, "text/plain", "missing n", 8);
        return;
    }
    int32_t fn = (int32_t)strtol(nbuf, NULL, 10);

    pthread_mutex_lock(&g_lock);
    render_frame(&g_active, fn);
    char json[16384];
    frame_json(&g_active, fn, json, sizeof json);
    uint8_t *png;
    size_t plen = png_encode(g_fb, FB_W, FB_H, &png);
    pthread_mutex_unlock(&g_lock);

    if (!plen) {
        send_response(cfd, "text/plain", "encode error", 12);
        return;
    }
    size_t jlen = strlen(json);
    size_t body_len = 4 + jlen + plen;
    uint8_t *body = malloc(body_len);
    if (!body) {
        free(png);
        send_response(cfd, "text/plain", "oom", 3);
        return;
    }
    body[0] = (uint8_t)(jlen >> 24);
    body[1] = (uint8_t)(jlen >> 16);
    body[2] = (uint8_t)(jlen >> 8);
    body[3] = (uint8_t)jlen;
    memcpy(body + 4, json, jlen);
    memcpy(body + 4 + jlen, png, plen);
    free(png);
    send_response(cfd, "application/octet-stream", body, body_len);
    free(body);
}

static void handle_info(int cfd) {
    pthread_mutex_lock(&g_lock);
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
    pthread_mutex_unlock(&g_lock);
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

static void handle_upload(int cfd, const char *query, size_t content_len) {
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
    if (read_body(cfd, body, content_len) != 0) {
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
    "<title>SLP Debug Viewer</title><style>"
    "body{background:#15171c;color:#ddd;font-family:ui-monospace,monospace;"
    "margin:0;padding:12px;text-align:center}"
    "h1{font-size:16px;font-weight:600;margin:0 0 8px;color:#9ab}"
    "#wrap{display:inline-block;position:relative}"
    "canvas{background:#000;image-rendering:pixelated;border:1px solid #333;"
    "border-radius:4px;max-width:100%}"
    "#hud{position:absolute;left:8px;top:8px;text-align:left;font-size:12px;"
    "color:#fff;text-shadow:1px 1px 2px #000;pointer-events:none;line-height:1.4}"
    "#hud b{display:inline-block;width:10px;height:10px;border-radius:2px;"
    "margin-right:5px;vertical-align:-1px}"
    "#bar{display:flex;gap:8px;align-items:center;justify-content:center;"
    "margin:10px auto;max-width:980px;flex-wrap:wrap}"
    "select,input[type=file],button{background:#24272e;color:#ddd;border:1px "
    "solid #333;border-radius:4px;padding:5px 8px;font:inherit;font-size:13px}"
    "input[type=range]{flex:1;min-width:200px}"
    "#frameLabel{font-size:13px;min-width:120px;text-align:left}"
    "#help{color:#666;font-size:11px;margin-top:6px}"
    "</style></head><body>"
    "<h1>SLP Debug Viewer</h1>"
    "<div id=wrap><canvas id=cv width=960 height=540></canvas>"
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
    "let info=null,playing=false,cur=0,raf=0,lastT=0,seq=0;"
    "const pngCache=new Map(),pending=new Map();"
    "let nextFill=0,inflight=0;"
    "const MAXF=4,PREFETCH=120;"
    "const W=960,H=540;"
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
    "function fillCache(){"
    "if(!info)return;"
    "const hi=Math.min(info.last,cur+PREFETCH);"
    "if(cur>nextFill)nextFill=cur+1;"
    "while(inflight<MAXF&&nextFill<=hi){"
    "const n=nextFill++;"
    "if(pngCache.has(n)||pending.has(n))continue;"
    "inflight++;"
    "requestFrame(n).finally(()=>{inflight--;});"
    "}"
    "}"
    "async function load(name){"
    "const r=await fetch('/api/set?f='+encodeURIComponent(name));info=await r.json();"
    "pngCache.clear();pending.clear();inflight=0;"
    "sl.min=info.start;sl.max=info.last;cur=info.start;sl.value=cur;"
    "nextFill=cur+1;"
    "document.title='SLP Debug Viewer - '+info.name;"
    "await show(cur);"
    "}"
    "async function show(n){"
    "const my=++seq;"
    "try{"
    "const ab=await requestFrame(n);"
    "if(my!==seq)return;"
    "const dv=new DataView(ab);"
    "const jlen=dv.getUint32(0,false);"
    "const st=JSON.parse(new TextDecoder().decode(new Uint8Array(ab,4+jlen)));"
    "const bmp=await createImageBitmap(new Blob([ab.slice(4+jlen)],{type:'image/png'}));"
    "if(my!==seq){bmp.close();return;}"
    "ctx.drawImage(bmp,0,0);bmp.close();"
    "fl.textContent=st.frame+' / '+info.last;"
    "hud.innerHTML=st.players.map(p=>{"
    "const c=COL[(p.port-1)%4];"
    "return`<b style=\"background:${c}\"></b>`+"
    "`${p.name||('P'+p.port)} ${p.char} ${p.percent.toFixed(1)}% x${p.stocks}`+"
    "`<span style=\"color:#888\"> ${p.state}</span><br>`;"
    "}).join('');"
    "fillCache();"
    "}catch(e){}"
    "}"
    "function toggle(){"
    "playing=!playing;playBtn.textContent=playing?'Pause':'Play';"
    "if(playing){lastT=performance.now();raf=requestAnimationFrame(loop);}"
    "else{cancelAnimationFrame(raf);}"
    "}"
    "function loop(t){"
    "if(!playing)return;"
    "const dt=t-lastT;lastT=t;"
    "if(dt>=16){"
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

static void handle_root(int cfd) {
    send_response(cfd, "text/html; charset=utf-8", html_doc, strlen(html_doc));
}

/* ------------------------------------------------------------------ */

static void *conn_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    struct timeval tv = {30, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[8192];
    if (read_request(cfd, req, sizeof req) == 0) {
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
        else if (strcmp(path, "/api/set") == 0)
            handle_set(cfd, query);
        else if (strcmp(path, "/api/upload") == 0) {
            char cl[32] = "0";
            header_value(req, "Content-Length", cl, sizeof cl);
            size_t clen = (size_t)strtoul(cl, NULL, 10);
            handle_upload(cfd, query, clen);
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
