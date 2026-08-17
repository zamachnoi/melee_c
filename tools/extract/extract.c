/*
 * extract.c - melee_c asset extractor (ISO -> cache).
 *
 * Reads a GameCube Melee ISO, walks FST -> DAT -> HSD (JOBJ/DOBJ/POBJ/MOBJ/
 * TOBJ), decodes fighter models + figatree animations + stages, and writes
 * the binary cache consumed by src/asset.c (see src/asset.h).
 *
 * `--effects` walks Ef*Data.dat tables, ItCo.dat articles, and each fighter's
 * ftData item list, then writes schema-4 `.model` files plus `effects.json`.
 *
 * Usage: extract --iso=game.iso --out=cache [--char=falco] [--stage=FD] [--effects] [--icons]
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <zlib.h>

#include "../../src/asset.h"

/* ================================================================== */
/* Byte readers (big-endian)                                          */
/* ================================================================== */

static uint16_t r16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static uint32_t r32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static float rf32(const uint8_t *p){uint32_t x=r32(p);float f;memcpy(&f,&x,4);return f;}
static uint16_t r16le(const uint8_t *p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t r32le(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static float rf32le(const uint8_t *p){uint32_t x=r32le(p);float f;memcpy(&f,&x,4);return f;}
static void die(const char*m){fprintf(stderr,"extract: %s\n",m);exit(1);}

/* ================================================================== */
/* ISO / FST                                                          */
/* ================================================================== */

typedef struct {char *path;uint32_t offset,size;} fst_file_t;
typedef struct {fst_file_t *items;size_t count,cap;} fst_list_t;

static void fst_push(fst_list_t*l,fst_file_t e){
    if(l->count==l->cap){l->cap=l->cap?l->cap*2:256;l->items=realloc(l->items,l->cap*sizeof(fst_file_t));if(!l->items)die("oom");}
    l->items[l->count++]=e;
}

static fst_list_t iso_index_dats(const uint8_t*iso,size_t len){
    fst_list_t out={0};
    if(len<0x430)return out;
    uint32_t fst_off=r32(iso+0x424),fst_size=r32(iso+0x428);
    if(fst_off>=len||fst_size>len-fst_off)return out;
    const uint8_t*fst=iso+fst_off;
    uint32_t entry_count=r32(fst+8);
    if(entry_count==0||entry_count>fst_size/12)return out;
    size_t name_base=(size_t)entry_count*12;
    typedef struct{uint32_t end;char path[1024];}DirFrame;
    DirFrame stack[128];size_t sp=0;char cur[1024]="";
    for(uint32_t i=1;i<entry_count;i++){
        const uint8_t*e=fst+(size_t)i*12;
        while(sp>0&&i>=stack[sp-1].end){sp--;cur[0]=0;if(sp>0)snprintf(cur,sizeof cur,"%s",stack[sp-1].path);}
        uint32_t tn=r32(e);uint32_t is_dir=(tn>>24)&0xFF;uint32_t no=tn&0xFFFFFF;
        char name[256];name[0]=0;
        if(name_base+no<fst_size){size_t avail=fst_size-(name_base+no);size_t want=avail<255?avail:255;memcpy(name,fst+name_base+no,want);name[want]=0;}
        if(is_dir){
            uint32_t end=r32(e+8);if(sp>=128)die("dir overflow");
            DirFrame*df=&stack[sp++];df->end=end;
            if(cur[0])snprintf(df->path,sizeof df->path,"%s/%s",cur,name);else snprintf(df->path,sizeof df->path,"%s",name);
            snprintf(cur,sizeof cur,"%s",df->path);
        }else{
            uint32_t off=r32(e+4),size=r32(e+8);size_t n=strlen(name);
            if(n>=4&&strcasecmp(name+n-4,".dat")==0){
                char full[1024];
                if(cur[0])snprintf(full,sizeof full,"%s/%s",cur,name);else snprintf(full,sizeof full,"%s",name);
                fst_push(&out,(fst_file_t){.path=strdup(full),.offset=off,.size=size});
            }
        }
    }
    return out;
}

static const fst_file_t*iso_find(const fst_list_t*l,const char*base){
    for(size_t i=0;i<l->count;i++){
        const char*p=l->items[i].path;
        const char*slash=strrchr(p,'/');
        const char*bn=slash?slash+1:p;
        if(strcasecmp(bn,base)==0)return &l->items[i];
    }
    return NULL;
}

/* ================================================================== */
/* DAT container                                                      */
/* ================================================================== */

typedef struct{
    const uint8_t*bytes;size_t len;
    uint32_t data_block_size,reloc_count,root_count,ref_count;
    size_t reloc_start,roots_start,refs_start,string_start;
}dat_t;

static int dat_open(const uint8_t*bytes,size_t len,dat_t*d){
    memset(d,0,sizeof*d);d->bytes=bytes;d->len=len;
    if(len<0x20)return -1;
    if(r32(bytes)!=(uint32_t)len)return -2;
    d->data_block_size=r32(bytes+4);d->reloc_count=r32(bytes+8);
    d->root_count=r32(bytes+0x0C);d->ref_count=r32(bytes+0x10);
    d->reloc_start=0x20+d->data_block_size;
    d->roots_start=d->reloc_start+(size_t)d->reloc_count*4;
    d->refs_start=d->roots_start+(size_t)d->root_count*8;
    d->string_start=d->refs_start+(size_t)d->ref_count*8;
    if(d->reloc_start>len||d->roots_start>len||d->refs_start>len||d->string_start>len)return -3;
    return 0;
}
static const char*dat_name(const dat_t*d,uint32_t rel){size_t o=d->string_start+rel;return o<d->len?(const char*)d->bytes+o:"";}
static uint32_t dat_abs(const dat_t*d,uint32_t rel){(void)d;return 0x20+rel;}
static const uint8_t*dat_at(const dat_t*d,uint32_t rel){uint32_t a=dat_abs(d,rel);return a<d->len?d->bytes+a:NULL;}

typedef struct{uint32_t*offs;size_t count;}reloc_idx_t;
static int cmp_u32(const void*a,const void*b){uint32_t x=*(const uint32_t*)a,y=*(const uint32_t*)b;return x<y?-1:x>y?1:0;}
static void reloc_build(const dat_t*d,reloc_idx_t*ri){
    ri->offs=malloc((d->reloc_count?d->reloc_count:1)*sizeof(uint32_t));ri->count=0;
    for(uint32_t i=0;i<d->reloc_count;i++){
        uint32_t rel=r32(d->bytes+d->reloc_start+(size_t)i*4),a=dat_abs(d,rel);
        if(a<d->len)ri->offs[ri->count++]=a;
    }
    qsort(ri->offs,ri->count,sizeof(uint32_t),cmp_u32);
    size_t w=0;for(size_t i=0;i<ri->count;i++){if(w&&ri->offs[w-1]==ri->offs[i])continue;ri->offs[w++]=ri->offs[i];}
    ri->count=w;
}
static bool reloc_has(const reloc_idx_t*ri,uint32_t a){
    size_t lo=0,hi=ri->count;
    while(lo<hi){size_t m=(lo+hi)/2;if(ri->offs[m]<a)lo=m+1;else hi=m;}
    return lo<ri->count&&ri->offs[lo]==a;
}

/* Read a data-relative pointer that may or may not be relocated. Returns
 * resolved absolute offset or 0. Accepts either a reloc-field pointer or a
 * plain pointer that lands in the data block. */
static uint32_t rdptr(const dat_t*d,const reloc_idx_t*ri,uint32_t node_rel,uint32_t off){
    uint32_t field=dat_abs(d,node_rel)+off;
    if(field+4>d->len)return 0;
    uint32_t raw=r32(d->bytes+field);
    uint32_t t=dat_abs(d,raw);
    /* A relocated zero is a valid pointer to data-block offset zero.  Check
       relocation membership before applying the usual raw-zero NULL rule. */
    if(reloc_has(ri,field))return (t<d->len&&t<d->reloc_start)?t:0;
    if(raw==0)return 0;
    /* not a reloc field: only accept if it resolves into the data block */
    return (t<d->len&&t<d->reloc_start)?t:0;
}

/* Length of a pointed-to DAT node: the next root/reference/relocation target
 * starts the following node. */
static size_t node_span_len(const dat_t*d,const reloc_idx_t*ri,uint32_t start){
    uint32_t end=(uint32_t)d->reloc_start;
    for(uint32_t i=0;i<d->root_count;i++){
        uint32_t p=dat_abs(d,r32(d->bytes+d->roots_start+(size_t)i*8));
        if(p>start&&p<end)end=p;
    }
    for(uint32_t i=0;i<d->ref_count;i++){
        uint32_t p=dat_abs(d,r32(d->bytes+d->refs_start+(size_t)i*8));
        if(p>start&&p<end)end=p;
    }
    for(size_t i=0;i<ri->count;i++){
        uint32_t field=ri->offs[i];
        if(field+4>d->len)continue;
        uint32_t p=dat_abs(d,r32(d->bytes+field));
        if(p>start&&p<end&&p<d->reloc_start)end=p;
    }
    return end>start?(size_t)(end-start):0;
}

typedef struct{bool enabled;uint8_t group[256];}dobj_filter_t;

/* Fighter data contains costume lookup tables whose byte values select the
 * high-poly DOBJ indices from the mesh DAT. */
static int parse_high_poly_filter(const dat_t*d,dobj_filter_t*out){
    memset(out,0,sizeof*out);memset(out->group,0xFF,sizeof out->group);
    reloc_idx_t ri;reloc_build(d,&ri);
    uint32_t lookup_tables=0;
    for(uint32_t i=0;i<d->root_count&&!lookup_tables;i++){
        uint32_t root_rel=r32(d->bytes+d->roots_start+(size_t)i*8);
        lookup_tables=rdptr(d,&ri,root_rel,0x08);
    }
    if(!lookup_tables){free(ri.offs);return -1;}
    uint32_t costume_array=rdptr(d,&ri,lookup_tables-0x20,0x04);
    if(!costume_array){free(ri.offs);return -1;}
    size_t costume_count=node_span_len(d,&ri,costume_array)/0x10;
    if(!costume_count){free(ri.offs);return -1;}
    uint32_t high_poly_array=rdptr(d,&ri,costume_array-0x20,0x00);
    if(!high_poly_array){free(ri.offs);return -1;}
    size_t high_poly_count=node_span_len(d,&ri,high_poly_array)/0x08;
    if(!high_poly_count){free(ri.offs);return -1;}

    uint8_t group=0;size_t selected=0;
    for(size_t hi=0;hi<high_poly_count;hi++){
        uint32_t table=high_poly_array+(uint32_t)(hi*8);
        uint32_t entries=rdptr(d,&ri,table-0x20,0x04);
        if(!entries)continue;
        size_t entry_count=node_span_len(d,&ri,entries)/0x08;
        for(size_t ei=0;ei<entry_count;ei++,group++){
            uint32_t entry=entries+(uint32_t)(ei*8);
            int32_t count=(int32_t)r32(d->bytes+entry);
            if(count<=0)continue;
            uint32_t values=rdptr(d,&ri,entry-0x20,0x04);
            if(!values||(size_t)count>node_span_len(d,&ri,values))continue;
            for(int32_t vi=0;vi<count;vi++){
                uint8_t dobj=d->bytes[values+(uint32_t)vi];
                if(out->group[dobj]==0xFF){out->group[dobj]=group;selected++;}
            }
        }
    }
    free(ri.offs);
    out->enabled=selected>0;
    return out->enabled?0:-1;
}

/* ================================================================== */
/* Matrix math                                                        */
/* ================================================================== */

typedef float mtx3x4[12];
static void mtx_identity(mtx3x4 m){memset(m,0,12*sizeof(float));m[0]=m[4]=m[8]=1;}
static void mtx_mul(const mtx3x4 a,const mtx3x4 b,mtx3x4 out){
    float t[12];int c,r,k;
    for(c=0;c<4;c++)for(r=0;r<3;r++){
        float acc=0;for(k=0;k<3;k++)acc+=a[k*3+r]*b[c*3+k];t[c*3+r]=acc;
    }
    t[9]+=a[9];t[10]+=a[10];t[11]+=a[11];
    memcpy(out,t,sizeof t);
}
static void mtx_from_srt(const float*s,const float*rot,const float*t,mtx3x4 out){
    float rx=rot[0],ry=rot[1],rz=rot[2];
    float ti=rz*0.5f,tj=ry*0.5f,th=rx*0.5f;
    float si=sinf(ti),ci=cosf(ti),sj=sinf(tj),cj=cosf(tj),sh=sinf(th),ch=cosf(th);
    float cc=ci*ch,cs=ci*sh,sc=si*ch,ss=si*sh;
    float qx=cj*cs-sj*sc,qy=cj*ss+sj*cc,qz=cj*sc-sj*cs,qw=cj*cc+sj*ss;
    if(qw<0){qx=-qx;qy=-qy;qz=-qz;qw=-qw;}
    float x2=qx+qx,y2=qy+qy,z2=qz+qz,xx=qx*x2,xy=qx*y2,xz=qx*z2,yy=qy*y2,yz=qy*z2,zz=qz*z2,wx=qw*x2,wy=qw*y2,wz=qw*z2;
    float sx=s[0],sy=s[1],sz=s[2];
    out[0]=(1-(yy+zz))*sx;out[1]=(xy+wz)*sx;out[2]=(xz-wy)*sx;
    out[3]=(xy-wz)*sy;out[4]=(1-(xx+zz))*sy;out[5]=(yz+wx)*sy;
    out[6]=(xz+wy)*sz;out[7]=(yz-wx)*sz;out[8]=(1-(xx+yy))*sz;
    out[9]=t[0];out[10]=t[1];out[11]=t[2];
}
static void mtx_invert_affine(const mtx3x4 m,mtx3x4 out){
    float a00=m[0],a01=m[3],a02=m[6],a10=m[1],a11=m[4],a12=m[7],a20=m[2],a21=m[5],a22=m[8],tx=m[9],ty=m[10],tz=m[11];
    float det=a00*(a11*a22-a12*a21)-a01*(a10*a22-a12*a20)+a02*(a10*a21-a11*a20);
    if(fabsf(det)<1e-9f){mtx_identity(out);return;}
    float id=1.0f/det;
    float i00=(a11*a22-a12*a21)*id,i01=(a02*a21-a01*a22)*id,i02=(a01*a12-a02*a11)*id;
    float i10=(a12*a20-a10*a22)*id,i11=(a00*a22-a02*a20)*id,i12=(a02*a10-a00*a12)*id;
    float i20=(a10*a21-a11*a20)*id,i21=(a01*a20-a00*a21)*id,i22=(a00*a11-a01*a10)*id;
    out[0]=i00;out[1]=i10;out[2]=i20;out[3]=i01;out[4]=i11;out[5]=i21;out[6]=i02;out[7]=i12;out[8]=i22;
    out[9]=-(i00*tx+i01*ty+i02*tz);out[10]=-(i10*tx+i11*ty+i12*tz);out[11]=-(i20*tx+i21*ty+i22*tz);
}

/* ================================================================== */
/* Texture decode (TPL -> RGBA)                                       */
/* ================================================================== */

static uint32_t pal_entry_rgb5a3(uint16_t p){
    if(p&(1<<15)){
        uint8_t r=(uint8_t)((((p>>10)&0x1F)*255)/31),g=(uint8_t)((((p>>5)&0x1F)*255)/31),b=(uint8_t)(((p&0x1F)*255)/31);
        return 0xFF000000u|((uint32_t)r)|((uint32_t)g<<8)|((uint32_t)b<<16);
    }
    uint8_t a=(uint8_t)((((p>>12)&7)*255)/7),r=(uint8_t)((((p>>8)&0xF)*255)/15),g=(uint8_t)((((p>>4)&0xF)*255)/15),b=(uint8_t)(((p&0xF)*255)/15);
    return ((uint32_t)a<<24)|((uint32_t)r)|((uint32_t)g<<8)|((uint32_t)b<<16);
}
static uint32_t pal_entry_rgb565(uint16_t p){
    uint8_t r=(uint8_t)(((p>>11)&0x1F)<<3),g=(uint8_t)(((p>>5)&0x3F)<<2),b=(uint8_t)((p&0x1F)<<3);
    return 0xFF000000u|((uint32_t)r)|((uint32_t)g<<8)|((uint32_t)b<<16);
}
static uint32_t pal_entry_ia8(uint16_t p){
    uint8_t i=(uint8_t)(p&0xFF),a=(uint8_t)(p>>8);
    return ((uint32_t)a<<24)|((uint32_t)i)|((uint32_t)i<<8)|((uint32_t)i<<16);
}

static void tex_put(uint8_t*rgba,uint32_t idx,uint32_t v){
    rgba[idx*4+0]=(uint8_t)(v&0xFF);
    rgba[idx*4+1]=(uint8_t)((v>>8)&0xFF);
    rgba[idx*4+2]=(uint8_t)((v>>16)&0xFF);
    rgba[idx*4+3]=(uint8_t)((v>>24)&0xFF);
}

static void decode_texture(uint32_t fmt,uint16_t w,uint16_t h,const uint8_t*data,
                           const uint32_t*pal,size_t paln,uint8_t*rgba){
    size_t npx=(size_t)w*h;
    memset(rgba,0,npx*4);
    size_t inp=0;
    switch(fmt){
        case 0:{ /* I4 */
            for(uint32_t y=0;y<h;y+=8)for(uint32_t x=0;x<w;x+=8)
            for(uint32_t y1=y;y1<y+8&&y1<h;y1++)for(uint32_t x1=x;x1<x+8;x1+=2){
                uint8_t p=data[inp++];uint8_t i=(uint8_t)((p&0xF0)|(p>>4));
                if(x1<w)tex_put(rgba,y1*w+x1,((uint32_t)i<<24)|i|((uint32_t)i<<8)|((uint32_t)i<<16));
                if(x1+1<w){uint8_t j=(uint8_t)((p&0x0F)<<4|(p&0x0F));tex_put(rgba,y1*w+x1+1,((uint32_t)j<<24)|j|((uint32_t)j<<8)|((uint32_t)j<<16));}
            }
            break;}
        case 1:{ /* I8 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=8)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+8&&x1<w;x1++){
                uint8_t p=data[inp++];tex_put(rgba,y1*w+x1,((uint32_t)p<<24)|p|((uint32_t)p<<8)|((uint32_t)p<<16));
            }
            break;}
        case 2:{ /* IA4 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=8)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+8&&x1<w;x1++){
                uint8_t p=data[inp++];uint8_t i=(uint8_t)((p&0x0F)<<4|(p&0x0F)),a=(uint8_t)((p>>4)<<4|(p>>4));
                tex_put(rgba,y1*w+x1,((uint32_t)a<<24)|i|((uint32_t)i<<8)|((uint32_t)i<<16));
            }
            break;}
        case 3:{ /* IA8 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=4)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+4&&x1<w;x1++){
                uint16_t p=r16(data+inp);inp+=2;uint8_t i=(uint8_t)(p&0xFF),a=(uint8_t)(p>>8);
                tex_put(rgba,y1*w+x1,((uint32_t)a<<24)|i|((uint32_t)i<<8)|((uint32_t)i<<16));
            }
            break;}
        case 4:{ /* RGB565 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=4)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+4&&x1<w;x1++){
                tex_put(rgba,y1*w+x1,pal_entry_rgb565(r16(data+inp)));inp+=2;
            }
            break;}
        case 5:{ /* RGB5A3 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=4)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+4&&x1<w;x1++){
                tex_put(rgba,y1*w+x1,pal_entry_rgb5a3(r16(data+inp)));inp+=2;
            }
            break;}
        case 6:{ /* RGBA8 */
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=4){
                for(uint32_t k=0;k<2;k++)
                for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+4&&x1<w;x1++){
                    uint16_t p=r16(data+inp);inp+=2;uint8_t hi=(uint8_t)(p>>8),lo=(uint8_t)(p&0xFF);
                    uint32_t idx=y1*w+x1;
                    if(k==0){rgba[idx*4+0]=lo;rgba[idx*4+3]=hi;}
                    else{rgba[idx*4+1]=lo;rgba[idx*4+2]=hi;}
                }
            }
            break;}
        case 8:{ /* CI4 */
            if(!pal)break;
            for(uint32_t y=0;y<h;y+=8)for(uint32_t x=0;x<w;x+=8)
            for(uint32_t y1=y;y1<y+8&&y1<h;y1++)for(uint32_t x1=x;x1<x+8;x1+=2){
                uint8_t p=data[inp++];
                if(x1<w)tex_put(rgba,y1*w+x1,pal[p>>4]);
                if(x1+1<w)tex_put(rgba,y1*w+x1+1,pal[p&0xF]);
            }
            break;}
        case 9:{ /* CI8 */
            if(!pal)break;
            for(uint32_t y=0;y<h;y+=4)for(uint32_t x=0;x<w;x+=8)
            for(uint32_t y1=y;y1<y+4&&y1<h;y1++)for(uint32_t x1=x;x1<x+8&&x1<w;x1++){
                tex_put(rgba,y1*w+x1,pal[data[inp++]]);
            }
            break;}
        case 10:{ /* CI14X2 */
            if(!pal)break;
            for(uint32_t i=0;i<npx&&inp+2<=npx*2;i++){uint16_t p=r16(data+inp);inp+=2;uint32_t idx=p&0x3FFF;if(idx<paln)tex_put(rgba,i,pal[idx]);}
            break;}
        case 14:{ /* CMPR */
            uint32_t ww=(w%8==0)?w:w+(8-w%8);
            for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++){
                uint32_t x0=x&3,x1=(x>>2)&1,x2=x>>3,y0=y&3,y1=(y>>2)&1,y2=y>>3;
                size_t off=(size_t)(8*x1)+(16*y1)+(32*x2)+(4*ww*y2);
                if(off+8>(size_t)(((w+7)/8)*((h+7)/8)*32)){tex_put(rgba,y*w+x,0);continue;}
                uint32_t c0w=r16(data+off),c1w=r16(data+off+2);
                uint8_t c0r=(uint8_t)(((c0w>>11)&0x1F)<<3),c0g=(uint8_t)(((c0w>>5)&0x3F)<<2),c0b=(uint8_t)((c0w&0x1F)<<3);
                uint8_t c1r=(uint8_t)(((c1w>>11)&0x1F)<<3),c1g=(uint8_t)(((c1w>>5)&0x3F)<<2),c1b=(uint8_t)((c1w&0x1F)<<3);
                bool mode=c0w>c1w;
                uint32_t c2,c3;
                if(mode){c2=0xFF000000u|((uint32_t)((2*c0r+c1r)/3))|((uint32_t)((2*c0g+c1g)/3)<<8)|((uint32_t)((2*c0b+c1b)/3)<<16);
                         c3=0xFF000000u|((uint32_t)((c0r+2*c1r)/3))|((uint32_t)((c0g+2*c1g)/3)<<8)|((uint32_t)((c0b+2*c1b)/3)<<16);}
                else{c2=0xFF000000u|((uint32_t)((c0r+c1r)/2))|((uint32_t)((c0g+c1g)/2)<<8)|((uint32_t)((c0b+c1b)/2)<<16);c3=0;}
                uint32_t pixel=r32(data+off+4);
                uint32_t ix=x0+4*y0,pidx=(pixel>>(30-2*ix))&3;
                uint32_t col=pidx==0?(0xFF000000u|c0r|((uint32_t)c0g<<8)|((uint32_t)c0b<<16)):
                              pidx==1?(0xFF000000u|c1r|((uint32_t)c1g<<8)|((uint32_t)c1b<<16)):
                              pidx==2?c2:c3;
                if(pidx==3&&!mode)col=0;
                tex_put(rgba,y*w+x,col);
            }
            break;}
        default:break;
    }
}

/* ================================================================== */
/* HSD model builder                                                  */
/* ================================================================== */

typedef struct{asset_vertex_t*verts;size_t vcount,vcap;uint16_t*indices;size_t icount,icap;}geo_t;
static void geo_push_v(geo_t*g,asset_vertex_t v){if(g->vcount==g->vcap){g->vcap=g->vcap?g->vcap*2:4096;g->verts=realloc(g->verts,g->vcap*sizeof(asset_vertex_t));}g->verts[g->vcount++]=v;}
static void geo_push_i(geo_t*g,uint16_t i){if(g->icount==g->icap){g->icap=g->icap?g->icap*2:4096;g->indices=realloc(g->indices,g->icap*sizeof(uint16_t));}g->indices[g->icount++]=i;}

typedef struct{uint8_t attr,type,comp_cnt,comp_type,scale;uint16_t stride;uint32_t data_rel;}attr_t;

static float comp_v(const uint8_t*p,uint8_t ct,float div){
    switch(ct){case 0:return (float)p[0]/div;case 1:return (float)(int8_t)p[0]/div;case 2:return (float)r16(p)/div;case 3:return (float)(int16_t)r16(p)/div;case 4:return rf32(p);default:return 0;}
}

static uint8_t expand_bits(uint32_t v,uint32_t max){return (uint8_t)((v*255+max/2)/max);}
static size_t decode_color(const uint8_t*p,uint8_t ct,uint8_t out[4]){
    out[0]=out[1]=out[2]=out[3]=255;
    switch(ct){
        case 0:{uint16_t v=r16(p);out[0]=expand_bits(v>>11,31);out[1]=expand_bits((v>>5)&63,63);out[2]=expand_bits(v&31,31);return 2;}
        case 1:out[0]=p[0];out[1]=p[1];out[2]=p[2];return 3;
        case 2:out[0]=p[0];out[1]=p[1];out[2]=p[2];return 4;
        case 3:{uint16_t v=r16(p);out[0]=expand_bits(v>>12,15);out[1]=expand_bits((v>>8)&15,15);out[2]=expand_bits((v>>4)&15,15);out[3]=expand_bits(v&15,15);return 2;}
        case 4:{uint32_t v=((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];out[0]=expand_bits(v>>18,63);out[1]=expand_bits((v>>12)&63,63);out[2]=expand_bits((v>>6)&63,63);out[3]=expand_bits(v&63,63);return 3;}
        case 5:memcpy(out,p,4);return 4;
        default:return 0;
    }
}

static int read_attrs(const dat_t*d,uint32_t rel,attr_t*out,int max){
    int n=0;uint32_t abs=dat_abs(d,rel);
    while(n<max&&abs+0x18<=d->len){
        uint32_t attr=r32(d->bytes+abs);
        if((attr&0xFF)==0xFF)break;
        const uint8_t*a=d->bytes+abs;
        out[n++]=(attr_t){.attr=(uint8_t)(attr&0xFF),.type=(uint8_t)r32(a+4),.comp_cnt=(uint8_t)r32(a+8),.comp_type=(uint8_t)r32(a+0x0C),.scale=a[0x10],.stride=r16(a+0x12),.data_rel=r32(a+0x14)};
        abs+=0x18;
    }
    return n;
}

typedef struct{float weight[4];uint16_t bone[4];}envelope_t;

static int bone_for_abs(const dat_t*d,const uint32_t*bone_rels,size_t bone_count,
                        uint32_t jobj_abs){
    for(size_t i=0;i<bone_count;i++)if(dat_abs(d,bone_rels[i])==jobj_abs)return (int)i;
    return -1;
}

static size_t collect_envelopes(const dat_t*d,const reloc_idx_t*ri,
                                uint32_t pobj_rel,const uint32_t*bone_rels,
                                size_t bone_count,envelope_t*out,size_t cap){
    const uint8_t*pd=dat_at(d,pobj_rel);
    if(!pd||(r16(pd+0x0C)&0x2000)==0)return 0;
    uint32_t array_abs=rdptr(d,ri,pobj_rel,0x14);
    if(!array_abs)return 0;
    size_t count=0;
    for(size_t ei=0;ei<cap;ei++){
        uint32_t env_abs=rdptr(d,ri,array_abs-0x20,(uint32_t)(ei*4));
        if(!env_abs)break;
        envelope_t env;memset(&env,0,sizeof env);
        for(size_t wi=0;wi<6;wi++){
            uint32_t jobj_abs=rdptr(d,ri,env_abs-0x20,(uint32_t)(wi*8));
            if(!jobj_abs)break;
            if(env_abs+wi*8+8>d->len)break;
            if(wi<4){
                int bone=bone_for_abs(d,bone_rels,bone_count,jobj_abs);
                if(bone>=0){env.weight[wi]=rf32(d->bytes+env_abs+wi*8+4);env.bone[wi]=(uint16_t)bone;}
            }
        }
        out[count++]=env;
    }
    return count;
}

static void decode_pobj(const dat_t*d,const reloc_idx_t*ri,uint32_t pobj_rel,
                        int32_t bone_idx,const uint32_t*bone_rels,
                        size_t bone_count,geo_t*g){
    const uint8_t*pd=dat_at(d,pobj_rel);
    if(!pd)return;
    uint32_t attrs_rel=r32(pd+0x08);uint16_t dlsz=r16(pd+0x0E);uint32_t dl_rel=r32(pd+0x10);
    if(!attrs_rel||!dl_rel||dlsz==0||dlsz>0x2000)return;
    attr_t attrs[16];int nattr=read_attrs(d,attrs_rel,attrs,16);if(nattr<=0)return;
    uint32_t dl_abs=dat_abs(d,dl_rel);size_t dl_len=(size_t)dlsz*32;
    if(dl_abs < 0x20 || dl_abs >= d->len) return;
    if(dl_abs+dl_len>d->len)dl_len=d->len-dl_abs;
    const uint8_t*dl=d->bytes+dl_abs;
    envelope_t envelopes[256];
    size_t envelope_count=collect_envelopes(d,ri,pobj_rel,bone_rels,bone_count,
                                            envelopes,256);
    size_t cur=0;uint16_t tmp[4096];size_t tn=0;
    while(cur<dl_len){
        uint8_t ptype=dl[cur];if(ptype==0)break;cur++;
        if(cur+2>dl_len)break;
        uint16_t vcount=r16(dl+cur);cur+=2;tn=0;
        for(uint32_t vi=0;vi<vcount;vi++){
            asset_vertex_t v;memset(&v,0,sizeof v);memset(v.color,255,sizeof v.color);v.weight[0]=1.0f;v.bone[0]=(uint16_t)(bone_idx>=0?bone_idx:0);
            for(int a=0;a<nattr;a++){
                const attr_t*at=&attrs[a];uint8_t t=at->type;
                if(t==0)continue;
                if(t==1){
                    if(at->attr==11||at->attr==12){
                        int n=at->comp_type==0?2:(at->comp_type==1?3:(at->comp_type==3?2:(at->comp_type==4?3:4)));
                        if(cur+n>dl_len)break;
                        if(at->attr==11)decode_color(dl+cur,at->comp_type,v.color);
                        cur+=n;continue;
                    }
                    if(cur>=dl_len)break;
                    uint8_t direct=dl[cur++];
                    if(at->attr==0&&envelope_count){
                        size_t envelope_idx=(size_t)direct/3;
                        if(envelope_idx<envelope_count){
                            memcpy(v.weight,envelopes[envelope_idx].weight,sizeof v.weight);
                            memcpy(v.bone,envelopes[envelope_idx].bone,sizeof v.bone);
                        }
                    }
                    continue;
                }
                uint32_t index;
                if(t==2){if(cur>=dl_len)break;index=dl[cur++];}
                else if(t==3){if(cur+2>dl_len)break;index=r16(dl+cur);cur+=2;}
                else continue;
                uint32_t data_abs=at->data_rel?dat_abs(d,at->data_rel):0x20;
                if(data_abs>=d->len)continue;
                if(at->attr==11||at->attr==12){
                    int cs=at->comp_type==0?2:(at->comp_type==1?3:(at->comp_type==3?2:(at->comp_type==4?3:4)));
                    int stride=at->stride?at->stride:cs;
                    uint32_t start=data_abs+index*(uint32_t)stride;
                    if(start+(uint32_t)cs<=d->len&&at->attr==11)decode_color(d->bytes+start,at->comp_type,v.color);
                    continue;
                }
                int cs=at->comp_type<=1?1:(at->comp_type<=3?2:4);
                float div=at->comp_type==4?1.0f:(float)(1u<<(at->scale&0x1F));
                int count=0;float*vp=NULL;
                if(at->attr==9){count=3;vp=v.pos;}else if(at->attr==10){count=3;vp=v.nrm;}else if(at->attr==13){count=2;vp=v.uv;}else continue;
                int stride=at->stride?at->stride:cs*count;
                uint32_t start=data_abs+index*stride;
                if(start+stride>d->len)continue;
                const uint8_t*p=d->bytes+start;
                int maxc=stride/cs;if(maxc>count)maxc=count;
                for(int i=0;i<maxc;i++){vp[i]=comp_v(p+i*cs,at->comp_type,div);}
                for(int i=maxc;i<count;i++)vp[i]=0;
            }
            geo_push_v(g,v);tmp[tn++]=(uint16_t)(g->vcount-1);
        }
        switch(ptype){
            case 0x90:for(size_t i=0;i+2<tn;i+=3)geo_push_i(g,tmp[i]),geo_push_i(g,tmp[i+1]),geo_push_i(g,tmp[i+2]);break;
            case 0x98:for(size_t i=0;i+2<tn;i++){if(i&1)geo_push_i(g,tmp[i]),geo_push_i(g,tmp[i+2]),geo_push_i(g,tmp[i+1]);else geo_push_i(g,tmp[i]),geo_push_i(g,tmp[i+1]),geo_push_i(g,tmp[i+2]);}break;
            case 0xA0:for(size_t i=1;i+1<tn;i++)geo_push_i(g,tmp[0]),geo_push_i(g,tmp[i]),geo_push_i(g,tmp[i+1]);break;
            case 0x80:for(size_t i=0;i+3<tn;i+=4)geo_push_i(g,tmp[i]),geo_push_i(g,tmp[i+1]),geo_push_i(g,tmp[i+2]),geo_push_i(g,tmp[i+2]),geo_push_i(g,tmp[i+3]),geo_push_i(g,tmp[i]);break;
            default:break;
        }
    }
}

/* builder context */
typedef struct{uint32_t rel;int16_t tex;}texmap_entry_t;
typedef struct{
    const dat_t*d;
    const reloc_idx_t*ri;
    geo_t geo;
    asset_model_t*m;
    const dobj_filter_t*filter;size_t dobj_index;
    uint32_t*bone_rels;size_t bone_rel_count,bone_rel_cap;
    texmap_entry_t*texmap;size_t texmap_n,texmap_cap;
}builder_t;

typedef struct {
    asset_texture_t tex;
    uint32_t image_rel;
    uint32_t wrap_s, wrap_t;
    uint8_t repeat_s, repeat_t;
    float scale_x, scale_y, translate_x, translate_y;
} tobj_img_t;

static int tobj_decode(builder_t*b,uint32_t tobj_rel,tobj_img_t*out){
    memset(out,0,sizeof*out);
    const uint8_t*td=dat_at(b->d,tobj_rel);if(!td)return -1;
    uint32_t image_rel=r32(td+0x4C),tlut_rel=r32(td+0x50);
    if(!image_rel)return -1;
    const uint8_t*im=dat_at(b->d,image_rel);if(!im)return -1;
    uint32_t data_rel=r32(im);uint16_t w=r16(im+4),h=r16(im+6);uint32_t fmt=r32(im+8);
    uint32_t*pal=NULL;size_t paln=0;
    if(tlut_rel){
        const uint8_t*tl=dat_at(b->d,tlut_rel);
        if(tl){
            uint32_t ldata=r32(tl),pal_fmt=r32(tl+0x04);
            uint16_t ncols=r16(tl+0x0C);
            if(ldata&&ncols){
                uint32_t lda=dat_abs(b->d,ldata);paln=ncols;pal=malloc(ncols*sizeof(uint32_t));
                for(uint16_t i=0;i<ncols&&lda+i*2+2<=b->d->len;i++){
                    uint16_t p=r16(b->d->bytes+lda+i*2);
                    if(pal_fmt==0)pal[i]=pal_entry_ia8(p);
                    else if(pal_fmt==1)pal[i]=pal_entry_rgb565(p);
                    else pal[i]=pal_entry_rgb5a3(p);
                }
            }
        }
    }
    size_t ts=0;
    switch(fmt){case 0:case 8:case 14:ts=(size_t)((w+7)/8)*((h+7)/8)*32;break;case 1:case 2:case 9:ts=(size_t)((w+7)/8)*((h+3)/4)*32;break;case 3:case 4:case 5:case 10:ts=(size_t)((w+3)/4)*((h+3)/4)*32;break;case 6:ts=(size_t)((w+3)/4)*((h+3)/4)*64;break;default:ts=(size_t)w*h*4;}
    uint32_t da=dat_abs(b->d,data_rel);if(da+ts>b->d->len)ts=b->d->len-da;
    if(w == 0 || h == 0 || w > 1024 || h > 1024) { free(pal); return -1; }
    asset_texture_t tex;memset(&tex,0,sizeof tex);tex.width=w;tex.height=h;tex.format=fmt;
    tex.rgba=malloc((size_t)w*h*4);
    decode_texture(fmt,w,h,b->d->bytes+da,pal,paln,tex.rgba);
    free(pal);
    out->tex=tex;
    out->image_rel=image_rel;
    out->wrap_s=r32(td+0x34);
    out->wrap_t=r32(td+0x38);
    out->repeat_s=td[0x3C]?td[0x3C]:1;
    out->repeat_t=td[0x3D]?td[0x3D]:1;
    out->scale_x=rf32(td+0x1C);
    out->scale_y=rf32(td+0x20);
    out->translate_x=rf32(td+0x28);
    out->translate_y=rf32(td+0x2C);
    if(!(out->scale_x==out->scale_x) || fabsf(out->scale_x)<1e-6f) out->scale_x=1;
    if(!(out->scale_y==out->scale_y) || fabsf(out->scale_y)<1e-6f) out->scale_y=1;
    return 0;
}

static int16_t tex_register(builder_t*b,uint32_t image_rel,asset_texture_t tex){
    if(image_rel){
        for(size_t i=0;i<b->texmap_n;i++)if(b->texmap[i].rel==image_rel){
            free(tex.rgba);
            return b->texmap[i].tex;
        }
    }
    int16_t idx=(int16_t)b->m->texture_count;
    if(b->m->texture_count%16==0)b->m->textures=realloc(b->m->textures,(b->m->texture_count+16)*sizeof(asset_texture_t));
    b->m->textures[b->m->texture_count++]=tex;
    if(image_rel){
        if(b->texmap_n==b->texmap_cap){b->texmap_cap=b->texmap_cap?b->texmap_cap*2:32;b->texmap=realloc(b->texmap,b->texmap_cap*sizeof(*b->texmap));}
        texmap_entry_t entry = { image_rel, idx };
        b->texmap[b->texmap_n++] = entry;
    }
    return idx;
}

static int16_t tex_resolve(builder_t*b,uint32_t tobj_rel){
    tobj_img_t img;
    if(tobj_decode(b,tobj_rel,&img))return -1;
    return tex_register(b,img.image_rel,img.tex);
}

static int16_t tex_resolve_mobj(builder_t*b,uint32_t tobj_rel){
    int16_t first=-1;
    uint32_t t=tobj_rel;
    while(t){
        int16_t idx=tex_resolve(b,t);
        if(first<0&&idx>=0)first=idx;
        const uint8_t*td=dat_at(b->d,t);if(!td)break;
        t=r32(td+4);
    }
    return first;
}

static void build_bones(builder_t*b,uint32_t root_rel,int32_t parent){
    if(b->m->bone_count >= 512) return;
    const uint8_t*j=dat_at(b->d,root_rel);if(!j)return;
    uint32_t child_rel=r32(j+0x08),next_rel=r32(j+0x0C);
    float rot[3]={rf32(j+0x14),rf32(j+0x18),rf32(j+0x1C)};
    float scl[3]={rf32(j+0x20),rf32(j+0x24),rf32(j+0x28)};
    float trs[3]={rf32(j+0x2C),rf32(j+0x30),rf32(j+0x34)};
    uint32_t flags=r32(j+0x04);
    int32_t bone=(int32_t)b->m->bone_count;
    b->m->bones=realloc(b->m->bones,(b->m->bone_count+1)*sizeof(asset_bone_t));
    asset_bone_t*bone_out=&b->m->bones[b->m->bone_count++];
    memset(bone_out,0,sizeof*bone_out);bone_out->parent=parent>=0?(uint16_t)parent:UINT16_MAX;bone_out->flags=flags;
    mtx_from_srt(scl,rot,trs,bone_out->base);
    if(b->bone_rel_count==b->bone_rel_cap){b->bone_rel_cap=b->bone_rel_cap?b->bone_rel_cap*2:128;b->bone_rels=realloc(b->bone_rels,b->bone_rel_cap*sizeof(uint32_t));}
    b->bone_rels[b->bone_rel_count++]=root_rel;
    if(child_rel)build_bones(b,child_rel,bone);
    if(next_rel)build_bones(b,next_rel,parent);
}

static void build_geometry(builder_t*b,uint32_t root_rel){
    const uint8_t*j=dat_at(b->d,root_rel);if(!j)return;
    uint32_t child_rel=r32(j+0x08),next_rel=r32(j+0x0C),dobj_rel=r32(j+0x10);
    int bone=bone_for_abs(b->d,b->bone_rels,b->bone_rel_count,dat_abs(b->d,root_rel));
    if(bone<0)return;
    asset_bone_t*bone_out=&b->m->bones[bone];
    bone_out->pgroup_start=(uint16_t)b->m->pgroup_count;
    if(dobj_rel){
        uint32_t cursor=dobj_rel;
        while(cursor){
            const uint8_t*dd=dat_at(b->d,cursor);if(!dd)break;
            uint32_t next_d=r32(dd+0x04),mobj_rel=r32(dd+0x08),pobj_rel=r32(dd+0x0C);
            size_t dobj_index=b->dobj_index++;
            if(b->filter&&b->filter->enabled&&
               (dobj_index>=256||b->filter->group[dobj_index]==0xFF)){
                cursor=next_d;continue;
            }
            uint32_t idx_start=(uint32_t)b->geo.icount;int16_t tex=-1;uint32_t mfl=0;
            asset_phong_t material;memset(&material,0,sizeof material);
            memset(material.ambient,255,sizeof material.ambient);
            memset(material.diffuse,255,sizeof material.diffuse);
            memset(material.specular,255,sizeof material.specular);
            material.alpha=1.0f;
            if(mobj_rel){
                const uint8_t*md=dat_at(b->d,mobj_rel);
                if(md){
                    mfl=r32(md+0x04);
                    uint32_t tobj_rel=r32(md+0x08),mat_rel=r32(md+0x0C);
                    if(tobj_rel)tex=tex_resolve_mobj(b,tobj_rel);
                    if(mat_rel){
                        const uint8_t*mat=dat_at(b->d,mat_rel);
                        if(mat){
                            memcpy(material.ambient,mat,4);
                            memcpy(material.diffuse,mat+4,4);
                            memcpy(material.specular,mat+8,4);
                            material.alpha=rf32(mat+0x0C);
                            material.shininess=rf32(mat+0x10);
                        }
                    }
                }
            }
            if(pobj_rel){uint32_t pc=pobj_rel;while(pc){const uint8_t*pd=dat_at(b->d,pc);if(!pd)break;uint32_t np=r32(pd+0x04);decode_pobj(b->d,b->ri,pc,bone,b->bone_rels,b->bone_rel_count,&b->geo);pc=np;}}
            if(b->geo.icount>idx_start){
                b->m->pgroups=realloc(b->m->pgroups,(b->m->pgroup_count+1)*sizeof(asset_pgroup_t));
                uint8_t group=(b->filter&&b->filter->enabled&&dobj_index<256)?b->filter->group[dobj_index]:0;
                b->m->pgroups[b->m->pgroup_count++]=(asset_pgroup_t){.texture_idx=tex,.indices_start=idx_start,.indices_len=(uint32_t)(b->geo.icount-idx_start),.mobj_flags=mfl,.model_group_idx=group};
                b->m->phongs=realloc(b->m->phongs,(b->m->phong_count+1)*sizeof(asset_phong_t));
                b->m->phongs[b->m->phong_count++]=material;
            }
            cursor=next_d;
        }
    }
    bone_out->pgroup_len=(uint16_t)(b->m->pgroup_count-bone_out->pgroup_start);
    if(child_rel)build_geometry(b,child_rel);
    if(next_rel)build_geometry(b,next_rel);
}

static asset_model_t*build_model(const dat_t*d,uint32_t root_rel,
                                 const dobj_filter_t*filter){
    asset_model_t*m=calloc(1,sizeof(asset_model_t));
    reloc_idx_t ri;reloc_build(d,&ri);
    builder_t b={.d=d,.ri=&ri,.m=m,.filter=filter};
    build_bones(&b,root_rel,-1);
    mtx3x4*world=calloc(m->bone_count?m->bone_count:1,sizeof(mtx3x4));
    for(uint32_t i=0;i<m->bone_count;i++){
        if(m->bones[i].parent==UINT16_MAX)memcpy(world[i],m->bones[i].base,sizeof(mtx3x4));
        else mtx_mul(world[m->bones[i].parent],m->bones[i].base,world[i]);
        mtx_invert_affine(world[i],m->bones[i].inv_world);
    }
    free(world);
    build_geometry(&b,root_rel);
    m->vertices=b.geo.verts;m->vertex_count=(uint32_t)b.geo.vcount;
    m->indices=b.geo.indices;m->index_count=(uint32_t)b.geo.icount;
    free(b.texmap);free(b.bone_rels);free(ri.offs);
    return m;
}

/* ================================================================== */
/* Cache writer                                                       */
/* ================================================================== */

static void w32(FILE*f,uint32_t v){uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};fwrite(b,1,4,f);}
static void w16(FILE*f,uint16_t v){uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v};fwrite(b,1,2,f);}
static void w8(FILE*f,uint8_t v){fwrite(&v,1,1,f);}
static void wf(FILE*f,float v){uint32_t x;memcpy(&x,&v,4);w32(f,x);}

static void write_model(FILE*f,const asset_model_t*m){
    w32(f,ASSET_MAGIC);w32(f,ASSET_SCHEMA_VERSION);
    w32(f,m->bone_count);w32(f,m->vertex_count);w32(f,m->index_count);
    w32(f,m->pgroup_count);w32(f,m->phong_count);w32(f,m->texture_count);
    for(uint32_t i=0;i<m->bone_count;i++){
        w16(f,m->bones[i].parent);w16(f,m->bones[i].pgroup_start);w16(f,m->bones[i].pgroup_len);w16(f,m->bones[i].pad);
        w32(f,m->bones[i].flags);
        for(int k=0;k<12;k++)wf(f,m->bones[i].base[k]);
        for(int k=0;k<12;k++)wf(f,m->bones[i].inv_world[k]);
    }
    for(uint32_t i=0;i<m->vertex_count;i++){
        const asset_vertex_t*v=&m->vertices[i];
        for(int k=0;k<3;k++)wf(f,v->pos[k]);
        for(int k=0;k<3;k++)wf(f,v->nrm[k]);
        for(int k=0;k<2;k++)wf(f,v->uv[k]);
        fwrite(v->color,1,4,f);
        for(int k=0;k<4;k++)wf(f,v->weight[k]);
        w16(f,v->bone[0]);w16(f,v->bone[1]);w16(f,v->bone[2]);w16(f,v->bone[3]);
    }
    for(uint32_t i=0;i<m->index_count;i++)w16(f,m->indices[i]);
    for(uint32_t i=0;i<m->pgroup_count;i++){
        w16(f,(uint16_t)m->pgroups[i].texture_idx);
        w32(f,m->pgroups[i].indices_start);w32(f,m->pgroups[i].indices_len);
        w32(f,m->pgroups[i].mobj_flags);w8(f,m->pgroups[i].model_group_idx);
        w8(f,0);w16(f,0);
    }
    for(uint32_t i=0;i<m->phong_count;i++){
        fwrite(m->phongs[i].ambient,1,4,f);fwrite(m->phongs[i].diffuse,1,4,f);fwrite(m->phongs[i].specular,1,4,f);
        wf(f,m->phongs[i].alpha);wf(f,m->phongs[i].shininess);
    }
    for(uint32_t i=0;i<m->texture_count;i++){
        w16(f,m->textures[i].width);w16(f,m->textures[i].height);w32(f,m->textures[i].format);
        fwrite(m->textures[i].rgba,1,(size_t)m->textures[i].width*m->textures[i].height*4,f);
    }
}

/* ---- model cache writer (model file) ---- */
static void write_model_file(const char*dir,const char*name,const asset_model_t*m){
    char path[1200];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE*f=fopen(path,"wb");if(!f)die("cannot write cache file");
    write_model(f,m);fclose(f);
    printf("wrote %s: bones=%u verts=%u idx=%u pg=%u tex=%u\n",
           name,m->bone_count,m->vertex_count,m->index_count,m->pgroup_count,m->texture_count);
}

/* ================================================================== */
/* Animation (figatree) decoder + cache writer                        */
/* ================================================================== */

/* action table entry */
typedef struct{uint32_t name_ref;uint32_t anim_off,anim_size;uint32_t flags;char name[48];}action_entry_t;

static uint32_t read_packed(const uint8_t*p,size_t*cur,size_t len);
static float read_packed_value(const uint8_t*p,size_t*cur,size_t len,int fmt,int shift);

static int extract_actions(const dat_t*fd,action_entry_t**out,uint32_t*count_out){
    /* ftData root at 0; action table at root+0x0C */
    uint32_t ftdata_rel=0;
    for(uint32_t i=0;i<fd->root_count;i++){
        uint32_t data_off=r32(fd->bytes+fd->roots_start+(size_t)i*8);
        uint32_t name_off=r32(fd->bytes+fd->roots_start+(size_t)i*8+4);
        if(strstr(dat_name(fd,name_off),"ftData")){ftdata_rel=data_off;break;}
    }
    if(!ftdata_rel)return -1;
    const uint8_t*fd0=dat_at(fd,ftdata_rel);
    if(!fd0)return -1;
    uint32_t at_rel=r32(fd0+0x0C);
    if(!at_rel)return -1;
    uint32_t at_abs=dat_abs(fd,at_rel);
    /* table length: span until ftData root end (use fd->len cap) */
    uint32_t table_len = (dat_abs(fd, ftdata_rel) - at_abs);
    if (table_len == 0 || table_len > 0x4000) table_len = 0x4000;
    uint32_t count=table_len/0x18;
    action_entry_t*entries=calloc(count?count:1,sizeof(action_entry_t));
    for(uint32_t i=0;i<count;i++){
        uint32_t e=at_abs+i*0x18;
        if(e+0x18>fd->len)break;
        const uint8_t*p=fd->bytes+e;
        uint32_t name_ref=r32(p);
        uint32_t anim_off=r32(p+0x04),anim_size=r32(p+0x08),flags=r32(p+0x10);
        /* Keep empty/undecodable slots in place.  Slippi's animation_index
           addresses this table directly, so compacting it makes every clip
           after the first hole refer to the wrong animation. */
        entries[i].name_ref=name_ref;entries[i].anim_off=anim_off;
        entries[i].anim_size=anim_size;entries[i].flags=flags;
        if(name_ref){
            uint32_t na=dat_abs(fd,name_ref);
            size_t l=0;while(na+l<fd->len&&fd->bytes[na+l]&&l<47)l++;
            memcpy(entries[i].name,fd->bytes+na,l);entries[i].name[l]=0;
        }
    }
    *out=entries;*count_out=count;
    return 0;
}

/* decode one figatree clip (slice of AJ bytes) into asset_action_t */
static int decode_clip(const uint8_t*aj,size_t aj_len,uint32_t clip_off,uint32_t clip_size,
                       const action_entry_t*ae,asset_action_t*act){
    if(clip_off>=aj_len||clip_size>aj_len-clip_off)return -1;
    const uint8_t*buf=aj+clip_off;size_t len=clip_size;
    dat_t c;if(dat_open(buf,len,&c))return -1;
    if(c.root_count==0)return -1;
    uint32_t root_rel=0;
    for(uint32_t i=0;i<c.root_count;i++){
        root_rel=r32(c.bytes+c.roots_start+(size_t)i*8);break;
    }
    const uint8_t*fig=dat_at(&c,root_rel);
    if(!fig)return -1;
    float end_frame=rf32(fig+0x08);
    uint32_t track_info_rel=r32(fig+0x0C),track_data_rel=r32(fig+0x10);
    if(!track_info_rel||!track_data_rel)return -1;
    uint32_t ti_abs=dat_abs(&c,track_info_rel),td_abs=dat_abs(&c,track_data_rel);
    reloc_idx_t clip_ri;reloc_build(&c,&clip_ri);

    /* count bones: find 0xFF terminator */
    uint32_t nbones=0;
    while(ti_abs+nbones<c.len&&c.bytes[ti_abs+nbones]!=0xFF&&nbones<256)nbones++;

    act->end_frame=end_frame;
    act->loop=(ae->flags&(1<<28))!=0;
    snprintf(act->name,sizeof act->name,"%s",ae->name[0]?ae->name:"action");

    /* count total tracks */
    uint32_t ntrack=0;
    for(uint32_t i=0;i<nbones;i++)ntrack+=c.bytes[ti_abs+i];

    /* joint anims: build one per bone with >=1 track */
    act->joints=calloc(nbones?nbones:1,sizeof(asset_joint_anim_t));
    act->joint_count=0;
    uint32_t ti=0; /* track index */
    for(uint32_t bi=0;bi<nbones;bi++){
        uint8_t tc=c.bytes[ti_abs+bi];
        if(tc==0)continue;
        asset_joint_anim_t*ja=&act->joints[act->joint_count++];
        ja->bone_index=(uint16_t)bi;
        ja->tracks=calloc(tc,sizeof(asset_track_t));
        ja->track_count=tc;
        for(uint8_t k=0;k<tc;k++){
            uint32_t tr=td_abs+ti*0x0C;ti++;
            if(tr+0x0C>c.len)continue;
            const uint8_t*t=c.bytes+tr;
            asset_track_t*tk=&ja->tracks[k];
            tk->channel=t[0x04];
            tk->start_frame=r16(t+0x02);
            uint8_t vflag=t[0x05];
            uint32_t da=rdptr(&c,&clip_ri,tr-0x20,0x08);
            if(!da){tk->key_count=0;tk->keys=NULL;continue;}
            size_t stream_len=node_span_len(&c,&clip_ri,da);
            if(stream_len==0||stream_len>c.len-da)stream_len=c.len-da;
            /* value format+scale */
            int vfmt=(vflag>>5)&7; /* 0 f32,1 s16,2 u16,3 s8,4 u8 */
            int vshift=vflag&0x1F;
            /* parse keyframe stream (little-endian) */
            size_t cur=0;
            uint32_t cap=32,nk=0;
            asset_key_t*keys=malloc(cap*sizeof(asset_key_t));
            float frame=0.0f;
            uint32_t op;
            while(cur<stream_len){
                /* packed header */
                uint32_t packed=read_packed(c.bytes+da,&cur,stream_len);
                op=packed&0x0F;
                if(op==0)break;
                uint32_t kcount=(packed>>4)+1;
                for(uint32_t q=0;q<kcount;q++){
                    float val=0,tan=0;
                    if(op==1||op==2||op==3){
                        val=read_packed_value(c.bytes+da,&cur,stream_len,vfmt,vshift);
                    }else if(op==4){
                        val=read_packed_value(c.bytes+da,&cur,stream_len,vfmt,vshift);
                        /* tangent uses tanflag; approximate with same shift */
                        tan=read_packed_value(c.bytes+da,&cur,stream_len,(t[0x06]>>5)&7,t[0x06]&0x1F);
                    }else if(op==5){
                        /* slope-only tangent */
                        if(nk>0)keys[nk-1].out_tan=read_packed_value(c.bytes+da,&cur,stream_len,(t[0x06]>>5)&7,t[0x06]&0x1F);
                        continue;
                    }else if(op==6){
                        read_packed_value(c.bytes+da,&cur,stream_len,vfmt,vshift);
                        continue;
                    }
                    if(nk==cap){cap*=2;keys=realloc(keys,cap*sizeof(asset_key_t));}
                    asset_key_t*k=&keys[nk++];
                    k->frame=frame;k->value=val;k->in_tan=k->out_tan=tan;
                    k->interp=op==1?ATK_STEP:op==2?ATK_LINEAR:ATK_HERMITE;
                    /* frame delta (packed, LSB-first) */
                    uint32_t d=read_packed(c.bytes+da,&cur,stream_len);
                    frame+=(float)d;
                }
            }
            /* realloc to actual */
            tk->keys=realloc(keys,(nk?nk:1)*sizeof(asset_key_t));
            tk->key_count=nk;
            (void)ntrack;
        }
    }
    free(clip_ri.offs);
    return 0;
}

/* packed uleb128-ish value from figatree stream (LSB-first groups) */
static uint32_t read_packed(const uint8_t*p,size_t*cur,size_t len){
    if(*cur>=len)return 0;
    uint8_t a=p[(*cur)++];
    if(a&0x80){
        if(*cur>=len)return a&0x7F;
        uint8_t b=p[(*cur)++];
        return (a&0x7F)|((uint32_t)b<<7);
    }
    return a;
}
static float read_packed_value(const uint8_t*p,size_t*cur,size_t len,int fmt,int shift){
    float div=(float)(1u<<(shift&0x1F));
    switch(fmt){
        case 0:{if(*cur+4>len)return 0;float v=rf32le(p+*cur);*cur+=4;return v;}
        case 1:{if(*cur+2>len)return 0;int16_t v=(int16_t)r16le(p+*cur);*cur+=2;return (float)v/div;}
        case 2:{if(*cur+2>len)return 0;uint16_t v=r16le(p+*cur);*cur+=2;return (float)v/div;}
        case 3:{if(*cur+1>len)return 0;int8_t v=(int8_t)p[*cur];*cur+=1;return (float)v/div;}
        case 4:{if(*cur+1>len)return 0;uint8_t v=p[*cur];*cur+=1;return (float)v/div;}
        default:return 0;
    }
}

/* forward-declare helpers defined after main-independent code */
static void write_anims(FILE*f,const asset_anims_t*a);
static void write_anims_file(const char*dir,const char*name,const asset_anims_t*a){
    char path[1200];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE*f=fopen(path,"wb");if(!f)die("cannot write anim cache");
    write_anims(f,a);fclose(f);
    printf("wrote %s: %u actions\n",name,a->action_count);
}

/* ================================================================== */
/* Stage decoder                                                      */
/* ================================================================== */

static int decode_stage(const dat_t*d,asset_stage_t*st){
    memset(st,0,sizeof*st);
    uint32_t map_rel=0,gp_rel=0,plit_rel=0;
    for(uint32_t i=0;i<d->root_count;i++){
        uint32_t data_off=r32(d->bytes+d->roots_start+(size_t)i*8);
        uint32_t name_off=r32(d->bytes+d->roots_start+(size_t)i*8+4);
        const char*nm=dat_name(d,name_off);
        if(strcmp(nm,"map_head")==0)map_rel=data_off;
        else if(strcmp(nm,"grGroundParam")==0)gp_rel=data_off;
        else if(strcmp(nm,"map_plit")==0)plit_rel=data_off;
    }
    if(!map_rel||!gp_rel)return -1;
    /* grGroundParam: scale +0x00, camera +0x50 */
    const uint8_t*gp=dat_at(d,gp_rel);
    st->scale=rf32(gp+0x00);
    st->cam_pos[0]=rf32(gp+0x50);st->cam_pos[1]=rf32(gp+0x54);st->cam_pos[2]=rf32(gp+0x58);
    st->cam_fov=rf32(gp+0x5C);st->cam_vert=rf32(gp+0x60);st->cam_horiz=rf32(gp+0x64);
    /* map_head: model groups at +0x08 (ptr,count) */
    const uint8_t*mh=dat_at(d,map_rel);
    uint32_t mg_rel=r32(mh+0x08),mg_count=r32(mh+0x0C);
    if(mg_rel&&mg_count<64){
        st->sections=calloc(mg_count,sizeof(asset_model_t));
        st->section_count=0;
        for(uint32_t i=0;i<mg_count;i++){
            uint32_t gabs=dat_abs(d,mg_rel)+i*0x34;
            if(gabs+0x34>d->len)continue;
            uint32_t root_jobj_rel=r32(d->bytes+gabs);
            if(!root_jobj_rel)continue;
            asset_model_t*m=build_model(d,root_jobj_rel,NULL);
            st->sections[st->section_count++]=*m;
            free(m);
        }
    }
    /* lights from map_plit */
    if(plit_rel){
        st->lights=malloc(8*sizeof(asset_light_t));
        uint32_t lc=0;
        uint32_t e=dat_abs(d,plit_rel);
        while(e+4<=d->len&&lc<8){
            uint32_t lr=r32(d->bytes+e);
            if(!lr)break;
            const uint8_t*lref=dat_at(d,lr);
            if(!lref)break;
            uint32_t lobj_rel=r32(lref);
            const uint8_t*lo=dat_at(d,lobj_rel);
            if(!lo)break;
            asset_light_t*l=&st->lights[lc++];
            memset(l,0,sizeof* l);
            uint16_t flags=r16(lo+0x08);
            l->kind=(uint8_t)(flags&3);
            l->flags=(uint8_t)flags;
            memcpy(l->color,lo+0x0C,4);
            /* position WObj at +0x10 */
            uint32_t wpos_rel=r32(lo+0x10),wint_rel=r32(lo+0x14);
            const uint8_t*wp=wpos_rel?dat_at(d,wpos_rel):NULL;
            if(wp){l->pos[0]=rf32(wp+0x04);l->pos[1]=rf32(wp+0x08);l->pos[2]=rf32(wp+0x0C);}
            const uint8_t*wi=wint_rel?dat_at(d,wint_rel):NULL;
            if(wi){
                float ix=rf32(wi+0x04),iy=rf32(wi+0x08),iz=rf32(wi+0x0C);
                float dx=l->pos[0]-ix,dy=l->pos[1]-iy,dz=l->pos[2]-iz;
                float il=sqrtf(dx*dx+dy*dy+dz*dz);
                if(il>1e-6f){l->dir[0]=dx/il;l->dir[1]=dy/il;l->dir[2]=dz/il;}
            }
            e+=4;
        }
        st->light_count=lc;
    }
    return 0;
}

static void write_stage(FILE*f,const asset_stage_t*s);
static void write_stage_file(const char*dir,const char*name,const asset_stage_t*s){
    char path[1200];snprintf(path,sizeof path,"%s/%s",dir,name);
    FILE*f=fopen(path,"wb");if(!f)die("cannot write stage cache");
    write_stage(f,s);fclose(f);
    printf("wrote %s: scale=%.3f sections=%u lights=%u\n",name,s->scale,s->section_count,s->light_count);
}

/* anim cache serialization */
static void write_anims(FILE*f,const asset_anims_t*a){
    w32(f,ASSET_MAGIC);w32(f,ASSET_SCHEMA_VERSION);
    w32(f,a->action_count);
    for(uint32_t i=0;i<a->action_count;i++){
        const asset_action_t*act=&a->actions[i];
        fwrite(act->name,1,48,f);
        wf(f,act->end_frame);
        w8(f,act->loop?1:0);w8(f,0);w16(f,0);
        w32(f,act->joint_count);
        for(uint32_t j=0;j<act->joint_count;j++){
            const asset_joint_anim_t*ja=&act->joints[j];
            w16(f,ja->bone_index);
            w32(f,ja->track_count);
            for(uint32_t k=0;k<ja->track_count;k++){
                const asset_track_t*tk=&ja->tracks[k];
                w8(f,tk->channel);
                w16(f,tk->start_frame);
                w32(f,tk->key_count);
                for(uint32_t q=0;q<tk->key_count;q++){
                    const asset_key_t*k=&tk->keys[q];
                    wf(f,k->frame);wf(f,k->value);wf(f,k->in_tan);wf(f,k->out_tan);
                    w8(f,k->interp);
                }
            }
        }
    }
}

/* stage cache serialization */
static void write_stage(FILE*f,const asset_stage_t*s){
    w32(f,ASSET_MAGIC);w32(f,ASSET_SCHEMA_VERSION);
    wf(f,s->scale);
    for(int i=0;i<3;i++)wf(f,s->cam_pos[i]);
    wf(f,s->cam_fov);wf(f,s->cam_vert);wf(f,s->cam_horiz);
    w32(f,s->section_count);w32(f,s->light_count);
    for(uint32_t i=0;i<s->section_count;i++)write_model(f,&s->sections[i]);
    for(uint32_t i=0;i<s->light_count;i++){
        w8(f,s->lights[i].kind);w8(f,s->lights[i].flags);
        fwrite(s->lights[i].color,1,4,f);
        for(int k=0;k<3;k++)wf(f,s->lights[i].pos[k]);
        for(int k=0;k<3;k++)wf(f,s->lights[i].dir[k]);
        wf(f,s->lights[i].a0);wf(f,s->lights[i].a1);wf(f,s->lights[i].a2);
        wf(f,s->lights[i].k0);wf(f,s->lights[i].k1);wf(f,s->lights[i].k2);
    }
}

/* meta.json writer */
static void write_meta(const char*dir,uint32_t schema){
    char path[1200];snprintf(path,sizeof path,"%s/meta.json",dir);
    FILE*f=fopen(path,"w");if(!f)return;
    fprintf(f,"{\"schema_version\":%u,\"iso\":\"melee\"}\n",schema);
    fclose(f);
}

/* ================================================================== */
/* Generic character + costume extractor                              */
/* ================================================================== */

/* Costume -> mesh DAT arrays, per character (order = costume index).  */
/* Transcribed from the community DAT mapping used by the Slippi        */
/* ecosystem (behavioral reference only — data, not code).             */
static const char* mc_mario_meshes[]={"PlMrNr.dat","PlMrYe.dat","PlMrBk.dat","PlMrBu.dat","PlMrGr.dat"};
static const char* mc_fox_meshes[]={"PlFxNr.dat","PlFxOr.dat","PlFxLa.dat","PlFxGr.dat"};
static const char* mc_captain_falcon_meshes[]={"PlCaNr.dat","PlCaGy.dat","PlCaRe.dat","PlCaWh.dat","PlCaGr.dat","PlCaBu.dat"};
static const char* mc_donkey_kong_meshes[]={"PlDkNr.dat","PlDkBk.dat","PlDkRe.dat","PlDkBu.dat","PlDkGr.dat"};
static const char* mc_kirby_meshes[]={"PlKbNr.dat","PlKbYe.dat","PlKbBu.dat","PlKbRe.dat","PlKbGr.dat","PlKbWh.dat"};
static const char* mc_bowser_meshes[]={"PlKpNr.dat","PlKpRe.dat","PlKpBu.dat","PlKpBk.dat"};
static const char* mc_link_meshes[]={"PlLkNr.dat","PlLkRe.dat","PlLkBu.dat","PlLkBk.dat","PlLkWh.dat"};
static const char* mc_sheik_meshes[]={"PlSkNr.dat","PlSkRe.dat","PlSkBu.dat","PlSkGr.dat","PlSkWh.dat"};
static const char* mc_ness_meshes[]={"PlNsNr.dat","PlNsYe.dat","PlNsBu.dat","PlNsGr.dat"};
static const char* mc_peach_meshes[]={"PlPeNr.dat","PlPeYe.dat","PlPeWh.dat","PlPeBu.dat","PlPeGr.dat"};
static const char* mc_popo_meshes[]={"PlPpNr.dat","PlPpGr.dat","PlPpOr.dat","PlPpRe.dat"};
static const char* mc_nana_meshes[]={"PlNnNr.dat","PlNnYe.dat","PlNnAq.dat","PlNnWh.dat"};
static const char* mc_pikachu_meshes[]={"PlPkNr.dat","PlPkRe.dat","PlPkBu.dat","PlPkGr.dat"};
static const char* mc_samus_meshes[]={"PlSsNr.dat","PlSsPi.dat","PlSsBk.dat","PlSsGr.dat","PlSsLa.dat"};
static const char* mc_yoshi_meshes[]={"PlYsNr.dat","PlYsRe.dat","PlYsBu.dat","PlYsYe.dat","PlYsPi.dat","PlYsAq.dat"};
static const char* mc_jigglypuff_meshes[]={"PlPrNr.dat","PlPrRe.dat","PlPrBu.dat","PlPrGr.dat","PlPrYe.dat"};
static const char* mc_mewtwo_meshes[]={"PlMtNr.dat","PlMtRe.dat","PlMtBu.dat","PlMtGr.dat"};
static const char* mc_luigi_meshes[]={"PlLgNr.dat","PlLgWh.dat","PlLgAq.dat","PlLgPi.dat"};
static const char* mc_marth_meshes[]={"PlMsNr.dat","PlMsRe.dat","PlMsGr.dat","PlMsBk.dat","PlMsWh.dat"};
static const char* mc_zelda_meshes[]={"PlZdNr.dat","PlZdRe.dat","PlZdBu.dat","PlZdGr.dat","PlZdWh.dat"};
static const char* mc_young_link_meshes[]={"PlClNr.dat","PlClRe.dat","PlClBu.dat","PlClWh.dat","PlClBk.dat"};
static const char* mc_dr_mario_meshes[]={"PlDrNr.dat","PlDrRe.dat","PlDrBu.dat","PlDrGr.dat","PlDrBk.dat"};
static const char* mc_falco_meshes[]={"PlFcNr.dat","PlFcRe.dat","PlFcBu.dat","PlFcGr.dat"};
static const char* mc_pichu_meshes[]={"PlPcNr.dat","PlPcRe.dat","PlPcBu.dat","PlPcGr.dat"};
static const char* mc_mr_game_and_watch_meshes[]={"PlGwNr.dat","PlGwNr.dat","PlGwNr.dat","PlGwNr.dat"};
static const char* mc_ganondorf_meshes[]={"PlGnNr.dat","PlGnRe.dat","PlGnBu.dat","PlGnGr.dat","PlGnLa.dat"};
static const char* mc_roy_meshes[]={"PlFeNr.dat","PlFeRe.dat","PlFeBu.dat","PlFeGr.dat","PlFeYe.dat"};

typedef struct{
    const char*name;      /* canonical lowercase name (also accepted by --char=) */
    const char*data_dat;  /* Pl<pre>.dat  : ftData action table */
    const char*anim_dat;  /* Pl<pre>AJ.dat: animation clips */
    const char*const*meshes; /* costume mesh dats, indexed by costume index */
    int nmeshes;
    int ckind;            /* CSS / external character id */
}char_info_t;

static const char_info_t CHAR_INFO[]={
    {"mario", "PlMr.dat", "PlMrAJ.dat", mc_mario_meshes, 5, 8},
    {"fox", "PlFx.dat", "PlFxAJ.dat", mc_fox_meshes, 4, 2},
    {"captain_falcon", "PlCa.dat", "PlCaAJ.dat", mc_captain_falcon_meshes, 6, 0},
    {"donkey_kong", "PlDk.dat", "PlDkAJ.dat", mc_donkey_kong_meshes, 5, 1},
    {"kirby", "PlKb.dat", "PlKbAJ.dat", mc_kirby_meshes, 6, 4},
    {"bowser", "PlKp.dat", "PlKpAJ.dat", mc_bowser_meshes, 4, 5},
    {"link", "PlLk.dat", "PlLkAJ.dat", mc_link_meshes, 5, 6},
    {"sheik", "PlSk.dat", "PlSkAJ.dat", mc_sheik_meshes, 5, 19},
    {"ness", "PlNs.dat", "PlNsAJ.dat", mc_ness_meshes, 4, 11},
    {"peach", "PlPe.dat", "PlPeAJ.dat", mc_peach_meshes, 5, 12},
    {"popo", "PlPp.dat", "PlPpAJ.dat", mc_popo_meshes, 4, 14},
    {"nana", "PlNn.dat", "PlNnAJ.dat", mc_nana_meshes, 4, 14},
    {"pikachu", "PlPk.dat", "PlPkAJ.dat", mc_pikachu_meshes, 4, 13},
    {"samus", "PlSs.dat", "PlSsAJ.dat", mc_samus_meshes, 5, 16},
    {"yoshi", "PlYs.dat", "PlYsAJ.dat", mc_yoshi_meshes, 6, 17},
    {"jigglypuff", "PlPr.dat", "PlPrAJ.dat", mc_jigglypuff_meshes, 5, 15},
    {"mewtwo", "PlMt.dat", "PlMtAJ.dat", mc_mewtwo_meshes, 4, 10},
    {"luigi", "PlLg.dat", "PlLgAJ.dat", mc_luigi_meshes, 4, 7},
    {"marth", "PlMs.dat", "PlMsAJ.dat", mc_marth_meshes, 5, 9},
    {"zelda", "PlZd.dat", "PlZdAJ.dat", mc_zelda_meshes, 5, 18},
    {"young_link", "PlCl.dat", "PlClAJ.dat", mc_young_link_meshes, 5, 21},
    {"dr_mario", "PlDr.dat", "PlDrAJ.dat", mc_dr_mario_meshes, 5, 22},
    {"falco", "PlFc.dat", "PlFcAJ.dat", mc_falco_meshes, 4, 20},
    {"pichu", "PlPc.dat", "PlPcAJ.dat", mc_pichu_meshes, 4, 24},
    {"mr_game_and_watch", "PlGw.dat", "PlGwAJ.dat", mc_mr_game_and_watch_meshes, 4, 3},
    {"ganondorf", "PlGn.dat", "PlGnAJ.dat", mc_ganondorf_meshes, 5, 25},
    {"roy", "PlFe.dat", "PlFeAJ.dat", mc_roy_meshes, 5, 23},
};
#define CHAR_INFO_N (sizeof(CHAR_INFO)/sizeof(CHAR_INFO[0]))

/* resolve --char value to a char_info (by name, or by raw DAT prefix). */
static const char_info_t*char_lookup(const char*key){
    for(size_t i=0;i<CHAR_INFO_N;i++)
        if(strcasecmp(key,CHAR_INFO[i].name)==0)return &CHAR_INFO[i];
    /* raw prefix form e.g. "Fc" -> find by data_dat "PlFc.dat" */
    for(size_t i=0;i<CHAR_INFO_N;i++){
        char want[32];snprintf(want,sizeof want,"Pl%s.dat",key);
        if(strcasecmp(want,CHAR_INFO[i].data_dat)==0)return &CHAR_INFO[i];
    }
    return NULL;
}

/* find a *_joint mesh root (skipping shapeanim/matanim) in a mesh dat */
static uint32_t find_mesh_root(const dat_t*d){
    for(uint32_t i=0;i<d->root_count;i++){
        uint32_t data_off=r32(d->bytes+d->roots_start+(size_t)i*8);
        uint32_t name_off=r32(d->bytes+d->roots_start+(size_t)i*8+4);
        const char*nm=dat_name(d,name_off);
        if(strstr(nm,"_joint")&&!strstr(nm,"shapeanim")&&!strstr(nm,"matanim"))return data_off;
    }
    return 0;
}

/* Decode a character's full animation bank once.  Every costume of the same
   character shares Pl<X>AJ.dat, so the result is reused across costumes. */
static asset_anims_t*decode_character_anims(const fst_list_t*dats,const uint8_t*iso,
                                            const char*data_dat,const char*anim_dat,
                                            uint32_t*ok_out){
    const fst_file_t*fc=iso_find(dats,data_dat);
    const fst_file_t*ajf=iso_find(dats,anim_dat);
    if(!fc||!ajf)return NULL;
    dat_t dc;if(dat_open(iso+fc->offset,fc->size,&dc))return NULL;
    action_entry_t*acts;uint32_t nact;
    if(extract_actions(&dc,&acts,&nact)!=0)return NULL;
    asset_anims_t*anims=calloc(1,sizeof(asset_anims_t));
    anims->actions=calloc(nact?nact:1,sizeof(asset_action_t));
    anims->action_count=nact;
    uint32_t ok=0;
    for(uint32_t i=0;i<nact;i++){
        if(acts[i].anim_off>=ajf->size)continue;
        asset_action_t*act=&anims->actions[i];
        if(decode_clip(iso+ajf->offset,ajf->size,acts[i].anim_off,
                       acts[i].anim_size?acts[i].anim_size:(ajf->size-acts[i].anim_off),
                       &acts[i],act)==0){
            if(act->joint_count)ok++;
        }
    }
    if(ok_out)*ok_out=ok;
    printf("  decoded %u/%u clips with joints\n",ok,nact);
    free(acts);
    return anims;
}

/* extract model + animations for one character+costume into <out>/<slug>.* */
static void extract_char(const fst_list_t*dats,const uint8_t*iso,
                         const char*data_dat,const char*mesh_dat,const char*anim_dat,
                         const char*slug,const char*out,asset_anims_t*shared_anims){
    const fst_file_t*fc=iso_find(dats,data_dat);
    const fst_file_t*nrf=iso_find(dats,mesh_dat);
    if(!fc&&!nrf){fprintf(stderr,"extract: %s / %s not on disc\n",data_dat,mesh_dat);return;}

    dat_t dc;int have_dc=fc&&dat_open(iso+fc->offset,fc->size,&dc)==0;
    dobj_filter_t filter;memset(&filter,0,sizeof filter);
    if(have_dc)parse_high_poly_filter(&dc,&filter);

    /* mesh from the chosen costume file, or fall back to the main dat */
    const fst_file_t*mesh_src=nrf?nrf:fc;
    dat_t dm;
    if(dat_open(iso+mesh_src->offset,mesh_src->size,&dm)){fprintf(stderr,"extract: cannot open %s\n",mesh_src->path);return;}
    uint32_t root_rel=find_mesh_root(&dm);
    if(root_rel){
        asset_model_t*model=build_model(&dm,root_rel,filter.enabled?&filter:NULL);
        char mname[128];snprintf(mname,sizeof mname,"%s.model",slug);
        write_model_file(out,mname,model);
    }else{
        printf("  (%s: no *_joint mesh root, skipping model)\n",slug);
    }

    /* animations: action table from main dat, clips from AJ dat */
    if(shared_anims){
        char aname[128];snprintf(aname,sizeof aname,"%s.anims",slug);
        write_anims_file(out,aname,shared_anims);
        return;
    }
    const fst_file_t*ajf=iso_find(dats,anim_dat);
    if(have_dc&&ajf){
        uint32_t ok=0;
        asset_anims_t*anims=decode_character_anims(dats,iso,data_dat,anim_dat,&ok);
        if(anims){
            char aname[128];snprintf(aname,sizeof aname,"%s.anims",slug);
            write_anims_file(out,aname,anims);
        }
    }
}

static void dat_code(const char *bn, char *out, size_t cap) {
    const char *s = bn;
    if (strncasecmp(s, "ef", 2) == 0 || strncasecmp(s, "it", 2) == 0) s += 2;
    size_t o = 0;
    for (; *s && o + 1 < cap; s++) {
        if (strncasecmp(s, "data", 4) == 0 && (s[4] == '.' || s[4] == 0)) break;
        if (strncasecmp(s, ".dat", 4) == 0) break;
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[o++] = (char)c;
    }
    out[o] = 0;
    if (!out[0]) snprintf(out, cap, "unk");
}

/* Order matches melee efAsync_DatEntries; gfx_id = index * 1000 + local. */
static const char *EF_DAT_FILES[] = {
    "EfCoData.dat", "EfMrData.dat", "EfSsData.dat", "EfFxData.dat",
    "EfCaData.dat", "EfKbData.dat", "EfLkData.dat", "EfPkData.dat",
    "EfDkData.dat", "EfYsData.dat", "EfNsData.dat", "EfPrData.dat",
    "EfKpData.dat", "EfMtData.dat", "EfIcData.dat", "EfPeData.dat",
    "EfMsData.dat", "EfZdData.dat", "EfLgData.dat", "EfGnData.dat",
    "EfKbMs.dat", "EfKbZd.dat", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "EfMnData.dat", "EfKbMr.dat", "EfKbFx.dat", "EfKbSs.dat", NULL,
    "EfKbPk.dat", "EfKbLg.dat", "EfKbCa.dat", "EfKbDk.dat", NULL,
    "EfKbKp.dat", NULL, NULL, NULL, NULL, "EfKbIc.dat", "EfKbGn.dat",
    "EfKbFe.dat", "EfFeData.dat", NULL,
};
#define EF_DAT_FILE_N (sizeof EF_DAT_FILES / sizeof EF_DAT_FILES[0])

static int ef_table_index(const char *bn) {
    for (int i = 0; i < (int)EF_DAT_FILE_N; i++)
        if (EF_DAT_FILES[i] && strcasecmp(bn, EF_DAT_FILES[i]) == 0) return i;
    return -1;
}

typedef struct {
    char id[80];
    char file[80];
    int gfx;
    int item_kind;
} catalog_row_t;

typedef struct {
    catalog_row_t *rows;
    size_t count, cap;
} catalog_t;

static void catalog_add(catalog_t *c, const char *id, const char *file, int gfx, int item_kind) {
    if (c->count == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->rows = realloc(c->rows, c->cap * sizeof *c->rows);
        if (!c->rows) die("oom");
    }
    catalog_row_t *row = &c->rows[c->count++];
    snprintf(row->id, sizeof row->id, "%s", id);
    snprintf(row->file, sizeof row->file, "%s", file);
    row->gfx = gfx;
    row->item_kind = item_kind;
}

static const char *alias_for_gfx(int gfx) {
    switch (gfx) {
        case 11: return "shield";
        case 12: return "powershield";
        case 3000: return "shine";
        case 3001: return "shine-start";
        case 3002: return "shine-hit";
        case 3003: return "firefox-charge";
        case 3004: return "firefox";
        default: return NULL;
    }
}

static const char *alias_for_item(int kind) {
    switch (kind) {
        case 0x36: return "fox-laser";
        case 0x37: return "falco-laser";
        case 0x38: return "fox-illusion";
        case 0x39: return "falco-phantasm";
        default: return NULL;
    }
}

/* Fox/Falco article list index → ItemKind. Shot/gun kinds are stored on ext_attr. */
static int fighter_item_kind(const char *name, int index, const dat_t *d,
                             const reloc_idx_t *ri, uint32_t ftdata_rel) {
    uint32_t ext = rdptr(d, ri, ftdata_rel, 4);
    int shot = -1, gun = -1;
    if (ext && ext + 0x24 <= d->len) {
        shot = (int)r32(d->bytes + ext + 0x1C);
        gun = (int)r32(d->bytes + ext + 0x20);
        if (shot < 0 || shot > 255) shot = -1;
        if (gun < 0 || gun > 255) gun = -1;
    }
    if (strcmp(name, "fox") == 0) {
        if (index == 0) return shot >= 0 ? shot : 0x36;
        if (index == 1) return gun >= 0 ? gun : 0x4A;
        if (index == 2) return 0x38;
    } else if (strcmp(name, "falco") == 0) {
        if (index == 0) return shot >= 0 ? shot : 0x37;
        if (index == 1) return gun >= 0 ? gun : 0x4B;
        if (index == 3) return 0x39;
    }
    return -1;
}

static int looks_like_jobj(const dat_t *d, uint32_t abs) {
    if (abs < 0x20 || abs + 0x40 > d->reloc_start) return 0;
    const uint8_t *j = d->bytes + abs;
    uint32_t child = r32(j + 8), next = r32(j + 12), dobj = r32(j + 16);
    float sx = rf32(j + 0x20), sy = rf32(j + 0x24), sz = rf32(j + 0x28);
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sz)) return 0;
    if (fabsf(sx) > 1000.f || fabsf(sy) > 1000.f || fabsf(sz) > 1000.f) return 0;
    uint32_t ptrs[5] = { child, next, dobj, r32(j + 0x38), r32(j + 0x3C) };
    for (int i = 0; i < 5; i++) {
        if (!ptrs[i]) continue;
        uint32_t a = dat_abs(d, ptrs[i]);
        if (a < 0x20 || a >= d->reloc_start) return 0;
    }
    return 1;
}

static int write_joint_model(const dat_t *d, uint32_t joint_abs, const char *out,
                             const char *file) {
    if (!looks_like_jobj(d, joint_abs)) return 0;
    asset_model_t *model = build_model(d, joint_abs - 0x20, NULL);
    if (!model || !model->vertex_count || !model->index_count) return 0;
    write_model_file(out, file, model);
    return 1;
}

static void extract_effect_table(const dat_t *d, const reloc_idx_t *ri, uint32_t table_abs,
                                 int table_index, const char *code, const char *out,
                                 catalog_t *cat, unsigned *wrote) {
    /* Public symbol is EF_DAT_Entry-shaped: +0 particle header, +4 bank, +8 EffectDesc[]. */
    uint32_t descs_abs = table_abs + 8;
    if (descs_abs + 0x14 > d->reloc_start) return;
    uint32_t count = 0;
    for (; count < 512; count++) {
        uint32_t entry_abs = descs_abs + count * 0x14;
        if (entry_abs + 0x14 > d->reloc_start) break;
        uint32_t field = entry_abs + 4;
        uint32_t raw = r32(d->bytes + field);
        if (raw != 0 && !reloc_has(ri, field)) break;
        float life = rf32(d->bytes + entry_abs);
        if (!isfinite(life) || life < 0.f || life > 1e6f) break;
        uint32_t joint = rdptr(d, ri, entry_abs - 0x20, 4);
        if (joint && !looks_like_jobj(d, joint)) break;
    }
    printf("  effect table %s gfx %dxxx count=%u\n", code, table_index * 1000, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry_rel = (descs_abs - 0x20) + i * 0x14;
        uint32_t joint = rdptr(d, ri, entry_rel, 4);
        if (!joint) continue;
        int gfx = table_index >= 0 ? table_index * 1000 + (int)i : -1;
        char file[80];
        snprintf(file, sizeof file, "ef-%s-%u.model", code, i);
        if (!write_joint_model(d, joint, out, file)) continue;
        const char *alias = gfx >= 0 ? alias_for_gfx(gfx) : NULL;
        catalog_add(cat, alias ? alias : file, file, gfx, -1);
        (*wrote)++;
    }
}

static void extract_one_article(const dat_t *d, const reloc_idx_t *ri, uint32_t article,
                                int kind, const char *fallback_file, const char *out,
                                catalog_t *cat, unsigned *wrote);

static void extract_article_array(const dat_t *d, const reloc_idx_t *ri, uint32_t array_abs,
                                  int kind_base, const char *out, catalog_t *cat, unsigned *wrote) {
    if (!array_abs) return;
    size_t span = node_span_len(d, ri, array_abs);
    uint32_t count = (uint32_t)(span / 4);
    if (count > 512) count = 512;
    printf("  article array base=%d count=%u\n", kind_base, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t article = rdptr(d, ri, array_abs - 0x20, i * 4);
        int kind = kind_base + (int)i;
        char fallback[80];
        snprintf(fallback, sizeof fallback, "it-%d.model", kind);
        extract_one_article(d, ri, article, kind, fallback, out, cat, wrote);
    }
}

static void extract_one_article(const dat_t *d, const reloc_idx_t *ri, uint32_t article,
                                int kind, const char *fallback_file, const char *out,
                                catalog_t *cat, unsigned *wrote) {
    if (!article) return;
    uint32_t model_desc = rdptr(d, ri, article - 0x20, 0x10);
    if (!model_desc) return;
    uint32_t joint = rdptr(d, ri, model_desc - 0x20, 0);
    if (!joint) return;
    char file[80];
    if (kind >= 0) snprintf(file, sizeof file, "it-%d.model", kind);
    else snprintf(file, sizeof file, "%s", fallback_file);
    if (!write_joint_model(d, joint, out, file)) return;
    const char *alias = kind >= 0 ? alias_for_item(kind) : NULL;
    char id[80];
    if (alias) snprintf(id, sizeof id, "%s", alias);
    else if (kind >= 0) snprintf(id, sizeof id, "item-%d", kind);
    else snprintf(id, sizeof id, "%s", file);
    catalog_add(cat, id, file, -1, kind);
    (*wrote)++;
}

/* Character projectiles live on ftData.x48_items, not ItCo.dat. */
static void extract_fighter_articles(const fst_list_t *dats, const uint8_t *iso,
                                     const char *out, catalog_t *cat, unsigned *wrote) {
    for (size_t ci = 0; ci < CHAR_INFO_N; ci++) {
        const char_info_t *info = &CHAR_INFO[ci];
        const fst_file_t *fc = iso_find(dats, info->data_dat);
        if (!fc) continue;
        dat_t d;
        if (dat_open(iso + fc->offset, fc->size, &d)) continue;
        reloc_idx_t ri;
        reloc_build(&d, &ri);
        uint32_t ftdata_rel = 0;
        for (uint32_t r = 0; r < d.root_count; r++) {
            uint32_t data_off = r32(d.bytes + d.roots_start + (size_t)r * 8);
            uint32_t name_off = r32(d.bytes + d.roots_start + (size_t)r * 8 + 4);
            if (strstr(dat_name(&d, name_off), "ftData")) { ftdata_rel = data_off; break; }
        }
        if (!ftdata_rel) { free(ri.offs); continue; }
        uint32_t items = rdptr(&d, &ri, ftdata_rel, 0x48);
        if (!items) { free(ri.offs); continue; }
        size_t span = node_span_len(&d, &ri, items);
        uint32_t count = (uint32_t)(span / 4);
        if (count > 32) count = 32;
        printf("  %s fighter articles count=%u\n", info->name, count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t article = rdptr(&d, &ri, items - 0x20, i * 4);
            int kind = fighter_item_kind(info->name, (int)i, &d, &ri, ftdata_rel);
            char fallback[80];
            snprintf(fallback, sizeof fallback, "pl-%s-%u.model", info->name, i);
            extract_one_article(&d, &ri, article, kind, fallback, out, cat, wrote);
        }
        free(ri.offs);
    }
}

static void write_effects_catalog(const char *dir, const catalog_t *cat) {
    char path[1200];
    snprintf(path, sizeof path, "%s/effects.json", dir);
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write effects.json");
    fputs("{\"schema\":4,\"aliases\":{", f);
    int first = 1;
    for (size_t i = 0; i < cat->count; i++) {
        const char *alias = NULL;
        if (cat->rows[i].gfx >= 0) alias = alias_for_gfx(cat->rows[i].gfx);
        if (!alias && cat->rows[i].item_kind >= 0) alias = alias_for_item(cat->rows[i].item_kind);
        if (!alias) continue;
        fprintf(f, "%s\"%s\":\"%s\"", first ? "" : ",", alias, cat->rows[i].file);
        first = 0;
    }
    fputs("},\"items\":{", f);
    first = 1;
    for (size_t i = 0; i < cat->count; i++) {
        if (cat->rows[i].item_kind < 0) continue;
        fprintf(f, "%s\"%d\":\"%s\"", first ? "" : ",", cat->rows[i].item_kind, cat->rows[i].file);
        first = 0;
    }
    fputs("},\"gfx\":{", f);
    first = 1;
    for (size_t i = 0; i < cat->count; i++) {
        if (cat->rows[i].gfx < 0) continue;
        fprintf(f, "%s\"%d\":\"%s\"", first ? "" : ",", cat->rows[i].gfx, cat->rows[i].file);
        first = 0;
    }
    fputs("}}\n", f);
    fclose(f);
    printf("wrote effects.json (%zu entries)\n", cat->count);
}

/* Walk eff*DataTable / itPublicData and dump every JOBJ mesh plus a catalog. */
static void extract_effects(const fst_list_t *dats, const uint8_t *iso, const char *out) {
    unsigned wrote = 0;
    catalog_t cat = {0};
    for (size_t i = 0; i < dats->count; i++) {
        const char *path = dats->items[i].path;
        const char *slash = strrchr(path, '/');
        const char *bn = slash ? slash + 1 : path;
        if (strncasecmp(bn, "It", 2) != 0 && strncasecmp(bn, "Ef", 2) != 0) continue;
        dat_t d;
        if (dat_open(iso + dats->items[i].offset, dats->items[i].size, &d)) continue;
        reloc_idx_t ri; reloc_build(&d, &ri);
        char code[32];
        dat_code(bn, code, sizeof code);
        printf("extracting effects from %s (%u roots)\n", bn, d.root_count);
        for (uint32_t r = 0; r < d.root_count; r++) {
            uint32_t data_off = r32(d.bytes + d.roots_start + (size_t)r * 8);
            uint32_t name_off = r32(d.bytes + d.roots_start + (size_t)r * 8 + 4);
            const char *nm = dat_name(&d, name_off);
            uint32_t abs = dat_abs(&d, data_off);
            if (strstr(nm, "DataTable") || strstr(nm, "eff")) {
                extract_effect_table(&d, &ri, abs, ef_table_index(bn), code, out, &cat, &wrote);
            } else if (strstr(nm, "itPublicData") || strstr(nm, "PublicData")) {
                /* it_804D6D20_t: x4 common articles, x8 character articles. */
                uint32_t common = rdptr(&d, &ri, data_off, 4);
                uint32_t character = rdptr(&d, &ri, data_off, 8);
                extract_article_array(&d, &ri, common, 0, out, &cat, &wrote);
                extract_article_array(&d, &ri, character, 43, out, &cat, &wrote);
            } else if (strstr(nm, "_joint") && !strstr(nm, "shapeanim") && !strstr(nm, "matanim")) {
                char file[160];
                snprintf(file, sizeof file, "%s-%u.model", code, r);
                if (write_joint_model(&d, abs, out, file)) {
                    catalog_add(&cat, file, file, -1, -1);
                    wrote++;
                }
            }
        }
        free(ri.offs);
    }
    extract_fighter_articles(dats, iso, out, &cat, &wrote);
    write_effects_catalog(out, &cat);
    free(cat.rows);
    printf("wrote %u item/effect meshes\n", wrote);
}

/* ================================================================== */
/* Stock icons from IfAll.dat::Stc_scemdls                            */
/* ================================================================== */

/* CSS id + costume → TOBJ animation frame. Zelda/Sheik share a CSS pair;
   Sheik's fighter kind (7) selects the later atlas row. After Sheik the
   remaining CSS ids skip that shared slot (`ckind - 1`). */
static int stock_icon_index(int ckind, int costume, int ftkind) {
    int base;
    if (ckind == 18 || ckind == 19) base = (ftkind == 7) ? 25 : 18;
    else if (ckind == 14) base = 14;
    else if (ckind > 19) base = ckind - 1;
    else base = ckind;
    return base + costume * 30;
}

static void png_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void write_png_chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t lenb[4], crc_b[4];
    png_be32(lenb, (uint32_t)len);
    fwrite(lenb, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc32(0L, Z_NULL, 0);
    c = crc32(c, (const Bytef *)type, 4);
    if (len) c = crc32(c, data, (uInt)len);
    png_be32(crc_b, c);
    fwrite(crc_b, 1, 4, f);
}

static int write_png_rgba(const char *path, uint16_t w, uint16_t h, const uint8_t *rgba) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    png_be32(ihdr, w); png_be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    write_png_chunk(f, "IHDR", ihdr, 13);
    size_t raw_len = ((size_t)w * 4 + 1) * h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) { fclose(f); return -1; }
    size_t o = 0;
    for (uint16_t y = 0; y < h; y++) {
        raw[o++] = 0;
        memcpy(raw + o, rgba + (size_t)y * w * 4, (size_t)w * 4);
        o += (size_t)w * 4;
    }
    uLongf clen = compressBound(raw_len);
    uint8_t *z = malloc(clen);
    int rc = z && compress2(z, &clen, raw, (uLong)raw_len, Z_BEST_COMPRESSION) == Z_OK ? 0 : -1;
    if (rc == 0) write_png_chunk(f, "IDAT", z, clen);
    write_png_chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z);
    return rc;
}

static int decode_image_abs(const dat_t *d, uint32_t image_abs, uint32_t tlut_abs,
                            uint16_t *w_out, uint16_t *h_out, uint8_t **rgba_out) {
    if (!image_abs || image_abs + 0x18 > d->len) return -1;
    const uint8_t *im = d->bytes + image_abs;
    uint32_t data_rel = r32(im);
    uint16_t w = r16(im + 4), h = r16(im + 6);
    uint32_t fmt = r32(im + 8);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) return -1;
    uint32_t *pal = NULL;
    size_t paln = 0;
    if (tlut_abs && tlut_abs + 0x10 <= d->len) {
        const uint8_t *tl = d->bytes + tlut_abs;
        uint32_t ldata = r32(tl), pal_fmt = r32(tl + 4);
        uint16_t ncols = r16(tl + 0x0C);
        if (ldata && ncols) {
            uint32_t lda = dat_abs(d, ldata);
            paln = ncols;
            pal = malloc(ncols * sizeof(uint32_t));
            if (!pal) return -1;
            for (uint16_t i = 0; i < ncols && lda + (uint32_t)i * 2 + 2 <= d->len; i++) {
                uint16_t p = r16(d->bytes + lda + (uint32_t)i * 2);
                if (pal_fmt == 0) pal[i] = pal_entry_ia8(p);
                else if (pal_fmt == 1) pal[i] = pal_entry_rgb565(p);
                else pal[i] = pal_entry_rgb5a3(p);
            }
        }
    }
    size_t ts = 0;
    switch (fmt) {
        case 0: case 8: case 14: ts = (size_t)((w + 7) / 8) * ((h + 7) / 8) * 32; break;
        case 1: case 2: case 9: ts = (size_t)((w + 7) / 8) * ((h + 3) / 4) * 32; break;
        case 3: case 4: case 5: case 10: ts = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 32; break;
        case 6: ts = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 64; break;
        default: ts = (size_t)w * h * 4; break;
    }
    uint32_t da = dat_abs(d, data_rel);
    if (da >= d->len) { free(pal); return -1; }
    if (da + ts > d->len) ts = d->len - da;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) { free(pal); return -1; }
    decode_texture(fmt, w, h, d->bytes + da, pal, paln, rgba);
    free(pal);
    *w_out = w; *h_out = h; *rgba_out = rgba;
    return 0;
}

typedef struct {
    uint32_t imagetbl, tluttbl, aobj;
    uint16_t n_image, n_tlut;
} texanim_ref_t;

static void consider_texanim(const dat_t *d, const reloc_idx_t *ri, uint32_t tex_abs,
                             texanim_ref_t *best) {
    while (tex_abs && tex_abs + 0x18 <= d->len) {
        uint32_t rel = tex_abs - 0x20;
        uint32_t imagetbl = rdptr(d, ri, rel, 0x0C);
        uint32_t tluttbl = rdptr(d, ri, rel, 0x10);
        uint32_t aobj = rdptr(d, ri, rel, 0x08);
        uint16_t n_image = r16(d->bytes + tex_abs + 0x14);
        uint16_t n_tlut = r16(d->bytes + tex_abs + 0x16);
        if (n_image > best->n_image && imagetbl) {
            best->imagetbl = imagetbl;
            best->tluttbl = tluttbl;
            best->aobj = aobj;
            best->n_image = n_image;
            best->n_tlut = n_tlut;
        }
        tex_abs = rdptr(d, ri, rel, 0);
    }
}

static void walk_matanim(const dat_t *d, const reloc_idx_t *ri, uint32_t mat_abs,
                         texanim_ref_t *best, int depth) {
    for (int i = 0; mat_abs && mat_abs + 0x10 <= d->len && i < 64; i++) {
        uint32_t rel = mat_abs - 0x20;
        consider_texanim(d, ri, rdptr(d, ri, rel, 8), best);
        mat_abs = rdptr(d, ri, rel, 0);
    }
    (void)depth;
}

static void walk_matanim_joint(const dat_t *d, const reloc_idx_t *ri, uint32_t joint_abs,
                               texanim_ref_t *best, int depth) {
    if (!joint_abs || joint_abs + 12 > d->len || depth > 24) return;
    uint32_t rel = joint_abs - 0x20;
    walk_matanim(d, ri, rdptr(d, ri, rel, 8), best, depth);
    walk_matanim_joint(d, ri, rdptr(d, ri, rel, 0), best, depth + 1);
    walk_matanim_joint(d, ri, rdptr(d, ri, rel, 4), best, depth);
}

static uint32_t first_tobj_tlut(const dat_t *d, const reloc_idx_t *ri, uint32_t jobj_abs, int depth) {
    if (!jobj_abs || jobj_abs + 0x40 > d->len || depth > 24) return 0;
    uint32_t rel = jobj_abs - 0x20;
    uint32_t dobj = rdptr(d, ri, rel, 0x10);
    while (dobj && dobj + 0x10 <= d->len) {
        uint32_t drel = dobj - 0x20;
        uint32_t mobj = rdptr(d, ri, drel, 8);
        if (mobj && mobj + 0x18 <= d->len) {
            uint32_t tobj = rdptr(d, ri, mobj - 0x20, 8);
            if (tobj && tobj + 0x5C <= d->len) {
                uint32_t tlut = rdptr(d, ri, tobj - 0x20, 0x50);
                if (tlut) return tlut;
            }
        }
        dobj = rdptr(d, ri, drel, 4);
    }
    uint32_t found = first_tobj_tlut(d, ri, rdptr(d, ri, rel, 8), depth + 1);
    if (found) return found;
    return first_tobj_tlut(d, ri, rdptr(d, ri, rel, 0x0C), depth);
}

static int collect_stc_texanim(const dat_t *d, const reloc_idx_t *ri, uint32_t root_abs,
                               texanim_ref_t *best, uint32_t *fallback_tlut) {
    /* Root is DynamicModelDesc**: an array of model pointers. */
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t desc = rdptr(d, ri, root_abs - 0x20, i * 4);
        if (!desc) break;
        uint32_t drel = desc - 0x20;
        uint32_t joint = rdptr(d, ri, drel, 0);
        uint32_t matanims = rdptr(d, ri, drel, 8);
        if (matanims) {
            uint32_t mat0 = rdptr(d, ri, matanims - 0x20, 0);
            walk_matanim_joint(d, ri, mat0, best, 0);
        }
        if (joint && !*fallback_tlut) *fallback_tlut = first_tobj_tlut(d, ri, joint, 0);
    }
    return best->n_image ? 0 : -1;
}

static uint32_t tlut_for_index(const dat_t *d, const reloc_idx_t *ri, const texanim_ref_t *atlas,
                               uint32_t fallback, int index) {
    if (!atlas->tluttbl || atlas->n_tlut == 0) return fallback;
    int slot = index;
    if (slot >= atlas->n_tlut) slot = 0;
    uint32_t tlut = rdptr(d, ri, atlas->tluttbl - 0x20, (uint32_t)slot * 4);
    return tlut ? tlut : fallback;
}

typedef struct { float frame; float value; } fobj_key_t;

static int parse_fobj_keys(const dat_t *d, const reloc_idx_t *ri, uint32_t aobj_abs,
                           int want_type, fobj_key_t **out, uint32_t *n_out) {
    *out = NULL; *n_out = 0;
    if (!aobj_abs || aobj_abs + 0x10 > d->len) return -1;
    uint32_t fobj_abs = rdptr(d, ri, aobj_abs - 0x20, 8);
    while (fobj_abs && fobj_abs + 0x14 <= d->len) {
        uint32_t rel = fobj_abs - 0x20;
        uint8_t type = d->bytes[fobj_abs + 12];
        uint8_t vflag = d->bytes[fobj_abs + 13];
        uint8_t sflag = d->bytes[fobj_abs + 14];
        if (type == want_type) {
            uint32_t da = rdptr(d, ri, rel, 0x10);
            uint32_t length = r32(d->bytes + fobj_abs + 4);
            float start = rf32(d->bytes + fobj_abs + 8);
            if (!da) return -1;
            size_t stream_len = length;
            if (stream_len == 0 || da + stream_len > d->len) stream_len = d->len - da;
            int vfmt = (vflag >> 5) & 7, vshift = vflag & 0x1F;
            int sfmt = (sflag >> 5) & 7, sshift = sflag & 0x1F;
            size_t cur = 0;
            uint32_t cap = 32, nk = 0;
            fobj_key_t *keys = malloc(cap * sizeof(fobj_key_t));
            float frame = start;
            while (cur < stream_len) {
                uint32_t packed = read_packed(d->bytes + da, &cur, stream_len);
                uint32_t op = packed & 0x0F;
                if (op == 0) break;
                uint32_t kcount = (packed >> 4) + 1;
                for (uint32_t q = 0; q < kcount; q++) {
                    float val = 0;
                    if (op == 1 || op == 2 || op == 3) {
                        val = read_packed_value(d->bytes + da, &cur, stream_len, vfmt, vshift);
                    } else if (op == 4) {
                        val = read_packed_value(d->bytes + da, &cur, stream_len, vfmt, vshift);
                        read_packed_value(d->bytes + da, &cur, stream_len, sfmt, sshift);
                    } else if (op == 5) {
                        read_packed_value(d->bytes + da, &cur, stream_len, sfmt, sshift);
                        continue;
                    } else if (op == 6) {
                        read_packed_value(d->bytes + da, &cur, stream_len, vfmt, vshift);
                        continue;
                    }
                    if (nk == cap) { cap *= 2; keys = realloc(keys, cap * sizeof(fobj_key_t)); }
                    keys[nk].frame = frame;
                    keys[nk].value = val;
                    nk++;
                    frame += (float)read_packed(d->bytes + da, &cur, stream_len);
                }
            }
            *out = keys; *n_out = nk;
            return 0;
        }
        fobj_abs = rdptr(d, ri, rel, 0);
    }
    return -1;
}

static int sample_step(const fobj_key_t *keys, uint32_t n, float frame, int fallback) {
    if (!n) return fallback;
    uint32_t best = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (keys[i].frame <= frame + 1e-3f) best = i;
        else break;
    }
    int v = (int)(keys[best].value + (keys[best].value < 0 ? -0.5f : 0.5f));
    return v;
}

static void extract_stock_icons(const fst_list_t *dats, const uint8_t *iso, const char *out) {
    const fst_file_t *file = iso_find(dats, "IfAll.dat");
    if (!file) { fprintf(stderr, "extract: IfAll.dat not on disc\n"); return; }
    dat_t d;
    if (dat_open(iso + file->offset, file->size, &d)) {
        fprintf(stderr, "extract: cannot open IfAll.dat\n");
        return;
    }
    reloc_idx_t ri;
    reloc_build(&d, &ri);
    uint32_t stc_root = 0;
    for (uint32_t r = 0; r < d.root_count; r++) {
        uint32_t data_off = r32(d.bytes + d.roots_start + (size_t)r * 8);
        uint32_t name_off = r32(d.bytes + d.roots_start + (size_t)r * 8 + 4);
        if (strcmp(dat_name(&d, name_off), "Stc_scemdls") == 0) {
            stc_root = dat_abs(&d, data_off);
            break;
        }
    }
    if (!stc_root) { fprintf(stderr, "extract: Stc_scemdls missing\n"); free(ri.offs); return; }
    texanim_ref_t atlas = {0};
    uint32_t fallback_tlut = 0;
    if (collect_stc_texanim(&d, &ri, stc_root, &atlas, &fallback_tlut) != 0) {
        fprintf(stderr, "extract: no stock-icon TexAnim in Stc_scemdls\n");
        free(ri.offs);
        return;
    }
    printf("stock icons: %u images, %u palettes\n", atlas.n_image, atlas.n_tlut);
    fobj_key_t *timg_keys = NULL, *tclt_keys = NULL;
    uint32_t n_timg = 0, n_tclt = 0;
    parse_fobj_keys(&d, &ri, atlas.aobj, 1, &timg_keys, &n_timg);
    parse_fobj_keys(&d, &ri, atlas.aobj, 10, &tclt_keys, &n_tclt);
    printf("  TIMG keys=%u TCLT keys=%u\n", n_timg, n_tclt);

    uint8_t **decoded = calloc(atlas.n_image, sizeof(uint8_t *));
    uint16_t *widths = calloc(atlas.n_image, sizeof(uint16_t));
    uint16_t *heights = calloc(atlas.n_image, sizeof(uint16_t));
    if (!decoded || !widths || !heights) die("oom");
    unsigned decoded_n = 0;
    for (uint16_t i = 0; i < atlas.n_image; i++) {
        uint32_t image = rdptr(&d, &ri, atlas.imagetbl - 0x20, (uint32_t)i * 4);
        if (!image) continue;
        uint32_t tlut = tlut_for_index(&d, &ri, &atlas, fallback_tlut, i);
        if (decode_image_abs(&d, image, tlut, &widths[i], &heights[i], &decoded[i]) == 0)
            decoded_n++;
    }
    printf("  decoded %u/%u image descriptors\n", decoded_n, atlas.n_image);

    char icon_dir[1100];
    snprintf(icon_dir, sizeof icon_dir, "%s/icons", out);
    if (mkdir(icon_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "extract: cannot create %s\n", icon_dir);
        for (uint16_t i = 0; i < atlas.n_image; i++) free(decoded[i]);
        free(decoded); free(widths); free(heights); free(timg_keys); free(tclt_keys); free(ri.offs);
        return;
    }

    unsigned wrote = 0;
    for (size_t c = 0; c < CHAR_INFO_N; c++) {
        const char_info_t *ci = &CHAR_INFO[c];
        for (int col = 0; col < ci->nmeshes; col++) {
            int anim = stock_icon_index(ci->ckind, col, (int)c);
            int idx = sample_step(timg_keys, n_timg, (float)anim, anim);
            int used = col;
            while ((idx < 0 || idx >= atlas.n_image || !decoded[idx]) && used > 0) {
                used--;
                anim = stock_icon_index(ci->ckind, used, (int)c);
                idx = sample_step(timg_keys, n_timg, (float)anim, anim);
            }
            if (idx < 0 || idx >= atlas.n_image || !decoded[idx]) {
                fprintf(stderr, "extract: no stock icon for %s costume %d\n", ci->name, col);
                continue;
            }
            char path[1200];
            snprintf(path, sizeof path, "%s/icons/%s-%d.png", out, ci->name, col);
            if (write_png_rgba(path, widths[idx], heights[idx], decoded[idx]) == 0) wrote++;
        }
    }
    for (uint16_t i = 0; i < atlas.n_image; i++) free(decoded[i]);
    free(decoded); free(widths); free(heights); free(timg_keys); free(tclt_keys); free(ri.offs);
    printf("wrote %u character/costume stock icons\n", wrote);
}

int main(int argc,char**argv){
    const char*iso_path="fixtures/game.iso";
    const char*out="cache";
    const char*which_char=NULL,*which_stage=NULL;
    int want_effects=0,all_mode=0,want_icons=0;
    for(int i=1;i<argc;i++){
        if(strncmp(argv[i],"--iso=",6)==0)iso_path=argv[i]+6;
        else if(strncmp(argv[i],"--out=",6)==0)out=argv[i]+6;
        else if(strncmp(argv[i],"--char=",7)==0)which_char=argv[i]+7;
        else if(strncmp(argv[i],"--stage=",8)==0)which_stage=argv[i]+8;
        else if(strcmp(argv[i],"--effects")==0)want_effects=1;
        else if(strcmp(argv[i],"--icons")==0)want_icons=1;
        else if(strcmp(argv[i],"--all")==0){
            all_mode=1;want_effects=1;want_icons=1;
            if(!which_char)which_char="all";
            if(!which_stage)which_stage="all";
        }
    }

    FILE*iso=fopen(iso_path,"rb");if(!iso)die("cannot open iso");
    fseek(iso,0,SEEK_END);long sz=ftell(iso);rewind(iso);
    uint8_t*iso_bytes=malloc((size_t)sz);
    if(fread(iso_bytes,1,(size_t)sz,iso)!=(size_t)sz)die("read iso");
    fclose(iso);

    fst_list_t dats=iso_index_dats(iso_bytes,(size_t)sz);
    printf("FST indexed %zu .dat files\n",dats.count);

    if(mkdir(out,0755)!=0&&errno!=EEXIST)die("cannot create output directory");

    if(which_char){
        if(strcasecmp(which_char,"all")==0||all_mode){
            for(size_t c=0;c<CHAR_INFO_N;c++){
                const char_info_t*ci=&CHAR_INFO[c];
                asset_anims_t*anims=NULL;
                for(int col=0;col<ci->nmeshes;col++){
                    char slug[128];snprintf(slug,sizeof slug,"%s-%d",ci->name,col);
                    if(!anims)anims=decode_character_anims(&dats,iso_bytes,
                                                          ci->data_dat,ci->anim_dat,NULL);
                    printf("extracting %s (costume %d) mesh=%s\n",ci->name,col,ci->meshes[col]);
                    extract_char(&dats,iso_bytes,ci->data_dat,ci->meshes[col],ci->anim_dat,
                                 slug,out,anims);
                }
            }
        }else{
            const char_info_t*ci=char_lookup(which_char);
            if(!ci){fprintf(stderr,"extract: unknown character '%s'\n",which_char);return 1;}
            int have_color=0,color_idx=0;
            for(int i=1;i<argc;i++)
                if(strncmp(argv[i],"--color=",8)==0){color_idx=atoi(argv[i]+8);have_color=1;break;}
            if(have_color){
                if(color_idx<0||color_idx>=ci->nmeshes){
                    fprintf(stderr,"extract: %s has no costume index %d (0..%d)\n",ci->name,color_idx,ci->nmeshes-1);
                    return 1;
                }
                const char*mesh=ci->meshes[color_idx];
                char slug[128];snprintf(slug,sizeof slug,"%s-%d",ci->name,color_idx);
                printf("extracting %s (costume %d) mesh=%s\n",ci->name,color_idx,mesh);
                extract_char(&dats,iso_bytes,ci->data_dat,mesh,ci->anim_dat,slug,out,NULL);
            }else{
                asset_anims_t*anims=NULL;
                for(int c=0;c<ci->nmeshes;c++){
                    const char*mesh=ci->meshes[c];
                    char slug[128];snprintf(slug,sizeof slug,"%s-%d",ci->name,c);
                    if(!anims)anims=decode_character_anims(&dats,iso_bytes,
                                                          ci->data_dat,ci->anim_dat,NULL);
                    printf("extracting %s (costume %d) mesh=%s\n",ci->name,c,mesh);
                    extract_char(&dats,iso_bytes,ci->data_dat,mesh,ci->anim_dat,slug,out,anims);
                }
            }
        }
    }
    if(which_stage||all_mode){
        const char*stname=NULL;
        if(strcasecmp(which_stage,"all")==0||all_mode){
            for(size_t i=0;i<dats.count;i++){
                const char*p=strrchr(dats.items[i].path,'/');
                const char*bn=p?p+1:dats.items[i].path;
                if(strncmp(bn,"Gr",2)!=0)continue;
                if(strncmp(bn,"GrT",3)==0)continue; /* target-test stages */
                if(strncmp(bn,"GrEF",4)==0)continue; /* event stages */
                dat_t sd;
                if(dat_open(iso_bytes+dats.items[i].offset,dats.items[i].size,&sd))continue;
                asset_stage_t*st=calloc(1,sizeof(asset_stage_t));
                if(decode_stage(&sd,st)==0){
                    char name[256];int n=0;
                    for(const char*q=bn;*q&&*q!='.';q++)name[n++]=tolower((unsigned char)*q);
                    snprintf(name+n,sizeof name-(size_t)n,".stage");
                    write_stage_file(out,name,st);
                    if(strcasecmp(bn,"GrNLa.dat")==0)write_stage_file(out,"fd.stage",st);
                }else{
                    printf("stage decode failed: %s\n",bn);
                }
                free(st->sections);
                free(st->lights);
                free(st);
            }
        }else if(which_stage){
            if(strcasecmp(which_stage,"FD")==0||strcasecmp(which_stage,"final-destination")==0)stname="GrNLa.dat";
            if(stname){
                const fst_file_t*sf=iso_find(&dats,stname);
                if(!sf)die("stage dat not found");
                dat_t sd;
                if(dat_open(iso_bytes+sf->offset,sf->size,&sd))die("stage dat");
                asset_stage_t*st=calloc(1,sizeof(asset_stage_t));
                if(decode_stage(&sd,st)==0){
                    write_stage_file(out,"fd.stage",st);
                }else{
                    printf("stage decode failed\n");
                }
            }
        }
    }
    if(want_effects) extract_effects(&dats,iso_bytes,out);
    if(want_icons || which_char) extract_stock_icons(&dats,iso_bytes,out);
    write_meta(out,ASSET_SCHEMA_VERSION);
    free(iso_bytes);
    printf("done\n");
    return 0;
}
