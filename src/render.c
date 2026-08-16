/*
 * render.c - software 2D rasterizer for extracted Melee assets.
 *
 * Given an asset_model_t + asset_anims_t, samples an action at a frame,
 * computes per-bone animated world transforms, skins the vertices, projects
 * to a side-view 2D camera and fills triangles (painter's algorithm, flat
 * shading from primitive-group material flags). Produces RGBA into a caller
 * framebuffer. No windowing / no deps beyond libc + asset.c.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "asset.h"

/* ------------------------------------------------------------------ */
/* Matrix math (column-major 3x4, matching tools/extract conventions)  */
/* ------------------------------------------------------------------ */

typedef float mtx[12];

/* out = a * b (affine: rotation part multiplied, translation added) */
static void mtx_mul(const mtx a, const mtx b, mtx out) {
    float t[12];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 3; r++) {
            float acc = 0;
            for (int k = 0; k < 3; k++) acc += a[k * 3 + r] * b[c * 3 + k];
            t[c * 3 + r] = acc;
        }
    /* The c=3 pass already computes a.rotation * b.translation.  Finish
       the affine product by adding a.translation; do not replace it with
       the unrotated sum. */
    t[9] += a[9]; t[10] += a[10]; t[11] += a[11];
    memcpy(out, t, sizeof t);
}

/* build transform from scale/rotation(ZYX)/translation (radians) */
static void mtx_from_srt(const float *s, const float *rot, const float *t, mtx out) {
    float rx = rot[0], ry = rot[1], rz = rot[2];
    float ti = rz * 0.5f, tj = ry * 0.5f, th = rx * 0.5f;
    float si = sinf(ti), ci = cosf(ti), sj = sinf(tj), cj = cosf(tj),
          sh = sinf(th), ch = cosf(th);
    float cc = ci * ch, cs = ci * sh, sc = si * ch, ss = si * sh;
    float qx = cj * cs - sj * sc, qy = cj * ss + sj * cc,
          qz = cj * sc - sj * cs, qw = cj * cc + sj * ss;
    if (qw < 0) { qx = -qx; qy = -qy; qz = -qz; qw = -qw; }
    float x2 = qx + qx, y2 = qy + qy, z2 = qz + qz;
    float xx = qx * x2, xy = qx * y2, xz = qx * z2;
    float yy = qy * y2, yz = qy * z2, zz = qz * z2;
    float wx = qw * x2, wy = qw * y2, wz = qw * z2;
    float sx = s[0], sy = s[1], sz = s[2];
    out[0] = (1 - (yy + zz)) * sx; out[1] = (xy + wz) * sx; out[2] = (xz - wy) * sx;
    out[3] = (xy - wz) * sy; out[4] = (1 - (xx + zz)) * sy; out[5] = (yz + wx) * sy;
    out[6] = (xz + wy) * sz; out[7] = (yz - wx) * sz; out[8] = (1 - (xx + yy)) * sz;
    out[9] = t[0]; out[10] = t[1]; out[11] = t[2];
}

/* apply affine 3x4 matrix to a 3D point */
static void mtx_apply(const mtx m, const float *p, float *out) {
    out[0] = m[0]*p[0] + m[3]*p[1] + m[6]*p[2] + m[9];
    out[1] = m[1]*p[0] + m[4]*p[1] + m[7]*p[2] + m[10];
    out[2] = m[2]*p[0] + m[5]*p[1] + m[8]*p[2] + m[11];
}

/* ------------------------------------------------------------------ */
/* Animation sampling                                                  */
/* ------------------------------------------------------------------ */

static float sample_key(const asset_track_t *tk, float frame) {
    const asset_key_t *k = tk->keys;
    uint32_t n = tk->key_count;
    if (n == 0) return 0;
    if (frame <= k[0].frame) return k[0].value;
    if (frame >= k[n - 1].frame) return k[n - 1].value;
    /* find bracketing keys */
    uint32_t lo = 0, hi = n - 1;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (k[mid].frame <= frame) lo = mid; else hi = mid;
    }
    const asset_key_t *a = &k[lo], *b = &k[hi];
    float span = b->frame - a->frame;
    float t = (span > 0) ? (frame - a->frame) / span : 0;
    switch (a->interp) {
        case ATK_STEP:   return a->value;
        case ATK_LINEAR: return a->value + (b->value - a->value) * t;
        default: { /* hermite; guard against broken/oversized frame spans */
            float t2 = t * t, t3 = t2 * t;
            float h00 = 2*t3 - 3*t2 + 1, h10 = t3 - 2*t2 + t;
            float h01 = -2*t3 + 3*t2,   h11 = t3 - t2;
            float v = h00*a->value + h10*a->out_tan*span +
                      h01*b->value + h11*b->in_tan*span;
            /* if the frame delta looks corrupt the tangent*span term explodes;
               fall back to linear when the hermite result is wildly off the
               two bracketing values */
            float vlo = fminf(a->value, b->value), vhi = fmaxf(a->value, b->value);
            float margin = (vhi - vlo) * 2.0f + 1.0f;
            if (v < vlo - margin || v > vhi + margin)
                v = a->value + (b->value - a->value) * t;
            return v;
        }
    }
}

/* sample all channels of a joint: decompose the base transform to SRT,
   override the channels the animation drives, and rebuild. Mirrors the
   reference implementation (start from base, not identity). */
static void sample_joint(const asset_joint_anim_t *ja, float frame,
                         const asset_bone_t *base, float *scl, float *rot,
                         float *trs) {
    const float *m = base->base;
    float x_axis[3] = {m[0], m[1], m[2]};
    float y_axis[3] = {m[3], m[4], m[5]};
    float z_axis[3] = {m[6], m[7], m[8]};
    scl[0] = sqrtf(x_axis[0]*x_axis[0] + x_axis[1]*x_axis[1] + x_axis[2]*x_axis[2]);
    scl[1] = sqrtf(y_axis[0]*y_axis[0] + y_axis[1]*y_axis[1] + y_axis[2]*y_axis[2]);
    scl[2] = sqrtf(z_axis[0]*z_axis[0] + z_axis[1]*z_axis[1] + z_axis[2]*z_axis[2]);
    trs[0] = m[9]; trs[1] = m[10]; trs[2] = m[11];

    /* normalized rotation axes (euler ZYX extraction, column-major) */
    float ix = 1.0f / (scl[0] > 1e-9f ? scl[0] : 1.0f);
    float iy = 1.0f / (scl[1] > 1e-9f ? scl[1] : 1.0f);
    float iz = 1.0f / (scl[2] > 1e-9f ? scl[2] : 1.0f);
    float m00 = x_axis[0]*ix, m10 = x_axis[1]*ix, m20 = x_axis[2]*ix;
    float m11 = y_axis[1]*iy, m21 = y_axis[2]*iy;
    float m12 = z_axis[1]*iz, m22 = z_axis[2]*iz;
    float ry = asinf(fmaxf(-1.0f, fminf(1.0f, -m20)));
    float cy = cosf(ry);
    float rx = 0, rz = 0;
    if (fabsf(cy) > 1e-5f) {
        rx = atan2f(m21, m22);
        rz = atan2f(m10, m00);
    } else {
        rx = atan2f(-m12, m11);
    }
    rot[0] = rx; rot[1] = ry; rot[2] = rz;

    /* HSD JOBJ rotation tracks use radians, matching the base transform. */
    for (uint32_t i = 0; i < ja->track_count; i++) {
        const asset_track_t *tk = &ja->tracks[i];
        if(frame<(float)tk->start_frame)continue;
        float v = sample_key(tk, frame-(float)tk->start_frame);
        switch (tk->channel) {
            case 1: rot[0] = v; break;
            case 2: rot[1] = v; break;
            case 3: rot[2] = v; break;
            case 5: trs[0] = v; break;
            case 6: trs[1] = v; break;
            case 7: trs[2] = v; break;
            case 8: scl[0] = v; break;
            case 9: scl[1] = v; break;
            case 10: scl[2] = v; break;
            case 11: trs[0] = v; break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Pose evaluation                                                     */
/* ------------------------------------------------------------------ */

/* Compute world matrices for every bone.  Extracted POBJ positions are local
   to their owning JOBJ, so rigid vertices are transformed by these matrices
   directly (rather than by animated_world * inverse_bind). */
static void eval_pose(const asset_model_t *m, const asset_anims_t *a,
                      uint32_t action_idx, float frame, mtx *world_out) {
    /* animated local transforms: base pose, overridden by animation */
    mtx *local = malloc((size_t)m->bone_count * sizeof(mtx));
    for (uint32_t i = 0; i < m->bone_count; i++) memcpy(local[i], m->bones[i].base, sizeof(mtx));

    if (action_idx != UINT32_MAX && a) {
        const asset_action_t *act = &a->actions[action_idx];
        float frame_number=frame<=0.0f?1.0f:frame+1.0f;
        if(act->loop&&act->end_frame>0.0f)
            while(frame_number>act->end_frame)frame_number-=act->end_frame;
        for (uint32_t j = 0; j < act->joint_count; j++) {
            const asset_joint_anim_t *ja = &act->joints[j];
            if (ja->bone_index >= m->bone_count) continue;
            float scl[3], rot[3], trs[3];
            sample_joint(ja, frame_number, &m->bones[ja->bone_index], scl, rot, trs);
            /* Slippi's post-frame x/y is the fighter's authoritative world
               root.  Applying figatree root translation as well double-counts
               locomotion and can put grounded poses through the floor. */
            if (ja->bone_index == 0) {
                trs[0] = m->bones[0].base[9];
                trs[1] = m->bones[0].base[10];
                trs[2] = m->bones[0].base[11];
            }
            mtx_from_srt(scl, rot, trs, local[ja->bone_index]);
        }
    }

    /* world + skin */
    for (uint32_t i = 0; i < m->bone_count; i++) {
        const asset_bone_t *b = &m->bones[i];
        if (b->parent == UINT16_MAX) {
            memcpy(world_out[i], local[i], sizeof(mtx));
        } else {
            mtx_mul(world_out[b->parent], local[i], world_out[i]);
        }
    }
    free(local);
}

/* ------------------------------------------------------------------ */
/* Rasterizer                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *fb;       /* RGBA, W*H*4 */
    int W, H;
} fb_t;

typedef struct { uint32_t i0, i1, i2, pg; float z; } tri_ref_t;

static int cmp_tri_depth(const void *av, const void *bv) {
    const tri_ref_t *a = av, *b = bv;
    return a->z < b->z ? -1 : a->z > b->z ? 1 : 0;
}

static void put_px(fb_t *f, int x, int y, uint32_t rgba) {
    if (x < 0 || y < 0 || x >= f->W || y >= f->H) return;
    uint8_t *p = &f->fb[(y * f->W + x) * 4];
    uint8_t a = (uint8_t)(rgba >> 24), r = (uint8_t)(rgba >> 16),
            g = (uint8_t)(rgba >> 8), bb = (uint8_t)rgba;
    p[0] = (uint8_t)((r * a + p[0] * (255 - a)) / 255);
    p[1] = (uint8_t)((g * a + p[1] * (255 - a)) / 255);
    p[2] = (uint8_t)((bb * a + p[2] * (255 - a)) / 255);
    p[3] = 255;
}

static float clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Affine UV mapping is sufficient for the current orthographic side view.
   Texture pixels are stored as ordinary RGBA by the extractor. */
static void fill_tri_textured(fb_t *f, float x0, float y0, float u0, float v0, const uint8_t *c0,
                              float x1, float y1, float u1, float v1,
                              const uint8_t *c1, float x2, float y2, float u2, float v2, const uint8_t *c2,
                              const asset_texture_t *tex, uint32_t tint) {
    float den = (y1-y2)*(x0-x2) + (x2-x1)*(y0-y2);
    if (fabsf(den) < 1e-8f) return;

    int xmin = (int)floorf(fminf(fminf(x0, x1), x2));
    int xmax = (int)ceilf (fmaxf(fmaxf(x0, x1), x2));
    int ymin = (int)floorf(fminf(fminf(y0, y1), y2));
    int ymax = (int)ceilf (fmaxf(fmaxf(y0, y1), y2));
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= f->W) xmax = f->W - 1;
    if (ymax >= f->H) ymax = f->H - 1;
    uint32_t tint_a=(tint>>24)&255,tint_r=(tint>>16)&255,
             tint_g=(tint>>8)&255,tint_b=tint&255;

    for (int y = ymin; y <= ymax; y++) {
        float py = (float)y + 0.5f;
        for (int x = xmin; x <= xmax; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((y1-y2)*(px-x2) + (x2-x1)*(py-y2)) / den;
            float w1 = ((y2-y0)*(px-x2) + (x0-x2)*(py-y2)) / den;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;

            float u = clamp01(w0*u0 + w1*u1 + w2*u2);
            float v = clamp01(w0*v0 + w1*v1 + w2*v2);
            uint8_t white[4]={255,255,255,255};const uint8_t*p=white;
            if(tex&&tex->rgba&&tex->width&&tex->height){
                uint32_t tx=(uint32_t)lroundf(u*(float)(tex->width-1));
                uint32_t ty=(uint32_t)lroundf(v*(float)(tex->height-1));
                p=&tex->rgba[((size_t)ty*tex->width+tx)*4];
            }
            uint32_t vr=(uint32_t)lroundf(w0*c0[0]+w1*c1[0]+w2*c2[0]);
            uint32_t vg=(uint32_t)lroundf(w0*c0[1]+w1*c1[1]+w2*c2[1]);
            uint32_t vb=(uint32_t)lroundf(w0*c0[2]+w1*c1[2]+w2*c2[2]);
            uint32_t va=(uint32_t)lroundf(w0*c0[3]+w1*c1[3]+w2*c2[3]);
            uint32_t a=(uint32_t)p[3]*tint_a*va/(255*255),
                     r=(uint32_t)p[0]*tint_r*vr/(255*255),
                     g=(uint32_t)p[1]*tint_g*vg/(255*255),
                     b=(uint32_t)p[2]*tint_b*vb/(255*255);
            if(a==0)continue;
            uint32_t rgba=(a<<24)|(r<<16)|(g<<8)|b;
            put_px(f, x, y, rgba);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public pose render API                                              */
/* ------------------------------------------------------------------ */

static void transform_pose_vertices(const asset_model_t *m,
                                    const asset_anims_t *a,
                                    uint32_t action_idx, float frame,
                                    float *px, float *py, float *pz) {
    mtx *world = malloc((size_t)m->bone_count * sizeof(mtx));
    eval_pose(m, a, action_idx, frame, world);

    for (uint32_t i = 0; i < m->vertex_count; i++) {
        const asset_vertex_t *v = &m->vertices[i];
        float acc[3] = {0, 0, 0};
        int influence_count = 0;
        for (int k = 0; k < 4; k++)
            if (v->weight[k] > 0.0001f && v->bone[k] < m->bone_count)
                influence_count++;

        float wsum = 0;
        for (int k = 0; k < 4; k++) {
            float w = v->weight[k];
            uint16_t b = v->bone[k];
            if (w <= 0.0001f || b >= m->bone_count) continue;
            float tmp[3];
            if (influence_count > 1) {
                mtx skin;
                mtx_mul(world[b], m->bones[b].inv_world, skin);
                mtx_apply(skin, v->pos, tmp);
            } else {
                mtx_apply(world[b], v->pos, tmp);
            }
            acc[0] += tmp[0] * w;
            acc[1] += tmp[1] * w;
            acc[2] += tmp[2] * w;
            wsum += w;
        }
        if (wsum > 0) {
            acc[0] /= wsum;
            acc[1] /= wsum;
            acc[2] /= wsum;
        } else {
            acc[0] = v->pos[0];
            acc[1] = v->pos[1];
            acc[2] = v->pos[2];
        }
        px[i] = acc[0];
        py[i] = acc[1];
        if (pz) pz[i] = acc[2];
    }
    free(world);
}

int render_pose_bounds(const asset_model_t *m, const asset_anims_t *a,
                       uint32_t action_idx, float frame, float bounds[4]) {
    if (!m || !m->vertex_count || !bounds) return -1;
    float *px = malloc((size_t)m->vertex_count * sizeof(float));
    float *py = malloc((size_t)m->vertex_count * sizeof(float));
    if (!px || !py) { free(px); free(py); return -1; }
    transform_pose_vertices(m, a, action_idx, frame, px, py, NULL);
    bounds[0] = bounds[2] = px[0];
    bounds[1] = bounds[3] = py[0];
    for (uint32_t i = 1; i < m->vertex_count; i++) {
        if (px[i] < bounds[0]) bounds[0] = px[i];
        if (py[i] < bounds[1]) bounds[1] = py[i];
        if (px[i] > bounds[2]) bounds[2] = px[i];
        if (py[i] > bounds[3]) bounds[3] = py[i];
    }
    free(px); free(py);
    return 0;
}

/* Find an action by (exact, prefix, or substring) name. */
uint32_t render_find_action(const asset_anims_t *a, const char *name) {
    if (!name || !name[0]) return UINT32_MAX;
    for (uint32_t i = 0; i < a->action_count; i++) {
        if (strcasecmp(a->actions[i].name, name) == 0) return i;
        if (strncmp(a->actions[i].name, name, strlen(name)) == 0) return i;
        if (strstr(a->actions[i].name, name)) return i;
    }
    return UINT32_MAX;
}

/* Rasterize a posed model into fb. Returns number of triangles drawn.
   cam: world->screen affine (sx = (wx*scale)+tx, sy = -(wy*scale)+ty).
   facing < 0 mirrors on x. action_idx UINT32_MAX = bind pose. */
static size_t render_pose_projected(const asset_model_t *m,
                          const asset_anims_t *a, uint32_t action_idx,
                          float frame, int facing, float scale, float tx,
                          float ty, float z_to_x, float z_to_y, int profile,
                          uint8_t *fb, int W, int H) {
    fb_t f = {fb, W, H};

    /* Transform and project every vertex once. */
    float *sx = malloc((size_t)m->vertex_count * sizeof(float));
    float *sy = malloc((size_t)m->vertex_count * sizeof(float));
    float *px = malloc((size_t)m->vertex_count * sizeof(float));
    float *py = malloc((size_t)m->vertex_count * sizeof(float));
    float *pz = malloc((size_t)m->vertex_count * sizeof(float));
    transform_pose_vertices(m, a, action_idx, frame, px, py, pz);
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        float x = profile ? pz[i] : px[i] + pz[i] * z_to_x;
        if (facing < 0) x = -x;
        sx[i] = x * scale + tx;
        sy[i] = -((py[i] + pz[i] * z_to_y) * scale) + ty;
    }

    size_t tri_cap=(size_t)m->index_count/3,tri_count=0;
    tri_ref_t*tri_refs=malloc((tri_cap?tri_cap:1)*sizeof*tri_refs);
    for (uint32_t pg = 0; pg < m->pgroup_count; pg++) {
        const asset_pgroup_t *p = &m->pgroups[pg];
        uint32_t start = p->indices_start, len = p->indices_len;
        if (start + len > m->index_count) len = m->index_count - start;
        for(uint32_t i=0;i+2<len;i+=3){
            uint32_t i0=m->indices[start+i],i1=m->indices[start+i+1],i2=m->indices[start+i+2];
            if(i0>=m->vertex_count||i1>=m->vertex_count||i2>=m->vertex_count)continue;
            float d0=profile?px[i0]:pz[i0],d1=profile?px[i1]:pz[i1],
                  d2=profile?px[i2]:pz[i2];
            tri_refs[tri_count++]=(tri_ref_t){i0,i1,i2,pg,(d0+d1+d2)/3.0f};
        }
    }
    qsort(tri_refs,tri_count,sizeof*tri_refs,cmp_tri_depth);

    for(size_t ti=0;ti<tri_count;ti++){
        uint32_t pg=tri_refs[ti].pg;
        const asset_pgroup_t*p=&m->pgroups[pg];
        /* Use extracted MOBJ material colors; old caches fall back to a
           deterministic shade derived from the render flags. */
        uint32_t mf = p->mobj_flags;
        uint32_t r = 150 + ((mf >> 4) & 0x3F), g = 170 + ((mf >> 12) & 0x3F),
                 b = 200 + ((mf >> 20) & 0x3F);
        uint32_t alpha=255;
        if(pg<m->phong_count){
            const asset_phong_t*mat=&m->phongs[pg];
            r=mat->diffuse[0];g=mat->diffuse[1];b=mat->diffuse[2];
            float ma=clamp01(mat->alpha);
            alpha=(uint32_t)lroundf(ma*(float)mat->diffuse[3]);
        }
        uint32_t c=(alpha<<24)|(r<<16)|(g<<8)|b;
        const asset_texture_t *tex = NULL;
        if (p->texture_idx >= 0 && (uint32_t)p->texture_idx < m->texture_count)
            tex = &m->textures[p->texture_idx];
        {
            uint32_t i0=tri_refs[ti].i0,i1=tri_refs[ti].i1,i2=tri_refs[ti].i2;
            if (tex) {
                fill_tri_textured(&f,
                    sx[i0], sy[i0], m->vertices[i0].uv[0], m->vertices[i0].uv[1],m->vertices[i0].color,
                    sx[i1], sy[i1], m->vertices[i1].uv[0], m->vertices[i1].uv[1],m->vertices[i1].color,
                    sx[i2], sy[i2], m->vertices[i2].uv[0], m->vertices[i2].uv[1],m->vertices[i2].color,
                    tex,c);
            } else {
                fill_tri_textured(&f,
                    sx[i0],sy[i0],0,0,m->vertices[i0].color,
                    sx[i1],sy[i1],0,0,m->vertices[i1].color,
                    sx[i2],sy[i2],0,0,m->vertices[i2].color,NULL,c);
            }
        }
    }

    free(tri_refs);free(pz);free(px);free(py);free(sx);free(sy);
    return tri_count;
}

size_t render_pose_tilted(const asset_model_t *m, const asset_anims_t *a,
                          uint32_t action_idx, float frame, int facing,
                          float scale, float tx, float ty,
                          float z_to_x, float z_to_y,
                          uint8_t *fb, int W, int H) {
    return render_pose_projected(m, a, action_idx, frame, facing, scale, tx,
                                 ty, z_to_x, z_to_y, 0, fb, W, H);
}

size_t render_pose(const asset_model_t *m, const asset_anims_t *a,
                   uint32_t action_idx, float frame, int facing,
                   float scale, float tx, float ty,
                   uint8_t *fb, int W, int H) {
    return render_pose_tilted(m, a, action_idx, frame, facing, scale, tx, ty,
                              0.0f, 0.0f, fb, W, H);
}

size_t render_pose_profile(const asset_model_t *m, const asset_anims_t *a,
                           uint32_t action_idx, float frame, int facing,
                           float scale, float tx, float ty,
                           uint8_t *fb, int W, int H) {
    return render_pose_projected(m, a, action_idx, frame, facing, scale, tx,
                                 ty, 0.0f, 0.0f, 1, fb, W, H);
}
