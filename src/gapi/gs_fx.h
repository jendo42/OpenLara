#ifndef H_GS_FX
#define H_GS_FX

// PS2 (GS) engine-side effects: everything that needs engine types (TR::Level, Controller, Lara,
// MeshBuilder) and therefore can not live in gapi/gs.h, which is included from core.h before them.
// Included from level.h after its includes; tunables are in the config block at the top of gapi/gs.h.

// ---- MeshBuilder helpers (declared in mesh.h) ---------------------------------------------------

// Common UI textures (normally generated into the atlas, see Level::fillCallback CommonTexData):
// flash / health / oxygen are 1x5 vertical gradients, option is a 5x5 patch with alpha falloff.
// Stored as 4x8 / 8x8 textures (GS uploads whole qwords), UVs map the bar onto the texel centers.
static Texture *gsBarTextures[CTEX_OPTION + 1];

inline Texture* gsBarTex(int type) {
    static const uint32 gradients[3][5] = {
        { 0x00000000, 0xFFA20058, 0xFFFFFFFF, 0xFFA20058, 0x00000000 }, // flash
        { 0xFF2C5D71, 0xFF5E81AE, 0xFF2C5D71, 0xFF1B4557, 0xFF16304F }, // health
        { 0xFF647464, 0xFFA47848, 0xFF647464, 0xFF4C504C, 0xFF303030 }, // oxygen
    };
    static const uint32 option[5] = { 0x20202020, 0x60606060, 0x80808080, 0x60606060, 0x20202020 };

    if (!gsBarTextures[type]) {
        uint32 data[8 * 8];
        int w = (type == CTEX_OPTION) ? 8 : 4;
        for (int y = 0; y < 8; y++) {
            int r = min(y, 4);
            for (int x = 0; x < w; x++) {
                if (type == CTEX_OPTION) {  // 5x5: transparent outer columns, rows fade in/out
                    // the engine data is premultiplied (color == alpha) for bmPremult; the GS blends
                    // with plain alpha, so use white + the same alpha (the color comes from the vertex)
                    int c = min(x, 4);
                    data[y * w + x] = (c == 0 || c == 4) ? 0x00FFFFFF : ((option[r] & 0xFF000000) | 0x00FFFFFF);
                } else {
                    data[y * w + x] = gradients[type][r];
                }
            }
        }
        for (int i = 0; i < w * 8; i++) {  // GS alpha scale: 0x80 = 1.0
            uint32 a = data[i] >> 24;
            data[i] = (data[i] & 0x00FFFFFF) | (((a + 1) >> 1) << 24);
        }
        gsBarTextures[type] = new Texture(w, 8, 1, FMT_RGBA, 0, data);
        gsBarTextures[type]->alphaRGBA = true;
    }
    return gsBarTextures[type];
}

inline void gsFreeBarTextures() {
    for (int i = 0; i < COUNT(gsBarTextures); i++) {
        delete gsBarTextures[i];
        gsBarTextures[i] = NULL;
    }
}

// Texture animation crossfade: Level::shiftAnimTex moves objectTextures[seq[j + 1]] into
// objectTextures[seq[j]], so the next frame of slot seq[j] is currently in slot seq[j + 1].
// Returns per objectTexture the slot holding its next frame, -1 when not animated.
inline int16* gsBuildAnimNext(TR::Level *level) {
    int16 *animNext = new int16[max(1, int(level->objectTexturesCount))];
    for (int i = 0; i < level->objectTexturesCount; i++) animNext[i] = -1;
    for (int a = 0; a < level->animTexturesCount; a++) {
        const TR::AnimTexture &seq = level->animTextures[a];
        for (int j = 0; j < seq.count; j++) {
            if (seq.textures[j] < level->objectTexturesCount) {
                animNext[seq.textures[j]] = int16(seq.textures[(j + 1) % seq.count]);
            }
        }
    }
    return animNext;
}

// ---- Level effects ------------------------------------------------------------------------------
// A template on the owner (Level), so the bodies can use Level members although Level is incomplete
// here. Level holds one instance (Level::gs) and calls it from a few hooks.
template <class Owner>
struct GSEffects {
    struct GSWaterField {
        int16  room;
        int    w, h;            // grid points
        float  x0, z0, y;       // world position of point (0,0), surface height
        float  *cur, *prev;
        uint8  *mask;           // 0 - outside, 1 - shore (moves, reflects waves), 2 - water,
                                // 3 - room border (pinned at 0: the water continues in the next room)
        float  idleTimer;
        int16  *faceTex;        // per surface sector: objectTexture of the original PS1 water face, -1 none
        uint8  *faceMap;        // per sector, 4 corners (x0z0, x1z0, x0z1, x1z1): texCoordAtlas index
        float  *faceRect;       // per sector: world rect of its face (minX, minZ, maxX, maxZ), faces may span sectors
        int16  *faceTexNext;    // per sector: objectTexture slot holding the next animation frame, -1 not animated
    };

    Owner        *owner;
    TR::Level    &level;

    int16        waterRooms[32];        // visible water rooms this frame
    int          waterRoomsCount;
    GSWaterField fields[GS_WAVE_FIELDS];
    int          fieldsCount;
    float        waveTimer;
    int          laraStand;             // Lara's previous Character::Stand (water transitions)
    float        laraWake;
    vec3         laraWakePos;
    float        laraSplash;            // follow-up splash drops after jumping in

    GSEffects(Owner *owner) : owner(owner), level(owner->level), waterRoomsCount(0), fieldsCount(0),
        waveTimer(0.0f), laraStand(-1), laraWake(0.0f), laraWakePos(0.0f), laraSplash(0.0f) {}

    ~GSEffects() {
        freeWaterFields();
    }

    // ---- per view ----------------------------------------------------------------------------------
    void beginView() {
        owner->mesh->animBlend = clamp(owner->animTexTimer / ANIM_TEX_TIMESTEP, 0.0f, 1.0f);   // texture animation crossfade
        GAPI::setUnderwaterView(Core::pass == Core::passCompose && owner->camera->isUnderwater());
        waterRoomsCount = 0;
    }

    void endView() {
        if (GAPI::underwaterView) {
            GAPI::underwaterRipple(Core::params.x);
        }
        GAPI::setUnderwaterView(false);
    }

    // shadow map shadows for all visible shadow casters (transparent entity pass)
    void renderShadows() {
        Core::setFog(FOG_NONE);
        for (int i = 0; i < level.entitiesCount && GS_SHADOWS; i++) {
            TR::Entity &entity = level.entities[i];
            Controller *controller = (Controller*)entity.controller;
            if (controller && controller->flags.rendered && controller->getEntity().castShadow())
                renderShadow(controller);
        }
        Core::setBlendMode(bmNone);
    }

    // ---- shadows ---------------------------------------------------------------------------------
    // Shadow map: the entity's silhouette is rendered from the light direction (orthographic) into a
    // small target (GAPI::shadowMapBegin), then one quad on the floor plane, textured with it, darkens the
    // floor. Overlapping parts can not double-darken and the quad has a correct depth (no drawing over Lara).
    void renderShadow(Controller *controller) {
        MeshBuilder *mesh = owner->mesh;
        if (Core::pass != Core::passCompose || level.isCutsceneLevel()) return;

        Box boxL = controller->getBoundingBoxLocal();
        Box boxA = boxL * controller->getMatrix();
        vec3 center = boxA.center();

        TR::Level::FloorInfo info;
        controller->getFloorInfo(controller->getRoomIndex(), center, info);
        if (info.floor == TR::NO_FLOOR) return;

        float height = info.floor - boxA.max.y;     // TR: y grows downwards
        float fade = 1.0f - clamp(height / GS_SHADOW_FADE, 0.0f, 1.0f);
        if (fade <= 0.0f) return;

        // light direction (pointing from the light down to the entity): straight down (top-down shadow),
        // or tilted away from the main room light with GS_SHADOW_FROM_LIGHT
        vec3 L = vec3(0.0f, 1.0f, 0.0f);
        if (GS_SHADOW_FROM_LIGHT && controller->mainLightColor.w > 1.0f) {   // radius 1 = no main light
            vec3 d = center - controller->mainLightPos;
            if (d.length2() > 1.0f) {
                d = d.normal();
                if (d.y < GS_SHADOW_MIN_Y) {
                    float k = sqrtf((1.0f - GS_SHADOW_MIN_Y * GS_SHADOW_MIN_Y) / max(0.0001f, d.x * d.x + d.z * d.z));
                    d = vec3(d.x * k, GS_SHADOW_MIN_Y, d.z * k);
                }
                L = d;
            }
        }

        // floor plane (slightly lifted) and the projection along L onto it
        vec3 n  = info.getNormal();
        vec3 p0 = vec3(center.x, float(info.floor), center.z) + n * GS_SHADOW_LIFT;
        float dLn = L.dot(n);
        if (fabsf(dLn) < 0.1f || fabsf(n.y) < 0.1f) return;
        float dp = p0.dot(n);

        // light camera: orthographic along L around the entity
        vec3 size = boxA.max - boxA.min;
        float ext = max(size.x, max(size.y, size.z)) * 0.6f + 32.0f;
        vec3 up   = (fabsf(L.y) > 0.95f) ? vec3(0.0f, 0.0f, 1.0f) : vec3(0.0f, -1.0f, 0.0f);
        mat4 lightBasis(center - L * 2048.0f, center, up);
        mat4 lightView = lightBasis.inverseOrtho();
        mat4 lightProj = GAPI::ortho(-ext, ext, -ext, ext, 0.0f, 4096.0f);

        // 1. silhouette into the shadow map (opaque parts of the model)
        int transp = mesh->transparent;
        mesh->transparent = 0;
        GAPI::setViewProj(lightView, lightProj);
        if (GAPI::shadowMapBegin(1.0f)) {   // full alpha in the map, darkness is applied per blur tap
            controller->render(NULL, mesh, Shader::ENTITY, false);
            GAPI::shadowMapEnd();
        }
        GAPI::setViewProj(Core::mView, Core::mProj);
        mesh->transparent = transp;

        // 2. quad on the floor: the light-space rectangle projected along L onto the plane
        vec3 r = lightBasis.right().xyz();
        vec3 u = lightBasis.up().xyz();
        GAPI::initWaterSurface();
        GAPI::Vertex *vtx = GAPI::waterVtx;
        Index        *idx = GAPI::waterIdx;
        const float s = 1.0f / 32767.0f;
        static const float SX[4] = { -1.0f,  1.0f, 1.0f, -1.0f };
        static const float SY[4] = {  1.0f,  1.0f, -1.0f, -1.0f };
        for (int c = 0; c < 4; c++) {
            vec3 q = center + r * (SX[c] * ext) + u * (SY[c] * ext);
            q = q - L * ((q.dot(n) - dp) / dLn);        // onto the plane along the light direction
            GAPI::Vertex &v = vtx[c];
            v.coord.x = q.x * s; v.coord.y = q.y * s; v.coord.z = q.z * s; v.coord.w = 1.0f;
            v.normal.x = n.x; v.normal.y = n.y; v.normal.z = n.z; v.normal.w = 0.0f;
            v.texCoord.u = (SX[c] + 1.0f) * 0.5f;       // DIP maps NDC y up -> screen y down
            v.texCoord.v = (1.0f - SY[c]) * 0.5f;
            v.color.r = v.color.g = v.color.b = 0x80;
            v.color.q = 1.0f;
        }

        // soft shadow: the quad drawn 5 times (center + 4 diagonal offsets), each tap with alpha a so that
        // where all taps overlap the stacked blends give exactly D = 1 - (1 - a)^5; edges get a penumbra
        static const float TX[5] = { 0.0f, -1.0f,  1.0f, -1.0f, 1.0f };
        static const float TY[5] = { 0.0f, -1.0f, -1.0f,  1.0f, 1.0f };
        const int taps = (GS_SHADOW_SOFT > 0.0f) ? 5 : 1;
        float D = GS_SHADOW_DARKNESS * fade;
        float a = 1.0f - powf(1.0f - D, 1.0f / float(taps));
        float off = GS_SHADOW_SOFT / float(GS_SHADOW_MAP_SIZE);
        int vCount = 0, iCount = 0;
        for (int t = 0; t < taps; t++) {
            for (int c = 0; c < 4; c++) {
                GAPI::Vertex &v = vtx[vCount + c];
                v = vtx[c];
                v.texCoord.u += TX[t] * off;
                v.texCoord.v += TY[t] * off;
                v.color.a = uint8(clamp(a * 128.0f, 0.0f, 128.0f));
            }
            Index b = Index(vCount);
            idx[iCount++] = b; idx[iCount++] = b + 1; idx[iCount++] = b + 2;
            idx[iCount++] = b; idx[iCount++] = b + 2; idx[iCount++] = b + 3;
            vCount += 4;
        }

        Core::mModel.identity();
        Core::setFog(FOG_NONE);
        Core::setBlendMode(bmAlpha);
        Core::setDepthTest(true);
        Core::setDepthWrite(false);
        Core::setCullMode(cmNone);
        Core::setAlphaTest(false);
        Core::validateRenderState();

        GAPI::drawWaterMesh(vCount, iCount, GAPI::shadowTex);

        Core::setDepthWrite(true);
        Core::setCullMode(cmFront);
    }

    // SPLIT_BY_TILE has no generated common textures: whiteRoom / whiteObject / whiteSprite stay zero
    // (tile 0, uv 0 = a random texel). Colored (untextured) faces use them, so point them at the
    // brightest opaque texel found in the level's object textures.
    void initWhiteTexel() {
        int bestTile = 0, bestClut = 0, bestX = 0, bestY = 0, best = -1;
        for (int i = 0; i < level.objectTexturesCount && best < 3 * 31; i++) {
            const TR::TextureInfo &t = level.objectTextures[i];
            if (t.tile >= level.tilesCount || t.clut >= level.clutsCount) continue;
            int x0 = 255, y0 = 255, x1 = 0, y1 = 0;
            for (int k = 0; k < 4; k++) {
                x0 = min(x0, int(t.texCoord[k].x)); x1 = max(x1, int(t.texCoord[k].x));
                y0 = min(y0, int(t.texCoord[k].y)); y1 = max(y1, int(t.texCoord[k].y));
            }
            const Tile4 &tile = level.tiles4[t.tile];
            const CLUT  &clut = level.cluts[t.clut];
            for (int y = y0; y <= y1 && best < 3 * 31; y++) {
                for (int x = x0; x <= x1; x++) {
                    const ColorIndex4 &ci = tile.index[(y * 256 + x) / 2];
                    const ColorCLUT &c = clut.color[(x & 1) ? ci.b : ci.a];
                    if (!c.a) continue;
                    int v = c.r + c.g + c.b;
                    if (v > best) {
                        best = v; bestTile = t.tile; bestClut = t.clut; bestX = x; bestY = y;
                        if (best >= 3 * 31) break;
                    }
                }
            }
        }
        const int types[3] = { CTEX_WHITE_ROOM, CTEX_WHITE_OBJECT, CTEX_WHITE_SPRITE };
        for (int i = 0; i < 3; i++) {
            TR::TextureInfo &t = CommonTex[types[i]];
            t.tile = uint16(bestTile);
            t.clut = uint16(bestClut);
            for (int k = 0; k < 4; k++) {
                t.texCoordAtlas[k] = short2(short(bestX * 128 + 64), short(bestY * 128 + 64));
            }
        }
        LOG("white texel: tile %d clut %d (%d, %d) brightness %d\n", bestTile, bestClut, bestX, bestY, best);
    }

    // ---- water surfaces (without WaterCache: wave fields + GAPI::drawWaterMesh) --------------------
    void addWaterSurface(int underwaterRoom) {
        TR::Room &r = level.rooms[underwaterRoom];
        if (!r.flags.water) return;
        for (int i = 0; i < waterRoomsCount; i++) {
            if (waterRooms[i] == underwaterRoom) return;
        }
        if (waterRoomsCount < COUNT(waterRooms)) {
            waterRooms[waterRoomsCount++] = int16(underwaterRoom);
        }
    }

    void renderWaterSurfaces(RoomDesc *roomsList, int roomsCount) {
        // water rooms from the final visible list (not from portal crossings: a room is visited once,
        // so the water/dry portal is not always the one that reaches it -> layer would come and go)
        waterRoomsCount = 0;
        for (int i = 0; i < roomsCount; i++) {
            addWaterSurface(roomsList[i].index);
        }
        renderWaterTint();
    }

    // ---- wave fields ---------------------------------------------------------------------------

    bool isSurfaceSector(const TR::Room &r, int x, int z) {
        if (x < 0 || z < 0 || x >= r.xSectors || z >= r.zSectors) return false;
        const TR::Room::Sector &s = r.sectors[x * r.zSectors + z];
        return s.roomAbove != TR::NO_ROOM && !level.rooms[s.roomAbove].flags.water;
    }

    void freeWaterFields() {
        for (int i = 0; i < fieldsCount; i++) {
            delete[] fields[i].cur;
            delete[] fields[i].prev;
            delete[] fields[i].mask;
            delete[] fields[i].faceTex;
            delete[] fields[i].faceMap;
            delete[] fields[i].faceRect;
            delete[] fields[i].faceTexNext;
        }
        fieldsCount = 0;
    }

    GSWaterField* getWaterField(int roomIndex) {
        for (int i = 0; i < fieldsCount; i++) {
            if (fields[i].room == roomIndex) return &fields[i];
        }
        if (fieldsCount >= GS_WAVE_FIELDS) return NULL;

        TR::Room &r = level.rooms[roomIndex];
        // waterLevelSurface is only filled when water quality > LOW (MeshBuilder::roomRemoveWaterSurfaces),
        // waterLevel[] is always computed (MeshBuilder::calcWaterLevel) and is the surface height of a water room
        int posY = r.waterLevel[level.state.flags.flipped];
        if (posY == TR::NO_WATER) posY = r.waterLevelSurface;
        if (posY == TR::NO_WATER) return NULL;

        bool any = false;
        for (int z = 0; z < r.zSectors && !any; z++)
            for (int x = 0; x < r.xSectors && !any; x++)
                any = isSurfaceSector(r, x, z);
        if (!any) return NULL;

        GSWaterField &f = fields[fieldsCount++];
        f.room = int16(roomIndex);
        f.w    = r.xSectors * 4 + 1;
        f.h    = r.zSectors * 4 + 1;
        f.x0   = float(r.info.x);
        f.z0   = float(r.info.z);
        f.y    = float(posY);
        f.idleTimer = 0.0f;
        int n = f.w * f.h;
        f.cur  = new float[n];
        f.prev = new float[n];
        f.mask = new uint8[n];
        memset(f.cur,  0, n * sizeof(float));
        memset(f.prev, 0, n * sizeof(float));

        // a point is water when all sectors touching it are surface sectors; when only some are, it is
        // shore (next to a wall: moves with the water) or a room border (next to a portal sector that
        // leads into another water room: pinned at 0, the neighbour room's field is pinned on the same
        // line, so the two surfaces meet without a seam)
        for (int j = 0; j < f.h; j++) {
            for (int i = 0; i < f.w; i++) {
                int sx = i / 4, sz = j / 4;
                bool onX = (i % 4) == 0, onZ = (j % 4) == 0;
                int total = 0, surf = 0;
                bool portal = false;
                for (int dz = (onZ ? -1 : 0); dz <= 0; dz++) {
                    for (int dx = (onX ? -1 : 0); dx <= 0; dx++) {
                        int nx = sx + dx, nz = sz + dz;
                        total++;
                        if (isSurfaceSector(r, nx, nz)) {
                            surf++;
                        } else if (nx < 0 || nz < 0 || nx >= r.xSectors || nz >= r.zSectors) {
                            portal = true;      // outside the room: continues in another room
                        } else {
                            int next = level.getNextRoom(&r.sectors[nx * r.zSectors + nz]);
                            if (next != TR::NO_ROOM && level.rooms[next].flags.water) {
                                portal = true;
                            }
                        }
                    }
                }
                uint8 m = (surf == total) ? 2 : (surf ? (portal ? 3 : 1) : 0);
                f.mask[j * f.w + i] = m;
            }
        }

        // original PS1 water faces (taken out of the room geometry by roomRemoveWaterSurfaces) of the
        // water room and the dry rooms above it, mapped onto the sectors of this field
        int sCount = r.xSectors * r.zSectors;
        f.faceTex = new int16[sCount];
        f.faceMap = new uint8[sCount * 4];
        f.faceRect = new float[sCount * 4];
        f.faceTexNext = new int16[sCount];
        for (int i = 0; i < sCount; i++) f.faceTex[i] = -1;

        int16 rooms[16];
        int roomsCount = 0;
        rooms[roomsCount++] = int16(roomIndex);
        for (int i = 0; i < sCount; i++) {
            int above = r.sectors[i].roomAbove;
            if (above == TR::NO_ROOM) continue;
            bool known = false;
            for (int k = 0; k < roomsCount; k++) known |= (rooms[k] == above);
            if (!known && roomsCount < COUNT(rooms)) rooms[roomsCount++] = int16(above);
        }

        for (int k = 0; k < roomsCount; k++) {
            TR::Room &fr = level.rooms[rooms[k]];
            const TR::Room::Data &d = fr.data;
            for (int i = 0; i < d.fCount; i++) {
                const TR::Face &face = d.faces[i];
                if (!face.water || face.triangle) continue;
                // the face's own world rectangle (a face can span several sectors)
                float vx[4], vz[4];
                float minX = 1e9f, minZ = 1e9f, maxX = -1e9f, maxZ = -1e9f;
                for (int c = 0; c < 4; c++) {
                    vx[c] = float(fr.info.x + d.vertices[face.vertices[c]].pos.x);
                    vz[c] = float(fr.info.z + d.vertices[face.vertices[c]].pos.z);
                    minX = min(minX, vx[c]); maxX = max(maxX, vx[c]);
                    minZ = min(minZ, vz[c]); maxZ = max(maxZ, vz[c]);
                }
                if (maxX - minX < 1.0f || maxZ - minZ < 1.0f) continue;
                float cx = (minX + maxX) * 0.5f, cz = (minZ + maxZ) * 0.5f;

                // which texCoordAtlas entry belongs to which corner of the rectangle. PSX quads use
                // texCoordAtlas[i] for face vertex i except vertices 2 and 3, which are swapped
                // (MeshBuilder::addTexCoord: swap(texCoord 2, texCoord 3) for VER_PSX)
                static const uint8 PSX_UV[4] = { 0, 1, 3, 2 };
                bool psx = (level.version & TR::VER_PSX) != 0;
                uint8 map[4] = { 0, 1, 2, 3 };
                for (int c = 0; c < 4; c++) {
                    map[(vx[c] > cx ? 1 : 0) + (vz[c] > cz ? 2 : 0)] = psx ? PSX_UV[c] : uint8(c);
                }

                // assign to every sector the rectangle covers
                int sx0 = int((minX - f.x0) / 1024.0f + 0.01f), sx1 = int((maxX - f.x0) / 1024.0f - 0.01f);
                int sz0 = int((minZ - f.z0) / 1024.0f + 0.01f), sz1 = int((maxZ - f.z0) / 1024.0f - 0.01f);
                for (int sz = max(0, sz0); sz <= min(r.zSectors - 1, sz1); sz++) {
                    for (int sx = max(0, sx0); sx <= min(r.xSectors - 1, sx1); sx++) {
                        int si = sz * r.xSectors + sx;
                        f.faceTex[si] = int16(face.flags.texture);
                        for (int c = 0; c < 4; c++) f.faceMap[si * 4 + c] = map[c];
                        f.faceRect[si * 4 + 0] = minX;
                        f.faceRect[si * 4 + 1] = minZ;
                        f.faceRect[si * 4 + 2] = maxX;
                        f.faceRect[si * 4 + 3] = maxZ;
                    }
                }
            }
        }
        // animated water: Level::shiftAnimTex moves objectTextures[seq[j + 1]] into objectTextures[seq[j]]
        // every ANIM_TEX_TIMESTEP, so the slot showing the next frame is seq[j + 1]
        for (int i = 0; i < sCount; i++) {
            f.faceTexNext[i] = -1;
            if (f.faceTex[i] < 0) continue;
            for (int a = 0; a < level.animTexturesCount && f.faceTexNext[i] < 0; a++) {
                const TR::AnimTexture &seq = level.animTextures[a];
                for (int j = 0; j < seq.count; j++) {
                    if (seq.textures[j] == uint16(f.faceTex[i])) {
                        f.faceTexNext[i] = int16(seq.textures[(j + 1) % seq.count]);
                        break;
                    }
                }
            }
        }
        return &f;
    }

    void addDrop(GSWaterField &f, float x, float z, float radius, float amount) {
        float gx = (x - f.x0) / GS_WAVE_CELL;
        float gz = (z - f.z0) / GS_WAVE_CELL;
        float rc = max(1.0f, radius / GS_WAVE_CELL);
        int i0 = max(0, int(gx - rc)), i1 = min(f.w - 1, int(gx + rc) + 1);
        int j0 = max(0, int(gz - rc)), j1 = min(f.h - 1, int(gz + rc) + 1);
        for (int j = j0; j <= j1; j++) {
            for (int i = i0; i <= i1; i++) {
                int k = j * f.w + i;
                if (!f.mask[k] || f.mask[k] == 3) continue;
                float d = sqrtf((float(i) - gx) * (float(i) - gx) + (float(j) - gz) * (float(j) - gz));
                if (d >= rc) continue;
                f.cur[k] += amount * 0.5f * (1.0f + cosf(PI * d / rc));
            }
        }
    }

    // drop at x/z into whichever field covers it (amount in height units)
    void dropAt(float x, float z, float radius, float amount) {
        for (int i = 0; i < fieldsCount; i++) {
            GSWaterField &f = fields[i];
            if (x < f.x0 || z < f.z0 || x > f.x0 + (f.w - 1) * GS_WAVE_CELL || z > f.z0 + (f.h - 1) * GS_WAVE_CELL) continue;
            addDrop(f, x, z, radius, amount);
        }
    }

    // The engine only reports diving (Lara::goUnderwater) and swim bubbles through waterDrop; detect
    // the other water transitions from Lara's stand: jump in, climb out, surface, plus a swimming wake
    void laraWaterEvents() {
        Lara *lara = owner->players[0];
        if (!lara) return;

        int stand = int(lara->stand);
        bool inWater  = (stand == Character::STAND_ONWATER || stand == Character::STAND_UNDERWATER);
        bool wasWater = (laraStand == Character::STAND_ONWATER || laraStand == Character::STAND_UNDERWATER);
        vec3 p = lara->pos;

        if (laraStand >= 0 && stand != laraStand) {
            if (inWater && !wasWater) {
                dropAt(p.x, p.z, 900.0f, 10.0f);   // jump / fall into the water
                laraSplash = 0.3f;                // + a short burst of follow-up drops
            } else if (!inWater && wasWater && stand != Character::STAND_AIR) {
                dropAt(p.x, p.z, 768.0f, 4.5f);   // pull out of the pool
            } else if (wasWater && !inWater) {
                dropAt(p.x, p.z, 768.0f, 4.0f);   // jump out of the water
            } else if (laraStand == Character::STAND_UNDERWATER && stand == Character::STAND_ONWATER) {
                dropAt(p.x, p.z, 768.0f, 4.0f);   // surfacing
            } else if (laraStand == Character::STAND_ONWATER && stand == Character::STAND_UNDERWATER) {
                dropAt(p.x, p.z, 768.0f, 4.0f);   // diving (stronger than the engine's own drop)
            }
        }
        laraStand = stand;

        if (laraSplash > 0.0f) {
            float prev = laraSplash;
            laraSplash -= Core::deltaTime;
            // drops at 0.2, 0.1 and 0.0 s left, scattered around the entry point
            for (int k = 0; k < 3; k++) {
                float at = 0.2f - k * 0.1f;
                if (prev > at && laraSplash <= at) {
                    dropAt(p.x + (randf() - 0.5f) * 512.0f, p.z + (randf() - 0.5f) * 512.0f, 512.0f, 3.0f);
                }
            }
        }

        // wake while swimming on the surface
        // (surface swimming is animation driven, so use the distance moved, not velocity)
        if (stand == Character::STAND_ONWATER) {
            laraWake += Core::deltaTime;
            if (laraWake > 0.1f) {
                float dx = p.x - laraWakePos.x, dz = p.z - laraWakePos.z;
                if (dx * dx + dz * dz > 16.0f * 16.0f) {
                    dropAt(p.x, p.z, 512.0f, 1.6f);   // swimming wake
                } else if (rand() % 4 == 0) {
                    dropAt(p.x, p.z, 384.0f, 0.6f);   // treading water
                }
                laraWake = 0.0f;
                laraWakePos = p;
            }
        } else {
            laraWakePos = p;
        }
    }

    // Level::waterDrop (Lara swimming / entering, bubbles popping, enemies, falling objects)
    void waterDrop(const vec3 &pos, float radius, float strength) {
        for (int i = 0; i < fieldsCount; i++) {
            GSWaterField &f = fields[i];
            if (fabsf(pos.y - f.y) > 512.0f) continue;
            if (pos.x < f.x0 || pos.z < f.z0 || pos.x > f.x0 + (f.w - 1) * GS_WAVE_CELL || pos.z > f.z0 + (f.h - 1) * GS_WAVE_CELL) continue;
            addDrop(f, pos.x, pos.z, radius, strength * GS_WAVE_DROP);
        }
    }

    void stepWaterField(GSWaterField &f) {
        // two-buffer wave equation with explicit speed: next = 2h - prev + c^2 * laplacian(h),
        // written into prev, then swapped. Reflecting (zero slope) boundary: a missing neighbour takes the
        // point's own height, so waves bounce off the walls and the shore vertices move with the water.
        for (int j = 0; j < f.h; j++) {
            int k = j * f.w;
            for (int i = 0; i < f.w; i++, k++) {
                if (!f.mask[k] || f.mask[k] == 3) continue;
                float h  = f.cur[k];
                float nl = (i > 0       && f.mask[k - 1])   ? f.cur[k - 1]   : h;
                float nr = (i < f.w - 1 && f.mask[k + 1])   ? f.cur[k + 1]   : h;
                float nu = (j > 0       && f.mask[k - f.w]) ? f.cur[k - f.w] : h;
                float nd = (j < f.h - 1 && f.mask[k + f.w]) ? f.cur[k + f.w] : h;
                float lap = nl + nr + nu + nd - 4.0f * h;
                float v = (2.0f * h - f.prev[k] + GS_WAVE_SPEED2 * lap) * GS_WAVE_DAMPING;
                f.prev[k] = (v > GS_WAVE_MAX) ? GS_WAVE_MAX : ((v < -GS_WAVE_MAX) ? -GS_WAVE_MAX : v);
            }
        }
        float *t = f.cur; f.cur = f.prev; f.prev = t;
    }

    void updateWaterFields() {
        laraWaterEvents();
        waveTimer = min(waveTimer + Core::deltaTime, GS_WAVE_STEP * 3.0f);
        int steps = 0;
        while (waveTimer >= GS_WAVE_STEP && steps < 3) {
            waveTimer -= GS_WAVE_STEP;
            steps++;
        }
        for (int r = 0; r < waterRoomsCount; r++) {
            GSWaterField *f = getWaterField(waterRooms[r]);
            if (!f) continue;
            // idle motion: weak random drops so calm water still moves
            f->idleTimer += Core::deltaTime;
            if (f->idleTimer > 0.35f) {
                f->idleTimer = 0.0f;
                for (int tries = 0; tries < 8; tries++) {
                    int i = 1 + rand() % max(1, f->w - 2);
                    int j = 1 + rand() % max(1, f->h - 2);
                    if (f->mask[j * f->w + i] == 2) {
                        addDrop(*f, f->x0 + i * GS_WAVE_CELL, f->z0 + j * GS_WAVE_CELL, GS_WAVE_CALM_RADIUS, GS_WAVE_CALM_AMOUNT);
                        break;
                    }
                }
            }
            for (int s = 0; s < steps; s++) {
                stepWaterField(*f);
            }
        }
    }

    // ---- surface grid: wave height, Fresnel opacity + fake reflection, specular, shore foam ----
    inline float fieldH(const GSWaterField &f, int i, int j) {
        i = clamp(i, 0, f.w - 1);
        j = clamp(j, 0, f.h - 1);
        return f.cur[j * f.w + i];
    }

    // Wave specular (0..1) at a grid point. Only the part caused by the waves: the reflection of a fixed
    // light for the wave normal minus the reflection a flat surface would give from the same view. The
    // flat part depends on the camera only (a big blob that drifts with the view); without it the
    // highlights sit on the wave flanks facing the light and move with the ripples. Plus a crest term.

    inline float specTerm(const vec3 &N, const vec3 &V, const vec3 &L) {
        vec3 R = N * (2.0f * N.dot(V)) - V;
        float sp = max(0.0f, R.dot(L));
        sp *= sp; sp *= sp; sp *= sp;   // ^8
        return sp;
    }

    float waterSpecular(const GSWaterField &f, int i, int j) {
        float h  = fieldH(f, i, j);
        float hx = (fieldH(f, i + 1, j) - fieldH(f, i - 1, j)) * 0.5f;
        float hz = (fieldH(f, i, j + 1) - fieldH(f, i, j - 1)) * 0.5f;
        vec3 P = vec3(f.x0 + i * GS_WAVE_CELL, f.y - h * GS_WAVE_HEIGHT, f.z0 + j * GS_WAVE_CELL);
        vec3 N = vec3(-hx * GS_WAVE_SHADE, -1.0f, -hz * GS_WAVE_SHADE).normal();
        vec3 V = (Core::viewPos.xyz() - P).normal();
        static const vec3 L = vec3(0.3f, -1.0f, 0.25f).normal();   // TR y down
        static const vec3 F = vec3(0.0f, -1.0f, 0.0f);             // flat water normal

        float wave  = specTerm(N, V, L) - specTerm(F, V, L);    // reflection added by the waves
        float crest = max(0.0f, h) * (1.0f / GS_WAVE_MAX);          // wave tops
        float sp = max(0.0f, wave) * GS_SPEC_GAIN + crest * GS_SPEC_CREST;
        return (sp > 1.0f) ? 1.0f : sp;
    }

    void waterVertex(GAPI::Vertex &v, const GSWaterField &f, int i, int j, float su, float sv) {
        const float s = 1.0f / 32767.0f;
        float h  = fieldH(f, i, j);
        float hx = (fieldH(f, i + 1, j) - fieldH(f, i - 1, j)) * 0.5f;
        float hz = (fieldH(f, i, j + 1) - fieldH(f, i, j - 1)) * 0.5f;

        vec3 P = vec3(f.x0 + i * GS_WAVE_CELL, f.y - h * GS_WAVE_HEIGHT, f.z0 + j * GS_WAVE_CELL);
        // up normal of y(x,z) = Y - A h (TR y points down)
        const float k = GS_WAVE_SHADE;   // exaggerated slope for shading
        vec3 N = vec3(-hx * k, -1.0f, -hz * k).normal();
        vec3 V = (Core::viewPos.xyz() - P).normal();
        // Fresnel from the flat water normal (view angle only): smooth, no white streaks along the waves;
        // the only highlight is the wave specular through the original texture (renderWaterFaces)
        float c = fabsf(V.y);

        // Fresnel: clear looking down, more opaque + darker teal "reflection" at grazing angles
        float fr = (1.0f - c) * (1.0f - c);    // ^2: Fresnel kicks in earlier than ^3
        fr = min(1.0f, fr * GS_WATER_FRESNEL);
        vec3 col = vec3(0.6f, 0.9f, 0.9f).lerp(vec3(0.45f, 0.7f, 0.78f), fr);
        float alpha = GS_WATER_ALPHA_MIN + fr * 0.7f;
        // (the wave specular is applied through the original texture, see renderWaterFaces)

        // shore foam
        if (f.mask[clamp(j, 0, f.h - 1) * f.w + clamp(i, 0, f.w - 1)] == 1) {
            float foam = 0.3f;
            col += vec3(foam);
            alpha += 0.19f;
        }

        v.coord.x = P.x * s;
        v.coord.y = P.y * s;
        v.coord.z = P.z * s;
        v.coord.w = 1.0f;
        v.normal.x = N.x; v.normal.y = N.y; v.normal.z = N.z; v.normal.w = 0.0f;
        v.texCoord.u = P.x / 1024.0f + su + hx * GS_WAVE_UV;
        v.texCoord.v = P.z / 1024.0f + sv + hz * GS_WAVE_UV;
        v.color.r = u8(clamp(col.x * 128.0f, 0.0f, 255.0f));
        v.color.g = u8(clamp(col.y * 128.0f, 0.0f, 255.0f));
        v.color.b = u8(clamp(col.z * 128.0f, 0.0f, 255.0f));
        v.color.a = u8(clamp(alpha * 128.0f, 0.0f, 128.0f));
        v.color.q = 1.0f;
    }

    void renderWaterTint() {
        if (!waterRoomsCount) return;

        updateWaterFields();

        GAPI::initWaterSurface();
        GAPI::Vertex *vtx = GAPI::waterVtx;
        Index        *idx = GAPI::waterIdx;
        int vCount = 0, iCount = 0;

        const float su = Core::params.x * 0.05f, sv = Core::params.x * 0.03f;

        for (int r = 0; r < waterRoomsCount; r++) {
            GSWaterField *f = getWaterField(waterRooms[r]);
            if (!f) continue;
            TR::Room &room = level.rooms[f->room];

            // 4x4 quads per surface sector, 5x5 vertices per sector (edges duplicated, same values)
            for (int sz = 0; sz < room.zSectors; sz++) {
                for (int sx = 0; sx < room.xSectors; sx++) {
                    if (!isSurfaceSector(room, sx, sz)) continue;
                    if (vCount + 25 > GS_WATER_MAX_VERTS || iCount + 96 > GS_WATER_MAX_INDICES) break;

                    int base = vCount;
                    for (int j = 0; j <= 4; j++) {
                        for (int i = 0; i <= 4; i++) {
                            waterVertex(vtx[vCount++], *f, sx * 4 + i, sz * 4 + j, su, sv);
                        }
                    }
                    for (int j = 0; j < 4; j++) {
                        for (int i = 0; i < 4; i++) {
                            Index a = Index(base + j * 5 + i);
                            idx[iCount++] = a;     idx[iCount++] = a + 1; idx[iCount++] = a + 6;
                            idx[iCount++] = a;     idx[iCount++] = a + 6; idx[iCount++] = a + 5;
                        }
                    }
                }
            }
        }

        if (!iCount) return;

        Core::mModel.identity();
        Core::setFog(owner->camera->isUnderwater() ? owner->underwaterFogParams : owner->levelFogParams);
        Core::setBlendMode(bmAlpha);
        Core::setDepthWrite(false);
        Core::setCullMode(cmNone);
        Core::setAlphaTest(false);
        Core::validateRenderState();

        GAPI::drawWaterMesh(vCount, iCount, GAPI::waterTex);

        renderWaterFaces();

        Core::setDepthWrite(true);
        Core::setCullMode(cmFront);
        Core::setBlendMode(bmNone);
    }

    // original PS1 (animated) water texture on the same displaced wave grid, translucent
    void renderWaterFaces() {
        GAPI::Vertex *vtx = GAPI::waterVtx;
        Index        *idx = GAPI::waterIdx;
        int vCount = 0, iCount = 0;
        int curTile = -1, curClut = -1;

        const float s = 1.0f / 32767.0f;

        // crossfade the texture animation: current frame at (1 - t), next frame at t
        float blend = clamp(owner->animTexTimer / ANIM_TEX_TIMESTEP, 0.0f, 1.0f);

        for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < waterRoomsCount; r++) {
            GSWaterField *f = getWaterField(waterRooms[r]);
            if (!f) continue;
            TR::Room &room = level.rooms[f->room];

            for (int sz = 0; sz < room.zSectors; sz++) {
                for (int sx = 0; sx < room.xSectors; sx++) {
                    int si = sz * room.xSectors + sx;
                    if (f->faceTex[si] < 0) continue;
                    int texIndex;
                    float weight;
                    if (f->faceTexNext[si] < 0) {       // not animated: one full pass
                        if (pass) continue;
                        texIndex = f->faceTex[si];
                        weight   = 1.0f;
                    } else {
                        texIndex = pass ? f->faceTexNext[si] : f->faceTex[si];
                        weight   = pass ? blend : 1.0f - blend;
                        if (weight < 0.02f) continue;
                    }
                    const TR::TextureInfo &t = level.objectTextures[texIndex];

                    if (iCount && (t.tile != curTile || t.clut != curClut || vCount + 25 > GS_WATER_MAX_VERTS)) {
                        owner->atlasRooms->bindTile(curTile, curClut);
                        GAPI::drawWaterMesh(vCount, iCount, owner->atlasRooms);
                        vCount = iCount = 0;
                    }
                    curTile = t.tile;
                    curClut = t.clut;

                    // corner UVs (tile space, texCoordAtlas is texel * 128 + 64)
                    float cu[4], cv[4];
                    for (int c = 0; c < 4; c++) {
                        const short2 &uv = t.texCoordAtlas[f->faceMap[si * 4 + c]];
                        cu[c] = float(uv.x) / 32767.0f;
                        cv[c] = float(uv.y) / 32767.0f;
                    }

                    int base = vCount;
                    for (int j = 0; j <= 4; j++) {
                        for (int i = 0; i <= 4; i++) {
                            int gi = sx * 4 + i, gj = sz * 4 + j;
                            float h = fieldH(*f, gi, gj);
                            // position inside the face rectangle (the face may span several sectors)
                            const float *rc = &f->faceRect[si * 4];
                            float wx = f->x0 + gi * GS_WAVE_CELL, wz = f->z0 + gj * GS_WAVE_CELL;
                            float fx = (wx - rc[0]) / (rc[2] - rc[0]);
                            float fz = (wz - rc[1]) / (rc[3] - rc[1]);
                            GAPI::Vertex &v = vtx[vCount++];
                            v.coord.x = (f->x0 + gi * GS_WAVE_CELL) * s;
                            v.coord.y = (f->y - h * GS_WAVE_HEIGHT) * s;
                            v.coord.z = (f->z0 + gj * GS_WAVE_CELL) * s;
                            v.coord.w = 1.0f;
                            v.normal.x = v.normal.z = v.normal.w = 0.0f;
                            v.normal.y = -1.0f;
                            v.texCoord.u = (cu[0] * (1 - fx) + cu[1] * fx) * (1 - fz) + (cu[2] * (1 - fx) + cu[3] * fx) * fz;
                            v.texCoord.v = (cv[0] * (1 - fx) + cv[1] * fx) * (1 - fz) + (cv[2] * (1 - fx) + cv[3] * fx) * fz;
                            // the original texture masks the specular: visible (and bright) where the
                            // wave reflects the light, faint elsewhere
                            float m = GS_WATER_SPEC_MIN + waterSpecular(*f, gi, gj) * GS_WATER_SPECULAR;
                            m = (m > 1.0f) ? 1.0f : m;
                            // mask = opacity only: never brighter than the original texture (a brightness boost
                            // turned its faint uniform texels into untextured-looking white blobs)
                            v.color.r = v.color.g = v.color.b = 0x74;
                            v.color.a = uint8(clamp(m * weight * GS_WATER_FACE_ALPHA * 2.0f * 128.0f, 0.0f, 128.0f));
                            v.color.q = 1.0f;
                        }
                    }
                    for (int j = 0; j < 4; j++) {
                        for (int i = 0; i < 4; i++) {
                            Index a = Index(base + j * 5 + i);
                            idx[iCount++] = a;     idx[iCount++] = a + 1; idx[iCount++] = a + 6;
                            idx[iCount++] = a;     idx[iCount++] = a + 6; idx[iCount++] = a + 5;
                        }
                    }
                }
            }
        }

        if (iCount) {
            owner->atlasRooms->bindTile(curTile, curClut);
            GAPI::drawWaterMesh(vCount, iCount, owner->atlasRooms);
        }
    }
};

#endif
