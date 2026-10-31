


#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../doomdef.h"
#include "../i_system.h"
#include "../hardware/hw_drv.h"
#include "../hardware/hw_glob.h"
#include "hwr3ds.h"
#include "hwr3ds_shbin.h"

#define MAX_HWR_VERTS 65536

#define HWR_CMDBUF_BYTES (4u * 1024u * 1024u)

#define HWR_CMD_RESERVE_WORDS 4096u

#define HWR_TEX_MAX 256


#define TOP_W 320
#define TOP_H 200
#define LCD_W 400
#define LCD_H 240
#define HWR_BORDER_VERTS 24
#ifndef HWR_MAX_BIG_BATCHES
#define HWR_MAX_BIG_BATCHES 1
#endif
#if HWR_MAX_BIG_BATCHES
#define HWR_BATCH_VERTS 6144
#else
#define HWR_BATCH_VERTS 3072
#endif
#define HWR_TEX_HASH_BUCKETS 1024
#define HWR_MAX3_SMART_BLEND 1
#define HWR_MAX3_TRANSFORM_CACHE 1
#define HWR_MAX3_CLIP_CACHE 1
#define HWR_MAX3_SPECIAL_CACHE 1

#define HWR_MAX2_16BIT_OPAQUE 1       
#define HWR_MAX2_VRAM_PRELOAD 1       
#define HWR_MAX2_VRAM_CAP (1024u * 1024u)  

#define HWR_MAX_FRAME_VERTEX_FLUSH 1  
#define HWR_MAX_PRELOAD_8MB 1         
#define HWR_MAX_MORTON_LUT 1          


#if HWR_MAX_PRELOAD_8MB
#define HWR_PRELOAD_GPU_BUDGET (8u * 1024u * 1024u)
#else
#define HWR_PRELOAD_GPU_BUDGET (6u * 1024u * 1024u)
#endif
#define HWR_LASTSTAND_PRELOAD 1
#define HWR_LASTSTAND_SWIZZLE 1
#define HWR_LASTSTAND_ENV_CACHE 1
#define HWR_LASTSTAND_BIND_CACHE 1

#define HWR_STABLE_TINT_BATCH 1
#define HWR_STABLE_EFFECT_KEY 1
#define HWR_STABLE_FAN_VERTICES 1

#define HWR_BACKFACE_CULL 1
#define HWR_DEFER_TEXTURES 1
#define HWR_SCREEN_BOUNDS_CULL 1
#define HWR_SPRITE_BOUNDS_CULL 1
#define HWR_VIEWPORT_SCISSOR 1
#define HWR_FRUSTUM_CULL 1  
#define HWR_FAR 15360.0f
#define HWR_TRANSFER (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | \
                     GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
                     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
                     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct
{
	float x, y, z;
	float u, v;
	float r, g, b, a;
} hwrvert_t;

typedef struct texcache_s
{
	C3D_Tex tex;
	FTextureInfo *owner;
	struct texcache_s *next;
	struct texcache_s *hnext;
} texcache_t;

static C3D_RenderTarget *target;
static DVLB_s *vshader;
static shaderProgram_s program;
static C3D_Mtx projection, modelview, hud_projection, hud_modelview;
static int loc_proj, loc_view;
static hwrvert_t *verts;
static unsigned vcount;
static texcache_t *textures, *bound, *retired;
static texcache_t *texhash[HWR_TEX_HASH_BUCKETS];
static boolean batch_open;
static unsigned batch_first, batch_size;
static texcache_t *batch_tex;
static FBITFIELD batch_flags;

static FBITFIELD h3d_last_effect_flags;
static boolean h3d_effect_valid;
static texcache_t *h3d_last_wrap_tex;
static int h3d_last_wrap_x, h3d_last_wrap_y;
static int h3d_last_matrix_mode = -1;
static unsigned h3d_transform_generation, h3d_sent_generation;
 

static void H3D_FlushBatch(void);
static void H3D_FlushSortQueue(void);
static void H3D_Max4ResetQueue(void);
static unsigned H3D_Hash(FTextureInfo *t)
{
	return (((unsigned long)t >> 4) ^ ((unsigned long)t >> 13)) & (HWR_TEX_HASH_BUCKETS - 1);
}
static unsigned int lost_textures;
static RGBA_t palette[256];
static boolean ready, inframe, warned_full, warned_cmd_full;


#define HWR_MAX4_SORT_OPAQUE 1
#define HWR_MAX4_SORT_QUEUE 6144u
#define HWR_MAX4_GROUP_HASH 1024u
#define HWR_MAX4_MAX_WINDOW 2048u  

typedef struct {
    unsigned first, count, next; 
    texcache_t *tex;
    FBITFIELD flags;
} h3d_max4_batch_t;
typedef struct {
    unsigned head, tail, total, hash_next; 
    texcache_t *tex;
    FBITFIELD key, flags;
} h3d_max4_group_t;
static h3d_max4_batch_t h3d_max4_items[HWR_MAX4_SORT_QUEUE];
static h3d_max4_group_t h3d_max4_groups[HWR_MAX4_SORT_QUEUE];
static unsigned h3d_max4_hash[HWR_MAX4_GROUP_HASH];
static unsigned h3d_max4_count, h3d_max4_ngroups;
static unsigned h3d_max4_first, h3d_max4_total;
static hwrvert_t *h3d_max4_scratch; 
static boolean h3d_preloading;
static unsigned h3d_max2_vram_bytes;
static unsigned h3d_preload_bytes;

static int h3d_env_mode = -1;
static texcache_t *h3d_gpu_bound;
static boolean filtering;
static float fov = 90.0f, nearz = 0.9f;
static unsigned palettecolor;



typedef struct { float x, y, z, w; } h3d_plane_t;
static h3d_plane_t h3d_frustum[5]; 
static boolean h3d_frustum_valid;

static C3D_Mtx h3d_screen_clip;
static boolean h3d_screen_clip_valid;
static float h3d_camera_x, h3d_camera_z;
static boolean h3d_camera_valid;


static FTransform h3d_max3_cached_world;
static boolean h3d_max3_world_cached;


static FTextureInfo *h3d_pending_owner;
static boolean h3d_pending_change;





static boolean H3D_BackWallOutside(const FOutVector *p, FUINT n, FBITFIELD flags)
{
    float dx, dz, side, tol;
    if (!h3d_camera_valid || n != 4 || !(flags & PF_Occlude) || !(flags & PF_Clip) ||
        (flags & (PF_NoDepthTest | PF_Translucent | PF_Additive |
                  PF_Masked | PF_Substractive | PF_Environment | PF_Decal)))
        return false;
    
    if (fabsf(p[0].x-p[3].x) > 0.01f || fabsf(p[0].z-p[3].z) > 0.01f ||
        fabsf(p[1].x-p[2].x) > 0.01f || fabsf(p[1].z-p[2].z) > 0.01f ||
        p[2].y <= p[1].y + 0.01f || p[3].y <= p[0].y + 0.01f)
        return false;
    dx=p[1].x-p[0].x;
    dz=p[1].z-p[0].z;
    tol=0.001f*(fabsf(dx)+fabsf(dz)+1.0f);
    side=dx*(h3d_camera_z-p[0].z)-dz*(h3d_camera_x-p[0].x);
    return side > tol;
}




static boolean H3D_ScreenBoundsOutside(const FOutVector *p, FUINT n)
{
    FUINT i;
    boolean left=true,right=true,above=true,below=true;
    const float margin=1.15f;
    if (!h3d_screen_clip_valid || n < 3) return false;
    for (i=0;i<n;i++)
    {
        const float x=p[i].x, y=p[i].y, z=p[i].z;
        const float w=h3d_screen_clip.r[3].x*x + h3d_screen_clip.r[3].y*y +
                      h3d_screen_clip.r[3].z*z + h3d_screen_clip.r[3].w;
        float cx,cy;
        if (w < 0.01f || !isfinite(w)) return false;
        cx=h3d_screen_clip.r[0].x*x + h3d_screen_clip.r[0].y*y +
           h3d_screen_clip.r[0].z*z + h3d_screen_clip.r[0].w;
        cy=h3d_screen_clip.r[1].x*x + h3d_screen_clip.r[1].y*y +
           h3d_screen_clip.r[1].z*z + h3d_screen_clip.r[1].w;
        if (!isfinite(cx) || !isfinite(cy)) return false;
        
        if (cx >= -margin*w) left=false;
        if (cx <=  margin*w) right=false;
        if (cy >= -margin*w) below=false;
        if (cy <=  margin*w) above=false;
        if (!left && !right && !above && !below) return false;
    }
    return left || right || above || below;
}

static void H3D_UpdateFrustum(void)
{
    C3D_Mtx clip;
    C3D_FVec x,y,w;
    const float k=0.88f;
    unsigned i;
    h3d_plane_t side[5];
    Mtx_Multiply(&clip, &projection, &modelview);
    h3d_screen_clip=clip;
    h3d_screen_clip_valid=true;
    if (!h3d_frustum_valid) return;
    x=clip.r[0]; y=clip.r[1]; w=clip.r[3];
    side[0]=(h3d_plane_t){w.x+k*x.x,w.y+k*x.y,w.z+k*x.z,w.w+k*x.w};
    side[1]=(h3d_plane_t){w.x-k*x.x,w.y-k*x.y,w.z-k*x.z,w.w-k*x.w};
    side[2]=(h3d_plane_t){w.x+k*y.x,w.y+k*y.y,w.z+k*y.z,w.w+k*y.w};
    side[3]=(h3d_plane_t){w.x-k*y.x,w.y-k*y.y,w.z-k*y.z,w.w-k*y.w};
    side[4]=(h3d_plane_t){w.x,w.y,w.z,w.w};
    for (i=0;i<5;i++) h3d_frustum[i]=side[i];
}




static boolean H3D_OutsideView(const FOutVector *p, FUINT n)
{
    float xlo,xhi,ylo,yhi,zlo,zhi;
    FUINT i;
    unsigned j;
    if (!h3d_frustum_valid || n<3) return false;
    xlo=xhi=p[0].x; ylo=yhi=p[0].y; zlo=zhi=p[0].z;
    for (i=1;i<n;i++) {
        const FOutVector *v=&p[i];
        if (v->x<xlo) xlo=v->x;
        if (v->x>xhi) xhi=v->x;
        if (v->y<ylo) ylo=v->y;
        if (v->y>yhi) yhi=v->y;
        if (v->z<zlo) zlo=v->z;
        if (v->z>zhi) zhi=v->z;
    }
    for (j=0;j<5;j++) {
        const h3d_plane_t *f=&h3d_frustum[j];
        float maxdist=f->w;
        maxdist+=f->x*(f->x>=0.0f ? xhi:xlo);
        maxdist+=f->y*(f->y>=0.0f ? yhi:ylo);
        maxdist+=f->z*(f->z>=0.0f ? zhi:zlo);
        if (maxdist < -0.0001f) return true;
    }
    return false;
}

static unsigned morton8(unsigned x, unsigned y)
{
	return (x & 1) | ((y & 1)<<1) | ((x & 2)<<1) | ((y & 2)<<2)
		| ((x & 4)<<2) | ((y & 4)<<3);
}


#if HWR_MAX_MORTON_LUT
static const u8 h3d_max_morton8_lut[64] = {
    0, 1, 4, 5, 16, 17, 20, 21,
    2, 3, 6, 7, 18, 19, 22, 23,
    8, 9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63
};
#define H3D_MAX_MORTON8(x,y) h3d_max_morton8_lut[((((y) & 7u)<<3) | ((x) & 7u))]
#else
#define H3D_MAX_MORTON8(x,y) morton8((x)&7u,(y)&7u)
#endif

static void H3D_Reprojection(void)
{
	
	Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(fov), (float)TOP_W / (float)TOP_H,
		nearz, HWR_FAR, false);
	H3D_UpdateFrustum();
}




static void H3D_FreeRetired(void)
{
	while (retired)
	{
		texcache_t *next = retired->next;
		C3D_TexDelete(&retired->tex);
		free(retired);
		retired = next;
	}
}


int HWR3DS_PreloadBegin(void)
{
    if (!HWR_LASTSTAND_PRELOAD || !ready || inframe || h3d_preloading)
        return 0;
    C3D_FrameSync(); 
    H3D_FreeRetired();
    
    h3d_max2_vram_bytes = 0;
    h3d_preload_bytes = 0;
    h3d_preloading = true;
    return 1;
}

void HWR3DS_PreloadEnd(void)
{
    if (!h3d_preloading) return;
    h3d_preloading = false;
    bound = NULL;
    h3d_pending_change = false;
    h3d_pending_owner = NULL;
}

static void H3D_ClearTextures(void)
{
	texcache_t *t = textures;
	H3D_FlushBatch(); 
	H3D_FlushSortQueue(); 
	memset(texhash, 0, sizeof texhash);
	h3d_pending_change = false;
	h3d_pending_owner = NULL;
	textures = bound = NULL;
	while (t)
	{
		texcache_t *next = t->next;
		
		t->next = retired;
		retired = t;
		t = next;
	}
}

static void H3D_Close(void)
{
	if (!ready)
		return;
	if (inframe)
		HWR3DS_EndFrame(0);
	H3D_ClearTextures();
	
	C3D_Fini();
	H3D_FreeRetired();
    if (h3d_max4_scratch) free(h3d_max4_scratch);
    h3d_max4_scratch = NULL;
    H3D_Max4ResetQueue();
	if (verts)
		linearFree(verts);
	verts = NULL;
	shaderProgramFree(&program);
	if (vshader)
		DVLB_Free(vshader);
	vshader = NULL;
	
	target = NULL;
	ready = false;
}

static boolean H3D_Init(I_Error_t ErrorFunction)
{
	unsigned i;
	(void)ErrorFunction;
	if (ready)
		return true;
	for (i=0; i<256; i++)
	{
		palette[i].s.red = palette[i].s.green = palette[i].s.blue = (byte)i;
		palette[i].s.alpha = 255;
	}
	if (!C3D_Init(HWR_CMDBUF_BYTES))
	{
		I_OutputMsg("HWR: cannot allocate %u byte GPU cmd buffer\n", (unsigned)HWR_CMDBUF_BYTES);
		return false;
	}
	target = C3D_RenderTargetCreate(LCD_H, LCD_W, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (!target)
		goto fail;
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, HWR_TRANSFER);
	vshader = DVLB_ParseFile((u32*)hwr3ds_shbin, hwr3ds_shbin_size);
	if (!vshader)
		goto fail;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader->DVLE[0]);
	C3D_BindProgram(&program);
	loc_proj = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	loc_view = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	if (loc_proj < 0 || loc_view < 0)
		goto fail_program;
	{
		C3D_AttrInfo *attr = C3D_GetAttrInfo();
		AttrInfo_Init(attr);
		AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
		AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
		AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 4);
	}
	verts = linearAlloc(MAX_HWR_VERTS * sizeof(*verts));
	if (!verts)
		goto fail_program;
#if HWR_MAX4_SORT_OPAQUE
    
    h3d_max4_scratch = malloc(MAX_HWR_VERTS * sizeof(*verts));
    if (!h3d_max4_scratch)
        I_OutputMsg("HWR: texture sort buffer unavailable; using original draw order\n");
#endif
    H3D_Max4ResetQueue();
	{
		C3D_BufInfo *buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, verts, sizeof(*verts), 3, 0x210);
	}
	Mtx_Identity(&modelview);
	Mtx_Scale(&modelview, 1.0f, 1.0f, -1.0f);
	
	
	Mtx_Identity(&hud_modelview);
	Mtx_OrthoTilt(&hud_projection, 0.0f, TOP_W, 0.0f, TOP_H, 0.0f, 1.0f, true);
	H3D_Reprojection();
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
	C3D_AlphaTest(true, GPU_GREATER, 0);
    
    h3d_max3_world_cached = false;
    h3d_camera_valid = false;
	ready = true;
	
	 
	return true;
fail_program:
	shaderProgramFree(&program);
	DVLB_Free(vshader);
	vshader = NULL;
fail:
	C3D_Fini();
	target = NULL;
	return false;
}

void HWR3DS_BeginFrame(void)
{
	C3D_TexEnv *env;
	if (!ready || inframe)
		return;
	if (!C3D_FrameBegin(0))
		return;
    H3D_Max4ResetQueue();
	inframe = true;
	h3d_effect_valid = false;
	h3d_last_wrap_tex = NULL;
	h3d_last_matrix_mode = -1;
    h3d_env_mode = -1;
    h3d_gpu_bound = NULL;
	vcount = 0;
	batch_open = false;
	batch_size = 0;
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
	C3D_FrameDrawOn(target);
	
	C3D_SetViewport((LCD_H-TOP_H)/2, (LCD_W-TOP_W)/2, TOP_H, TOP_W);
	C3D_SetScissor(GPU_SCISSOR_NORMAL,
		(LCD_H-TOP_H)/2, (LCD_W-TOP_W)/2,
		(LCD_H+TOP_H)/2, (LCD_W+TOP_W)/2);
	C3D_BindProgram(&program);
	C3D_CullFace(GPU_CULL_NONE);
	C3D_DepthMap(true, -1.0f, 0.0f);
	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
	C3D_AlphaTest(true, GPU_GREATER, 0);
	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA,
		GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
	bound = NULL; 
	h3d_pending_change = false;
	h3d_pending_owner = NULL;
}



static void H3D_BlackBorders(void)
{
    static const float rect[4][4] = {
        {0, 0, 40, 240}, {360, 0, 400, 240},
        {40, 0, 360, 20}, {40, 220, 360, 240}
    };
    static const int corners[6] = {0, 1, 2, 0, 2, 3};
    C3D_Mtx bp,bv;
    C3D_TexEnv *env;
    unsigned first,i,j;
    u32 *cmd, words, used;
    if (!inframe || vcount + 24 > MAX_HWR_VERTS) return;
    GPUCMD_GetBuffer(&cmd, &words, &used);
    if (!cmd || used >= words || words-used < HWR_CMD_RESERVE_WORDS) return;
    first = vcount;
    for (i=0;i<4;i++) {
        const float xy[4][2] = {
            {rect[i][0],rect[i][1]}, {rect[i][2],rect[i][1]},
            {rect[i][2],rect[i][3]}, {rect[i][0],rect[i][3]}
        };
        for(j=0;j<6;j++) {
            hwrvert_t *v=&verts[vcount++];
            v->x=xy[corners[j]][0]; v->y=xy[corners[j]][1]; v->z=0.5f;
            v->u=v->v=v->r=v->g=v->b=0.0f; v->a=1.0f;
        }
    }
    
    C3D_SetViewport(0,0,LCD_H,LCD_W);
    C3D_SetScissor(GPU_SCISSOR_DISABLE,0,0,0,0);
    Mtx_Identity(&bv);
    Mtx_OrthoTilt(&bp,0,LCD_W,0,LCD_H,0,1,true);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR,0,0);
    C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc_proj,&bp);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc_view,&bv);
#if !HWR_MAX_FRAME_VERTEX_FLUSH
    GSPGPU_FlushDataCache(&verts[first],24*sizeof(*verts));
#endif
    C3D_DrawArrays(GPU_TRIANGLES,first,24);
}

void HWR3DS_EndFrame(int waitvbl)
{
	if (!ready)
		return;
	if (!inframe)
		HWR3DS_BeginFrame();
	if (!inframe)
		return;
	H3D_FlushBatch();
    H3D_FlushSortQueue(); 
	H3D_BlackBorders();
#if HWR_MAX_FRAME_VERTEX_FLUSH
    
    
    
    if (vcount)
        GSPGPU_FlushDataCache(verts, vcount * sizeof(*verts));
#endif
	C3D_FrameEnd(GX_CMDLIST_FLUSH);
	inframe = false;
	if (waitvbl)
		gspWaitForVBlank();
}

static void H3D_SetPalette(RGBA_t *pal, RGBA_t *gamma)
{
	unsigned i;
	(void)gamma;
	if (!pal)
		return;
	if (memcmp(palette, pal, sizeof palette) == 0)
		return;
	for (i=0; i<256; i++)
		palette[i] = pal[i];
	
	H3D_ClearTextures();
}

static unsigned H3D_TexSide(unsigned oldside)
{
	unsigned side = 8;
	while (side < oldside && side < HWR_TEX_MAX)
		side <<= 1;
	return side;
}

static void H3D_SetTextureNow(FTextureInfo *t)
{
	texcache_t *node;
	unsigned x,y,srcw,srch,w,h;
    unsigned tex_bytes;
    GPU_TEXCOLOR gpu_format;
    void *write_pixels = NULL, *staging = NULL;
	const byte *pixels;
	if (!ready)
		return;
	if (!t)
	{
		H3D_FlushBatch();
		bound = NULL;
		return;
	}
    
    if (bound && bound->owner == t) return;
	
	for (node=texhash[H3D_Hash(t)]; node; node=node->hnext)
		if (node->owner == t)
			break;
	if (node)
	{
        if (bound != node)
        {
            H3D_FlushBatch();
        }
		bound = node;
		return;
	}
	if (!t->grInfo.data)
	{
		H3D_FlushBatch();
		bound = NULL;
		return;
	}
	srcw = t->width;
	srch = t->height;
	if (!srcw || !srch)
		return;
	w = H3D_TexSide(srcw);
	h = H3D_TexSide(srch);
    
    
    gpu_format = (HWR_MAX2_16BIT_OPAQUE &&
                  t->grInfo.format == GR_TEXFMT_P_8 &&
                  !(t->flags & TF_CHROMAKEYED)) ? GPU_RGB565 : GPU_RGBA8;
    tex_bytes = w*h*(gpu_format == GPU_RGB565 ? 2u : 4u);
    if (h3d_preloading &&
        (h3d_preload_bytes + tex_bytes > HWR_PRELOAD_GPU_BUDGET))
    {
        bound = NULL;
        return;
    }
	node = calloc(1,sizeof(*node));
	if (!node)
	{
		H3D_FlushBatch();
		bound = NULL;
		return;
	}
#if HWR_MAX2_VRAM_PRELOAD
    
    
    
    if (h3d_preloading &&
        h3d_max2_vram_bytes + tex_bytes <= HWR_MAX2_VRAM_CAP &&
        C3D_TexInitVRAM(&node->tex, w, h, gpu_format))
    {
        staging = linearAlloc(tex_bytes);
        if (staging)
        {
            write_pixels = staging;
            h3d_max2_vram_bytes += tex_bytes;
        }
        else
            C3D_TexDelete(&node->tex); 
    }
#endif
    if (!write_pixels && !C3D_TexInit(&node->tex, w, h, gpu_format))
	{
		free(node);
		H3D_FlushBatch();
		bound = NULL; 
		if (++lost_textures <= 8)
			I_OutputMsg("HWR: GPU texture alloc FAILED %ux%u (source %ux%u) # %u\n",
				w, h, srcw, srch, lost_textures);
		return;
	}
    if (!write_pixels) write_pixels = node->tex.data;
	pixels = (const byte*)t->grInfo.data;
#if HWR_MAX2_16BIT_OPAQUE
    if (gpu_format == GPU_RGB565)
    {
        u16 *out16 = (u16*)write_pixels;
        
        for (y=0; y<h; ++y)
            for (x=0; x<w; ++x)
            {
                const unsigned ix = pixels[(y*srch/h)*srcw + (x*srcw/w)];
                const RGBA_t c = palette[ix];
                const unsigned slot = ((y>>3)*(w>>3)+(x>>3))*64
                                     + H3D_MAX_MORTON8(x,y);
                out16[slot] = ((u16)(c.s.red>>3)<<11) |
                              ((u16)(c.s.green>>2)<<5) | (c.s.blue>>3);
            }
    }
    else
#endif
    {
#if HWR_LASTSTAND_SWIZZLE
    {
        
        
        
        unsigned sx[HWR_TEX_MAX], sy[HWR_TEX_MAX];
        u32 *out = (u32*)write_pixels;
        unsigned palrgba[256], i;
        const int fmt = t->grInfo.format;
        for (x=0; x<w; ++x) sx[x] = x*srcw/w;
        for (y=0; y<h; ++y) sy[y] = y*srch/h;
        if (fmt == GR_TEXFMT_P_8 || fmt == GR_TEXFMT_AP_88)
        {
            for (i=0; i<256; ++i)
                palrgba[i] = ((u32)palette[i].s.red<<24) |
                    ((u32)palette[i].s.green<<16) |
                    ((u32)palette[i].s.blue<<8);
        }
        
        
        for (y=0; y<h; ++y)
        {
            const unsigned row=sy[y]*srcw;
            const unsigned dstrow=(y>>3)*(w>>3)*64;
            if (fmt == GR_RGBA)
            {
                const RGBA_t *in=(const RGBA_t*)pixels;
                for (x=0; x<w; ++x)
                {
                    const RGBA_t c=in[row+sx[x]];
                    out[dstrow+(x>>3)*64+H3D_MAX_MORTON8(x,y)] =
                        ((u32)c.s.red<<24)|((u32)c.s.green<<16)|
                        ((u32)c.s.blue<<8)|c.s.alpha;
                }
            }
            else if (fmt == GR_TEXFMT_P_8)
            {
                const boolean keyed=(t->flags & TF_CHROMAKEYED)!=0;
                for (x=0; x<w; ++x)
                {
                    const unsigned ix=pixels[row+sx[x]];
                    const unsigned a=(keyed && ix == HWR_PATCHES_CHROMAKEY_COLORINDEX) ? 0 : 255;
                    out[dstrow+(x>>3)*64+H3D_MAX_MORTON8(x,y)] = palrgba[ix]|a;
                }
            }
            else if (fmt == GR_TEXFMT_AP_88)
            {
                const boolean keyed=(t->flags & TF_CHROMAKEYED)!=0;
                for (x=0; x<w; ++x)
                {
                    const unsigned off=2u*(row+sx[x]);
                    const unsigned ix=pixels[off];
                    const unsigned a=(keyed && ix == HWR_PATCHES_CHROMAKEY_COLORINDEX) ? 0 : pixels[off+1];
                    out[dstrow+(x>>3)*64+H3D_MAX_MORTON8(x,y)] = palrgba[ix]|a;
                }
            }
            else if (fmt == GR_TEXFMT_ALPHA_INTENSITY_88)
            {
                for (x=0; x<w; ++x)
                {
                    const unsigned off=2u*(row+sx[x]);
                    const unsigned lum=pixels[off], a=pixels[off+1];
                    out[dstrow+(x>>3)*64+H3D_MAX_MORTON8(x,y)] =
                        (lum<<24)|(lum<<16)|(lum<<8)|a;
                }
            }
            else
            {
                for (x=0; x<w; ++x)
                    out[dstrow+(x>>3)*64+H3D_MAX_MORTON8(x,y)] = 0xFFFFFFFF;
            }
        }
    }
#else
	memset(write_pixels, 0, tex_bytes);
	for (y=0; y<h; y)
		for (x=0; x<w; x++)
		{
			unsigned sx = x*srcw/w, sy = y*srch/h;
			unsigned rgba = 0xFFFFFFFF;
			unsigned p = sy*srcw+sx;
			RGBA_t c;
			if (t->grInfo.format == GR_RGBA)
			{
				c = ((const RGBA_t*)pixels)[p];
				rgba = ((u32)c.s.red<<24) | ((u32)c.s.green<<16)
					| ((u32)c.s.blue<<8) | c.s.alpha;
			}
			else if (t->grInfo.format == GR_TEXFMT_P_8 || t->grInfo.format == GR_TEXFMT_AP_88)
			{
				unsigned index = pixels[p*(t->grInfo.format == GR_TEXFMT_AP_88 ? 2 : 1)];
				unsigned alpha = t->grInfo.format == GR_TEXFMT_AP_88 ? pixels[2*p+1] : 255;
				if ((t->flags & TF_CHROMAKEYED) && index == HWR_PATCHES_CHROMAKEY_COLORINDEX)
					alpha = 0;
				c = palette[index];
				rgba = ((u32)c.s.red<<24) | ((u32)c.s.green<<16)
					| ((u32)c.s.blue<<8) | alpha;
			}
			else if (t->grInfo.format == GR_TEXFMT_ALPHA_INTENSITY_88)
			{
				unsigned lum = pixels[2*p], alpha = pixels[2*p+1];
				rgba = (lum<<24)|(lum<<16)|(lum<<8)|alpha;
			}
			
			((u32*)write_pixels)[((y>>3)*(w>>3)+(x>>3))*64 + H3D_MAX_MORTON8(x,y)] = rgba;
		}
#endif
    }
    if (h3d_preloading)
    {
        h3d_preload_bytes += tex_bytes;
    }
    if (staging)
    {
        
        
        GSPGPU_FlushDataCache(staging, tex_bytes);
        C3D_TexLoadImage(&node->tex, staging, GPU_TEXFACE_2D, 0);
        linearFree(staging);
    }
    else
        C3D_TexFlush(&node->tex);
	C3D_TexSetFilter(&node->tex, filtering ? GPU_LINEAR : GPU_NEAREST,
		filtering ? GPU_LINEAR : GPU_NEAREST);
	C3D_TexSetWrap(&node->tex,
		(t->flags & TF_WRAPX) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE,
		(t->flags & TF_WRAPY) ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
	node->owner = t;
	t->downloaded = 1;
	node->next = textures;
	textures = node;
	node->hnext = texhash[H3D_Hash(t)];
	texhash[H3D_Hash(t)] = node;
	H3D_FlushBatch();
	bound = node;
}


static void H3D_SetTexture(FTextureInfo *t)
{
    if (h3d_preloading) { H3D_SetTextureNow(t); return; }
#if HWR_DEFER_TEXTURES
    h3d_pending_owner = t;
    h3d_pending_change = true;
#else
    H3D_SetTextureNow(t);
#endif
}

static void H3D_ApplyBlend(FBITFIELD flags, texcache_t *tex)
{
	int src = GPU_ONE, dst = GPU_ZERO;
	if (flags & PF_Additive)
		src = GPU_SRC_ALPHA, dst = GPU_ONE;
	else if (flags & (PF_Translucent | PF_Masked))
		src = GPU_SRC_ALPHA, dst = GPU_ONE_MINUS_SRC_ALPHA;
	else if (flags & PF_Environment)
		src = GPU_ONE, dst = GPU_ONE_MINUS_SRC_ALPHA;
	else if (flags & PF_Substractive)
		src = GPU_ZERO, dst = GPU_ONE_MINUS_SRC_COLOR;
	
#if HWR_STABLE_EFFECT_KEY
	const FBITFIELD effect_key = flags & (PF_Additive | PF_Translucent |
		PF_Masked | PF_Environment | PF_Substractive |
		PF_NoAlphaTest | PF_NoDepthTest | PF_Occlude);
#else
	const FBITFIELD effect_key = flags;
#endif
	if (!h3d_effect_valid || h3d_last_effect_flags != effect_key)
	{
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, src, dst, src, dst);
		C3D_AlphaTest(!(flags & PF_NoAlphaTest), GPU_GREATER, 0);
		C3D_DepthTest(!(flags & PF_NoDepthTest), GPU_GEQUAL,
			(flags & PF_Occlude) ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
		h3d_last_effect_flags = effect_key;
		h3d_effect_valid = true;
	}
	if (tex)
	{
		FBITFIELD tf = tex->owner ? tex->owner->flags : 0;
		GPU_TEXTURE_WRAP_PARAM wrapx = ((tf & TF_WRAPX) || (flags & PF_ForceWrapX))
			? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
		GPU_TEXTURE_WRAP_PARAM wrapy = ((tf & TF_WRAPY) || (flags & PF_ForceWrapY))
			? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
		if (flags & PF_RemoveYWrap) wrapy = GPU_CLAMP_TO_EDGE;
		if (h3d_last_wrap_tex != tex || h3d_last_wrap_x != (int)wrapx
			|| h3d_last_wrap_y != (int)wrapy)
		{
			C3D_TexSetWrap(&tex->tex, wrapx, wrapy);
            h3d_gpu_bound = NULL; 
			h3d_last_wrap_tex = tex;
			h3d_last_wrap_x = (int)wrapx;
			h3d_last_wrap_y = (int)wrapy;
		}
	}
}

static boolean H3D_Max4Eligible(FBITFIELD flags, texcache_t *tex)
{
#if HWR_MAX4_SORT_OPAQUE
    
    
    const FBITFIELD unsafe = PF_NoDepthTest | PF_NoTexture | PF_Translucent |
        PF_Additive | PF_Masked | PF_Substractive | PF_Environment |
        PF_Decal | PF_NoAlphaTest;
    return h3d_max4_scratch && tex && tex->owner &&
        (flags & PF_Occlude) && !(flags & unsafe) &&
        tex->owner->grInfo.format == GR_TEXFMT_P_8 &&
        !(tex->owner->flags & TF_CHROMAKEYED);
#else
    (void)flags; (void)tex;
    return false;
#endif
}

static void H3D_DrawBatchNow(void); 

static void H3D_Max4ResetQueue(void)
{
    h3d_max4_count = h3d_max4_ngroups = h3d_max4_total = 0;
    
    memset(h3d_max4_hash, 0, sizeof(h3d_max4_hash));
}


static void H3D_FlushSortQueue(void)
{
    unsigned i, g, at, dst;
    boolean can_sort = false;
    if (!h3d_max4_count) return;
    can_sort = (h3d_max4_scratch && h3d_max4_count > h3d_max4_ngroups);
    
    if (can_sort)
    {
        dst = h3d_max4_first;
        for (g=0; g<h3d_max4_ngroups; ++g)
        {
            for (at=h3d_max4_groups[g].head; at; at=h3d_max4_items[at-1].next)
            {
                const h3d_max4_batch_t *b = &h3d_max4_items[at-1];
                memcpy(&h3d_max4_scratch[dst], &verts[b->first],
                       b->count*sizeof(*verts));
                dst += b->count;
            }
        }
        
        
        if (dst != h3d_max4_first+h3d_max4_total)
        {
            I_OutputMsg("HWR: texture sort geometry mismatch; skipping sorted batch\n");
            H3D_Max4ResetQueue();
            return;
        }
        memcpy(&verts[h3d_max4_first], &h3d_max4_scratch[h3d_max4_first],
               h3d_max4_total*sizeof(*verts));
        dst = h3d_max4_first;
        for (g=0; g<h3d_max4_ngroups; ++g)
        {
            const h3d_max4_group_t *grp = &h3d_max4_groups[g];
            unsigned left = grp->total;
            while (left)
            {
                unsigned chunk = left > HWR_BATCH_VERTS ? HWR_BATCH_VERTS : left;
                
                batch_open = true;
                batch_first = dst;
                batch_size = chunk;
                batch_flags = grp->flags;
                batch_tex = grp->tex;
                H3D_DrawBatchNow();
                dst += chunk;
                left -= chunk;
            }
        }
    }
    else
    {
        
        for (i=0; i<h3d_max4_count; ++i)
        {
            const h3d_max4_batch_t *b = &h3d_max4_items[i];
            batch_open = true;
            batch_first = b->first;
            batch_size = b->count;
            batch_flags = b->flags;
            batch_tex = b->tex;
            H3D_DrawBatchNow();
        }
    }
    H3D_Max4ResetQueue();
}

static void H3D_Max4QueueBatch(void)
{
    unsigned b, hash, gid, in_first, in_size;
    FBITFIELD key, in_flags;
    texcache_t *in_tex;
    if (!batch_open || !batch_size) return;
    
    
    in_first = batch_first;
    in_size = batch_size;
    in_flags = batch_flags;
    in_tex = batch_tex;
    if (h3d_max4_count >= HWR_MAX4_SORT_QUEUE ||
        h3d_max4_count >= HWR_MAX4_MAX_WINDOW ||
        (h3d_max4_count && in_first != h3d_max4_first+h3d_max4_total))
        H3D_FlushSortQueue();
    if (!h3d_max4_count)
        h3d_max4_first = in_first;
    key = in_flags & ~PF_Modulated; 
    hash = (((uintptr_t)in_tex >> 4) ^ (unsigned)key*2654435761u) &
           (HWR_MAX4_GROUP_HASH-1);
    for (gid=h3d_max4_hash[hash]; gid; gid=h3d_max4_groups[gid-1].hash_next)
        if (h3d_max4_groups[gid-1].tex == in_tex &&
            h3d_max4_groups[gid-1].key == key)
            break;
    if (!gid)
    {
        h3d_max4_group_t *grp = &h3d_max4_groups[h3d_max4_ngroups];
        gid = ++h3d_max4_ngroups;
        grp->head = grp->tail = grp->total = 0;
        grp->tex = in_tex;
        grp->key = key;
        grp->flags = in_flags;
        grp->hash_next = h3d_max4_hash[hash];
        h3d_max4_hash[hash] = gid;
    }
    b = h3d_max4_count++;
    h3d_max4_items[b].first = in_first;
    h3d_max4_items[b].count = in_size;
    h3d_max4_items[b].tex = in_tex;
    h3d_max4_items[b].flags = in_flags;
    h3d_max4_items[b].next = 0;
    {
        h3d_max4_group_t *grp = &h3d_max4_groups[gid-1];
        if (grp->tail) h3d_max4_items[grp->tail-1].next = b+1;
        else grp->head = b+1;
        grp->tail = b+1;
        grp->total += in_size;
    }
    h3d_max4_total += in_size;
    batch_open = false;
    batch_size = 0;
}



static void H3D_FlushBatch(void)
{
    if (!batch_open || !batch_size || !inframe)
    {
        batch_open = false;
        batch_size = 0;
        return;
    }
    if (H3D_Max4Eligible(batch_flags, batch_tex))
    {
        H3D_Max4QueueBatch();
        return;
    }
    H3D_FlushSortQueue();
    H3D_DrawBatchNow();
}

static void H3D_DrawBatchNow(void)
{
	C3D_TexEnv *env;
	u32 *gpu_buf, gpu_words, gpu_used;
	if (!batch_open || !batch_size || !inframe)
	{
		batch_open = false;
		batch_size = 0;
		return;
	}
	GPUCMD_GetBuffer(&gpu_buf, &gpu_words, &gpu_used);
	
	if (!gpu_buf || gpu_used >= gpu_words || gpu_words - gpu_used < HWR_CMD_RESERVE_WORDS)
	{
		if (!warned_cmd_full)
		{
			warned_cmd_full = true;
			I_OutputMsg("HWR: GPU cmd buffer full (%u/%u), dropping batch (not crashing)\n",
				(unsigned)gpu_used, (unsigned)gpu_words);
		}
		batch_open = false;
		batch_size = 0;
		return;
	}
	H3D_ApplyBlend(batch_flags, batch_tex);
#if HWR_LASTSTAND_ENV_CACHE
    {
        const int mode = ((batch_flags & PF_NoTexture) || !batch_tex) ? 0 : 1;
        if (h3d_env_mode != mode)
        {
            env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            if (mode)
            {
                C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            }
            else
            {
                C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
                C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            }
            h3d_env_mode = mode;
        }
        if (mode)
        {
#if HWR_LASTSTAND_BIND_CACHE
            if (h3d_gpu_bound != batch_tex)
#endif
                C3D_TexBind(0, &batch_tex->tex);
            h3d_gpu_bound = batch_tex;
        }
    }
#else
	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	if ((batch_flags & PF_NoTexture) || !batch_tex)
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	else
	{
		C3D_TexBind(0, &batch_tex->tex);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
#endif
	
	
		{
		int mode = (batch_flags & PF_NoDepthTest) ? 1 : 0;
		if (mode != h3d_last_matrix_mode || h3d_sent_generation != h3d_transform_generation)
		{
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc_proj,
				mode ? &hud_projection : &projection);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, loc_view,
				mode ? &hud_modelview : &modelview);
			h3d_last_matrix_mode = mode;
			h3d_sent_generation = h3d_transform_generation;
		}
	}
#if !HWR_MAX_FRAME_VERTEX_FLUSH
	GSPGPU_FlushDataCache(&verts[batch_first], batch_size * sizeof(*verts));
#endif
	C3D_DrawArrays(GPU_TRIANGLES, batch_first, batch_size);
	batch_open = false;
	batch_size = 0;
}

static void H3D_SetBlend(FBITFIELD flags)
{
    if (!inframe) return;
#if HWR_MAX3_SMART_BLEND
    
    
    if (batch_open && flags == batch_flags)
    {
        return;
    }
#endif
    H3D_FlushBatch();
    H3D_ApplyBlend(flags, bound);
}


static inline void H3D_StableMakeVertex(hwrvert_t *q, const FOutVector *v,
                                        FBITFIELD flags, boolean sky,
                                        float r, float g, float b, float a)
{
    if (flags & PF_NoDepthTest)
    {
        const float scale = sky ? 0.25f : 1.0f;
        q->x = (v->x*scale+1.0f)*TOP_W*0.5f;
        q->y = (v->y*scale+1.0f)*TOP_H*0.5f;
        q->z = 0.5f;
    }
    else
    {
        q->x=v->x; q->y=v->y; q->z=v->z;
    }
    q->u=v->sow; q->v=1.0f-v->tow;
    q->r=r; q->g=g; q->b=b; q->a=a;
}

static void H3D_DrawPolygon(FSurfaceInfo *surf, FOutVector *p, FUINT n, FBITFIELD flags)
{
	FUINT k;
	
	
	boolean sky = (surf == NULL && flags == 0 && n == 4);
	unsigned count;
	float r=1,g=1,b=1,a=1;
	if (!inframe || !p || n<3 || (flags & PF_MD2))
		return; 
	if (n > MAX_HWR_VERTS/3 + 2) return;
	if (sky) flags |= PF_NoDepthTest; 
	else if (!(flags & PF_NoDepthTest))
	{
		
		if (HWR_BACKFACE_CULL && H3D_BackWallOutside(p,n,flags))
        {
            return;
        }
		
		if (HWR_FRUSTUM_CULL && H3D_OutsideView(p,n))
        {
            return;
        }
		
		
		if (((h3d_camera_valid && HWR_SCREEN_BOUNDS_CULL) ||
		     (!h3d_camera_valid && HWR_SPRITE_BOUNDS_CULL)) &&
		    H3D_ScreenBoundsOutside(p,n))
        {
            return;
        }
	}
#if HWR_DEFER_TEXTURES
	if (h3d_pending_change)
	{
		FTextureInfo *owner=h3d_pending_owner;
		h3d_pending_change=false;
		H3D_SetTextureNow(owner);
	}
#endif
    
    
    if (!H3D_Max4Eligible(flags, bound))
    {
        H3D_FlushBatch();
        H3D_FlushSortQueue();
    }
	count = 3*(n-2);
	if (vcount + count > MAX_HWR_VERTS - HWR_BORDER_VERTS)
	{
		H3D_FlushBatch();
		if (!warned_full)
		{
			warned_full = true;
			I_OutputMsg("HWR: out of per-frame vertices (%u); dropping excess polygons\n", vcount);
		}
		return;
	}
	if (surf && (flags & PF_Modulated))
	{
		const RGBA_t c = surf->FlatColor;

		r = c.s.red/255.0f; g=c.s.green/255.0f;
		b = c.s.blue/255.0f; a=c.s.alpha/255.0f;
	}
	
	
#if HWR_STABLE_TINT_BATCH
	if (batch_open && (((batch_flags ^ flags) & ~PF_Modulated) != 0
		|| batch_tex != bound || batch_size + count > HWR_BATCH_VERTS))
#else
	if (batch_open && (batch_flags != flags || batch_tex != bound
		|| batch_size + count > HWR_BATCH_VERTS))
#endif
		H3D_FlushBatch();
	if (!batch_open)
	{
		batch_open = true;
		batch_first = vcount;
		batch_size = 0;
		batch_flags = flags;
		batch_tex = bound;

	}
#if HWR_STABLE_FAN_VERTICES
	{
		hwrvert_t first, previous, next;
		H3D_StableMakeVertex(&first, &p[0], flags, sky, r, g, b, a);
		H3D_StableMakeVertex(&previous, &p[1], flags, sky, r, g, b, a);
		for (k=2; k<n; ++k)
		{
			H3D_StableMakeVertex(&next, &p[k], flags, sky, r, g, b, a);
			verts[vcount++] = first;
			verts[vcount++] = previous;
			verts[vcount++] = next;
			previous = next;
		}
	}
#else
	for (k=1; k+1<n; k++)
	{
		FUINT idx[3] = {0,k,k+1};
		unsigned z;
		for (z=0; z<3; z++)
		{
			const FOutVector *v = &p[idx[z]];
			hwrvert_t *q = &verts[vcount++];
			if (flags & PF_NoDepthTest)
			{
				
				float scale = sky ? 0.25f : 1.0f;
				q->x = (v->x*scale+1.0f)*TOP_W*0.5f;
				q->y = (v->y*scale+1.0f)*TOP_H*0.5f;
				q->z = 0.5f;
			}
			else
				q->x=v->x, q->y=v->y, q->z=v->z;
			q->u=v->sow; q->v=1.0f-v->tow;
			q->r=r; q->g=g; q->b=b; q->a=a;
		}
	}
#endif
	batch_size += count;
	if (batch_size >= HWR_BATCH_VERTS)
		H3D_FlushBatch();
}

static void H3D_DrawLine(F2DCoord *a, F2DCoord *b, RGBA_t color)
{
	FOutVector quad[4];
	FSurfaceInfo surf;
	float dx=b->x-a->x, dy=b->y-a->y;
	float len=sqrtf(dx*dx+dy*dy);
	if (len<0.001f)
		return;
	
	dx /= len; dy /= len;
	memset(quad,0,sizeof quad);
	quad[0].x = (2.0f*a->x-dy)/TOP_W-1.0f;
	quad[1].x = (2.0f*b->x-dy)/TOP_W-1.0f;
	quad[2].x = (2.0f*b->x+dy)/TOP_W-1.0f;
	quad[3].x = (2.0f*a->x+dy)/TOP_W-1.0f;
	quad[0].y = 1.0f-(2.0f*a->y-dx)/TOP_H;
	quad[1].y = 1.0f-(2.0f*b->y-dx)/TOP_H;
	quad[2].y = 1.0f-(2.0f*b->y+dx)/TOP_H;
	quad[3].y = 1.0f-(2.0f*a->y+dx)/TOP_H;
	quad[0].z=quad[1].z=quad[2].z=quad[3].z=1.0f;
	memset(&surf,0,sizeof surf);
	surf.FlatColor=color;
	H3D_DrawPolygon(&surf,quad,4,PF_NoTexture|PF_Modulated|PF_NoDepthTest);
}

static void H3D_ClearBuffer(FBOOLEAN color, FBOOLEAN depth, FRGBAFloat *c)
{
	int mask=0;
	u32 rgba=0x000000FF;
	if (!inframe)
		return;
	H3D_FlushBatch();
    H3D_FlushSortQueue(); 
	if (color) mask |= C3D_CLEAR_COLOR;
	if (depth) mask |= C3D_CLEAR_DEPTH;
	if (c)
		rgba = (((u32)(c->red*255)<<24)|((u32)(c->green*255)<<16)|
			((u32)(c->blue*255)<<8)|(u32)(c->alpha*255));
	if (mask)
		C3D_RenderTargetClear(target, mask, rgba, 0);
}

static boolean H3D_Max3SameWorld(const FTransform *t)
{
    const FTransform *p = &h3d_max3_cached_world;
    const float nextfov = t->fovxangle < 1 ? 90.0f : t->fovxangle;
    return h3d_max3_world_cached && h3d_camera_valid &&
        p->x == t->x && p->y == t->y && p->z == t->z &&
        p->scalex == t->scalex && p->scaley == t->scaley &&
        p->scalez == t->scalez && p->anglex == t->anglex &&
        p->angley == t->angley && p->fovxangle == t->fovxangle &&
        fov == nextfov;
}
static void H3D_SetTransform(FTransform *t)
{
#if HWR_MAX3_TRANSFORM_CACHE
    if ((!t && !h3d_camera_valid) || (t && H3D_Max3SameWorld(t)))
    {
        return;
    }
#endif
	H3D_FlushBatch();
    H3D_FlushSortQueue(); 
    h3d_max3_world_cached = (t != NULL);
    if (t) h3d_max3_cached_world = *t;
	++h3d_transform_generation;
	Mtx_Identity(&modelview);
	h3d_frustum_valid = t != NULL;
	h3d_camera_valid = t != NULL;
	if (!t)
	{
		Mtx_Scale(&modelview,1,1,-1);
		H3D_UpdateFrustum(); 
		return;
	}
	h3d_camera_x = t->x;
	h3d_camera_z = t->y;
	Mtx_Scale(&modelview,t->scalex,t->scaley,-t->scalez);
	Mtx_RotateX(&modelview,C3D_AngleFromDegrees(t->anglex),true);
	Mtx_RotateY(&modelview,C3D_AngleFromDegrees(t->angley+270),true);
	Mtx_Translate(&modelview,-t->x,-t->z,-t->y,true);
	fov = t->fovxangle;
	if (fov<1) fov=90;
	H3D_Reprojection();
}

static void H3D_ClipRect(int minx,int miny,int maxx,int maxy,float nearclip)
{
	(void)minx; (void)miny; (void)maxx; (void)maxy;
#if HWR_MAX3_CLIP_CACHE
    
    
    if (nearz == (nearclip > 0 ? nearclip : 0.9f))
    {
        return;
    }
#endif
	H3D_FlushBatch();
    H3D_FlushSortQueue(); 
#if HWR_VIEWPORT_SCISSOR
	
	
	if (inframe)
		C3D_SetScissor(GPU_SCISSOR_NORMAL,
			(LCD_H-TOP_H)/2, (LCD_W-TOP_W)/2,
			(LCD_H+TOP_H)/2, (LCD_W+TOP_W)/2);
#endif
	++h3d_transform_generation;
	nearz = nearclip>0 ? nearclip : 0.9f;
	H3D_Reprojection();
	
}

static void H3D_SetSpecial(hwdspecialstate_t id, int value)
{
#if HWR_MAX3_SPECIAL_CACHE
    if ((id == HWD_SET_FOV && fov == (float)value) ||
        (id == HWD_SET_PALETTECOLOR && palettecolor == (unsigned)value) ||
        (id == HWD_SET_TEXTUREFILTERMODE &&
         filtering == (value != HWD_SET_TEXTUREFILTER_POINTSAMPLED)))
    {
        return;
    }
    
    
    if (id != HWD_SET_FOV && id != HWD_SET_PALETTECOLOR &&
        id != HWD_SET_TEXTUREFILTERMODE)
        return;
#endif
	H3D_FlushBatch();
    H3D_FlushSortQueue(); 
	if (id==HWD_SET_FOV) { fov=value; H3D_Reprojection(); ++h3d_transform_generation; }
	else if (id==HWD_SET_PALETTECOLOR) palettecolor=(unsigned)value;
	else if (id==HWD_SET_TEXTUREFILTERMODE)
		filtering=(value!=HWD_SET_TEXTUREFILTER_POINTSAMPLED);
	
}

static void H3D_ReadRect(int x,int y,int w,int h,int stride,unsigned short *dst)
{
	(void)x; (void)y;
	
	if (dst && w>0 && h>0 && stride>=w)
		memset(dst,0,(size_t)stride*h*2);
}

static void H3D_DrawMD2(int *cmd,md2_frame_t *frame,FTransform *pos,float scale)
{
	(void)cmd; (void)frame; (void)pos; (void)scale;
	
}

static int H3D_GetTextureUsed(void)
{
	texcache_t *t;
	int sum=0;
	for (t=textures;t;t=t->next)
		sum+=t->tex.width*t->tex.height*4;
	return sum;
}

static int H3D_GetVersion(void) { return VERSION; }

void HWR3DS_InstallDriver(void)
{
	
	HWD.pfnInit = H3D_Init;
	HWD.pfnFinishUpdate = HWR3DS_EndFrame;
	HWD.pfnDraw2DLine = H3D_DrawLine;
	HWD.pfnDrawPolygon = H3D_DrawPolygon;
	HWD.pfnSetBlend = H3D_SetBlend;
	HWD.pfnClearBuffer = H3D_ClearBuffer;
	HWD.pfnSetTexture = H3D_SetTexture;
	HWD.pfnReadRect = H3D_ReadRect;
	HWD.pfnGClipRect = H3D_ClipRect;
	HWD.pfnClearMipMapCache = H3D_ClearTextures;
	HWD.pfnSetSpecialState = H3D_SetSpecial;
	HWD.pfnSetPalette = H3D_SetPalette;
	HWD.pfnGetTextureUsed = H3D_GetTextureUsed;
	HWD.pfnDrawMD2 = H3D_DrawMD2;
	HWD.pfnSetTransform = H3D_SetTransform;
	HWD.pfnGetRenderVersion = H3D_GetVersion;
	HWD.pfnShutdown = H3D_Close;
}

void HWR3DS_Close(void)
{
	H3D_Close();
}
