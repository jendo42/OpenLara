#ifndef H_GAPI_GS
#define H_GAPI_GS

#include "core.h"

#include <dma.h>
#include <dma_tags.h>
#include <graph.h>
#include <gif_tags.h>
#include <gif_registers.h>
#include <gs_gp.h>
#include <gs_privileged.h>
#include <gs_psm.h>
#include <draw.h>
#include <libvux.h>
#include <kernel.h>
#include <stdlib.h>
#include <malloc.h>

#include "system.h"

#include <vector>
#include <unordered_set>

// =================================================================================================
// PS2 / GS configuration: every tunable of the GS backend (this file) and of the engine-side effects
// (gapi/gs_fx.h) in one place. All of them can be overridden from the Makefile (-DGS_xxx=value).
// =================================================================================================
// ---- debug switches (1 = on) --------------------------------------------------------------------
#ifndef GS_ANIM_CROSSFADE
    #define GS_ANIM_CROSSFADE   1           // crossfade texture animation frames (rooms + water)
#endif
#ifndef GS_SHADOWS
    #define GS_SHADOWS          1           // shadow map shadows under entities
#endif
#ifndef GS_WATER_WAVES
    #define GS_WATER_WAVES      1           // wave fields + water surface layers
#endif
#ifndef GS_PROFILE
    #define GS_PROFILE          1           // "PROF ms/frame" log line once per second
#endif

// ---- frame ----------------------------------------------------------------------------------------
#ifndef GS_LATE_SWAP
    #define GS_LATE_SWAP        1           // adaptive vsync: show a late frame immediately (may tear)
#endif
#ifndef GS_PACKET_RESERVE
    #define GS_PACKET_RESERVE   8192        // qwords of the frame packet kept free for state changes + frame end
#endif

// ---- lighting / fog -------------------------------------------------------------------------------
#ifndef GS_MIRROR_ENV
    #define GS_MIRROR_ENV       0.5f        // save crystal: average environment reflection
#endif
#ifndef GS_FOG_LUT_SCALE
    #define GS_FOG_LUT_SCALE    32.0f       // fog LUT index = dist * density * 32, covers exp(0 .. -8)
#endif

// ---- underwater -----------------------------------------------------------------------------------
#ifndef GS_WATER_DEPTH_RANGE
    #define GS_WATER_DEPTH_RANGE 3072.0f    // depth below the surface where the depth tint is full
#endif
#ifndef GS_RIPPLE_AMP
    #define GS_RIPPLE_AMP       3.0f        // screen ripple: horizontal offset in pixels
#endif
#ifndef GS_RIPPLE_STRIP
    #define GS_RIPPLE_STRIP     4           // screen ripple: strip height in pixels
#endif

// ---- water surface ----------------------------------------------------------------------------------
#ifndef GS_WATER_FACE_ALPHA
    #define GS_WATER_FACE_ALPHA 0.5f        // opacity of the original (PS1) water surface texture
#endif
#ifndef GS_WATER_SPECULAR
    #define GS_WATER_SPECULAR   0.60f       // specular strength (masks the original water texture)
#endif
#ifndef GS_WATER_SPEC_MIN
    #define GS_WATER_SPEC_MIN   0.2f        // original texture visibility without specular
#endif
#ifndef GS_WATER_ALPHA_MIN
    #define GS_WATER_ALPHA_MIN  0.20f       // tint layer opacity looking straight down
#endif
#ifndef GS_WATER_FRESNEL
    #define GS_WATER_FRESNEL    1.1f        // Fresnel strength (opacity + reflection color at grazing angles)
#endif
#ifndef GS_SPEC_GAIN
    #define GS_SPEC_GAIN        4.0f        // wave specular gain (reflection added by the waves)
#endif
#ifndef GS_SPEC_CREST
    #define GS_SPEC_CREST       0.35f       // extra highlight on wave crests
#endif

// ---- waves --------------------------------------------------------------------------------------------
#ifndef GS_WAVE_CELL
    #define GS_WAVE_CELL        256.0f      // world units per grid cell (4 cells per sector)
#endif
#ifndef GS_WAVE_HEIGHT
    #define GS_WAVE_HEIGHT      7.0f        // world units per height unit
#endif
#ifndef GS_WAVE_MAX
    #define GS_WAVE_MAX         11.0f       // height clamp in height units
#endif
#ifndef GS_WAVE_SPEED2
    #define GS_WAVE_SPEED2      0.003f      // c^2 in cells^2 per step (~840 units/s at 60 Hz)
#endif
#ifndef GS_WAVE_DAMPING
    #define GS_WAVE_DAMPING     0.997f      // per step
#endif
#ifndef GS_WAVE_DROP
    #define GS_WAVE_DROP        20.0f       // Level::waterDrop strength -> height units
#endif
#ifndef GS_WAVE_SHADE
    #define GS_WAVE_SHADE       3.0f        // slope exaggeration for normals (Fresnel, specular)
#endif
#ifndef GS_WAVE_UV
    #define GS_WAVE_UV          0.08f       // ripple texture distortion by wave slope
#endif
#ifndef GS_WAVE_FIELDS
    #define GS_WAVE_FIELDS      8           // max simulated pools at once
#endif
#ifndef GS_WAVE_STEP
    #define GS_WAVE_STEP        (1.0f / 60.0f)  // simulation step (30 Hz looked choppy)
#endif
#ifndef GS_WAVE_CALM_RADIUS
    #define GS_WAVE_CALM_RADIUS 1024.0f     // idle motion: random drop radius
#endif
#ifndef GS_WAVE_CALM_AMOUNT
    #define GS_WAVE_CALM_AMOUNT 0.1f        // idle motion: random drop strength
#endif

// ---- shadows ------------------------------------------------------------------------------------------
#ifndef GS_SHADOW_DARKNESS
    #define GS_SHADOW_DARKNESS  0.5f        // opacity on the ground
#endif
#ifndef GS_SHADOW_FADE
    #define GS_SHADOW_FADE      1024.0f     // height above the floor where the shadow has faded out
#endif
#ifndef GS_SHADOW_FROM_LIGHT
    #define GS_SHADOW_FROM_LIGHT 0          // 0: top-down shadow, 1: cast away from the main room light
#endif
#ifndef GS_SHADOW_MIN_Y
    #define GS_SHADOW_MIN_Y     0.6f        // GS_SHADOW_FROM_LIGHT: min vertical light component (max tilt ~53 deg)
#endif
#ifndef GS_SHADOW_LIFT
    #define GS_SHADOW_LIFT      6.0f        // shadow quad above the floor (avoids fighting with it)
#endif
#ifndef GS_SHADOW_SOFT
    #define GS_SHADOW_SOFT      2.0f        // soft edge: blur tap offset in shadow map texels (0 = hard)
#endif
#ifndef GS_SHADOW_MAP_SIZE
    #define GS_SHADOW_MAP_SIZE  128         // shadow map target size (pixels)
#endif

// ---- fixed constants (hardware / memory layout, do not change) ------------------------------------
#define GS_Z_MAX                16777215.0f                     // 24-bit Z buffer (PSMZ24); fog F uses the free top byte of XYZF2
#define GS_PAGE_WORDS           2048                            // VRAM page (8 KB)
#define GS_PAGES                (GRAPH_VRAM_MAX_WORDS / GS_PAGE_WORDS)
#define GS_TILE_WORDS           (256 * 256 / 2 / 4)             // PSMT4 tile in normal VRAM: 32 KB
#define GS_TILE_HL_WORDS        (32 * 2048)                     // 4HL/4HH tile: 32 pages of address space
#define GS_TILE_SLOTS           24                              // tile slots in the spare bytes of fb0 + fb1 + Z
#define GS_CAUSTIC_LUT_SCALE    (256.0f / PI)                   // caustic LUT: one |sin| period in 256 steps
#define GS_WATER_TEX_SIZE       64                              // ripple texture
#define GS_WATER_MAX_VERTS      4096                            // water / shadow quad vertex buffer
#define GS_WATER_MAX_INDICES    (GS_WATER_MAX_VERTS * 6)

#define PROFILE_MARKER(title)
#define PROFILE_LABEL(id, name, label)
#define PROFILE_TIMING(time)

#define   GS_LOG(f, ...) LOG("[I] GS %d @ %s: " f "\n", frame_begin == frame_end ? -1 : frame_end, __func__ __VA_OPT__(,) __VA_ARGS__)
//#define GS_DEBUG(f, ...) LOG("[D] GS %d @ %s: " f "\n", frame_begin == frame_end ? -1 : frame_end, __func__ __VA_OPT__(,) __VA_ARGS__)

#ifndef GS_DEBUG
#define GS_DEBUG(...)
#endif

#ifndef GS_LOG
#define GS_LOG(...)
#endif

#define CHECK_INFRAME(x) \
    if (!isInFrame()) { \
        GS_DEBUG("WARN: called outside frame!");\
        return x; \
    }

namespace GAPI {

    struct Vertex {
        texel_t texCoord;
        color_t color;
        VU_VECTOR normal;
        VU_VECTOR coord;
    };

    int idgsh;
    int idvblankh;

    volatile u32 vblankCounter;
    int vblankSema;
    int gsSema;
    int dmaSema;
    int cullMode;
    int blendMode = 0;      // RS_BLEND_* bits, 0 = blending off
    bool vsync = true;
    bool fog = false;
    vec4 fog_color;
    u32 qwc_max;

    // ---- frame profile (EE COP0 Count, 294.912 MHz): printed once per second -----------------
    static inline u32 gsCycles() {
        u32 c;
        asm volatile ("mfc0 %0, $9" : "=r"(c));
        return c;
    }
    struct GSProfile {
        u32 frameStart;     // Count at the previous endFrame
        u32 buildStart;     // Count at beginFrame
        u64 frame, build, dip, dipColor, dipEmit, gsWait, vsyncWait;
        u32 frames, dips, packetQw, workMax, late;
    } prof;
    struct GSProfScope {
        u64 &acc; u32 t0;
        GSProfScope(u64 &acc) : acc(acc), t0(gsCycles()) {}
        ~GSProfScope() { acc += u32(gsCycles() - t0); }
    };
    
    float origin_x, origin_y;
    framebuffer_t fbs[2];
    zbuffer_t zbs;

    int fbc;
    framebuffer_t *frontbuffer;
    framebuffer_t *backbuffer;
    zbuffer_t *zbuffer;
    atest_t atest;
    dtest_t dtest;
    ztest_t ztest;
    blend_t blend;
    qword_t *qp;
    qword_t *qs;


    u32 frame_begin;
    u32 frame_end;
    u8 cr;
    u8 cg;
    u8 cb;

    VU_MATRIX view;
    VU_MATRIX proj;

    const u32 qtotal = 8192*16;
    qword_t packets1[qtotal] __attribute__((aligned(64)));
    qword_t packets2[qtotal] __attribute__((aligned(64)));

    qword_t * const packets[2] = {
        &packets1[0],
        &packets2[0]
    };

    // dithering matrix from PSP
    const dithermx_t dith = {
        -4,  0, -3,  1,
         2, -2,  3, -1,
        -3,  1, -4,  0,
         3, -1,  2, -2
    };

    struct Texture;
    std::vector<Texture *> textureUploadList;
    // ---- VRAM page allocator ------------------------------------------------------------------
    // GS memory = 512 pages of 8 KB (2048 words). Framebuffers + Z are allocated once by libgraph at
    // the bottom; everything above (textures, tiles fallback, CLUTs) uses this first-fit page allocator,
    // so blocks can be freed in any order (libgraph's allocator is a stack).

    u16 vramPageOwner[GS_PAGES];    // per page: allocation size in pages at its first page, 0xFFFF inside, 0 free
    int vramHeapStart = GS_PAGES;   // first page managed here (set in init after the buffers)

    void vramInit(u32 heapStartWords) {
        vramHeapStart = (heapStartWords + GS_PAGE_WORDS - 1) / GS_PAGE_WORDS;
        memset(vramPageOwner, 0, sizeof(vramPageOwner));
    }

    void vramStats(int &used, int &largest) {
        used = largest = 0;
        int run = 0;
        for (int i = vramHeapStart; i < GS_PAGES; i++) {
            if (vramPageOwner[i]) {
                used++;
                run = 0;
            } else {
                run++;
                largest = max(largest, run);
            }
        }
    }

    // returns word address (page aligned) or (u32)-1
    u32 vramAlloc(u32 words) {
        int pages = (words + GS_PAGE_WORDS - 1) / GS_PAGE_WORDS;
        if (pages <= 0) pages = 1;
        for (int i = vramHeapStart; i + pages <= GS_PAGES; i++) {
            int n = 0;
            while (n < pages && !vramPageOwner[i + n]) n++;
            if (n == pages) {
                vramPageOwner[i] = u16(pages);
                for (int k = 1; k < pages; k++) {
                    vramPageOwner[i + k] = 0xFFFF;
                }
                int used, largest;
                vramStats(used, largest);
                GS_LOG("VRAM alloc %d pages at page %d (used %d of %d, largest free %d)", pages, i, used, GS_PAGES - vramHeapStart, largest);
                return u32(i) * GS_PAGE_WORDS;
            }
            i += n;     // skip the free run that was too small (loop adds 1 more: the used page)
        }
        int used, largest;
        vramStats(used, largest);
        GS_LOG("WARN: VRAM alloc of %d pages failed (used %d of %d, largest free %d)", pages, used, GS_PAGES - vramHeapStart, largest);
        return u32(-1);
    }

    void vramFree(u32 address) {
        int page = address / GS_PAGE_WORDS;
        if (page < vramHeapStart || page >= GS_PAGES || vramPageOwner[page] == 0 || vramPageOwner[page] == 0xFFFF) {
            GS_LOG("WARN: VRAM free of invalid address %X", address);
            return;
        }
        int pages = vramPageOwner[page];
        for (int k = 0; k < pages; k++) {
            vramPageOwner[page + k] = 0;
        }
    }

    Texture *diffuseTex = NULL; // in GAPI namespace
    Texture *gsTexture = NULL;

    using namespace Core;

    bool isInFrame() {
        return frame_begin > frame_end;
    }

    int scissorRect[4] = { 0, 639, 0, 447 };    // x0, x1, y0, y1 (inclusive, GS window coords)

    // TEXFLUSH as plain GIF data. NOT draw_texture_flush(): that one starts with a DMA tag (meant
    // to end a standalone upload chain) and corrupts the GIF stream inside the frame command list.
    inline qword_t *gsTexFlush(qword_t *q) {
        PACK_GIFTAG(q, GIF_SET_TAG(1, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
        q++;
        PACK_GIFTAG(q, 1, GS_REG_TEXFLUSH);
        q++;
        return q;
    }

    qword_t *emitScissor(qword_t *q) {
        return draw_scissor_area(q, 0, scissorRect[0], scissorRect[1], scissorRect[2], scissorRect[3]);
    }

// Shader
    struct Shader {
        void init(Pass pass, int type, int *def, int defCount) {}
        void deinit() {}
        void bind() {}
        void setParam(UniformType uType, const vec4  &value, int count = 1) {}
        void setParam(UniformType uType, const mat4  &value, int count = 1) {}
    };

// Texture
    struct Texture {
        int        width, height, depth, origWidth, origHeight, origDepth;//todo: depth
        TexFormat  fmt;
        uint32     opt;
        u32        memory;
        u32        size;
        int        psm;
        void       *buffer;
        int        tbw;
        int uploadW, uploadH;
        u32 *tileAddr;      // per tile: GS address (spare byte of a CT24/Z24 buffer, or fallback VRAM)
        u8  *tilePsm;       // per tile: GS_PSM_4HL / GS_PSM_4HH (in a 24-bit buffer) or GS_PSM_4 (fallback)
        int  tileCount;
        u32  tilesFallbackMem;
        bool alphaRGBA;     // sample texture alpha (TCC RGBA); texture alpha must be GS scaled (0x80 = 1.0)
        u32 clutsMem;
        Tile4 *tilesBase;
        CLUT *clutsBase;
        int curTile;
        int curClut;

        static int fmt2psm(TexFormat fmt) {
            switch (fmt) {
                case FMT_RGBA:
                    return GS_PSM_32;
                case FMT_RGB16:
                    return GS_PSM_16;
                case FMT_LUMINANCE:
                    return GS_PSM_8;
                default:
                    ASSERT(false);
                    return -1;
            }
        }

        static int fmt2components(TexFormat fmt) {
            switch (fmt) {
                case FMT_RGBA:
                    return TEXTURE_COMPONENTS_RGBA;
                case FMT_RGB16:
                    return TEXTURE_COMPONENTS_RGB;
                default:
                    ASSERT(false);
                    return -1;
            }
        }

        /*depth*/
        Texture(int width, int height, int depth, uint32 opt)
            : width(width)
            , height(height)
            , depth(depth)
            , origWidth(width)
            , origHeight(height)
            , origDepth(depth)
            , fmt(FMT_RGBA)
            , opt(opt)
            , memory(0)
            , size(0)
            , psm(GS_PSM_32)
            , buffer(NULL)
            , tbw((origWidth + 63) & ~63)
            , uploadW(width)
            , uploadH(height)
            , tileAddr(NULL)
            , tilePsm(NULL)
            , tileCount(0)
            , tilesFallbackMem(0)
            , alphaRGBA(false)
            , clutsMem(0)
            , tilesBase(NULL)
            , clutsBase(NULL)
            , curTile(-1)
            , curClut(-1)
        {

        }

        void init(void *data) {
            ASSERT((opt & OPT_PROXY) == 0);
            opt &= ~(OPT_CUBEMAP | OPT_MIPMAPS);
            psm = fmt2psm(fmt);
            if (psm == -1) {
                GS_LOG("WARN: Texture unsupported format (%d)", fmt);
                return;
            }
            // large static RGBA images (PSX title/loading screens: 512x256 15-bit source) are stored as
            // CT16 to fit next to the 24-bit framebuffers - lossless for 15-bit data
            if (psm == GS_PSM_32 && !(opt & OPT_DYNAMIC) && uploadW * uploadH > 256 * 256) {
                psm = GS_PSM_16;
            }
            int bpp = (psm == GS_PSM_32) ? 4 : (psm == GS_PSM_16) ? 2 : 1;
            size = uploadW * uploadH * bpp;
            if (size & 15) {
                GS_LOG("WARN: texture %dx%d psm %d is not a qword multiple, tail will not be uploaded", uploadW, uploadH, psm);
            }
            // GS memory is organised in 8KB pages (CT32: 64x32, CT16: 64x64, T8: 128x64 pixels).
            // A buffer always occupies whole page rows/columns, so allocate the full page footprint,
            // otherwise the last page row spills into the next allocation (or wraps to VRAM address 0).
            int pw = (psm == GS_PSM_8) ? 128 : 64;
            int ph = (psm == GS_PSM_32 || psm == GS_PSM_24) ? 32 : 64;
            if (psm == GS_PSM_8) {
                tbw = (tbw + 127) & ~127;   // PSMT8 needs an even TBW
            }
            int allocW = (tbw + pw - 1) / pw * pw;
            int allocH = (uploadH + ph - 1) / ph * ph;
            memory = vramAlloc(graph_vram_size(allocW, allocH, psm, GRAPH_ALIGN_PAGE));
            if (memory == -1) {
                memory = 0;
                GS_LOG("WARN: no more VRAM available for %dx%d psm %d", uploadW, uploadH, psm);
                return;
            }
            buffer = memalign(64, size);
            if (data) {
                fillBuffer(data);
            } else {
                memset(buffer, 0, size);
            }
            textureUploadList.push_back(this);
            GS_LOG("Allocated texture %dx%d psm %d at %X", uploadW, uploadH, psm, memory);
        }

        static void vramRelease(u32 addr) {
            vramFree(addr);
        }

        void deinit() {
            if (gsTexture == this)  gsTexture = NULL;
            if (diffuseTex == this) diffuseTex = NULL;
            for (size_t i = 0; i < textureUploadList.size(); i++) {   // drop a pending deferred upload
                if (textureUploadList[i] == this) {
                    textureUploadList.erase(textureUploadList.begin() + i);
                    break;
                }
            }
            if (memory) {
                vramRelease(memory);
                memory = 0;
            }
            if (buffer) {
                free(buffer);
                buffer = NULL;
            }
        }

        void generateMipMap() {

        }

        static void uploadSync(void *src, int w, int h, int psm, u32 addr, int tbw) {
            static qword_t uploadPacket[256] __attribute__((aligned(64)));
            qword_t *q = uploadPacket;
            q = draw_texture_transfer(q, src, w, h, psm, addr, tbw);
            q = draw_texture_flush(q);
            dma_channel_wait(DMA_CHANNEL_GIF, 0);
            FlushCache(0);
            dma_channel_send_chain(DMA_CHANNEL_GIF, uploadPacket, q - uploadPacket, 0, 0);
            dma_channel_wait(DMA_CHANNEL_GIF, 0);
        }

        // RGBA source into the GS buffer (converted to 5:5:5:1 when stored as CT16)
        void fillBuffer(const void *data) {
            if (fmt == FMT_RGBA && psm == GS_PSM_16) {
                const u8 *src = (const u8*)data;
                u16 *dst = (u16*)buffer;
                for (int i = 0; i < uploadW * uploadH; i++, src += 4) {
                    dst[i] = u16((src[0] >> 3) | ((src[1] >> 3) << 5) | ((src[2] >> 3) << 10) | ((src[3] >= 128) ? 0x8000 : 0));
                }
            } else {
                memcpy(buffer, data, size);
            }
        }

        void update(void *data) {
            if (data && size) {
                fillBuffer(data);
                //textureUploadList.push_back(this);
                uploadNow(this);
            }
        }

        void uploadNow(Texture *t) {
            if (t->memory && t->buffer)
                uploadSync(t->buffer, t->uploadW, t->uploadH, t->psm, t->memory, t->tbw);
        }

        qword_t *upload(qword_t *qp) {
            CHECK_INFRAME(qp);
            if (memory && buffer) {
                qp = draw_texture_transfer(qp, buffer, uploadW, uploadH, psm, memory, tbw);
            }
            return qp;
        }

        void bind(int sampler) {
            switch (sampler) {
                case sDiffuse:
                    Core::active.textures[sampler] = this;
                    diffuseTex = ((memory && !(opt & OPT_PROXY)) || (tileAddr && clutsMem)) ? this : NULL;
                    break;
                case sNormal:
                case sReflect:
                case sShadow:
                case sMask:
                    break;
            }
        }

        // PSX level tiles: 256x256 4-bit + CLUTs (CSM2 strip, 16 CLUTs per 256-px CT16 row).
        // Tiles live in the unused top byte of the 24-bit color/Z buffers as PSMT4HL / PSMT4HH:
        // the GS never writes bits 24..31 of CT24/Z24 pixels. A 256x256 4HL/4HH tile uses the CT32
        // page layout (TBW 4) = 32 consecutive pages; a 640x448 buffer (140 pages) holds 4 tiles per
        // nibble -> 8 slots per buffer, 24 slots in fb0 + fb1 + Z. Extra tiles go to normal VRAM.

        static u32 tileSlotBuffer(int b) {
            return (b < 2) ? fbs[b].address : zbuffer->address;
        }

        void initTiles(Tile4 *tiles, int tilesCount, CLUT *cluts, int clutsCount) {
            tilesBase = tiles;
            clutsBase = cluts;
            tileCount = tilesCount;
            tileAddr  = new u32[tilesCount];
            tilePsm   = new u8[tilesCount];

            int fallback = max(0, tilesCount - GS_TILE_SLOTS);
            if (fallback) {
                GS_LOG("WARN: %d tiles do not fit the 24-bit buffer slots, using normal VRAM", fallback);
                u32 mem = vramAlloc(graph_vram_size(256, 256 * fallback, GS_PSM_4, GRAPH_ALIGN_PAGE));
                if (mem == (u32)-1) {
                    GS_LOG("WARN: no VRAM for %d fallback tiles", fallback);
                    tilesCount = tileCount = GS_TILE_SLOTS;
                    fallback = 0;
                } else {
                    tilesFallbackMem = mem;
                }
            }

            for (int i = 0; i < tilesCount; i++) {
                if (i < GS_TILE_SLOTS) {
                    int b = i / 8;
                    tilePsm[i]  = ((i % 8) < 4) ? GS_PSM_4HL : GS_PSM_4HH;
                    tileAddr[i] = tileSlotBuffer(b) + (i % 4) * GS_TILE_HL_WORDS;
                } else {
                    tilePsm[i]  = GS_PSM_4;
                    tileAddr[i] = tilesFallbackMem + (i - GS_TILE_SLOTS) * GS_TILE_WORDS;
                }
            }

            int rows = (clutsCount + 15) / 16;
            clutsMem = vramAlloc(graph_vram_size(256, rows, GS_PSM_16, GRAPH_ALIGN_PAGE));
            if (clutsMem == (u32)-1) {
                GS_LOG("WARN: no VRAM for %d CLUTs", clutsCount);
                clutsMem = 0;
                return;
            }

            // DMA REF source must be 16-byte aligned, copy through a temp buffer if it isn't
            void *tmp = ((u32)tiles & 15) ? memalign(64, sizeof(Tile4)) : NULL;
            for (int i = 0; i < tilesCount; i++) {
                void *src = tiles + i;
                if (tmp) {
                    memcpy(tmp, src, sizeof(Tile4));
                    src = tmp;
                }
                uploadSync(src, 256, 256, tilePsm[i], tileAddr[i], 256);
            }
            if (tmp) free(tmp);

            int csize = rows * 16 * sizeof(CLUT);
            CLUT *cbuf = (CLUT*)memalign(64, csize);
            memset(cbuf, 0, csize);
            memcpy(cbuf, cluts, clutsCount * sizeof(CLUT));
            uploadSync(cbuf, 256, rows, GS_PSM_16, clutsMem, 256);
            free(cbuf);

            GS_LOG("Tiles: %d (%d in 24-bit buffer slots), CLUTs: %d @ %X",
                   tilesCount, min(tilesCount, GS_TILE_SLOTS), clutsCount, clutsMem);
        }

        void releaseTiles() {
            if (gsTexture == this)  gsTexture = NULL;
            if (diffuseTex == this) diffuseTex = NULL;
            if (clutsMem) {
                vramRelease(clutsMem);
            }
            if (tilesFallbackMem) {
                vramRelease(tilesFallbackMem);
            }
            delete[] tileAddr;
            delete[] tilePsm;
            tileAddr = NULL;
            tilePsm  = NULL;
            tileCount = 0;
            clutsMem = tilesFallbackMem = 0;
        }

        void bindTileCLUT(void *tile, void *clut) {
            ASSERT(tile);
            ASSERT(clut);
            ASSERT(tileAddr);

            int ti = (Tile4*)tile - tilesBase;
            int ci = (CLUT*)clut  - clutsBase;

            Core::active.textures[sDiffuse] = this;
            diffuseTex = (tileAddr && clutsMem && ti >= 0 && ti < tileCount) ? this : NULL;

            if (ti != curTile || ci != curClut) {
                curTile = ti;
                curClut = ci;
                if (gsTexture == this) {
                    gsTexture = NULL; // force TEX0 re-emit
                }
            }
        }

        void unbind(int sampler) {
            GS_LOG("Texture::unbind(%d)", sampler);
        }

        void setFilterQuality(int value) {
            if (value > Settings::LOW)
                opt &= ~OPT_NEAREST;
            else
                opt |= OPT_NEAREST;
        }
    };

// Mesh
    struct Mesh {
        Index        *iBuffer;
        GAPI::Vertex *vBuffer;

        int          iCount;
        int          vCount;
        bool         dynamic;

        Mesh(bool dynamic) : iBuffer(NULL), vBuffer(NULL), dynamic(dynamic) {

        }

        void init(Index *indices, int iCount, ::Vertex *vertices, int vCount, int aCount) {
            this->iCount = iCount;
            this->vCount = vCount;

            iBuffer = new Index[iCount];
            vBuffer = allocVertices(vCount);

            update(indices, iCount, vertices, vCount);
        }

        // VU0 lqc2/sqc2 silently ignore the low 4 address bits: vertex data must be 16-byte aligned
        static GAPI::Vertex *allocVertices(int count) {
            return (GAPI::Vertex*)memalign(64, count * sizeof(GAPI::Vertex));
        }

        void deinit() {
            delete[] iBuffer;
            free(vBuffer);
            iBuffer = NULL;
            vBuffer = NULL;
        }

        void update(Index *indices, int iCount, ::Vertex *vertices, int vCount) {
            if (this->iCount < iCount) {
                delete[] iBuffer;
                iBuffer = new Index[iCount];
                this->iCount = iCount;
            }
            if (this->vCount < vCount) {
                free(vBuffer);
                vBuffer = allocVertices(vCount);
                this->vCount = vCount;
            }
            if (indices) {
                memcpy(iBuffer, indices, iCount * sizeof(indices[0]));
            }
            if (vertices) {
                ::Vertex *src = vertices;
                GAPI::Vertex *dst = vBuffer;
                for (int i = 0; i < vCount; i++, dst++, src++) {
                    // convert the fixed point TOMB data into actual GS vertices
                    dst->texCoord.u = (float)src->texCoord.x / 32767.0f;
                    dst->texCoord.v = (float)src->texCoord.y / 32767.0f;
                    //dst->color.r = src->color.x;
                    //dst->color.g = src->color.y;
                    //dst->color.b = src->color.z;
                    //dst->color.a = 127;
                    dst->color.r = (src->light.x + 1) >> 1; // GS modulate: 0x80 = 1.0
                    dst->color.g = (src->light.y + 1) >> 1;
                    dst->color.b = (src->light.z + 1) >> 1;
                    dst->color.a = (src->light.w + 1) >> 1; // 0x80 = opaque for blending too
                    dst->color.q = 1.0f;
                    {   // unit normals for VU0 lighting (room normals are not normalized)
                        float nx = float(src->normal.x), ny = float(src->normal.y), nz = float(src->normal.z);
                        float len2 = nx * nx + ny * ny + nz * nz;
                        float inv = (len2 > 0.0f) ? 1.0f / sqrtf(len2) : 0.0f;
                        dst->normal.x = nx * inv;
                        dst->normal.y = ny * inv;
                        dst->normal.z = nz * inv;
                        dst->normal.w = 0.0f;
                    }
                    dst->coord.x = (float)src->coord.x / 32767.0f;
                    dst->coord.y = (float)src->coord.y / 32767.0f;
                    dst->coord.z = (float)src->coord.z / 32767.0f;
                    dst->coord.w = 1.0f;
                }
            }
        }

        void bind(const MeshRange &range) const {

        }

        void initNextRange(MeshRange &range, int &aIndex) const {
            range.aIndex = -1;
        }
    };

    inline mat4::ProjRange getProjRange() {
        return mat4::PROJ_ZERO_POS;
    }

    mat4 ortho(float l, float r, float b, float t, float znear, float zfar) {
        mat4 m;
        m.ortho(getProjRange(), l, r, b, t, znear, zfar);
        return m;
    }

    mat4 perspective(float fov, float aspect, float znear, float zfar, float eye) {
        mat4 m;
        m.perspective(getProjRange(), fov, aspect, znear, zfar, eye);
        return m;
    }


    bool depthTest  = true;
    bool depthWrite = true;

    void applyDepthState() {
        ztest.enable   = DRAW_ENABLE;                                   // ZTE must stay 1
        ztest.method   = depthTest ? ZTEST_METHOD_GREATER_EQUAL : ZTEST_METHOD_ALLPASS;
        zbuffer->mask  = (depthTest && depthWrite) ? 0 : 1;            // GL: no test -> no write
        if (isInFrame()) {
            qp = draw_pixel_test(qp, 0, &atest, &dtest, &ztest);
            qp = draw_zbuffer(qp, 0, zbuffer);
        }
    }

    void setDepthTest(bool enable) {
        depthTest = enable;
        applyDepthState();
    }

    void setDepthWrite(bool enable) {
        depthWrite = enable;
         applyDepthState();
    }

    void setFog(const vec4 &params) {   // state only (FOGCOL is written in DIP), valid outside a frame too
        fog_color = params;
        fog = params.w > 0.0f;
    }
    
    bool beginFrame() {
        GS_DEBUG("frame_begin: %u", frame_begin);
        //CHECK_INFRAME(false);

        ++frame_begin;
        gsTexture = NULL;

        prof.buildStart = gsCycles();

        // prepare command list
        qs = packets[fbc];

        // upload pending textures
        auto textures = textureUploadList.size();
        if (textures) {
            GS_LOG("Uploading %u texture(s)", textures);
            qp = qs;
        }
        for (auto texture : textureUploadList) {
            qp = texture->upload(qp);
        }
        if (textures) {
            qp = draw_texture_flush(qp);
            u32 qwc = qp - qs;
            GS_LOG("-> %u qwords used (%p, %p)", qwc, qs, qp);
            dma_channel_wait(DMA_CHANNEL_GIF, 0);
            FlushCache(0);
            dma_channel_send_chain(DMA_CHANNEL_GIF, qs, qwc, 0, 0);
            dma_channel_wait(DMA_CHANNEL_GIF, 0);
            textureUploadList.clear();
        }

        // reset command buffer
        qp = qs + 1;

        // reset drawing environment?
        //qp = draw_setup_environment(qp, 0, backbuffer, zbuffer);
        qp = draw_framebuffer(qp, 0, backbuffer);
        qp = draw_zbuffer(qp, 0, zbuffer);
        qp = draw_pixel_test(qp, 0, &atest, &dtest, &ztest);
        qp = draw_texture_expand_alpha(qp, 0x00, ALPHA_EXPAND_NORMAL, 0x80);   // CT16 A=0 -> 0, A=1 -> 0x80 (1.0)
        qp = draw_alpha_blending(qp, 0, &blend);
        qp = emitScissor(qp);
        return true;
    }

    void endFrame() {
        GS_DEBUG("frame_end: %u", frame_end);
        CHECK_INFRAME();

        // close command list
        qp = draw_finish(qp);
        int qwc = qp - qs;
        if (qwc > qwc_max + 2048) {     // packet high-water mark
            qwc_max = qwc;
            GS_LOG("-> frame packet high-water: %u of %u qwords (dips %u tris %u)", qwc, qtotal, Core::stats.dips, Core::stats.tris);
        }
        DMATAG_END(qs, qwc - 1, 0, 0, 0);

        u32 tBuilt = gsCycles();
        prof.build += u32(tBuilt - prof.buildStart);
        prof.packetQw += qwc;

        // wait until the previous frame's list (drawing into frontbuffer) is fully sent to the GS
        dma_channel_wait(DMA_CHANNEL_GIF, 0);
        u32 tGS = gsCycles();
        prof.gsWait += u32(tGS - tBuilt);

        // wait for a vblank. Late swap: if a vblank already passed while this frame was built, the frame
        // is late: show it right away (may tear for one field) instead of waiting a whole extra field,
        // which turns "slightly over budget" into a 30/40 fps cliff
        if (vsync) {
            bool late = false;
            while (PollSema(vblankSema) >= 0) late = true;     // drain, count missed vblanks
            if (!late || !GS_LATE_SWAP) {
                WaitSema(vblankSema);
            }
        }
        u32 tVS = gsCycles();
        prof.vsyncWait += u32(tVS - tGS);
        u32 work = u32(tGS - prof.frameStart);      // update + build of this frame (no waits)
        if (work > prof.workMax) prof.workMax = work;
        if (work > 294912000u / 60u) prof.late++;
        prof.frame += u32(tVS - prof.frameStart);
        prof.frameStart = tVS;
        prof.dips += Core::stats.dips;

    #if GS_PROFILE
        if (++prof.frames >= 60) {
            const float k = 1000.0f / 294912000.0f / float(prof.frames);  // cycles -> ms per frame
            GS_LOG("PROF ms/frame: total %.2f | update+other %.2f | build %.2f (DIP %.2f: color %.2f emit %.2f) | GS wait %.2f | vsync %.2f | worst %.2f late %u/%u | dips %u qw %u",
                float(prof.frame) * k, float(prof.frame - prof.build - prof.gsWait - prof.vsyncWait) * k,
                float(prof.build) * k, float(prof.dip) * k, float(prof.dipColor) * k, float(prof.dipEmit) * k,
                float(prof.gsWait) * k, float(prof.vsyncWait) * k, float(prof.workMax) * k * float(prof.frames), prof.late, prof.frames,
                prof.dips / prof.frames, prof.packetQw / prof.frames);
            u32 fs = prof.frameStart;
            memset(&prof, 0, sizeof(prof));
            prof.frameStart = fs;
        }
    #endif

        // display the previous frame, it's complete now
        // (never show backbuffer here: the GS is about to clear and draw into it)
        graph_set_framebuffer_filtered(frontbuffer->address, frontbuffer->width, frontbuffer->psm, 0, 0);

        // execute the new command list into backbuffer
        FlushCache(0);
        dma_channel_send_chain(DMA_CHANNEL_GIF, qs, qwc, 0, 0);

        // swap buffers
        fbc = ++frame_end & 1;
        backbuffer = fbs + fbc;
        frontbuffer = fbs + (fbc ^ 1);
    }

    void setAlphaTest(bool enable);
    void setBlendMode(int rsMask);

    // Core::reset() zeroes its cached render state (active.renderState = 0) and only re-sends what
    // differs from that afterwards, so the GS must really be in the "all off" state here.
    // Otherwise e.g. a leftover alpha test stays enabled while Core believes it is off.
    void resetState() {
        GS_DEBUG("");
        setAlphaTest(false);
        setBlendMode(0);
        cullMode   = 0;
        depthTest  = false;
        depthWrite = false;
        if (zbuffer) {
            applyDepthState();
        }
    }

    void bindTarget(Texture *texture, int face) {
        GS_DEBUG("");
        CHECK_INFRAME();
        // render to texture: texture VRAM is page aligned (vramAlloc), FBW comes from the 64 aligned tbw;
        // a texture without VRAM (proxy / allocation failed) falls back to the backbuffer instead of
        // drawing over address 0 (the Z buffer)
        if (!texture || !texture->memory) {
            qp = draw_framebuffer(qp, 0, backbuffer);
        } else {
            framebuffer_t fb;
            fb.mask    = 0;
            fb.width   = texture->tbw;
            fb.height  = texture->height;
            fb.psm     = texture->psm;
            fb.address = texture->memory;
            qp = draw_framebuffer(qp, 0, &fb);
        }
    }

    void discardTarget(bool color, bool depth) {
        GS_DEBUG("(%d, %d)", (int)color, (int)depth);
    }

    void copyTarget(Texture *dst, int xOffset, int yOffset, int x, int y, int width, int height) {
        GS_DEBUG("");
        // copy framebuffer into refraction texture
    }

    void setVSync(bool enable) {
        if (vsync != enable) {
            GS_LOG("vsync changed to: %s", enable ? "on" : "off");
        }
        vsync = enable;
    }

    void clear(bool color, bool depth) {
        GS_DEBUG("(%d, %d)", color, depth);
        CHECK_INFRAME();
        if (!color && !depth) {
            return;
        }

        // disable all pixel tests
        qp = draw_disable_tests(qp, 0, zbuffer);

        // clear color buffer
        u32 zmask = zbuffer->mask;
        zbuffer->mask = depth ? 0 : 1;
        qp = draw_zbuffer(qp, 0, zbuffer);

        if (!color) { // depth only: don't touch colour
            framebuffer_t fb = *backbuffer;
            fb.mask = 0xFFFFFFFF;
            qp = draw_framebuffer(qp, 0, &fb);
        }

        qp = draw_clear(qp, 0, origin_x, origin_y, backbuffer->width, backbuffer->height, cr, cg, cb);

        if (!color) {
            qp = draw_framebuffer(qp, 0, backbuffer);
        }

        // restore combined depth state
        zbuffer->mask = zmask;
        qp = draw_zbuffer(qp, 0, zbuffer);
        qp = draw_pixel_test(qp, 0, &atest, &dtest, &ztest);
    }

    void setClearColor(const vec4 &color) {
        cr = clamp(int(color.x * 255), 0, 255);
        cg = clamp(int(color.y * 255), 0, 255);
        cb = clamp(int(color.z * 255), 0, 255);
        GS_DEBUG("(%u, %u, %u)", cr, cg, cb);
    }

    // Previous: struct Viewport
    void setViewport(const short4 &vp) {
        //GS_LOG("(%d, %d, %d, %d)", vp.x, vp.y, vp.z, vp.w);
        CHECK_INFRAME();

        //qp = draw_primitive_xyoffset(qp, 0, origin_x + vp.x, origin_y + vp.y);
        //qp = draw_scissor_area(qp, 0, vp.x, vp.x + vp.z - 1, vp.y, vp.y + vp.w - 1);
    }
    
    // from other gapi, could be improved??
    // engine scissor is GL style (origin bottom-left), GS is top-left; portals use it to crop rooms
    void setScissor(const short4 &s) {
        int fbW = backbuffer ? int(backbuffer->width)  : Core::width;
        int fbH = backbuffer ? int(backbuffer->height) : Core::height;
        int x0 = s.x;
        int x1 = s.x + s.z - 1;
        int y0 = fbH - (s.y + s.w);
        int y1 = fbH - s.y - 1;
        x0 = max(x0, 0);  x1 = min(x1, fbW - 1);
        y0 = max(y0, 0);  y1 = min(y1, fbH - 1);
        if (x1 < x0 || y1 < y0) {   // empty: GS has no "nothing" scissor, use a 1px rect outside of the view
            x0 = x1 = fbW - 1;
            y0 = y1 = fbH;          // row below the visible framebuffer area
        }
        scissorRect[0] = x0; scissorRect[1] = x1;
        scissorRect[2] = y0; scissorRect[3] = y1;
        if (isInFrame()) {
            qp = emitScissor(qp);
        }
    }

    void setColorWrite(bool r, bool g, bool b, bool a) {
        GS_DEBUG("(%d, %d, %d, %d)", (int)r, (int)g, (int)b, (int)a);
        //sceGuPixelMask(~((r ? 0x000000FF : 0) | (g ? 0x0000FF00 : 0) | (b ? 0x00FF0000 : 0) | (a ? 0xFF000000 : 0)));
    }

    void setAlphaTest(bool enable) {
        GS_DEBUG("(%d)", (int)enable);
        atest.enable = enable ? DRAW_ENABLE : DRAW_DISABLE;
        if (isInFrame()) {
            qp = draw_pixel_test(qp, 0, &atest, &dtest, &ztest);
        }
    }

    void setCullMode(int rsMask) {
        GS_DEBUG("(%d)", rsMask);
        cullMode = rsMask;
        //switch (rsMask) {
        //    case RS_CULL_BACK  : sceGuFrontFace(GU_CCW);     break;
        //    case RS_CULL_FRONT : sceGuFrontFace(GU_CW);      break;
        //    default            : sceGuDisable(GU_CULL_FACE); return;
        //}
        //sceGuEnable(GU_CULL_FACE);
    }

    void setBlendMode(int rsMask) {
        GS_DEBUG("(%d)", rsMask);
        blendMode = rsMask & RS_BLEND;

        // GS: Cv = ((A - B) * C >> 7) + D,  A/B/D: 0 = Cs, 1 = Cd, 2 = 0;  C: 0 = As, 1 = Ad, 2 = FIX
        blend.fixed_alpha = 0x80;
        if (blendMode & RS_BLEND_ADD) {             // Cs + Cd
            blend.color1 = BLEND_COLOR_SOURCE;
            blend.color2 = BLEND_COLOR_ZERO;
            blend.alpha  = BLEND_ALPHA_FIXED;
            blend.color3 = BLEND_COLOR_DEST;
        } else if (blendMode & RS_BLEND_MULT) {     // Cd * Cs is not possible on GS, approximate Cd * As
            blend.color1 = BLEND_COLOR_DEST;
            blend.color2 = BLEND_COLOR_ZERO;
            blend.alpha  = BLEND_ALPHA_SOURCE;
            blend.color3 = BLEND_COLOR_ZERO;
        } else {                                    // ALPHA / PREMULT (approx): Cs * As + Cd * (1 - As)
            blend.color1 = BLEND_COLOR_SOURCE;
            blend.color2 = BLEND_COLOR_DEST;
            blend.alpha  = BLEND_ALPHA_SOURCE;
            blend.color3 = BLEND_COLOR_DEST;
        }
        if (isInFrame()) {
            qp = draw_alpha_blending(qp, 0, &blend);
        }
        //switch (rsMask) {
        //    case RS_BLEND_ALPHA   : GsEnableAlphaBlending1(GS_ENABLE);    break;
        //    case RS_BLEND_ADD     : sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xffffffff, 0xffffffff);        break;
        //    case RS_BLEND_MULT    : sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);                    break;
        //    case RS_BLEND_PREMULT : sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xffffffff, 0); break;
        //    default               : GsEnableAlphaBlending1(GS_DISABLE); return;
        //}
        //sceGuEnable(GU_BLEND);
    }

    void mat4toVuMatrix(VU_MATRIX *out, const mat4 &in) {
        out->m[0][0] = in.e00;
        out->m[0][1] = in.e10;
        out->m[0][2] = in.e20;
        out->m[0][3] = in.e30;
        out->m[1][0] = in.e01;
        out->m[1][1] = in.e11;
        out->m[1][2] = in.e21;
        out->m[1][3] = in.e31;
        out->m[2][0] = in.e02;
        out->m[2][1] = in.e12;
        out->m[2][2] = in.e22;
        out->m[2][3] = in.e32;
        out->m[3][0] = in.e03;
        out->m[3][1] = in.e13;
        out->m[3][2] = in.e23;
        out->m[3][3] = in.e33;
    }

    void setViewProj(const mat4 &mView, const mat4 &mProj) {
        // TODO: convert matix?
        GS_DEBUG("");
        mat4toVuMatrix(&GAPI::view, mView);
        mat4toVuMatrix(&GAPI::proj, mProj);
    }

    // ---- lighting ---------------------------------------------------------------------------
    // Same model as the engine shaders (compose.glsl _lighting):
    //   lv = (lightPos - P) * lightColor.w, att = lv.lv, I = max(0, N.normalize(lv)) * max(0, 1 - att)
    //   color = base * (ambient + sum(lightColor.rgb * I))
    // computed per vertex on VU0 for 4 lights at once (lights are transformed into model space per DIP).

    enum { LIGHT_NONE, LIGHT_ROOM, LIGHT_ENTITY, LIGHT_SPRITE, LIGHT_MIRROR };

    // MIRROR (save crystal): the shader uses vDiffuse = material.xyz (Crystal: 0.5, 0.5, 3.0) times a
    // reflected environment cube map; without the cube map use the material times an average reflection

    // Tone mapping (world geometry only): linear value v = (L / 128) * F, where L is the full vertex
    // color (PS1 scale, 128 = 1.0) and F the light factor. The GS vertex color is h = L / 2, so the
    // LUT index is simply h * F (one step = 1/64, covers v in [0, 4)).
    // Curve: x = v * exposure, out = 2x / (1 + x)  -> 1.0 at v = 1, lifted shadows, highlights roll off to 2.0
    u8  toneLUT[256];
    int toneLUTGamma = -1;

    void updateToneLUT() {
        int gamma = Core::settings.detail.gamma;
        if (gamma == toneLUTGamma) return;
        toneLUTGamma = gamma;
        float e = powf(2.0f, (float(gamma) - 5.0f) / 4.0f);
        for (int i = 0; i < 256; i++) {
            float x = float(i) / 64.0f * e;
            float out = 2.0f * x / (1.0f + x);
            toneLUT[i] = u8(clamp(out * 128.0f + 0.5f, 0.0f, 255.0f));
        }
        GS_LOG("tone LUT: gamma %d exposure %.2f", gamma, e);
    }

    int  lightMode = LIGHT_NONE;    // set from ShaderCache::bind (FFP has no shader objects)
    vec4 gsLightPos[4];
    vec4 gsLightColor[4];

    bool lightUnderwater = false;   // current draw is in a water room (FX_UNDERWATER)
    bool underwaterView  = false;   // camera is underwater (set per view by Level, drives underwaterRipple)

    // Shadow map: while shadowMapMode is on, DIP renders into the small shadowTex target (light view,
    // orthographic) as untextured black with the shadow alpha, no blending, no Z. Overlapping parts write
    // the same value, so nothing is darkened twice. The level then draws one textured quad on the floor.
    bool     shadowMapMode = false;
    u32      shadowAlpha   = 0;
    Texture *shadowTex     = NULL;
    int      targetW = 0, targetH = 0;  // DIP viewport (0 = backbuffer)

    void setUnderwaterView(bool enable) {
        underwaterView = enable;
    }


    void setLightingMode(int mode, bool underwater = false) {
        lightMode       = mode;
        lightUnderwater = underwater;
    }

    // Underwater caustics like compose.glsl VERT_CAUSTICS:
    //   light *= 0.5 + |sin(dot(worldPos, 1/1024) + time)| * 0.75
    // |sin| has period PI, the LUT covers one period in 256 steps
    float causticLUT[256];
    bool  causticLUTReady = false;

    struct CausticParams {
        float kx, ky, kz, c0;   // phase = coord . k + c0 (coord as stored in GAPI::Vertex, i.e. / 32767)
        float yx, yy, yz, y0;   // world y = coord . y + y0 (depth tint)
        float waterH;           // water surface height of the current room (Core::params.y)
    };

    // depth tint below the water surface + slightly cyan caustic bands, per channel factors

    inline void underwaterFactors(const CausticParams &cp, const VU_VECTOR &c, float cf, float &fr, float &fg, float &fb) {
        float wy = c.x * cp.yx + c.y * cp.yy + c.z * cp.yz + cp.y0;
        float t  = (wy - cp.waterH) * (1.0f / GS_WATER_DEPTH_RANGE);   // TR: y grows downwards
        t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
        float dark = 1.0f - 0.35f * t;
        fr = cf * 0.95f * (1.0f - 0.45f * t) * dark;
        fg = cf * 1.08f * (1.0f - 0.15f * t) * dark;
        fb = fg;
    }

    void setupCaustics(CausticParams &cp) {
        if (!causticLUTReady) {
            for (int i = 0; i < 256; i++) {
                causticLUT[i] = 0.5f + fabsf(sinf(float(i) / GS_CAUSTIC_LUT_SCALE)) * 0.75f;
            }
            causticLUTReady = true;
        }
        // world = mModel * (coord * 32767); dot(world, (1,1,1)) is linear in coord
        const mat4 &m = Core::mModel;
        vec4 w0 = m * vec4(0.0f, 0.0f, 0.0f, 1.0f);
        vec4 wx = m * vec4(1.0f, 0.0f, 0.0f, 1.0f);
        vec4 wy = m * vec4(0.0f, 1.0f, 0.0f, 1.0f);
        vec4 wz = m * vec4(0.0f, 0.0f, 1.0f, 1.0f);
        float s0 = w0.x + w0.y + w0.z;
        const float k = 32767.0f / 1024.0f * GS_CAUSTIC_LUT_SCALE;
        cp.kx = ((wx.x + wx.y + wx.z) - s0) * k;
        cp.ky = ((wy.x + wy.y + wy.z) - s0) * k;
        cp.kz = ((wz.x + wz.y + wz.z) - s0) * k;
        cp.c0 = (s0 / 1024.0f + Core::params.x) * GS_CAUSTIC_LUT_SCALE;
        cp.yx = (wx.y - w0.y) * 32767.0f;
        cp.yy = (wy.y - w0.y) * 32767.0f;
        cp.yz = (wz.y - w0.y) * 32767.0f;
        cp.y0 = w0.y;
        cp.waterH = Core::params.y;
    }

    inline float causticFactor(const CausticParams &cp, const VU_VECTOR &c) {
        float phase = c.x * cp.kx + c.y * cp.ky + c.z * cp.kz + cp.c0;   // already in LUT units
        return causticLUT[int(phase) & 255];
    }

    void updateLights(vec4 *lightPos, vec4 *lightColor, int count) {
        for (int i = 0; i < 4; i++) {
            if (i < count) {
                gsLightPos[i]   = lightPos[i];
                gsLightColor[i] = lightColor[i];
            } else {
                gsLightPos[i]   = vec4(0.0f);
                gsLightColor[i] = vec4(0.0f, 0.0f, 0.0f, 1.0f);
            }
        }
    }

    // VU0 registers (the transform kernel uses vf16..vf27, so they never collide):
    // vf3 ONES, vf4 LX, vf5 LY, vf6 LZ, vf7 R, vf8..vf11 C0..C3, vf12 AMB
    struct LightConsts {
        VU_VECTOR ones, lx, ly, lz, r, c[4], amb;
    } __attribute__((aligned(16)));

    inline void vu0LoadLightState(const LightConsts *lc) {
        asm __volatile__ (
            "lqc2           $vf3,  0x00(%0)     \n"
            "lqc2           $vf4,  0x10(%0)     \n"
            "lqc2           $vf5,  0x20(%0)     \n"
            "lqc2           $vf6,  0x30(%0)     \n"
            "lqc2           $vf7,  0x40(%0)     \n"
            "lqc2           $vf8,  0x50(%0)     \n"
            "lqc2           $vf9,  0x60(%0)     \n"
            "lqc2           $vf10, 0x70(%0)     \n"
            "lqc2           $vf11, 0x80(%0)     \n"
            "lqc2           $vf12, 0x90(%0)     \n"
            : : "r" (lc) : "memory"
        );
    }

    // per vertex: out.rgb = AMB + sum(C_i * I_i) for 4 lights in parallel (SIMD over lights)
    inline void vu0Light4(const VU_VECTOR *pos, const VU_VECTOR *normal, VU_VECTOR *out) {
        asm __volatile__ (
            "lqc2           $vf1,  0x00(%0)         \n" // P (model space / 32767)
            "lqc2           $vf2,  0x00(%1)         \n" // N
            "vmulx.xyzw     $vf13, $vf7,  $vf1      \n" // Px * R
            "vmuly.xyzw     $vf14, $vf7,  $vf1      \n"
            "vmulz.xyzw     $vf15, $vf7,  $vf1      \n"
            "vsub.xyzw      $vf13, $vf4,  $vf13     \n" // DX = LX - Px * R   (per light)
            "vsub.xyzw      $vf14, $vf5,  $vf14     \n" // DY
            "vsub.xyzw      $vf15, $vf6,  $vf15     \n" // DZ
            "vmula.xyzw     $ACC,  $vf13, $vf13     \n"
            "vmadda.xyzw    $ACC,  $vf14, $vf14     \n"
            "vmadd.xyzw     $vf28, $vf15, $vf15     \n" // att = DX^2 + DY^2 + DZ^2
            "vmulax.xyzw    $ACC,  $vf13, $vf2      \n"
            "vmadday.xyzw   $ACC,  $vf14, $vf2      \n"
            "vmaddz.xyzw    $vf29, $vf15, $vf2      \n" // dot = N . D
            "vrsqrt         $Q,    $vf0w, $vf28x    \n"
            "vsub.xyzw      $vf31, $vf3,  $vf28     \n" // 1 - att (while rsqrt runs)
            "vwaitq                                 \n"
            "vmulq.x        $vf30, $vf29, $Q        \n" // lum = dot / |D|
            "vrsqrt         $Q,    $vf0w, $vf28y    \n"
            "vmaxx.xyzw     $vf31, $vf31, $vf0      \n" // max(1 - att, 0)
            "vwaitq                                 \n"
            "vmulq.y        $vf30, $vf29, $Q        \n"
            "vrsqrt         $Q,    $vf0w, $vf28z    \n"
            "vwaitq                                 \n"
            "vmulq.z        $vf30, $vf29, $Q        \n"
            "vrsqrt         $Q,    $vf0w, $vf28w    \n"
            "vwaitq                                 \n"
            "vmulq.w        $vf30, $vf29, $Q        \n"
            "vmaxx.xyzw     $vf30, $vf30, $vf0      \n" // max(lum, 0)
            "vmul.xyzw      $vf30, $vf30, $vf31     \n" // I (per light)
            "vmulax.xyz     $ACC,  $vf8,  $vf30     \n"
            "vmadday.xyz    $ACC,  $vf9,  $vf30     \n"
            "vmaddaz.xyz    $ACC,  $vf10, $vf30     \n"
            "vmaddw.xyz     $vf13, $vf11, $vf30     \n" // sum(C_i * I_i)
            "vadd.xyz       $vf13, $vf13, $vf12     \n" // + ambient
            "sqc2           $vf13, 0x00(%2)         \n"
            : : "r" (pos), "r" (normal), "r" (out)
            : "memory"
        );
    }

    // returns true if this draw needs lighting; fills the VU0 constants and the ambient-only factor
    bool setupLighting(LightConsts &lc) {
        if (lightMode == LIGHT_NONE) {
            return false;
        }

        int first = (lightMode == LIGHT_ROOM) ? 1 : 0;   // rooms: main light is baked into the vertex color
        int active = 0;
        for (int i = first; i < 4; i++) {
            if (gsLightColor[i].w < 1.0f) active++;
        }

        if (lightMode == LIGHT_ROOM && !active) {
            return false;   // baked room lighting only: vertex color as is
        }

        float amb = Core::active.material.y;
        lc.ones = (VU_VECTOR){ 1.0f, 1.0f, 1.0f, 1.0f };
        lc.amb  = (VU_VECTOR){ amb, amb, amb, 0.0f };

        mat4 mInv = Core::mModel.inverseOrtho();   // FFP models: rigid joint matrix (rot + pos)

        float *lx = &lc.lx.x, *ly = &lc.ly.x, *lz = &lc.lz.x, *r = &lc.r.x;
        for (int i = 0; i < 4; i++) {
            VU_VECTOR &c = lc.c[i];
            if (i < first || gsLightColor[i].w >= 1.0f) {   // disabled: no color, far away (att >> 1)
                lx[i] = ly[i] = lz[i] = 1000.0f;
                r[i]  = 0.0f;
                c = (VU_VECTOR){ 0.0f, 0.0f, 0.0f, 0.0f };
                continue;
            }
            float invR = gsLightColor[i].w;
            vec4 lm = mInv * vec4(gsLightPos[i].xyz(), 1.0f);
            lx[i] = lm.x * invR;
            ly[i] = lm.y * invR;
            lz[i] = lm.z * invR;
            r[i]  = invR * 32767.0f;    // vertex coords are stored / 32767
            c = (VU_VECTOR){ gsLightColor[i].x, gsLightColor[i].y, gsLightColor[i].z, 0.0f };
        }
        return true;
    }

// ---- VU0 transform + clipping ---------------------------------------------------------------
//
// Every vertex of a range is transformed once on VU0 (macro mode, matrix kept in vf16-vf19).
// VU0 also computes the clip flags and the final GS screen coordinates, so the common case
// (triangle fully inside the guard band) is just three 64-bit copies per vertex into the packet.
// Triangles crossing the near plane or the guard band are clipped on the EE (Sutherland-Hodgman
// in homogeneous clip space) and emitted as a fan.

    struct ClipVertex {     // 32 bytes, written by VU0
        VU_VECTOR pos;      // clip space position
        s32 sx, sy, sz;     // GS screen coords: x/y 12.4 fixed point, z 16-bit (only valid when w > 0)
        float q;            // 1/w
    } __attribute__((aligned(16)));

    ClipVertex *cvBuf  = NULL;
    u8         *cvFlag = NULL;
    u32        *cvColor = NULL;     // final RGBA per span vertex (lit or the base vertex color)
    u64        *cvPacked = NULL;    // RGBAQ, ST, XYZ2 GIF words per span vertex (fast path: copy per index)
    int         cvCap  = 0;

    // VCLIP flag bits (most recent judgement), tested vector is (x/gx, y/gy, 2z - w) against |w|
    enum {
        CLIP_PX   = 1 << 0,     // x >  gx * w
        CLIP_NX   = 1 << 1,     // x < -gx * w
        CLIP_PY   = 1 << 2,
        CLIP_NY   = 1 << 3,
        CLIP_FAR  = 1 << 4,     // z > w (beyond far plane: only used for trivial reject, Z is clamped)
        CLIP_NEAR = 1 << 5,     // z < 0 (in front of near plane / behind the camera)
        CLIP_MASK = CLIP_PX | CLIP_NX | CLIP_PY | CLIP_NY | CLIP_NEAR,
    };

    // Fog: GS blends per vertex with F from XYZF2 (255 = no fog, 0 = fog color in FOGCOL).
    // Engine shaders use factor = exp(-dist * fogParams.w); we use the view depth (clip w) as dist
    // and a LUT instead of expf per vertex. F is packed into the top byte of the Z word (XYZF2 Z is 24 bit).
    u8    fogLUT[256];
    float fogLUTDensity = -1.0f;

    #define GS_FOG_REGLIST \
        (((u64)GIF_REG_RGBAQ) << 0 | ((u64)GIF_REG_ST) << 4 | ((u64)GIF_REG_XYZF2) << 8)

    void updateFogLUT(float density) {
        if (density == fogLUTDensity) return;
        fogLUTDensity = density;
        for (int i = 0; i < 256; i++) {
            fogLUT[i] = u8(clamp(expf(-float(i) / GS_FOG_LUT_SCALE) * 255.0f + 0.5f, 0.0f, 255.0f));
        }
        fogLUT[255] = 0;
    }

    inline u32 fogValue(float w) {    // returns F already shifted into the Z word (bits 24..31)
        float d = w * fog_color.w * GS_FOG_LUT_SCALE;
        int i = (d <= 0.0f) ? 0 : ((d >= 255.0f) ? 255 : int(d));
        return u32(fogLUT[i]) << 24;
    }

    struct ScreenParams {
        float hw, hh;           // half viewport size
        float ox, oy;           // hw + origin - 0.5 (GS samples at pixel corners, GL at centers)
        float gx, gy;           // guard band in NDC units
    };

    // vf16-19: MVP, vf21: clip test scale (1/gx, 1/gy, 2, 0)
    // vf25: screen scale (hw, -hh, -GS_Z_MAX, 0), vf26: screen offset (ox, oy, GS_Z_MAX, GS_Z_MAX)
    inline void vu0LoadClipState(const VU_MATRIX *m, const VU_VECTOR *clipScale, const VU_VECTOR *scrScale, const VU_VECTOR *scrOffset) {
        asm __volatile__ (
            "lqc2           $vf16, 0x00(%0)     \n"
            "lqc2           $vf17, 0x10(%0)     \n"
            "lqc2           $vf18, 0x20(%0)     \n"
            "lqc2           $vf19, 0x30(%0)     \n"
            "lqc2           $vf21, 0x00(%1)     \n"
            "lqc2           $vf25, 0x00(%2)     \n"
            "lqc2           $vf26, 0x00(%3)     \n"
            : : "r" (m), "r" (clipScale), "r" (scrScale), "r" (scrOffset)
            : "memory"
        );
    }

    // transforms one vertex, writes clip position + screen coords, returns 6 clip flag bits
    inline u32 vu0TransformClip(const VU_VECTOR *in, ClipVertex *out) {
        u32 flags;
        asm __volatile__ (
            "lqc2           $vf20, 0x00(%1)         \n"
            "vmulax.xyzw    $ACC,  $vf16, $vf20     \n"
            "vmadday.xyzw   $ACC,  $vf17, $vf20     \n"
            "vmaddaz.xyzw   $ACC,  $vf18, $vf20     \n"
            "vmaddw.xyzw    $vf20, $vf19, $vf20     \n" // clip = MVP * v
            "vmul.xyz       $vf22, $vf20, $vf21     \n" // (x/gx, y/gy, 2z)
            "vdiv           $Q,    $vf0w, $vf20w    \n" // Q = 1/w (runs in parallel)
            "vsubw.z        $vf22, $vf22, $vf20     \n" // 2z - w: maps z [0, w] to [-w, w]
            "vclipw.xyz     $vf22, $vf20            \n" // guard band x/y, near/far z
            "sqc2           $vf20, 0x00(%2)         \n"
            "vwaitq                                 \n"
            "vmulq.xyz      $vf23, $vf20, $Q        \n" // NDC
            "vmul.xyz       $vf24, $vf23, $vf25     \n"
            "vadd.xyz       $vf24, $vf24, $vf26     \n" // screen x, y in pixels, z = (1 - ndc.z) * GS_Z_MAX
            "vmaxx.z        $vf24, $vf24, $vf0      \n" // z >= 0
            "vminiw.z       $vf24, $vf24, $vf26     \n" // z <= GS_Z_MAX
            "vftoi4.xy      $vf27, $vf24            \n" // x, y -> 12.4 fixed point
            "vftoi0.z       $vf27, $vf24            \n" // z -> integer
            "vmulq.w        $vf27, $vf0,  $Q        \n" // q = 1/w
            "sqc2           $vf27, 0x10(%2)         \n"
            "vnop                                   \n"
            "cfc2           %0,    $18              \n" // ClipFlag, 6 newest bits = this vertex
            : "=r" (flags)
            : "r" (in), "r" (out)
            : "memory"
        );
        return flags & 0x3F;
    }

    inline u64 *emitVertex(u64 *dp, u32 rgba, float q, float s, float t, s32 x, s32 y, s32 z) {
        union { float f; u32 i; } fq, fs, ft;
        fq.f = q;
        fs.f = s;
        ft.f = t;
        dp[0] = u64(rgba) | (u64(fq.i) << 32);                                     // RGBAQ
        dp[1] = u64(fs.i) | (u64(ft.i) << 32);                                     // ST
        dp[2] = u64(u32(x) & 0xFFFF) | (u64(u32(y) & 0xFFFF) << 16) | (u64(u32(z)) << 32); // XYZ2
        return dp + 3;
    }

    struct ClipPoly {
        float x, y, z, w;   // clip space
        float s, t;         // texcoords (already scaled by su/sv)
        float r, g, b, a;   // GS color (0x80 = 1.0)
    };

    inline float clipDistance(const ClipPoly &p, int plane, const ScreenParams &sp) {
        switch (plane) {
            case 5  : return p.z;                   // near: z >= 0
            case 0  : return sp.gx * p.w - p.x;     // guard band +x
            case 1  : return sp.gx * p.w + p.x;     // guard band -x
            case 2  : return sp.gy * p.w - p.y;
            default : return sp.gy * p.w + p.y;
        }
    }

    // clips a triangle against the planes in mask and emits it as a triangle fan, returns new dp
    u64 *clipTriangle(u64 *dp, const GAPI::Vertex *vBase, const ClipVertex *cv, const u32 *colors, const u32 *idx, u32 mask, float su, float sv, const ScreenParams &sp) {
        ClipPoly bufA[12], bufB[12];
        ClipPoly *in = bufA, *out = bufB;
        int n = 3;

        for (int k = 0; k < 3; k++) {
            const VU_VECTOR    &p = cv[idx[k]].pos;
            const GAPI::Vertex &v = vBase[idx[k]];
            ClipPoly &d = in[k];
            d.x = p.x; d.y = p.y; d.z = p.z; d.w = p.w;
            d.s = v.texCoord.u * su;
            d.t = v.texCoord.v * sv;
            u32 c = colors[idx[k]];
            d.r = float(c & 0xFF);
            d.g = float((c >> 8) & 0xFF);
            d.b = float((c >> 16) & 0xFF);
            d.a = float(c >> 24);
        }

        // vertices created on the near plane have tiny w, so x/w, y/w can land anywhere:
        // after near clipping always clip against the whole guard band too
        // (x/y clip flags of vertices with w < 0 are meaningless anyway)
        if (mask & CLIP_NEAR) {
            mask |= CLIP_PX | CLIP_NX | CLIP_PY | CLIP_NY;
        }

        static const int planes[5] = { 5, 0, 1, 2, 3 }; // near first: everything after it has w > 0
        for (int pi = 0; pi < 5 && n >= 3; pi++) {
            int plane = planes[pi];
            if (!(mask & (1 << plane))) continue;

            int m = 0;
            ClipPoly *a = &in[n - 1];
            float da = clipDistance(*a, plane, sp);
            for (int k = 0; k < n; k++) {
                ClipPoly *b = &in[k];
                float db = clipDistance(*b, plane, sp);
                if (da >= 0.0f) {
                    out[m++] = *a;
                }
                if ((da >= 0.0f) != (db >= 0.0f)) {   // edge crosses the plane
                    float t = da / (da - db);
                    ClipPoly &o = out[m++];
                    o.x = a->x + (b->x - a->x) * t;
                    o.y = a->y + (b->y - a->y) * t;
                    o.z = a->z + (b->z - a->z) * t;
                    o.w = a->w + (b->w - a->w) * t;
                    o.s = a->s + (b->s - a->s) * t;
                    o.t = a->t + (b->t - a->t) * t;
                    o.r = a->r + (b->r - a->r) * t;
                    o.g = a->g + (b->g - a->g) * t;
                    o.b = a->b + (b->b - a->b) * t;
                    o.a = a->a + (b->a - a->a) * t;
                }
                a  = b;
                da = db;
            }
            n = m;
            ClipPoly *tmp = in; in = out; out = tmp;
        }

        if (n < 3) {
            return dp;
        }

        // project (same math as the VU0 path)
        s32 sx[12], sy[12], sz[12];
        float q[12];
        for (int k = 0; k < n; k++) {
            const ClipPoly &p = in[k];
            q[k] = 1.0f / p.w;
            float z = (1.0f - p.z * q[k]) * GS_Z_MAX;
            z = (z < 0.0f) ? 0.0f : ((z > GS_Z_MAX) ? GS_Z_MAX : z);
            float fx = ( p.x * q[k] * sp.hw + sp.ox) * 16.0f;
            float fy = (-p.y * q[k] * sp.hh + sp.oy) * 16.0f;
            // safety net: never let a coordinate wrap in the 16-bit GS fields
            fx = (fx < 0.0f) ? 0.0f : ((fx > 65535.0f) ? 65535.0f : fx);
            fy = (fy < 0.0f) ? 0.0f : ((fy > 65535.0f) ? 65535.0f : fy);
            sx[k] = s32(fx);
            sy[k] = s32(fy);
            sz[k] = s32(z);
            if (fog) {
                sz[k] |= fogValue(p.w);
            }
        }

        // culling: the clipped polygon keeps the winding of the source triangle
        if (cullMode & (RS_CULL_BACK | RS_CULL_FRONT)) {
            float area = 0.0f;
            for (int k = 0, j = n - 1; k < n; j = k++) {
                area += float(sx[j]) * float(sy[k]) - float(sx[k]) * float(sy[j]);
            }
            if ((cullMode & RS_CULL_BACK)  && area > 0.0f) return dp;
            if ((cullMode & RS_CULL_FRONT) && area < 0.0f) return dp;
        }

        u32 rgba[12];
        for (int k = 0; k < n; k++) {
            const ClipPoly &p = in[k];
            rgba[k] = u32(p.r + 0.5f) | (u32(p.g + 0.5f) << 8) | (u32(p.b + 0.5f) << 16) | (u32(p.a + 0.5f) << 24);
        }

        // triangle fan (0, k, k + 1)
        for (int k = 1; k < n - 1; k++) {
            const int f[3] = { 0, k, k + 1 };
            for (int j = 0; j < 3; j++) {
                int i = f[j];
                dp = emitVertex(dp, rgba[i], q[i], in[i].s * q[i], in[i].t * q[i], sx[i], sy[i], sz[i]);
            }
        }
        return dp;
    }

    void initShadowMap() {
        if (shadowTex) return;
        shadowTex = new Texture(GS_SHADOW_MAP_SIZE, GS_SHADOW_MAP_SIZE, 1, 0);
        shadowTex->fmt = FMT_RGBA;
        shadowTex->init(NULL);
        shadowTex->alphaRGBA = true;    // the silhouette is in the alpha channel
    }

    qword_t *emitShadowMapState(qword_t *q) {
        atest_t at = atest;      at.enable = DRAW_DISABLE;
        ztest_t zt = ztest;      zt.enable = DRAW_ENABLE; zt.method = ZTEST_METHOD_ALLPASS;
        zbuffer_t zb = *zbuffer; zb.mask = 1;   // the 640x448 Z buffer does not match the target: no Z
        q = draw_pixel_test(q, 0, &at, &dtest, &zt);
        return draw_zbuffer(q, 0, &zb);
    }

    // starts rendering a silhouette into shadowTex (the caller sets the light view / projection)
    bool shadowMapBegin(float darkness) {
        if (!isInFrame() || !shadowTex || !shadowTex->memory) return false;
        shadowMapMode = true;
        shadowAlpha   = u32(clamp(darkness * 128.0f, 0.0f, 128.0f));
        targetW = targetH = GS_SHADOW_MAP_SIZE;

        framebuffer_t fb;
        fb.address = shadowTex->memory;
        fb.width   = GS_SHADOW_MAP_SIZE;
        fb.height  = GS_SHADOW_MAP_SIZE;
        fb.psm     = GS_PSM_32;
        fb.mask    = 0;
        qp = draw_framebuffer(qp, 0, &fb);
        qp = draw_scissor_area(qp, 0, 0, GS_SHADOW_MAP_SIZE - 1, 0, GS_SHADOW_MAP_SIZE - 1);
        qp = emitShadowMapState(qp);

        // clear to transparent (RGBA 0): untextured sprite, no blending
        prim_t prim = {};
        prim.type = PRIM_SPRITE;
        prim.shading = PRIM_SHADE_FLAT;
        prim.mapping = DRAW_DISABLE;
        prim.fogging = DRAW_DISABLE;
        prim.blending = DRAW_DISABLE;
        prim.antialiasing = DRAW_DISABLE;
        prim.mapping_type = PRIM_MAP_ST;
        prim.colorfix = PRIM_UNFIXED;
        color_t c = {};
        c.q = 1.0f;
        qp = draw_prim_start(qp, 0, &prim, &c);
        u64 *dp = (u64*)qp;
        const int ox = int(origin_x * 16.0f), oy = int(origin_y * 16.0f), sz = GS_SHADOW_MAP_SIZE * 16;
        dp[0] = u64(u32(ox) & 0xFFFF) | (u64(u32(oy) & 0xFFFF) << 16);
        dp[1] = u64(u32(ox + sz) & 0xFFFF) | (u64(u32(oy + sz) & 0xFFFF) << 16);
        qp = draw_prim_end((qword_t*)(dp + 2), 2, DRAW_XYZ_REGLIST);
        return true;
    }

    void shadowMapEnd() {
        if (!shadowMapMode) return;
        shadowMapMode = false;
        targetW = targetH = 0;
        qp = draw_framebuffer(qp, 0, backbuffer);
        qp = emitScissor(qp);
        applyDepthState();
        qp = draw_alpha_blending(qp, 0, &blend);
        qp = gsTexFlush(qp);    // the target is sampled next
        gsTexture = NULL;
    }

    void DIP(Mesh *mesh, const MeshRange &range) {
        GSProfScope profScope(prof.dip);
        // prepare model matrix, copied from PSP
        GS_DEBUG("----8<--- BEGIN ----8<---");

        // frame packet guard: nothing may be written past the global packet (it is followed by other globals + heap)
        {
            u32 used = u32(qp - qs);
            if (!isInFrame() || used > qtotal - GS_PACKET_RESERVE) {
                static int logged = 0;
                if (logged < 8) {
                    GS_LOG("WARN: DIP skipped: inFrame %d used %u qwords, iCount %d vStart %d dips %u",
                           isInFrame(), used, range.iCount, range.vStart, Core::stats.dips);
                    logged++;
                }
                return;
            }
        }

        if (shadowMapMode) {    // engine state setters may have run in between: re-emit the map state
            qp = emitShadowMapState(qp);
        }

        VU_MATRIX model;
        mat4toVuMatrix(&model, mModel);

        const float k = 32767.0f;
        VU_MATRIX scale = {0};
        scale.m[0][0] = k;
        scale.m[1][1] = k;
        scale.m[2][2] = k;
        scale.m[3][3] = 1.0f;
        Vu0MulMatrix(&scale, &model, &model);

        // create MVP matrix
        VU_MATRIX mvp;
        Vu0MulMatrix(&model, &GAPI::view, &mvp);
        Vu0MulMatrix(&mvp, &GAPI::proj, &mvp);

        if (fog) {
            int r = clamp(fog_color.x * 255.0f, 0.0f, 255.0f);
            int g = clamp(fog_color.y * 255.0f, 0.0f, 255.0f);
            int b = clamp(fog_color.z * 255.0f, 0.0f, 255.0f);
            qp = draw_fog_color(qp, r, g, b);
            updateFogLUT(fog_color.w);
        }

        float su = 1.0f;
        float sv = 1.0f;
        bool textured = diffuseTex != NULL && !shadowMapMode;
        if (textured) {
            Texture *t = diffuseTex;
            int tw = draw_log2(t->width);
            int th = draw_log2(t->height);
            su = float(t->width)  / float(1 << tw);   // every draw, not only on TEX0 change
            sv = float(t->height) / float(1 << th);
            if (gsTexture != t) {
                texbuffer_t tb;
                clutbuffer_t clut = {};
                tb.info.width    = tw;
                tb.info.height   = th;
                tb.info.function = TEXTURE_FUNCTION_MODULATE;

                if (t->tileAddr) {
                    // 4-bit tile + CLUT from the CSM2 strip (16 CLUTs per row)
                    qp = draw_clut_offset(qp, 256 >> 6, t->curClut & 15, t->curClut >> 4);
                    tb.address = t->tileAddr[t->curTile];
                    tb.width   = 256;
                    tb.psm     = t->tilePsm[t->curTile];
                    tb.info.components = TEXTURE_COMPONENTS_RGBA;   // alpha from CLUT STP bit via TEXA
                    clut.address      = t->clutsMem;
                    clut.psm          = GS_PSM_16;
                    clut.storage_mode = CLUT_STORAGE_MODE2;
                    clut.start        = 0;
                    clut.load_method  = CLUT_LOAD;
                } else {
                    tb.address = t->memory;
                    tb.width   = t->tbw;
                    tb.psm     = t->psm;
                    tb.info.components = t->alphaRGBA ? TEXTURE_COMPONENTS_RGBA : TEXTURE_COMPONENTS_RGB;
                    clut.load_method   = CLUT_NO_LOAD;
                }
                qp = draw_texturebuffer(qp, 0, &tb, &clut);

                lod_t lod = {};
                lod.calculation = LOD_USE_K;
                lod.mag_filter = (t->opt & OPT_NEAREST) ? LOD_MAG_NEAREST : LOD_MAG_LINEAR;
                lod.min_filter = (t->opt & OPT_NEAREST) ? LOD_MIN_NEAREST : LOD_MIN_LINEAR;
                qp = draw_texture_sampling(qp, 0, &lod);

                // NPOT textures live inside a 2^TW x 2^TH space: clamp to the real size so bilinear
                // filtering at the edges does not pull in never-uploaded texels from the far side
                texwrap_t wrap;
                if (t->tileAddr || (t->opt & OPT_REPEAT)) {
                    wrap.horizontal = wrap.vertical = WRAP_REPEAT;
                    wrap.minu = wrap.maxu = wrap.minv = wrap.maxv = 0;
                } else {
                    wrap.horizontal = wrap.vertical = WRAP_REGION_CLAMP;
                    wrap.minu = 0;
                    wrap.maxu = t->width - 1;
                    wrap.minv = 0;
                    wrap.maxv = t->height - 1;
                }
                qp = draw_texture_wrapping(qp, 0, &wrap);
                gsTexture = t;
            }
        }

        prim_t prim;
        prim.type = PRIM_TRIANGLE;
	    prim.shading = PRIM_SHADE_GOURAUD;
        prim.fogging = (fog && !shadowMapMode) ? DRAW_ENABLE : DRAW_DISABLE;
	    prim.blending = (blendMode && !shadowMapMode) ? DRAW_ENABLE : DRAW_DISABLE;
	    prim.antialiasing = DRAW_ENABLE;   // AA1 blends edges by coverage, causes fringes
	    prim.colorfix = PRIM_UNFIXED;
        prim.mapping = textured ? DRAW_ENABLE : DRAW_DISABLE;
	    prim.mapping_type = PRIM_MAP_ST; // perspective-correct: S,T are divided by Q on the GS

        if (range.iCount < 3) {
            return;
        }

        Index *iB = mesh->iBuffer + range.iStart;
        GAPI::Vertex *vB = mesh->vBuffer + range.vStart;

        // vertex span referenced by this range: transform every vertex once, not once per index
        u32 iMin = 0xFFFFFFFF, iMax = 0;
        for (int i = 0; i < range.iCount; i++) {
            u32 idx = iB[i];
            if (idx < iMin) iMin = idx;
            if (idx > iMax) iMax = idx;
        }
        int spanCount = int(iMax - iMin) + 1;

        if (cvCap < spanCount) {
            free(cvBuf);
            free(cvFlag);
            free(cvColor);
            free(cvPacked);
            cvCap   = spanCount;
            cvBuf   = (ClipVertex*)memalign(64, cvCap * sizeof(ClipVertex));
            cvFlag  = (u8*)malloc(cvCap);
            cvColor = (u32*)malloc(cvCap * sizeof(u32));
            cvPacked = (u64*)memalign(64, cvCap * 3 * sizeof(u64));
            if (!cvBuf || !cvFlag || !cvColor || !cvPacked) {
                while (1) { osBreak(); }
            }
            GS_DEBUG("-> clip buffer grow to: %d", cvCap);
        }

        ScreenParams sp;
        sp.hw = float((targetW ? targetW : int(backbuffer->width))  >> 1);
        sp.hh = float((targetH ? targetH : int(backbuffer->height)) >> 1);
        sp.ox = sp.hw + origin_x - 0.5f;
        sp.oy = sp.hh + origin_y - 0.5f;
        sp.gx = 2000.0f / sp.hw;    // guard band: +-2000 px around the screen center (GS range is 0..4095)
        sp.gy = 2000.0f / sp.hh;

        VU_VECTOR clipScale = { 1.0f / sp.gx, 1.0f / sp.gy, 2.0f, 0.0f };
        VU_VECTOR scrScale  = { sp.hw, -sp.hh, -GS_Z_MAX, 0.0f };
        VU_VECTOR scrOffset = { sp.ox, sp.oy, GS_Z_MAX, GS_Z_MAX };

        // VU0: transform + clip flags + screen projection for the whole span
        const GAPI::Vertex *vBase = vB + iMin;
        vu0LoadClipState(&mvp, &clipScale, &scrScale, &scrOffset);
        for (int i = 0; i < spanCount; i++) {
            u32 f = vu0TransformClip(&vBase[i].coord, cvBuf + i);
            if (fog) {
                cvBuf[i].sz |= fogValue(cvBuf[i].pos.w);    // w <= 0 maps to F = 255, only used after clipping anyway
            }
            if (cvBuf[i].pos.w <= 0.0f) {
                // behind the eye: x/y flags were tested against |w| and are meaningless here,
                // keep only near (never fast path, never wrongly trivially rejected by x/y)
                f = CLIP_NEAR;
            }
            cvFlag[i] = f;
        }

        u32 tColor0 = gsCycles();

        // vertex colors: per vertex lighting on VU0 (entities, rooms lit by dynamic lights)
        // + tone mapping for all world geometry (entities, rooms, level sprites)
        const bool toneMap = (lightMode != LIGHT_NONE && lightMode != LIGHT_MIRROR);
        if (toneMap) {
            updateToneLUT();
        }
        // animated caustics for rooms and models in water rooms
        const bool caustics = toneMap && lightUnderwater && (lightMode == LIGHT_ROOM || lightMode == LIGHT_ENTITY);
        CausticParams cp;
        if (caustics) {
            setupCaustics(cp);
        }

        LightConsts lc;
        if (toneMap && setupLighting(lc)) {
            bool anyLight = (lc.c[0].x + lc.c[0].y + lc.c[0].z + lc.c[1].x + lc.c[1].y + lc.c[1].z +
                             lc.c[2].x + lc.c[2].y + lc.c[2].z + lc.c[3].x + lc.c[3].y + lc.c[3].z) > 0.0f;
            VU_VECTOR factor __attribute__((aligned(16))) = lc.amb;   // ambient only if no light is active
            if (anyLight) {
                vu0LoadLightState(&lc);
            }
            for (int i = 0; i < spanCount; i++) {
                const GAPI::Vertex &v = vBase[i];
                if (anyLight) {
                    vu0Light4(&v.coord, &v.normal, &factor);
                }
                // LUT index = h * F (h = halved base color), see updateToneLUT
                float fr = 1.0f, fg = 1.0f, fb = 1.0f;
                if (caustics) {
                    underwaterFactors(cp, v.coord, causticFactor(cp, v.coord), fr, fg, fb);
                }
                int r = int(float(v.color.r) * factor.x * fr);
                int g = int(float(v.color.g) * factor.y * fg);
                int b = int(float(v.color.b) * factor.z * fb);
                r = (r < 0) ? 0 : ((r > 255) ? 255 : r);
                g = (g < 0) ? 0 : ((g > 255) ? 255 : g);
                b = (b < 0) ? 0 : ((b > 255) ? 255 : b);
                cvColor[i] = u32(toneLUT[r]) | (u32(toneLUT[g]) << 8) | (u32(toneLUT[b]) << 16) | (u32(v.color.a) << 24);
            }
        } else if (toneMap) {   // unlit world geometry (baked room light, sprites): F = 1
            if (caustics) {
                for (int i = 0; i < spanCount; i++) {
                    const color_t &c = vBase[i].color;
                    float fr, fg, fb;
                    underwaterFactors(cp, vBase[i].coord, causticFactor(cp, vBase[i].coord), fr, fg, fb);
                    int r = min(255, int(float(c.r) * fr));
                    int g = min(255, int(float(c.g) * fg));
                    int b = min(255, int(float(c.b) * fb));
                    cvColor[i] = u32(toneLUT[r]) | (u32(toneLUT[g]) << 8) | (u32(toneLUT[b]) << 16) | (u32(c.a) << 24);
                }
            } else {
                for (int i = 0; i < spanCount; i++) {
                    const color_t &c = vBase[i].color;
                    cvColor[i] = u32(toneLUT[c.r]) | (u32(toneLUT[c.g]) << 8) | (u32(toneLUT[c.b]) << 16) | (u32(c.a) << 24);
                }
            }
        } else {
            for (int i = 0; i < spanCount; i++) {
                cvColor[i] = u32(vBase[i].color.rgbaq);
            }
        }

        // multiply blend (blob shadows): the GS can not do Cd * Cs, setBlendMode uses Cd * As instead,
        // so put the multiplier into alpha: shader result = mix(material.x, 1, vertex brightness)
        if (blendMode & RS_BLEND_MULT) {
            float lum = Core::active.material.x;
            for (int i = 0; i < spanCount; i++) {
                float c = float(cvColor[i] & 0xFF) * (1.0f / 128.0f);   // halved: 0x80 = 1.0
                c = (c > 1.0f) ? 1.0f : c;
                u32 a = u32((lum + c * (1.0f - lum)) * 128.0f);
                cvColor[i] = (cvColor[i] & 0x00FFFFFF) | (min(a, 128u) << 24);
            }
        }

        if (lightMode == LIGHT_MIRROR) {
            // fake reflection per facet: brightness from the facet normal against a fixed light (in world
            // space, the crystal rotates), white highlight on the facets facing it; material gives the color
            const vec4 &m = Core::active.material;
            static const vec3 L = vec3(0.4f, -0.8f, -0.45f).normal();   // TR y down: from above / front
            const mat4 &mm = Core::mModel;
            for (int i = 0; i < spanCount; i++) {
                const GAPI::Vertex &v = vBase[i];
                vec3 n = (mm * vec4(v.normal.x, v.normal.y, v.normal.z, 0.0f)).xyz();
                float len = n.length();
                float d = (len > 0.0f) ? max(0.0f, n.dot(L) / len) : 0.5f;
                float env  = GS_MIRROR_ENV * (0.35f + 1.3f * d);              // dark facets .. lit facets
                float spec = d * d; spec *= spec; spec *= spec;                // ^8 highlight
                spec *= 160.0f;
                const color_t &c = v.color;
                u32 r = min(255u, u32(float(c.r) * m.x * env + spec));
                u32 g = min(255u, u32(float(c.g) * m.y * env + spec));
                u32 b = min(255u, u32(float(c.b) * m.z * env + spec));
                cvColor[i] = r | (g << 8) | (b << 16) | (u32(c.a) << 24);
            }
        }

        if (shadowMapMode) {
            for (int i = 0; i < spanCount; i++) {
                cvColor[i] = shadowAlpha << 24;     // black, alpha = shadow darkness
            }
        }

        u32 tEmit0 = gsCycles();
        prof.dipColor += u32(tEmit0 - tColor0);
        GSProfScope profEmit(prof.dipEmit);

        // pack the GIF words once per vertex (a vertex is shared by ~2 triangles); only meaningful for
        // vertices in front of the eye, the clipper builds its own from cvBuf / cvColor
        {
            u64 *pk = cvPacked;
            for (int i = 0; i < spanCount; i++, pk += 3) {
                const ClipVertex   &c = cvBuf[i];
                const GAPI::Vertex &v = vBase[i];
                union { float f; u32 i; } fq, fs, ft;
                fq.f = c.q;
                fs.f = v.texCoord.u * su * c.q;
                ft.f = v.texCoord.v * sv * c.q;
                pk[0] = u64(cvColor[i]) | (u64(fq.i) << 32);
                pk[1] = u64(fs.i) | (u64(ft.i) << 32);
                pk[2] = u64(u32(c.sx) & 0xFFFF) | (u64(u32(c.sy) & 0xFFFF) << 16) | (u64(u32(c.sz)) << 32);
            }
        }

        color_t startColor = vBase[0].color;
        const u64 reglist = fog ? GS_FOG_REGLIST : DRAW_STQ_REGLIST;

        // packet limits: the frame packet is a fixed global buffer (qtotal qwords), keep room for the end
        // of the frame; a GIF REGLIST tag counts at most 32767 loops (vertices) per primitive
        u64 *dpLimit = (u64*)(qs + qtotal - GS_PACKET_RESERVE);
        const int maxPrimVerts = 32000;
        const int worstTriU64  = 6 * 3 * 3;      // a clipped triangle becomes up to a 6 triangle fan

        qword_t *qpStart = qp;
        qp = draw_prim_start(qp, 0, &prim, &startColor);
        u64 *dp = (u64 *)qp;
        u64 *dpStart = dp;
        u64 *dpPrim  = dp;   // start of the current primitive's vertex data

        const bool cull = (cullMode & (RS_CULL_BACK | RS_CULL_FRONT)) != 0 && !shadowMapMode;

        // generate command list for triangles: RGBAQ, ST, XYZ2 per vertex
        for (int i = 0; i < range.iCount; i += 3) {
            if (dp + worstTriU64 > dpLimit) {
                static int logged = 0;
                if (logged < 8) {
                    GS_LOG("WARN: frame packet full (%u qwords): DIP at %u qwords, iCount %d (tri %d) span %d vStart %d iStart %d dips %u mesh %p",
                           qtotal, u32(qpStart - qs), range.iCount, i / 3, spanCount, range.vStart, range.iStart, Core::stats.dips, mesh);
                    logged++;
                }
                break;
            }
            if ((dp - dpPrim) / 3 + 18 > maxPrimVerts) {   // close this primitive, start the next one
                if ((u32)dp & 15) *dp++ = 0;
                qp = draw_prim_end((qword_t*)dp, 3, reglist);
                qp = draw_prim_start(qp, 0, &prim, &startColor);
                dp = dpPrim = (u64*)qp;
            }

            u32 idx[3] = { iB[i] - iMin, iB[i + 1] - iMin, iB[i + 2] - iMin };
            u32 f0 = cvFlag[idx[0]], f1 = cvFlag[idx[1]], f2 = cvFlag[idx[2]];

            if (f0 & f1 & f2) {
                continue;   // all three vertices outside the same plane
            }

            if ((f0 | f1 | f2) & CLIP_MASK) {
                dp = clipTriangle(dp, vBase, cvBuf, cvColor, idx, (f0 | f1 | f2) & CLIP_MASK, su, sv, sp);
                continue;
            }

            // fast path: fully inside, use the VU0 results as they are
            const ClipVertex &A = cvBuf[idx[0]], &B = cvBuf[idx[1]], &C = cvBuf[idx[2]];
            if (cull) {
                float area = float(B.sx - A.sx) * float(C.sy - A.sy) - float(B.sy - A.sy) * float(C.sx - A.sx);
                if ((cullMode & RS_CULL_BACK)  && area > 0.0f) continue;   // flip the sign if the wrong side disappears
                if ((cullMode & RS_CULL_FRONT) && area < 0.0f) continue;
            }

            const u64 *p0 = cvPacked + idx[0] * 3, *p1 = cvPacked + idx[1] * 3, *p2 = cvPacked + idx[2] * 3;
            dp[0] = p0[0]; dp[1] = p0[1]; dp[2] = p0[2];
            dp[3] = p1[0]; dp[4] = p1[1]; dp[5] = p1[2];
            dp[6] = p2[0]; dp[7] = p2[1]; dp[8] = p2[2];
            dp += 9;
        }

        if (dp == dpPrim) {     // nothing in the last primitive: drop its header
            if (dpPrim == dpStart) {
                qp = qpStart;
            } else {
                qp = (qword_t*)dpPrim - 1;  // draw_prim_start's REGLIST tag placeholder
                // the previous primitive is already closed; remove the empty one (PRIM + RGBAQ + tag = 4 qwords)
                qp -= 3;
            }
            return;
        }

        // align to qword
        if ((u32)dp & 15) {
            *dp++ = 0;
        }
        qp = (qword_t *)dp;
        qp = draw_prim_end(qp, 3, reglist);
    }

    // ---- water surface ------------------------------------------------------------------------
    // The engine draws water surfaces with WaterCache (shaders + float textures), not available here.
    // Level code builds the water geometry (wave grid, light rays) into waterVtx / waterIdx and draws
    // it through the normal DIP path (VU0 transform, clipping, fog) with drawWaterMesh.

    Texture      *waterTex  = NULL;     // repeating ripple pattern
    GAPI::Vertex *waterVtx  = NULL;     // 16-byte aligned (VU0)
    Index        *waterIdx  = NULL;

    void initWaterSurface() {
        if (waterTex) return;
        // ripple pattern: brightness 0.7 .. 1.0 from a few tileable sine waves
        static u32 data[GS_WATER_TEX_SIZE * GS_WATER_TEX_SIZE] __attribute__((aligned(64)));
        for (int y = 0; y < GS_WATER_TEX_SIZE; y++) {
            for (int x = 0; x < GS_WATER_TEX_SIZE; x++) {
                float fx = float(x) * (2.0f * PI / GS_WATER_TEX_SIZE);
                float fy = float(y) * (2.0f * PI / GS_WATER_TEX_SIZE);
                float v = sinf(fx * 2.0f + sinf(fy) * 1.5f) * 0.5f + sinf(fy * 3.0f + fx) * 0.3f + sinf((fx + fy) * 4.0f) * 0.2f;
                int c = int((0.85f + v * 0.15f) * 255.0f);
                c = (c < 0) ? 0 : ((c > 255) ? 255 : c);
                data[y * GS_WATER_TEX_SIZE + x] = u32(c) | (u32(c) << 8) | (u32(c) << 16) | 0xFF000000;
            }
        }
        waterTex = new Texture(GS_WATER_TEX_SIZE, GS_WATER_TEX_SIZE, 1, OPT_REPEAT);
        waterTex->fmt = FMT_RGBA;
        waterTex->init(data);
        waterVtx = (GAPI::Vertex*)memalign(64, GS_WATER_MAX_VERTS * sizeof(GAPI::Vertex));
        waterIdx = new Index[GS_WATER_MAX_INDICES];
    }

    // draws vCount / iCount entries of waterVtx / waterIdx (world space, coords / 32767) with the given
    // texture; render state (blend, depth, cull, fog, identity model matrix) is set up by the caller
    void drawWaterMesh(int vCount, int iCount, Texture *tex) {
        if (!iCount || !isInFrame() || !tex) return;

        Mesh mesh(false);
        mesh.iBuffer = waterIdx;
        mesh.vBuffer = waterVtx;
        mesh.iCount  = iCount;
        mesh.vCount  = vCount;

        MeshRange range;
        range.iCount = iCount;

        int oldMode = lightMode;
        bool oldUnderwater = lightUnderwater;
        setLightingMode(LIGHT_NONE);
        tex->bind(sDiffuse);
        DIP(&mesh, range);
        setLightingMode(oldMode, oldUnderwater);

        mesh.iBuffer = NULL;    // static buffers, not owned by this Mesh
        mesh.vBuffer = NULL;
    }

    // ---- underwater screen ripple ---------------------------------------------------------------
    // After the 3D pass the Z buffer is not needed until the next frame's clear, so the finished
    // frame is copied into the Z buffer memory as CT24 (CT24 writes keep the top byte = level tiles),
    // then drawn back in horizontal strips with sine offsets. Two full-screen sprite passes on the GS.


    qword_t *rippleSetTexture(qword_t *q, u32 address, bool linear) {
        texbuffer_t tb;
        tb.address = address;
        tb.width   = backbuffer->width;          // TBW 10
        tb.psm     = GS_PSM_24;
        tb.info.width      = draw_log2(backbuffer->width);   // 1024
        tb.info.height     = draw_log2(backbuffer->height);  // 512
        tb.info.components = TEXTURE_COMPONENTS_RGB;
        tb.info.function   = TEXTURE_FUNCTION_DECAL;
        clutbuffer_t clut = {};
        clut.load_method = CLUT_NO_LOAD;
        q = draw_texturebuffer(q, 0, &tb, &clut);

        lod_t lod = {};
        lod.calculation = LOD_USE_K;
        lod.mag_filter = linear ? LOD_MAG_LINEAR : LOD_MAG_NEAREST;
        lod.min_filter = linear ? LOD_MIN_LINEAR : LOD_MIN_NEAREST;
        q = draw_texture_sampling(q, 0, &lod);

        texwrap_t wrap;
        wrap.horizontal = wrap.vertical = WRAP_REGION_CLAMP;
        wrap.minu = 0; wrap.maxu = backbuffer->width  - 1;
        wrap.minv = 0; wrap.maxv = backbuffer->height - 1;
        return draw_texture_wrapping(q, 0, &wrap);
    }

    // textured sprite from (x0,y0)-(x1,y1) window pixels, texels (u0,v0)-(u1,v1) (all in 12.4 / 14.4)
    inline u64 *rippleSprite(u64 *dp, int x0, int y0, int x1, int y1, int u0, int v0, int u1, int v1) {
        const int ox = int(origin_x * 16.0f), oy = int(origin_y * 16.0f);
        dp[0] = u64(u32(u0) & 0x3FFF) | (u64(u32(v0) & 0x3FFF) << 16);                       // UV
        dp[1] = u64(u32(x0 + ox) & 0xFFFF) | (u64(u32(y0 + oy) & 0xFFFF) << 16);             // XYZ2 (z = 0)
        dp[2] = u64(u32(u1) & 0x3FFF) | (u64(u32(v1) & 0x3FFF) << 16);
        dp[3] = u64(u32(x1 + ox) & 0xFFFF) | (u64(u32(y1 + oy) & 0xFFFF) << 16);
        return dp + 4;
    }

    void underwaterRipple(float time) {
        if (!isInFrame()) return;

        const int w = backbuffer->width, h = backbuffer->height;

        prim_t prim = {};
        prim.type         = PRIM_SPRITE;
        prim.shading      = PRIM_SHADE_FLAT;
        prim.mapping      = DRAW_ENABLE;
        prim.fogging      = DRAW_DISABLE;
        prim.blending     = DRAW_DISABLE;
        prim.antialiasing = DRAW_DISABLE;
        prim.mapping_type = PRIM_MAP_UV;
        prim.colorfix     = PRIM_UNFIXED;
        color_t color = {};
        color.r = color.g = color.b = color.a = 0x80;
        color.q = 1.0f;

        // no depth test / write, no alpha test, full screen scissor
        atest_t at = atest;   at.enable = DRAW_DISABLE;
        ztest_t zt = ztest;   zt.enable = DRAW_ENABLE; zt.method = ZTEST_METHOD_ALLPASS;
        zbuffer_t zb = *zbuffer; zb.mask = 1;
        qp = draw_pixel_test(qp, 0, &at, &dtest, &zt);
        qp = draw_zbuffer(qp, 0, &zb);
        qp = draw_scissor_area(qp, 0, 0, w - 1, 0, h - 1);

        // pass 1: backbuffer -> Z buffer memory (CT24), 1:1 in 32 px wide vertical strips
        framebuffer_t copy = *backbuffer;
        copy.address = zbuffer->address;
        copy.psm     = GS_PSM_24;
        copy.mask    = 0;
        qp = draw_framebuffer(qp, 0, &copy);
        qp = rippleSetTexture(qp, backbuffer->address, false);
        qp = draw_prim_start(qp, 0, &prim, &color);
        u64 *dp = (u64*)qp;
        for (int x = 0; x < w; x += 32) {
            int x1 = min(x + 32, w);
            dp = rippleSprite(dp, x * 16, 0, x1 * 16, h * 16, x * 16 + 8, 8, x1 * 16 + 8, h * 16 + 8);
        }
        qp = draw_prim_end((qword_t*)dp, 2, DRAW_UV_REGLIST);

        // pass 2: copy -> backbuffer, horizontal strips with sine offsets (bilinear, sub-pixel)
        qp = draw_framebuffer(qp, 0, backbuffer);
        qp = gsTexFlush(qp);            // texture cache may hold the old Z buffer page contents
        qp = rippleSetTexture(qp, zbuffer->address, true);
        qp = draw_prim_start(qp, 0, &prim, &color);
        dp = (u64*)qp;
        for (int y = 0; y < h; y += GS_RIPPLE_STRIP) {
            int y1 = min(y + GS_RIPPLE_STRIP, h);
            float yc = float(y) + GS_RIPPLE_STRIP * 0.5f;
            float dx = GS_RIPPLE_AMP * sinf(yc * (2.0f * PI / 96.0f) + time * 2.2f)
                     + GS_RIPPLE_AMP * 0.4f * sinf(yc * (2.0f * PI / 37.0f) - time * 3.1f);
            float dy = 1.0f * sinf(yc * (2.0f * PI / 140.0f) + time * 1.3f);
            int du = int(dx * 16.0f), dv = int(dy * 16.0f);
            // keep the source inside the image so the clamped edge column does not smear
            int u0, u1;
            if (du < 0) { u0 = 8; u1 = (w * 16 + du) + 8; }
            else        { u0 = du + 8; u1 = w * 16 + 8; }
            int v0 = clamp(y * 16 + dv, 0, (h - 1) * 16) + 8;
            int v1 = clamp(y1 * 16 + dv, 0, h * 16) + 8;
            dp = rippleSprite(dp, 0, y * 16, w * 16, y1 * 16, u0, v0, u1, v1);
        }
        qp = draw_prim_end((qword_t*)dp, 2, DRAW_UV_REGLIST);

        // restore state
        qp = gsTexFlush(qp);
        qp = draw_zbuffer(qp, 0, zbuffer);
        qp = draw_pixel_test(qp, 0, &atest, &dtest, &ztest);
        qp = emitScissor(qp);
        gsTexture = NULL;   // next DIP re-emits TEX0/TEX1/CLAMP
    }

    vec4 copyPixel(int x, int y) {
        // TODO: read from framebuffer
        return vec4(0.0f);
    }

    int gs_handler(s32 cause) {
        // check the drawing finished bit
        if (*GS_REG_CSR & 2) {
            *GS_REG_CSR |= 2;
            iSignalSema(gsSema);
        }
        // check the vblank bit
        //if (*GS_REG_CSR & 8) {
        if (cause == INTC_VBLANK_E) {
            //*GS_REG_CSR |= 8;
            ++vblankCounter;
            iSignalSema(vblankSema);
        }
        if (!dma_channel_wait(DMA_CHANNEL_GIF, 1)) {
            iSignalSema(dmaSema);
        }
        return 0;
    }

    void init() {
        LOG("Vendor   : %s\n", "Sony");
        LOG("Renderer : %s\n", "EE+GS");
        LOG("Version  : %s\n", "1.0");
        LOG("VRAM     : %d\n", GRAPH_VRAM_MAX_WORDS * 4);

        support.maxAniso       = 0;
        support.maxVectors     = 0;
        support.shaderBinary   = false;
        support.VAO            = false;
        support.VBO            = false;
        support.depthTexture   = false;
        support.shadowSampler  = false;
        support.discardFrame   = false;
        support.texNPOT        = true;
        support.texMinSize     = 4;     // GIF image transfer moves whole qwords: 1x1 RGBA (4 bytes) would upload nothing
        support.tex3D          = false;
        support.texRG          = false;
        support.texBorder      = false;
        support.texMaxLevel    = false;
        support.colorFloat     = false;
        support.colorHalf      = false;
        support.texFloatLinear = false;
        support.texFloat       = false;
        support.texHalfLinear  = false;
        support.texHalf        = false;
        //support.clipDist       = false;

        ee_sema_t sema = {
            .max_count = 1,
            .init_count = 0,
            .option = 0
        };
        vblankSema = CreateSema(&sema);
        gsSema = CreateSema(&sema);
        dmaSema = CreateSema(&sema);

        vblankCounter = 0;
        fbc = 0;
        frame_begin = 1;
        frame_end = 0;
        cr = 0;
        cg = 0;
        cb = 0;


        // init low-level graphics
        Core::width  = 640;
        Core::height = 448;

        origin_x = 2048.0f - Core::width / 2;
        origin_y = 2048.0f - Core::height / 2;

        // init GIF DMA CHANNEL
	    dma_channel_initialize(DMA_CHANNEL_GIF, (void *)gs_handler, 0);
	    dma_channel_fast_waits(DMA_CHANNEL_GIF);

        // alpha test
        atest.enable = DRAW_DISABLE;
        atest.method = ATEST_METHOD_GREATER_EQUAL;   // PSP: GU_GREATER 127 of 255 -> GS 0x40 of 0x80
        atest.compval = 0x40;
        atest.keep = ATEST_KEEP_ALL;    // AFAIL KEEP: failing pixels update nothing. NOT ATEST_KEEP_FRAMEBUFFER:
                                        // that is AFAIL = 2 (ZB_ONLY), transparent texels would still write depth

        // Destination Alpha Testing
        dtest.enable = DRAW_DISABLE;
        dtest.pass = DTEST_METHOD_PASS_ZERO;

        // depth test
        ztest.enable = DRAW_ENABLE;
        ztest.method = ZTEST_METHOD_GREATER_EQUAL;

        // initialize zbuffer
        zbuffer = &zbs;
        zbuffer->mask = 0;
        zbuffer->enable = ztest.enable;
        zbuffer->method = ztest.method;
        zbuffer->zsm = GS_PSMZ_24;     // 24-bit Z, top byte holds level tiles (PSMT4HL/HH)
        zbuffer->address = graph_vram_allocate(Core::width, Core::height, zbuffer->zsm, GRAPH_ALIGN_PAGE);

        // initialize framebuffer
        for (int i = 0; i < 2; i++) {
            framebuffer_t *fb = fbs + i;
            fb->mask = 0;
            fb->width = Core::width;
            fb->height = Core::height;
            fb->psm = GS_PSM_24;            // 24-bit color, top byte holds level tiles (PSMT4HL/HH)
            fb->address = graph_vram_allocate(fb->width, fb->height, fb->psm, GRAPH_ALIGN_PAGE);
        }

        // initial front-buffer
        frontbuffer = fbs + (fbc ^ 1);
        backbuffer = fbs + fbc;

        // everything above the framebuffers + Z is managed by the page allocator
        {
            u32 top = 0;
            for (int i = 0; i < 2; i++) {
                top = max(top, fbs[i].address + graph_vram_size(fbs[i].width, fbs[i].height, fbs[i].psm, GRAPH_ALIGN_PAGE));
            }
            top = max(top, zbuffer->address + graph_vram_size(Core::width, Core::height, zbuffer->zsm, GRAPH_ALIGN_PAGE));
            vramInit(top);
            GS_LOG("VRAM: buffers end at page %d, %d pages for textures", vramHeapStart, GS_PAGES - vramHeapStart);
        }

        initWaterSurface();
        initShadowMap();

        texwrap_t texwrap;
	    texwrap.horizontal = WRAP_REPEAT;
	    texwrap.vertical = WRAP_REPEAT;
	    texwrap.minu = 0;
        texwrap.maxu = 0;
	    texwrap.minv = 0;
        texwrap.maxv = 0;

        // install interrupts
        DI();
        
        idgsh = AddIntcHandler(INTC_GS, gs_handler, 0);
        idvblankh = AddIntcHandler(INTC_VBLANK_E, gs_handler, 0);

        EnableIntc(INTC_GS);
        EnableIntc(INTC_VBLANK_E);

        EI();

        // init CRTC, display frontbuffer
        graph_set_mode(GRAPH_MODE_INTERLACED, GRAPH_MODE_NTSC, GRAPH_MODE_FIELD, GRAPH_ENABLE);
        graph_set_screen(0, 0, frontbuffer->width, frontbuffer->height);
        graph_set_bgcolor(0, 0, 0);
        graph_set_framebuffer_filtered(frontbuffer->address, frontbuffer->width, frontbuffer->psm, 0, 0);
        graph_enable_output();

        // reset GS state, drawing to backbuffer
        qs = packets[fbc];
        qp = qs;
        qp = draw_setup_environment(qp, 0, frontbuffer, zbuffer);
        qp = draw_primitive_xyoffset(qp, 0, origin_x, origin_y);
        qp = draw_dithering(qp, DRAW_DISABLE);   // 24-bit framebuffer: no dithering needed
        qp = draw_dither_matrix(qp, (char *)dith);
        qp = draw_texture_wrapping(qp, 0, &texwrap);
        qp = draw_clear(qp, 0, origin_x, origin_y, frontbuffer->width, frontbuffer->height, 0, 0, 0);
        clear(true, true);
        qp = draw_finish(qp);
	    dma_channel_send_normal(DMA_CHANNEL_GIF, qs, qp - qs, 0, 0);
        dma_wait_fast();

        frame_begin = 0;
    }

    void deinit() {
        GS_DEBUG("");
        dma_channel_shutdown(DMA_CHANNEL_GIF, 0);

        DI();
        RemoveIntcHandler(INTC_GS, idgsh);
        RemoveIntcHandler(INTC_VBLANK_S, idvblankh);
        EI();
    }
}

#endif
