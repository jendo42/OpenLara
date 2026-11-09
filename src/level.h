#ifndef H_LEVEL
#define H_LEVEL

#include "core.h"
#include "utils.h"
#include "format.h"
#include "cache.h"
#include "enemy.h"
#include "camera.h"
#include "lara.h"
#include "objects.h"
#include "inventory.h"
#include "savegame.h"
#include "network.h"
#include "extension.h"

#if defined(_DEBUG) && defined(_GAPI_GL) && !defined(_GAPI_GLES)
    #define DEBUG_RENDER
#endif

#ifdef DEBUG_RENDER
    #include "debug.h"
#endif

#define ANIM_TEX_TIMESTEP (10.0f / 30.0f)
#define SKY_TIME_PERIOD   (1.0f / 0.005f)

extern void loadLevelAsync(Stream *stream, void *userData);

extern Array<SaveSlot> saveSlots;
extern SaveResult saveResult;
extern int loadSlot;

struct Level : IGame {

    TR::Level   level;
    Texture     *atlasRooms;
    Texture     *atlasObjects;
    Texture     *atlasSprites;
    Texture     *atlasGlyphs;
    MeshBuilder *mesh;

    Lara        *players[2], *player;
    Camera      *camera;
    Texture     *shadow[2];
    Texture     *scaleTex;

    struct Params {
        float   time;
        float   waterHeight;
        float   reserved[2];
    } *params;

    ZoneCache    *zoneCache;
    AmbientCache *ambientCache;
    WaterCache   *waterCache;

    Sound::Sample *sndTrack, *sndWater;
    bool waitTrack;

    bool lastTitle;
    bool isEnded;
    bool needRedrawTitleBG;
    bool needRedrawReflections;
    bool needRenderGame;
    bool needRenderInventory;
    bool showStats;
    bool skyIsVisible;
    bool paused;

    TR::LevelID nextLevel;

    TR::Effect::Type effect;
    float      effectTimer;
    int        effectIdx;
    float      cutsceneWaitTimer;
    float      animTexTimer;
    float      statsTimeDelta;

    vec3 underwaterColor;
    vec4 underwaterFogParams;
    vec4 levelFogParams;

    mat4 mLightProj[2];

// IGame implementation ========
    virtual void loadLevel(TR::LevelID id) {
        sndWater = sndTrack = NULL;
        Sound::stopAll();
        nextLevel = id;
    }

    virtual void loadNextLevel() {
        if (nextLevel != TR::LVL_MAX) return;

        TR::LevelID id = TR::LVL_MAX;
    //#ifdef _OS_WEB
    //    if (level.id == TR::LVL_TR1_2 && level.version != TR::VER_TR1_PC)
    //        id = TR::LVL_TR1_TITLE;
    //    else
    //#endif
        id = (level.isEnd() || level.isHome()) ? level.getTitleId() : TR::LevelID(level.id + 1);

        TR::isGameEnded = level.isEnd();

        if (!level.isTitle() && loadSlot == -1) {
        // update statistics info for current level
            if (!TR::isCutsceneLevel(level.id) && !level.isHome())
                saveGame(level.id, false, true);
        // save next level
            if (!TR::isCutsceneLevel(id) && !TR::isTitleLevel(id)) {
                saveGame(id, false, false);
                loadSlot  = getSaveSlot(id, false);
                showStats = true;
            }
        }
        loadLevel(id);
    }

    virtual void invShow(int playerIndex, int page, int itemIndex = -1) {
        if (itemIndex != -1 || page == Inventory::PAGE_SAVEGAME)
            inventory->pageItemIndex[page] = itemIndex;
        inventory->toggle(playerIndex, Inventory::Page(page));
    }

    SaveSlot createSaveSlot(TR::LevelID id, bool checkpoint, bool dummy = false) {
        SaveSlot slot;

        // allocate oversized data for save slot
        slot.data  = new uint8[sizeof(SaveStats) + sizeof(SaveItem) * inventory->itemsCount + // for every save
                               sizeof(SaveState) + sizeof(SaveEntity) * level.entitiesCount]; // only for checkpoints

        uint8 *ptr = slot.data;

    // level progress stats
        SaveStats *stats = (SaveStats*)ptr;
        if (!checkpoint)
            memset(stats, 0, sizeof(*stats));
        else
            *stats = saveStats;
        stats->level      = id;
        stats->checkpoint = checkpoint;
        ptr += sizeof(*stats);

    // inventory items
        int32 *itemsCount = (int32*)ptr;
        ptr += sizeof(*itemsCount);

        *itemsCount = 0;
        if (dummy) {
            SaveItem *item = (SaveItem*)ptr;
            ptr += sizeof(*item);

            item->type  = TR::Entity::INV_PISTOLS;
            item->count = UNLIMITED_AMMO;

            *itemsCount = 1;
        } else {
            for (int i = 0; i < inventory->itemsCount; i++) {
                Inventory::Item *invItem = inventory->items[i];

                if (!TR::Entity::isPickup(TR::Level::convFromInv(invItem->type)))
                    continue;

                if (!checkpoint) {
                    if (!TR::Entity::isCrossLevelItem(TR::Level::convFromInv(invItem->type)))
                        continue;
                    if (TR::isEmptyLevel(id))
                        continue;
                }

                SaveItem *item = (SaveItem*)ptr;
                ptr += sizeof(*item);

                item->type  = invItem->type;
                item->count = invItem->count;

                (*itemsCount)++;
            }
        }

        if (checkpoint) {
        // level state
            SaveState *state = (SaveState*)ptr;
            ptr += sizeof(*state);
            *state = level.state;

        // level entities
            int32 *entitiesCount = (int32*)ptr;
            ptr += sizeof(*entitiesCount);

            *entitiesCount = 0;
            for (int i = 0; i < level.entitiesCount; i++) {
                Controller *controller = (Controller*)level.entities[i].controller;
                SaveEntity *entity = (SaveEntity*)ptr;
                if (!controller || !controller->getSaveData(*entity)) continue;
                ptr += (sizeof(SaveEntity) - sizeof(SaveEntity::Extra)) + entity->extraSize;
                (*entitiesCount)++;
            }
        }

        slot.size = int32(ptr - slot.data);

        return slot;
    }

    void parseSaveSlot(const SaveSlot &slot) {
        clearInventory();

        uint8 *data = slot.data;
        uint8 *ptr  = data;

    // level progress stats
        if (slot.isCheckpoint())
            saveStats = *(SaveStats*)ptr; // start level current position

        ptr += sizeof(saveStats);

    // inventory items
        int32 itemsCount = *(int32*)ptr;
        ptr += sizeof(itemsCount);

        for (int i = 0; i < itemsCount; i++) {
            SaveItem *item = (SaveItem*)ptr;
            inventory->add(TR::Entity::Type(item->type), item->count, false);
            ptr += sizeof(*item);
        }

        if (slot.isCheckpoint()) {
            clearEntities();

        // level state
            level.state = *(SaveState*)ptr;
            ptr += sizeof(level.state);

        // level entities
            int32 entitiesCount = *(int32*)ptr;
            ptr += sizeof(entitiesCount);

            for (int i = 0; i < entitiesCount; i++) {
                SaveEntity *entity = (SaveEntity*)ptr;

                Controller *controller;
                if (i >= level.entitiesBaseCount)
                    controller = addEntity(TR::Entity::Type(entity->type), entity->room, vec3(float(entity->x), float(entity->y), float(entity->z)), TR::angle(entity->rotation));
                else
                    controller = (Controller*)level.entities[i].controller;

                controller->setSaveData(*entity);
                if (Controller::first != controller && controller->flags.state != TR::Entity::asNone) {
                    controller->next = Controller::first;
                    Controller::first = controller;
                }

                if (controller->getEntity().isLara()) {
                    Lara *lara = (Lara*)controller;
                    if (lara->camera)
                        lara->camera->reset();
                }

                ptr += (sizeof(SaveEntity) - sizeof(SaveEntity::Extra)) + entity->extraSize;
            }

            if (level.state.flags.flipped) {
                flipMap();
                level.state.flags.flipped = true;
            }

            uint8 track = level.state.flags.track;
            level.state.flags.track = 0;
            playTrack(track);
        }

        statsTimeDelta = 0.0f;
    }

    static void saveGameWriteAsync(Stream *stream, void *userData) {
        if (stream != NULL) {
            delete[] stream->data;
            delete stream;
            saveResult = SAVE_RESULT_SUCCESS;
            UI::showHint(STR_HINT_SAVING_DONE, 1.0f);
        } else {
            saveResult = SAVE_RESULT_ERROR;
            UI::showHint(STR_HINT_SAVING_ERROR, 3.0f);
        }
    }

    virtual void saveGame(TR::LevelID id, bool checkpoint, bool updateStats) {
        ASSERT(saveResult != SAVE_RESULT_WAIT);

        if (saveResult == SAVE_RESULT_WAIT)
            return;

        LOG("Save Game...\n");

        SaveSlot slot;
        if (updateStats) {
            removeSaveSlot(id, true);
            int index = getSaveSlot(id, false);
            if (index == -1) {
                slot = createSaveSlot(id, false, true);
                saveSlots.push(slot);
            } else
                slot = saveSlots[index];
            SaveStats *stats = (SaveStats*)slot.data;
            stats->level      = level.id;
            stats->checkpoint = checkpoint;
        } else {
            removeSaveSlot(id, checkpoint); // remove checkpoints and level saves
            saveSlots.push(createSaveSlot(id, checkpoint));
        }

        saveSlots.sort();

        if (!updateStats) {
            saveResult = SAVE_RESULT_WAIT;
            UI::showHint(STR_HINT_SAVING, 60.0f);

            int size;
            uint8 *data = writeSaveSlots(size);
            osWriteSlot(new Stream(SAVE_FILENAME, (const char*)data, size, saveGameWriteAsync, this));
        }
    }

    virtual void loadGame(int slot) {
        LOG("Load Game...\n");
        loadSlot = slot;
    }

    void clearInventory() {
        int i = inventory->itemsCount;

        while (i--) {
            if (TR::Entity::isPickup(TR::Level::convFromInv(inventory->items[i]->type)))
                inventory->remove(i);
        }
    }

    void clearEntities() {
        Controller::first = NULL;
        for (int i = 0; i < level.entitiesCount; i++) {
            TR::Entity &e = level.entities[i];
            Controller *controller = (Controller*)e.controller;
            if (controller) {
                controller->next = NULL;
                controller->flags.state = TR::Entity::asNone;
                if (i >= level.entitiesBaseCount) {

                    if (e.type == TR::Entity::ENEMY_SKATEBOARD) {
                        continue;
                    }

                    delete controller;
                    e.controller = NULL;
                }
            }
        }
    }

    void initShadow() {
        delete shadow[0];
        delete shadow[1];
        shadow[0] = shadow[1] = NULL;
    #ifndef FFP
        if (Core::settings.detail.shadows > Core::Settings::LOW) {
            if (level.isTitle())
                shadow[0] = new Texture(32, 32, 1, FMT_SHADOW); // init dummy shadow map
            else
                shadow[0] = new Texture(SHADOW_TEX_SIZE, SHADOW_TEX_SIZE, 1, FMT_SHADOW, OPT_TARGET);
        }
    #endif
    }

    virtual void applySettings(const Core::Settings &settings) {
        if (settings.detail.filter != Core::settings.detail.filter) {
            atlasRooms->setFilterQuality(settings.detail.filter);
            atlasObjects->setFilterQuality(settings.detail.filter);
            atlasSprites->setFilterQuality(settings.detail.filter);
        }

        bool rebuildMesh    = settings.detail.water    != Core::settings.detail.water;
        bool rebuildAmbient = settings.detail.lighting != Core::settings.detail.lighting;
        bool rebuildShadows = settings.detail.shadows  != Core::settings.detail.shadows;
        bool rebuildWater   = settings.detail.water    != Core::settings.detail.water;
        bool switchModels   = settings.detail.simple   != Core::settings.detail.simple;

        bool rebuildShaders = rebuildWater || rebuildAmbient || rebuildShadows;

        bool redraw = memcmp(&settings.detail, &Core::settings.detail, sizeof(settings.detail)) != 0;

        bool toggleVR = (settings.detail.stereo == Core::Settings::STEREO_VR) ^ (Core::settings.detail.stereo == Core::Settings::STEREO_VR);

        Core::settings = settings;

        if (toggleVR) {
            osToggleVR(Core::settings.detail.stereo == Core::Settings::STEREO_VR);
        }

        Core::setVSync(Core::settings.detail.vsync != 0);

        Stream::cacheWrite("settings", (char*)&settings, sizeof(settings));

        if (rebuildShaders) {
        #if !defined(_GAPI_D3D8) && !defined(_GAPI_D3D9) && !defined(_GAPI_D3D11) && !defined(_GAPI_GXM)
            delete shaderCache;
            shaderCache = new ShaderCache();
        #endif
        }

        if (rebuildMesh) {
            delete mesh;
            mesh = new MeshBuilder(&level, atlasRooms);
        }

        if (rebuildAmbient) {
            delete ambientCache;
            ambientCache = Core::settings.detail.lighting > Core::Settings::MEDIUM ? new AmbientCache(this) : NULL;
        }

        if (rebuildShadows)
            initShadow();
            
        if (rebuildWater) {
            delete waterCache;
            waterCache = Core::settings.detail.water > Core::Settings::LOW ? new WaterCache(this) : NULL;
        }

        if (redraw && inventory->active && !level.isTitle())
            needRedrawTitleBG = true;

        if (switchModels)
            resetModels();
    }

    virtual TR::Level* getLevel() {
        return &level;
    }

    virtual MeshBuilder* getMesh() {
        return mesh;
    }

    virtual ICamera* getCamera(int index = -1) {
        if (index == -1)
            return camera;
        if (players[index])
            return players[index]->camera;
        return NULL;
    }

    virtual Controller* getLara(int index = 0) {
        return players[index];
    }

    virtual Controller* getLara(const vec3 &pos) {
        if (!players[1])
            return players[0];
        if (players[0]->health <= 0.0f)
            return players[1];
        if (players[1]->health <= 0.0f)
            return players[0];
        return (players[0]->pos - pos).length2() < (players[1]->pos - pos).length2() ? players[0] : players[1];
    }

    virtual bool isCutscene() {
        if (level.isTitle()) return false;
        return camera->mode == Camera::MODE_CUTSCENE;
    }

    virtual uint16 getRandomBox(uint16 zone, uint16 *zones) { 
        ZoneCache::Item *item = zoneCache->getBoxes(zone, zones);
        return item->boxes[rand() % item->count];
    }
    
    virtual uint16 findPath(int ascend, int descend, bool big, int boxStart, int boxEnd, uint16 *zones, uint16 **boxes) {
        return zoneCache->findPath(ascend, descend, big, boxStart, boxEnd, zones, boxes);
    }

    void updateBlocks(bool rise) {
        for (int i = 0; i < level.entitiesBaseCount; i++) {
            Controller *controller = (Controller*)level.entities[i].controller;
            switch (level.entities[i].type) {
                case TR::Entity::BLOCK_1 :
                case TR::Entity::BLOCK_2 :
                case TR::Entity::BLOCK_3 :
                case TR::Entity::BLOCK_4 :
                case TR::Entity::BLOCK_5 :
                    ((Block*)controller)->updateFloor(rise);
                    break;
                case TR::Entity::MOVING_BLOCK :
                    ((MovingBlock*)controller)->updateFloor(rise);
                    break;
                default : ;
            }
        }
    }

    virtual void flipMap(bool water = true) {
        updateBlocks(false);
        if (water && waterCache) waterCache->flipMap();
    #ifdef _GAPI_GS
        gsFreeWaterFields();
    #endif
        mesh->flipMap();
        level.flipMap();
        updateBlocks(true);
    }

    virtual void setWaterParams(float height) {
        params->waterHeight = height;
    }

#ifndef GS_WATER_WAVES
    #define GS_WATER_WAVES 1    // PS2: wave fields + water surface layers (debug switch)
#endif

    virtual void waterDrop(const vec3 &pos, float radius, float strength) {
        if (waterCache)
            waterCache->addDrop(pos, radius, strength);
    #ifdef _GAPI_GS
        if (!waterCache)
            gsWaterDrop(pos, radius, strength);
    #endif
    }

    virtual void setShader(Core::Pass pass, Shader::Type type, bool underwater = false, bool alphaTest = false) {
        shaderCache->bind(pass, type, (underwater ? ShaderCache::FX_UNDERWATER : 0) | (alphaTest ? ShaderCache::FX_ALPHA_TEST : 0));
    }

    virtual void setRoomParams(int roomIndex, Shader::Type type, float diffuse, float ambient, float specular, float alpha, bool alphaTest = false) {
        if (Core::pass == Core::passShadow) {
            setShader(Core::pass, type, false, alphaTest);
            return;
        }

        TR::Room &room = level.rooms[roomIndex];

        if (room.dynLightsCount) {
            Core::lightPos[3]   = room.dynLights[0].pos;
            Core::lightColor[3] = room.dynLights[0].color;
        } else {
            Core::lightPos[3]   = vec4(0);
            Core::lightColor[3] = vec4(0, 0, 0, 1);
        }

        if (type == Shader::SPRITE) {
            alphaTest = true;
        }

    #ifdef FFP
        switch (type) {
            case Shader::SPRITE :
            case Shader::ROOM   :
                ambient = 1.0f;
                Core::lightColor[0].w = 1.0f;
                break;
            case Shader::FLASH  :
            case Shader::MIRROR :
                ambient = 1.0f;
                Core::lightColor[0].w = 
                Core::lightColor[1].w =
                Core::lightColor[2].w =
                Core::lightColor[3].w = 1.0f;
                break;
            default : ;
        }
    #endif

        vec4 material;
        if (Core::pass == Core::passAmbient) {
            if (room.flags.water) {
                material = vec4(underwaterColor, 1.0f);
            } else {
                material = vec4(1.0f);
            }
        } else {
            material = vec4(diffuse, ambient, specular, alpha);
        }
        
        setShader(Core::pass, type, (Core::pass == Core::passAmbient) ? false : room.flags.water, alphaTest);

        if (room.flags.water) {
            Core::setFog(underwaterFogParams);
        } else {
            Core::setFog(levelFogParams);
        }

        #ifdef _GAPI_SW
            GAPI::setPalette(room.flags.water ? GAPI::swPaletteWater : GAPI::swPaletteColor);
            GAPI::setShading(true);
        #endif

        Core::setMaterial(material.x, material.y, material.z, material.w);

        if (room.flags.water) {
            if (waterCache) {
                waterCache->bindCaustics(roomIndex);
            }
            setWaterParams(float(room.waterLevel[level.state.flags.flipped]));
        } else {
            setWaterParams(NO_WATER_HEIGHT);
        }

        Core::active.shader->setParam(uParam, Core::params);

        Core::updateLights();

        if (Core::settings.detail.shadows > Core::Settings::MEDIUM) {
            Core::active.shader->setParam(uContacts, Core::contacts[0], MAX_CONTACTS);
        }
    }

    virtual void setupBinding() {
        Core::whiteTex->bind(sNormal);
        Core::whiteTex->bind(sMask);
        Core::whiteTex->bind(sReflect);
        atlasRooms->bind(sDiffuse);

        if (Core::pass != Core::passShadow) {
            Texture *shadowMap = shadow[player ? player->camera->cameraIndex : 0];
            if (shadowMap) shadowMap->bind(sShadow);
        }

        Core::basis.identity();
    }

    virtual void renderEnvironment(int roomIndex, const vec3 &pos, Texture **targets, int stride = 0, Core::Pass pass = Core::passAmbient) {
        #ifdef FFP
            return;
        #endif

        #ifdef _GAPI_SW
            return;
        #endif

        #ifdef _GAPI_C3D
            GAPI::rotate90 = false;
        #endif

        #ifdef _GAPI_D3D8
            GAPI::setFrontFace(false);
        #endif

        PROFILE_MARKER("ENVIRONMENT");
        setupBinding();
        float      tmpEye  = Core::eye;
        Core::Pass tmpPass = Core::pass;
        Core::eye = 0.0f;

        int16 rIndex = roomIndex;
        level.getSector(rIndex, pos); // fix room index for overlapped blocks

    // render level into cube faces or texture images
        for (int i = 0; i < 6; i++) {
            setupCubeCamera(pos, i);
            Core::pass = pass;
            if (targets[0]->opt & OPT_CUBEMAP) {
                Core::setTarget(targets[0], NULL, RT_CLEAR_COLOR | RT_CLEAR_DEPTH | RT_STORE_COLOR, i);
            } else {
                Core::setTarget(targets[i * stride], NULL, RT_CLEAR_COLOR | RT_CLEAR_DEPTH | RT_STORE_COLOR);
            }
            renderView(rIndex, false, false);
        }

        #ifdef _GAPI_D3D8
            GAPI::setFrontFace(true);
        #endif

        #ifdef _GAPI_C3D
            GAPI::rotate90 = true;
        #endif

        Core::pass = tmpPass;
        Core::eye  = tmpEye;
    }

    virtual void renderModelFull(int modelIndex, bool underwater, Basis *joints) {
        vec4 ambient[6] = { vec4(0), vec4(0), vec4(0), vec4(0), vec4(0), vec4(0) };

        // opaque
        Core::setBlendMode(bmPremult); // inventory items has fade-out/in alpha
        mesh->transparent = 0;
        setShader(Core::passCompose, Shader::ENTITY, underwater, false);
        Core::setBasis(joints, level.models[modelIndex].mCount);
        Core::active.shader->setParam(uMaterial, Core::active.material);
        Core::active.shader->setParam(uAmbient, ambient[0], 6);
        Core::setFog(FOG_NONE);
        Core::updateLights();
        mesh->renderModel(modelIndex, underwater);
        // transparent
        mesh->transparent = 1;
        setShader(Core::passCompose, Shader::ENTITY, underwater, true);
        Core::setBasis(joints, level.models[modelIndex].mCount);
        Core::active.shader->setParam(uMaterial, Core::active.material);
        Core::active.shader->setParam(uAmbient, ambient[0], 6);
        Core::setFog(FOG_NONE);
        Core::updateLights();
        mesh->renderModel(modelIndex, underwater);
        // additive
        Core::setBlendMode(bmAdd);
        Core::setDepthWrite(false);
        mesh->transparent = 2;
        setShader(Core::passCompose, Shader::ENTITY, underwater, false);
        Core::setBasis(joints, level.models[modelIndex].mCount);
        Core::active.shader->setParam(uMaterial, Core::active.material);
        Core::active.shader->setParam(uAmbient, ambient[0], 6);
        Core::setFog(FOG_NONE);
        Core::updateLights();
        mesh->renderModel(modelIndex, underwater);
        Core::setDepthWrite(true);
        Core::setBlendMode(bmNone);
        mesh->transparent = 0;
    }
    
    virtual void setEffect(Controller *controller, TR::Effect::Type effect) {
        this->effect      = effect;
        this->effectTimer = 0.0f;
        this->effectIdx   = 0;

        switch (effect) {
            case TR::Effect::FLOOR_SHAKE :
                for (int i = 0; i < 2; i++)
                    if (players[i] && players[i]->camera)
                        players[i]->camera->shake = 0.5f * max(0.0f, 1.0f - (controller->pos - players[i]->camera->eye.pos).length2() / (15 * 1024 * 15 * 1024));
                return;
            case TR::Effect::FLOOD : {
                Sound::Sample *sample = playSound(TR::SND_FLOOD);
                if (sample)
                    sample->setVolume(0.0f, 4.0f);
                break;
            }
            case TR::Effect::STAIRS2SLOPE :
                playSound(TR::SND_STAIRS2SLOPE);
                break;
            case TR::Effect::EXPLOSION :
                playSound(TR::SND_TNT);
                shakeCamera(1.0f);
                break;
            default : ;
        }
    }

    virtual void checkTrigger(Controller *controller, bool heavy) {
        players[0]->checkTrigger(controller, heavy);
    }

    virtual void shakeCamera(float value, bool add = false) {
        for (int i = 0; i < 2; i++)
            if (players[i] && players[i]->camera) {
                if (add)
                    players[i]->camera->shake += value;
                else
                    players[i]->camera->shake  = value;
            }
    }


    virtual Controller* addEntity(TR::Entity::Type type, int room, const vec3 &pos, float angle) {
        int index;
        for (index = level.entitiesBaseCount; index < level.entitiesCount; index++) {
            TR::Entity &e = level.entities[index];
            if (!e.controller) {
                e.type          = type;
                e.room          = room;
                e.x             = int(pos.x);
                e.y             = int(pos.y);
                e.z             = int(pos.z);
                e.rotation      = TR::angle(normalizeAngle(angle));
                e.intensity     = -1;
                e.flags.value   = 0;
                e.flags.smooth  = true;
                e.modelIndex    = level.getModelIndex(e.type);
                break;
            }
        }

        if (index == level.entitiesCount)
            return NULL;

        TR::Entity &e = level.entities[index];
        if (e.isPickup())
            e.intensity = 4096;
        else
            if (e.isSprite()) {
                if (e.type == TR::Entity::LAVA_PARTICLE || e.type == TR::Entity::FLAME)
                    e.intensity = 0; // emissive
                else
                    e.intensity = 0x1FFF - level.rooms[room].ambient;
            }

        Controller *controller = initController(index);
        e.controller = controller;

        if (e.isEnemy() || e.isSprite()) {
            controller->flags.active = TR::ACTIVE;
            controller->activate();
        }

        return controller;
    }

    virtual void removeEntity(Controller *controller) {
        level.entities[controller->entity].controller = NULL;
        delete controller;
    }

    virtual void addMuzzleFlash(Controller *owner, int joint, const vec3 &offset, int lightIndex) {
        MuzzleFlash *mf = (MuzzleFlash*)addEntity(TR::Entity::MUZZLE_FLASH, owner->getRoomIndex(), offset, 0);
        if (mf) {
            mf->owner      = owner;
            mf->joint      = joint;
            mf->lightIndex = lightIndex;
        }
    }

    virtual bool invUse(int playerIndex, TR::Entity::Type type) {
        if (!players[playerIndex]->useItem(type))
            return inventory->use(type);
        return true;
    }

    virtual void invAdd(TR::Entity::Type type, int count) {
        inventory->add(type, count);
    }

    virtual int* invCount(TR::Entity::Type type) { 
        return inventory->getCountPtr(type);
    }

    virtual bool invChooseKey(int playerIndex, TR::Entity::Type hole) {
        return inventory->chooseKey(playerIndex, hole);
    }

    virtual Sound::Sample* playSound(int id, const vec3 &pos = vec3(0.0f), int flags = 0) const {
        if (level.version == TR::VER_TR1_PSX && id == TR::SND_SECRET)
            return NULL;

        if (!level.soundsInfo) return NULL;

        int16 a = level.soundsMap[id];
        if (a == -1) return NULL;

        TR::SoundInfo &b = level.soundsInfo[a];
        if (b.chance == 0 || randf() <= b.chance) {
            int   index  = b.index + rand() % b.flags.count;
            float volume = b.volume;
            float pitch  = 1.0f + (b.flags.pitch ? ((randf() - 0.5f) * b.pitch) : 0.0f);

            if (level.version == TR::VER_TR2_PSX) // fix for 8 kHz VAG in PSX TR2
                pitch *= 8000.0f / 11025.0f;

            if (!(flags & Sound::MUSIC)) {
                switch (b.flags.mode) {
                    case 0 : if (level.version & TR::VER_TR1) flags |= Sound::UNIQUE; break;
                    case 1 : flags |= (level.version & TR::VER_TR1) ? Sound::REPLAY : Sound::UNIQUE; break;
                    case 2 : if (level.version & TR::VER_TR1)    flags |= Sound::LOOP; break;
                    case 3 : if (!(level.version & TR::VER_TR1)) flags |= Sound::LOOP | Sound::UNIQUE; break;
                }
            }
            if (b.flags.gain) volume = max(0.0f, volume - randf() * 0.25f);
            //if (b.flags.camera) flags &= ~Sound::PAN;
            return Sound::play(level.getSampleStream(index), &pos, volume, pitch, flags, id);
        }
        return NULL;
    }

    void stopChannel(Sound::Sample *channel) {
        if (channel == sndTrack) {
            sndTrack = NULL;
            if (level.state.flags.track != TR::LEVEL_INFO[level.id].track && TR::LEVEL_INFO[level.id].track != TR::NO_TRACK) // play ambient track
                playTrack(0);
        }
    }

    struct TrackRequest {
        Level *level;
        int   flags;

        TrackRequest(Level *level, int flags) : level(level), flags(flags) {}
    };

    static void playAsync(Stream *stream, void *userData) {
        TrackRequest *req = (TrackRequest*)userData;
        Level *level = req->level;
        level->waitTrack = false;
        if (stream) {
            level->sndTrack = Sound::play(stream, NULL, 0.01f, 1.0f, req->flags);
            if (level->sndTrack) {
                if (level->level.isCutsceneLevel()) {
                    Core::resetTime();
                }
                level->sndTrack->volume = level->sndTrack->volumeTarget = 0.0f;
            }
        }

        delete req;
    }

    static void playAsyncBG(Stream *stream, void *userData) {
        TrackRequest *req = (TrackRequest*)userData;
        if (stream)
            Sound::play(stream, NULL, 1.0f, 1.0f, req->flags);
        delete req;
    }

    virtual void playTrack(uint8 track, bool background = false) {
        if (background) {
            TR::getGameTrack(level.version, track, playAsyncBG, new TrackRequest(this, Sound::MUSIC));
            return;
        }

        if (track == 0) {
            if (sndTrack) return;
            track = TR::LEVEL_INFO[level.id].track;
        }

        if (level.state.flags.track == track) {
        //    if (sndTrack) {
        //        sndTrack->replay();
        //        sndTrack->setVolume(1.0f, 0.2f);
        //    }
            return;
        }

        level.state.flags.track = track;

        if (sndTrack) {
            sndTrack->setVolume(-1.0f, 0.2f);
            sndTrack = NULL;
        }

        if (track == 0xFF) return;

        int flags = Sound::MUSIC;
        if (track == TR::LEVEL_INFO[level.id].track)
            flags |= Sound::LOOP;

        waitTrack = true;
        TR::getGameTrack(level.version, track, playAsync, new TrackRequest(this, flags));

        UI::showSubs(TR::getSubs(level.version, track));
    }

    virtual void stopTrack() {
        playTrack(0xFF);
    }
//==============================

    Level(Stream &stream) : level(stream), waitTrack(false), isEnded(false), cutsceneWaitTimer(0.0f), animTexTimer(0.0f), statsTimeDelta(0.0f) {
        paused = false;

        level.simpleItems = Core::settings.detail.simple == 1;
        level.initModelIndices();

    #ifdef _GAPI_GU
        GAPI::freeEDRAM();
    #endif
        nextLevel = TR::LVL_MAX;
        showStats = false;

        params = (Params*)&Core::params;
        params->time = 0.0f;

        memset(players, 0, sizeof(players));
        player = NULL;

        underwaterColor     = vec3(0.6f, 0.9f, 0.9f);
        underwaterFogParams = vec4(underwaterColor * 0.2f, 1.0f / (6 * 1024));
        levelFogParams      = TR::getFogParams(level.id);

        inventory->game = this;

        if (!level.isCutsceneLevel()) {
            inventory->reset();
            memset(&saveStats, 0, sizeof(saveStats));
            saveStats.level = level.id;
        }

        initTextures();
        mesh = new MeshBuilder(&level, atlasRooms);
        initEntities();

        shadow[0] = shadow[1] = NULL;
        scaleTex     = NULL;
        camera       = NULL;
        ambientCache = NULL;
        waterCache   = NULL;
        zoneCache    = NULL;

        needRedrawTitleBG = false;
        needRedrawReflections = true;

        initShadow();

        if (!(lastTitle = level.isTitle())) {
            ASSERT(players[0] != NULL);
            player = players[0];
            camera = player->camera;

            zoneCache    = new ZoneCache(this);
            ambientCache = Core::settings.detail.lighting > Core::Settings::MEDIUM ? new AmbientCache(this) : NULL;
            waterCache   = Core::settings.detail.water    > Core::Settings::LOW    ? new WaterCache(this)   : NULL;

            if (ambientCache) { // at first calculate ambient cube for Lara
                AmbientCache::Cube cube;
                ambientCache->getAmbient(players[0]->getRoomIndex(), players[0]->pos, cube); // add to queue
            }

            for (int i = 0; i < level.soundSourcesCount; i++) {
                TR::SoundSource &src = level.soundSources[i];
                int flags = Sound::PAN;
                if (src.flags & 64)  flags |= Sound::FLIPPED;
                if (src.flags & 128) flags |= Sound::UNFLIPPED;
                playSound(src.id, vec3(float(src.x), float(src.y), float(src.z)), flags);
            }

        }

        effect  = TR::Effect::NONE;

        sndWater = sndTrack = NULL;
        UI::init(this);

        /*
        if (level.id == TR::LVL_TR2_RIG) {
            lara->animation.setAnim(level.models[level.extra.laraSpec].animation);
            camera->doCutscene(lara->pos, lara->angle.y);
        }
        */

    #if DUMP_SAMPLES
        for (int i = 0; i < 256; i++) {
            int16 a = level.soundsMap[i];
            if (a == -1) continue;
            ASSERT(a < level.soundsInfoCount);
            TR::SoundInfo &b = level.soundsInfo[a];
            for (int j = 0; j < b.flags.count; j++) {
                //ASSERT((b.index + j) < level.soundOffsetsCount);
                if ((b.index + j) < level.soundOffsetsCount) {
                    Debug::Level::dumpSample(&level, b.index + j, i, j);
                }
            }
        }
        loadNextLevel();
    #endif

    #if DUMP_PALETTE
        Debug::Level::dumpPalette(&level, level.id);
        loadNextLevel();
    #endif

        saveResult = SAVE_RESULT_SUCCESS;
        if (loadSlot != -1 && saveSlots[loadSlot].getLevelID() == level.id) {
            parseSaveSlot(saveSlots[loadSlot]);
            loadSlot = -1;
        }

        Network::start(this);

        Core::resetTime();
    }

    virtual ~Level() {
    #ifdef _GAPI_GS
        gsFreeWaterFields();
    #endif
        UI::init(NULL);

        Network::stop();

        for (int i = 0; i < level.entitiesCount; i++)
            delete (Controller*)level.entities[i].controller;

        delete shadow[0];
        delete shadow[1];
        delete scaleTex;
        delete ambientCache;
        delete waterCache;
        delete zoneCache;

        delete atlasRooms;
        #ifndef SPLIT_BY_TILE
            delete atlasObjects;
            delete atlasSprites;
            delete atlasGlyphs;
        #endif
        delete mesh;

        Sound::stopAll();
    }

    void init(bool playLogo, bool playVideo) {
        inventory->init(playLogo, playVideo);
    }

    void initEntities() {
        for (int i = 0; i < level.entitiesBaseCount; i++) {
            TR::Entity &e = level.entities[i];
            e.controller = initController(i);
            if (e.type == TR::Entity::LARA || ((level.version & TR::VER_TR1) && e.type == TR::Entity::CUT_1))
                players[0] = (Lara*)e.controller;
        }

        Sound::listenersCount = 1;
    }

    void resetModels() {
        level.simpleItems = Core::settings.detail.simple == 1;
        level.initModelIndices();
        for (int i = 0; i < level.entitiesCount; i++) {
            TR::Entity &e = level.entities[i];
            if (!e.controller) continue;
            Controller *controller = (Controller*)e.controller;
            controller->updateModel();
        }
    }

    void addPlayer(int index) {
        if (level.isCutsceneLevel()) return;

        Controller *c = Controller::first;
        while (c) {
            Controller *next = c->next;
            if (c->getEntity().type == TR::Entity::FLAME && ((Flame*)c)->owner == players[index])
                removeEntity(c);
            c = next;
        }

        if (!players[index]) {
            players[index] = (Lara*)addEntity(TR::Entity::LARA, 0, vec3(0.0f), 0.0f);
            players[index]->camera->cameraIndex = index;
            Sound::listenersCount = 2;
        } else if (index == 1) {
            removePlayer(index);
            Sound::listenersCount = 1;
            return;
        }

        Lara *lead = players[index ^ 1];
        if (!lead) return;

        players[index]->reset(lead->getRoomIndex(), lead->pos, lead->angle.y, lead->stand);
    }

    void removePlayer(int index) {
        for (int i = 0; i < level.entitiesCount; i++) {
            if (level.entities[i].controller && level.entities[i].isEnemy()) {
                Enemy *e = (Enemy*)level.entities[i].controller;
                if (e->target == players[index]) {
                    e->target = NULL;
                }
            }
        }

        removeEntity(players[index]);
        players[index] = NULL;
    }

    Controller* initController(int index) {
        if (level.entities[index].type == TR::Entity::CUT_1 && (level.version & TR::VER_TR1))
            return new Lara(this, index);

        switch (level.entities[index].type) {
            case TR::Entity::LARA                  : return new Lara(this, index);
            case TR::Entity::ENEMY_DOPPELGANGER    : return new Doppelganger(this, index);
            case TR::Entity::ENEMY_WOLF            : return new Wolf(this, index);
            case TR::Entity::ENEMY_BEAR            : return new Bear(this, index);
            case TR::Entity::ENEMY_BAT             : return new Bat(this, index);
            case TR::Entity::ENEMY_PUMA            :
            case TR::Entity::ENEMY_LION_MALE       :
            case TR::Entity::ENEMY_LION_FEMALE     : return new Lion(this, index);
            case TR::Entity::ENEMY_RAT_LAND        :
            case TR::Entity::ENEMY_RAT_WATER       : return new Rat(this, index);
            case TR::Entity::ENEMY_REX             : return new Rex(this, index);
            case TR::Entity::ENEMY_RAPTOR          : return new Raptor(this, index);
            case TR::Entity::ENEMY_MUTANT_1        :
            case TR::Entity::ENEMY_MUTANT_2        :
            case TR::Entity::ENEMY_MUTANT_3        : return new Mutant(this, index);
            case TR::Entity::ENEMY_CENTAUR         : return new Centaur(this, index);
            case TR::Entity::ENEMY_MUMMY           : return new Mummy(this, index);
            case TR::Entity::ENEMY_CROCODILE_LAND  :
            case TR::Entity::ENEMY_CROCODILE_WATER : return new Crocodile(this, index);
            case TR::Entity::ENEMY_GORILLA         : return new Gorilla(this, index);
            case TR::Entity::ENEMY_LARSON          : return new Larson(this, index);
            case TR::Entity::ENEMY_PIERRE          : return new Pierre(this, index);
            case TR::Entity::ENEMY_SKATEBOY        : return new SkaterBoy(this, index);
            case TR::Entity::ENEMY_COWBOY          : return new Cowboy(this, index);
            case TR::Entity::ENEMY_MR_T            : return new MrT(this, index);
            case TR::Entity::ENEMY_NATLA           : return new Natla(this, index);
            case TR::Entity::ENEMY_GIANT_MUTANT    : return new GiantMutant(this, index);
            case TR::Entity::DOOR_1                :
            case TR::Entity::DOOR_2                :
            case TR::Entity::DOOR_3                :
            case TR::Entity::DOOR_4                :
            case TR::Entity::DOOR_5                :
            case TR::Entity::DOOR_6                :
            case TR::Entity::DOOR_7                :
            case TR::Entity::DOOR_8                : return new Door(this, index);
            case TR::Entity::TRAP_DOOR_1           :
            case TR::Entity::TRAP_DOOR_2           : return new TrapDoor(this, index);
            case TR::Entity::BRIDGE_1              :
            case TR::Entity::BRIDGE_2              :
            case TR::Entity::BRIDGE_3              : return new Bridge(this, index);
            case TR::Entity::GEARS_1               :
            case TR::Entity::GEARS_2               :
            case TR::Entity::GEARS_3               : return new Gear(this, index);
            case TR::Entity::INV_KEY_ITEM_1        :
            case TR::Entity::INV_KEY_ITEM_2        :
            case TR::Entity::INV_KEY_ITEM_3        :
            case TR::Entity::INV_KEY_ITEM_4        : return new KeyItemInv(this, index);
            case TR::Entity::TRAP_FLOOR            : return new TrapFloor(this, index);
            case TR::Entity::CRYSTAL               : return new Crystal(this, index);
            case TR::Entity::TRAP_SWING_BLADE      : return new TrapSwingBlade(this, index);
            case TR::Entity::TRAP_SPIKES           : return new TrapSpikes(this, index);
            case TR::Entity::TRAP_BOULDER          : 
            case TR::Entity::TRAP_BOULDERS         : return new TrapBoulder(this, index);
            case TR::Entity::DART                  : return new Dart(this, index);
            case TR::Entity::TRAP_DART_EMITTER     : return new TrapDartEmitter(this, index);
            case TR::Entity::DRAWBRIDGE            : return new Drawbridge(this, index);
            case TR::Entity::BLOCK_1               :
            case TR::Entity::BLOCK_2               :
            case TR::Entity::BLOCK_3               :
            case TR::Entity::BLOCK_4               :
            case TR::Entity::BLOCK_5               : return new Block(this, index);
            case TR::Entity::MOVING_BLOCK          : return new MovingBlock(this, index);
            case TR::Entity::TRAP_CEILING_1        :
            case TR::Entity::TRAP_CEILING_2        : return new TrapCeiling(this, index);
            case TR::Entity::TRAP_SLAM             : return new TrapSlam(this, index);
            case TR::Entity::TRAP_SWORD            : return new TrapSword(this, index);
            case TR::Entity::HAMMER_HANDLE         : return new ThorHammer(this, index);
            case TR::Entity::HAMMER_BLOCK          : return new ThorHammerBlock(this, index);
            case TR::Entity::LIGHTNING             : return new Lightning(this, index);
            case TR::Entity::MOVING_OBJECT         : return new MovingObject(this, index);
            case TR::Entity::SWITCH                :
            case TR::Entity::SWITCH_WATER          :
            case TR::Entity::SWITCH_BUTTON         : 
            case TR::Entity::SWITCH_BIG            : return new Switch(this, index);
            case TR::Entity::PUZZLE_HOLE_1         :
            case TR::Entity::PUZZLE_HOLE_2         :
            case TR::Entity::PUZZLE_HOLE_3         :
            case TR::Entity::PUZZLE_HOLE_4         :
            case TR::Entity::KEY_HOLE_1            :
            case TR::Entity::KEY_HOLE_2            :
            case TR::Entity::KEY_HOLE_3            :
            case TR::Entity::KEY_HOLE_4            : return new KeyHole(this, index);
            case TR::Entity::MIDAS_HAND            : return new MidasHand(this, index);
            case TR::Entity::SCION_TARGET          : return new ScionTarget(this, index);
            case TR::Entity::WATERFALL             : return new Waterfall(this, index);
            case TR::Entity::NATLA_BULLET          :
            case TR::Entity::MUTANT_BULLET         :
            case TR::Entity::CENTAUR_BULLET        : return new Bullet(this, index);
            case TR::Entity::TRAP_LAVA             : return new TrapLava(this, index);
            case TR::Entity::BUBBLE                : return new Bubble(this, index);
            case TR::Entity::EXPLOSION             : return new Explosion(this, index);
            case TR::Entity::WATER_SPLASH          :
            case TR::Entity::BLOOD                 :
            case TR::Entity::SMOKE                 :
            case TR::Entity::SPARKLES              : return new Sprite(this, index, true, Sprite::FRAME_ANIMATED);
            case TR::Entity::RICOCHET              : return new Sprite(this, index, true, Sprite::FRAME_RANDOM);
            case TR::Entity::CENTAUR_STATUE        : return new CentaurStatue(this, index);
            case TR::Entity::CABIN                 : return new Cabin(this, index);
            case TR::Entity::MUZZLE_FLASH          : return new MuzzleFlash(this, index);
            case TR::Entity::LAVA_PARTICLE         : ASSERT(false); return NULL;
            case TR::Entity::TRAP_LAVA_EMITTER     : return new TrapLavaEmitter(this, index);
            case TR::Entity::FLAME                 : return new Flame(this, index);
            case TR::Entity::TRAP_FLAME_EMITTER    : return new TrapFlameEmitter(this, index);
            case TR::Entity::BOAT                  : return new Boat(this, index);
            case TR::Entity::EARTHQUAKE            : return new Earthquake(this, index);
            case TR::Entity::MUTANT_EGG_SMALL      :
            case TR::Entity::MUTANT_EGG_BIG        : return new MutantEgg(this, index);

            case TR::Entity::ENEMY_DOG              : return new Dog(this, index);
            case TR::Entity::ENEMY_TIGER            : return new Tiger(this, index);
            case TR::Entity::ENEMY_GOON_MASK_1      :
            case TR::Entity::ENEMY_GOON_MASK_2      :
            case TR::Entity::ENEMY_GOON_MASK_3      :
            case TR::Entity::ENEMY_GOON_KNIFE       :
            case TR::Entity::ENEMY_GOON_SHOTGUN     :
            case TR::Entity::ENEMY_RAT              :
            case TR::Entity::ENEMY_DRAGON_FRONT     :
            case TR::Entity::ENEMY_DRAGON_BACK      :
            case TR::Entity::ENEMY_SHARK            :
            case TR::Entity::ENEMY_MORAY_1          :
            case TR::Entity::ENEMY_MORAY_2          :
            case TR::Entity::ENEMY_BARACUDA         :
            case TR::Entity::ENEMY_DIVER            :
            case TR::Entity::ENEMY_GUNMAN_1         :
            case TR::Entity::ENEMY_GUNMAN_2         :
            case TR::Entity::ENEMY_GOON_STICK_1     :
            case TR::Entity::ENEMY_GOON_STICK_2     :
            case TR::Entity::ENEMY_GOON_FLAME       :
            case TR::Entity::UNUSED_23              :
            case TR::Entity::ENEMY_SPIDER           :
            case TR::Entity::ENEMY_SPIDER_GIANT     :
            case TR::Entity::ENEMY_CROW             :
            case TR::Entity::ENEMY_MARCO            :
            case TR::Entity::ENEMY_GUARD_SPEAR        :
            case TR::Entity::ENEMY_GUARD_SPEAR_STATUE :
            case TR::Entity::ENEMY_GUARD_SWORD        :
            case TR::Entity::ENEMY_GUARD_SWORD_STATUE :
            case TR::Entity::ENEMY_YETI             :
            case TR::Entity::ENEMY_BIRD_MONSTER     :
            case TR::Entity::ENEMY_EAGLE            :
            case TR::Entity::ENEMY_MERCENARY_1      :
            case TR::Entity::ENEMY_MERCENARY_2      :
            case TR::Entity::ENEMY_MERCENARY_3      :
            case TR::Entity::ENEMY_MERCENARY_SNOWMOBILE :
            case TR::Entity::ENEMY_MONK_1           :
            case TR::Entity::ENEMY_MONK_2           : return new Enemy(this, index, 100, 10, 0.0f, 0.0f);
            case TR::Entity::ENEMY_WINSTON          : return new Winston(this, index);

            case TR::Entity::CRYSTAL_PICKUP         : return new CrystalPickup(this, index);
            case TR::Entity::STONE_ITEM_1           :
            case TR::Entity::STONE_ITEM_2           :
            case TR::Entity::STONE_ITEM_3           :
            case TR::Entity::STONE_ITEM_4           : return new StoneItem(this, index);

            case TR::Entity::WINDOW_1               :
            case TR::Entity::WINDOW_2               : return new BreakableWindow(this, index);

            case TR::Entity::HELICOPTER_FLYING      : return new HelicopterFlying(this, index);

            case TR::Entity::FISH_EMITTER           : return new DummyController(this, index);

            default                                 : return new Controller(this, index);
        }
    }

    #define ATLAS_PAGE_BARS   4096
    #define ATLAS_PAGE_GLYPHS 8192

    AtlasTile *tileData;
    uint8 *glyphsRU;
    uint8 *glyphsJA;
    uint8 *glyphsGR;
    uint8 *glyphsCN;

    static int getAdvGlyphPage(int index) {
        index -= UI::advGlyphsStart;
        if (index >= RU_GLYPH_COUNT) {
            index -= RU_GLYPH_COUNT;
            if (index >= JA_GLYPH_COUNT) {
                index -= JA_GLYPH_COUNT;
                if (index >= GR_GLYPH_COUNT) {
                    index -= GR_GLYPH_COUNT;
                    return 4 + index / 256; // CN
                } else {
                    return 3; // GR
                }
            } else {
                return 1 + index / 256; // JA
            }
        }
        return 0; // RU
    }

    static void fillCallback(Atlas *atlas, int id, int tileX, int tileY, int atlasWidth, int atlasHeight, Atlas::Tile &tile, void *userData, void *data) {
        static const uint32 CommonTexData[CTEX_MAX][25] = {
            // flash bar
                { 0x00000000, 0xFFA20058, 0xFFFFFFFF, 0xFFA20058, 0x00000000 },
            // health bar
                { 0xFF2C5D71, 0xFF5E81AE, 0xFF2C5D71, 0xFF1B4557, 0xFF16304F },
            // oxygen bar
                { 0xFF647464, 0xFFA47848, 0xFF647464, 0xFF4C504C, 0xFF303030 },
            // option bar
                { 0x00000000, 0x20202020, 0x20202020, 0x20202020, 0x00000000,
                  0x00000000, 0x60606060, 0x60606060, 0x60606060, 0x00000000,
                  0x00000000, 0x80808080, 0x80808080, 0x80808080, 0x00000000,
                  0x00000000, 0x60606060, 0x60606060, 0x60606060, 0x00000000,
                  0x00000000, 0x20202020, 0x20202020, 0x20202020, 0x00000000 },
            // white room
                { 0xFFFFFFFF },
            // white object
                { 0xFFFFFFFF },
            // white sprite
                { 0xFFFFFFFF },
            };

        int stride = 256, uvCount;
        short2 *uv = NULL;

        Level *owner = (Level*)userData;
        TR::Level *level = &owner->level;

        AtlasColor *src, *dst = (AtlasColor*)data;
        short4 mm;

        bool isSprite = false;

        if (id < level->objectTexturesCount) { // textures
            TR::TextureInfo &t = level->objectTextures[id];
            mm      = t.getMinMax();
            src     = owner->tileData->color;
            uv      = t.texCoordAtlas;
            uvCount = 4;
            if (data) {
                level->fillObjectTexture(owner->tileData, tile.uv, tile.tex);
            }
        } else {
            id -= level->objectTexturesCount;

            if (id < level->spriteTexturesCount) { // sprites
                TR::TextureInfo &t = level->spriteTextures[id];
                mm       = t.getMinMax();
                src      = owner->tileData->color;
                uv       = t.texCoordAtlas;
                uvCount  = 2;
                isSprite = true;
                if (data) {
                    if (id < UI::advGlyphsStart) {
                        level->fillObjectTexture(owner->tileData, tile.uv, tile.tex);
                    } else {
                        int page = getAdvGlyphPage(id);
                        int offset = ATLAS_PAGE_GLYPHS + page * 256;
                        short4 uv = tile.uv;
                        uv.y -= offset;
                        uv.w -= offset;
                        Color32 *glyphsData = NULL;

                        switch (page) {
                            case 0  : glyphsData = (Color32*)owner->glyphsRU; break;
                            case 1  :
                            case 2  : glyphsData = (Color32*)owner->glyphsJA + (page - 1) * 256 * 256; break;
                            case 3  : glyphsData = (Color32*)owner->glyphsGR; break;
                            case 4  :
                            case 5  :
                            case 6  :
                            case 7  : glyphsData = (Color32*)owner->glyphsCN + (page - 4) * 256 * 256; break;
                            default : ASSERT(false);
                        }

                        level->fillObjectTexture32(owner->tileData, glyphsData, uv, tile.tex);
                    }
                }
            } else { // common (generated) textures
                id -= level->spriteTexturesCount;

                TR::TextureInfo *tex;
                mm.x = mm.y = mm.z = mm.w = 0;
                stride  = 1;
                uvCount = 4;

                switch (id) {
                    case CTEX_FLASH        :
                    case CTEX_HEALTH       :
                    case CTEX_OXYGEN       : 
                    case CTEX_OPTION       :
                    case CTEX_WHITE_ROOM   :
                    case CTEX_WHITE_OBJECT :
                    case CTEX_WHITE_SPRITE :
                        src = owner->tileData->color;
                        tex = &CommonTex[id];
                        if (id != CTEX_WHITE_ROOM && id != CTEX_WHITE_OBJECT && id != CTEX_WHITE_SPRITE) {
                            mm.w = 4; // height - 1
                            if (id == CTEX_OPTION) {
                                stride = 5;
                                mm.z   = 4;
                            }
                        }

                        for (int i = 0; i < (mm.z + 1) * (mm.w + 1); i++) {
                            src[i] = ((Color32*)&CommonTexData[id])[i];
                        }
                        break;
                    default : return;
                }

                uv = tex->texCoordAtlas;
                uv[2].y += mm.w;
                uv[3].y += mm.w;
                uv[1].x += mm.z;
                uv[2].x += mm.z;
            }
        }

        int cx = -mm.x, cy = -mm.y;

        if (data) {
            int w = mm.z - mm.x + 1;
            int h = mm.w - mm.y + 1;
            dst += tileY * atlasWidth + tileX;
            for (int y = -atlas->border.y; y < h + atlas->border.w; y++) {
                for (int x = -atlas->border.x; x < w + atlas->border.z; x++) {
                    AtlasColor *p = &src[mm.y * stride + mm.x];
                    ASSERT((x + atlas->border.x + tileX) >= 0 && (x + atlas->border.x + tileX) < atlasWidth);
                    ASSERT((y + atlas->border.y + tileY) >= 0 && (y + atlas->border.y + tileY) < atlasHeight);
                    p += clamp(x, 0, w - 1);
                    p += clamp(y, 0, h - 1) * stride;

                    if (isSprite && (y < 0 || y >= h || x < 0 || x >= w)) {
                        dst[x + atlas->border.x].value = 0;
                    } else {
                        dst[x + atlas->border.x] = *p;
                    }
                }
                dst += atlasWidth;
            }

            cx += tileX + atlas->border.x;
            cy += tileY + atlas->border.y;
        }

        for (int i = 0; i < uvCount; i++) {
            if (uv[i].x == mm.z) uv[i].x++;
            if (uv[i].y == mm.w) uv[i].y++;
            uv[i].x += cx;
            uv[i].y += cy;

            uv[i].x = int32(uv[i].x) * 32767 / atlasWidth;
            uv[i].y = int32(uv[i].y) * 32767 / atlasHeight;
        }

    // apply ref for instanced tile
        if (data) return;

        int ref = tileX;
        if (ref < level->objectTexturesCount) { // textures
            mm = level->objectTextures[ref].getMinMaxAtlas();
        } else {
            ref -= level->objectTexturesCount;
            if (ref < level->spriteTexturesCount) { // sprites
                mm = level->spriteTextures[ref].getMinMaxAtlas();
            } else {
                ASSERT(!"only object textures and sprites can be instanced");
            }
        }

        for (int i = 0; i < uvCount; i++) {
            uv[i].x += mm.x;
            uv[i].y += mm.y;
        }
    }

#ifdef _DEBUG
    void dumpGlyphs() {
        ASSERT(level.tiles8);

        TR::SpriteSequence &seq = level.spriteSequences[level.extra.glyphs];
        short2 size = short2(0, 0);
        for (int i = 0; i < seq.sCount; i++) {
            TR::TextureInfo &sprite = level.spriteTextures[seq.sStart + i];
            short w = sprite.texCoord[1].x - sprite.texCoord[0].x + 1;
            short h = sprite.texCoord[1].y - sprite.texCoord[0].y + 1;
            size.y += h + 1;
            size.x = max(size.x, w);
        }
        size.x += 1;
        size.y += 1;
        size.x = (size.x + 3) / 4 * 4;
        Color32 *data = new Color32[int(size.x) * int(size.y)];
        memset(data, 128, int(size.x) * int(size.y) * 4);
        
        short2 pos = short2(1, 1);
        for (int i = 0; i < seq.sCount; i++) {
            TR::TextureInfo &sprite = level.spriteTextures[seq.sStart + i];
            short w = sprite.texCoord[1].x - sprite.texCoord[0].x + 1;
            short h = sprite.texCoord[1].y - sprite.texCoord[0].y + 1;

            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    Tile8 &tile = level.tiles8[sprite.tile];
                    
                    data[pos.x + x + (pos.y + y) * size.x] = level.getColor(tile.index[sprite.texCoord[0].x + x + (sprite.texCoord[0].y + y) * 256]);
                }
            pos.y += h + 1;
        }

        Texture::SaveBMP("pc_glyph.bmp", (char*)data, size.x, size.y);
        delete[] data;
    }

    void dumpKanji() {
        Stream stream("DATA/KANJI.PSX");
        int size = stream.readLE32() / 2;
        ColorIndex4 *buffer = new ColorIndex4[size];
        stream.raw(buffer, size);
        int width = 256;
        int height = size / width * 2;

        Color32 *image = new Color32[width * height];

        Tile4 *tile = (Tile4*)buffer;
        CLUT  &clut = level.cluts[level.spriteTextures[level.kanjiSprite].clut];

        ColorIndex4 *idx = buffer;
        Color32 *ptr = image;

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += 2) {
                *ptr++ = clut.color[idx->a];
                *ptr++ = clut.color[idx->b];
                idx++;
            }
        }

        Texture::SaveBMP("kanji", (char*)image, width, height);

        delete[] buffer;
        delete[] image;
    }
#endif

#ifdef _GAPI_GS
    // Shadow map: the entity's silhouette is rendered from the light direction (orthographic) into a
    // small target (GAPI::shadowMapBegin), then one quad on the floor plane, textured with it, darkens the
    // floor. Overlapping parts can not double-darken and the quad has a correct depth (no drawing over Lara).
    #define GS_SHADOW_DARKNESS  0.5f     // opacity on the ground
    #define GS_SHADOW_FADE      1024.0f  // height above the floor where the shadow has faded out
    #define GS_SHADOW_MIN_Y     0.6f     // light direction: min vertical component (max tilt ~53 deg)
    #define GS_SHADOW_LIFT      6.0f     // shadow quad above the floor (avoids fighting with it)
    #define GS_SHADOW_FROM_LIGHT 0       // 0: top-down shadow, 1: cast away from the main room light
    #define GS_SHADOW_SOFT      2.0f     // soft edge: blur tap offset in shadow map texels (0 = hard)

    void renderShadowGS(Controller *controller) {
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
#endif

    void initTextures() {
    #ifndef SPLIT_BY_TILE

        #if defined(_GAPI_SW) || defined(_GAPI_GU) || defined(_GAPI_GS)
            #error atlas packing is not allowed for this platform
        #endif

        #ifdef _DEBUG
            //dumpGlyphs();
            //dumpKanji();
        #endif

        UI::patchGlyphs(level);

        {
            uint32 glyphsW, glyphsH;
            Stream stream(NULL, GLYPH_RU, size_GLYPH_RU);
            glyphsRU = Texture::LoadPNG(stream, glyphsW, glyphsH);
        }

        {
            uint32 glyphsW, glyphsH;
            Stream stream(NULL, GLYPH_JA, size_GLYPH_JA);
            glyphsJA = Texture::LoadBMP(stream, glyphsW, glyphsH);
        }

        {
            uint32 glyphsW, glyphsH;
            Stream stream(NULL, GLYPH_GR, size_GLYPH_GR);
            glyphsGR = Texture::LoadBMP(stream, glyphsW, glyphsH);
        }

        {
            uint32 glyphsW, glyphsH;
            Stream stream(NULL, GLYPH_CN, size_GLYPH_CN);
            glyphsCN = Texture::LoadBMP(stream, glyphsW, glyphsH);
        }

    // repack texture tiles
        int maxTiles = level.objectTexturesCount + level.spriteTexturesCount + CTEX_MAX;
        Atlas *rAtlas = new Atlas(maxTiles, short4(4, 4, 4, 4), this, fillCallback);
        Atlas *oAtlas = new Atlas(maxTiles, short4(4, 4, 4, 4), this, fillCallback);
        Atlas *sAtlas = new Atlas(maxTiles, short4(4, 4, 4, 4), this, fillCallback);
        Atlas *gAtlas = new Atlas(maxTiles, short4(0, 0, 1, 1), this, fillCallback);
        // add textures
        for (int i = 0; i < level.objectTexturesCount; i++) {
            TR::TextureInfo &t = level.objectTextures[i];
            if (t.tile == 0xFFFF) continue;

            short4 uv;
            uv.x = min(min(t.texCoord[0].x, t.texCoord[1].x), t.texCoord[2].x);
            uv.y = min(min(t.texCoord[0].y, t.texCoord[1].y), t.texCoord[2].y);
            uv.z = max(max(t.texCoord[0].x, t.texCoord[1].x), t.texCoord[2].x) + 1;
            uv.w = max(max(t.texCoord[0].y, t.texCoord[1].y), t.texCoord[2].y) + 1;

            if (t.type == TR::TEX_TYPE_ROOM)
                rAtlas->add(i, uv, &t);
            else
                oAtlas->add(i, uv, &t);
        }
        // add sprites
        for (int i = 0; i < level.spriteTexturesCount; i++) {
            TR::TextureInfo &t = level.spriteTextures[i];
            if (t.tile == 0xFFFF) continue;

            short4 uv;
            uv.x = t.texCoord[0].x;
            uv.y = t.texCoord[0].y;
            uv.z = t.texCoord[1].x + 1;
            uv.w = t.texCoord[1].y + 1;

            if (i >= UI::advGlyphsStart) {
             // add virtual UV offset for additional glyph sprites
                int offset = ATLAS_PAGE_GLYPHS + getAdvGlyphPage(i) * 256;
                uv.y += offset;
                uv.w += offset;
            }

            if (level.extra.glyphs != -1) {
                TR::SpriteSequence &seq = level.spriteSequences[level.extra.glyphs];
                if ((i >= seq.sStart && i < seq.sStart + seq.sCount) || i >= UI::advGlyphsStart) {
                    gAtlas->add(level.objectTexturesCount + i, uv, &t);
                    continue;
                }
            }
            sAtlas->add(level.objectTexturesCount + i, uv, &t);
        }
        // add common textures
        const short2 CommonTexOffset[] = { short2(1, 5), short2(1, 5), short2(1, 5), short2(5, 5), short2(1, 1), short2(1, 1), short2(1, 1) };
        ASSERT(COUNT(CommonTexOffset) == CTEX_MAX);
        memset(CommonTex, 0, sizeof(CommonTex));
        for (int i = 0; i < CTEX_MAX; i++) {
            CommonTex[i].type = CommonTex[i].dataType = TR::TEX_TYPE_SPRITE;
            Atlas *dst = (i == CTEX_FLASH || i == CTEX_WHITE_OBJECT) ? oAtlas : ((i == CTEX_WHITE_ROOM) ? rAtlas : gAtlas);
            dst->add(level.objectTexturesCount + level.spriteTexturesCount + i, short4(i * 32, ATLAS_PAGE_BARS, i * 32 + CommonTexOffset[i].x, ATLAS_PAGE_BARS + CommonTexOffset[i].y), &CommonTex[i]);
        }

        // get result texture
        tileData = new AtlasTile();
        
        atlasRooms   = rAtlas->pack(OPT_MIPMAPS | OPT_VRAM_3DS);
        atlasObjects = oAtlas->pack(OPT_MIPMAPS);
        atlasSprites = sAtlas->pack(OPT_MIPMAPS);
        atlasGlyphs  = gAtlas->pack(0);

    #ifdef _OS_3DS
        ASSERT(atlasRooms->width   <= 1024 && atlasRooms->height   <= 1024);
        ASSERT(atlasObjects->width <= 1024 && atlasObjects->height <= 1024);
        ASSERT(atlasSprites->width <= 1024 && atlasSprites->height <= 1024);
    #endif

        delete[] tileData;
        tileData = NULL;

        delete[] glyphsRU;
        delete[] glyphsJA;
        delete[] glyphsGR;
        delete[] glyphsCN;

        glyphsRU = NULL;
        glyphsJA = NULL;
        glyphsGR = NULL;
        glyphsCN = NULL;

        atlasRooms->setFilterQuality(Core::settings.detail.filter);
        atlasObjects->setFilterQuality(Core::settings.detail.filter);
        atlasSprites->setFilterQuality(Core::settings.detail.filter);
        atlasGlyphs->setFilterQuality(Core::Settings::MEDIUM);

        delete rAtlas;
        delete oAtlas;
        delete sAtlas;
        delete gAtlas;

        LOG("rooms   : %d x %d\n", atlasRooms->width, atlasRooms->height);
        LOG("objects : %d x %d\n", atlasObjects->width, atlasObjects->height);
        LOG("sprites : %d x %d\n", atlasSprites->width, atlasSprites->height);
        LOG("glyphs  : %d x %d\n", atlasGlyphs->width, atlasGlyphs->height);
        PROFILE_LABEL(TEXTURE, atlasRooms->ID, "atlas_rooms");
        PROFILE_LABEL(TEXTURE, atlasObjects->ID, "atlas_objects");
        PROFILE_LABEL(TEXTURE, atlasSprites->ID, "atlas_sprites");
        PROFILE_LABEL(TEXTURE, atlasGlyphs->ID, "atlas_glyphs");

    #else
        ASSERT(level.tilesCount);

        #if defined(_GAPI_SW)
            atlasRooms   =
            atlasObjects =
            atlasSprites =
            atlasGlyphs  = new Texture(level.tiles8, level.tilesCount);
            GAPI::initPalette(level.palette, level.lightmap);
        #elif defined(_GAPI_GU) || defined(_GAPI_GS)
            atlasRooms   =
            atlasObjects =
            atlasSprites =
            atlasGlyphs  = new Texture(level.tiles4, level.tilesCount, level.cluts, level.clutsCount);
        #ifdef _GAPI_GS
            initWhiteTexel();
        #endif
        #else
            Texture::Tile *tiles = new Texture::Tile[level.tilesCount];
            for (int i = 0; i < level.tilesCount; i++) {
                tiles[i].width = tiles[i].height = 256;
                tiles[i].data = new uint32[256 * 256];
            }

            for (int i = 0; i < level.objectTexturesCount; i++) {
                TR::TextureInfo &t = level.objectTextures[i];
                short4 uv = t.getMinMax();
                uv.z++;
                uv.w++;
                level.fillObjectTexture((AtlasTile*)tiles[t.tile].data, uv, &t);
            }

            for (int i = 0; i < level.spriteTexturesCount; i++) {
                TR::TextureInfo &t = level.spriteTextures[i];
                short4 uv = t.getMinMax();
                uv.z++;
                uv.w++;
                level.fillObjectTexture((AtlasTile*)tiles[t.tile].data, uv, &t);
            }

            for (int i = 0; i < level.tilesCount; i++) {
                char buf[256];
                sprintf(buf, "texture/%s_%d.png", TR::LEVEL_INFO[level.id].name, i);
                if (Stream::exists(buf)) {
                    Stream stream(buf);
                    delete[] tiles[i].data;
                    tiles[i].data = (uint32*)Texture::LoadPNG(stream, tiles[i].width, tiles[i].height);
                }
            }

            atlasRooms   =
            atlasObjects =
            atlasSprites =
            atlasGlyphs  = new Texture(tiles, level.tilesCount);

            for (int i = 0; i < level.tilesCount; i++)
                delete[] tiles[i].data;
            delete[] tiles;
        #endif

        #ifndef _GAPI_SW
        for (int i = 0; i < level.objectTexturesCount; i++) {
            TR::TextureInfo &t = level.objectTextures[i];

            t.texCoordAtlas[0].x <<= 7;
            t.texCoordAtlas[0].y <<= 7;
            t.texCoordAtlas[1].x <<= 7;
            t.texCoordAtlas[1].y <<= 7;
            t.texCoordAtlas[2].x <<= 7;
            t.texCoordAtlas[2].y <<= 7;
            t.texCoordAtlas[3].x <<= 7;
            t.texCoordAtlas[3].y <<= 7;

            t.texCoordAtlas[0].x += 64;
            t.texCoordAtlas[0].y += 64;
            t.texCoordAtlas[1].x += 64;
            t.texCoordAtlas[1].y += 64;
            t.texCoordAtlas[2].x += 64;
            t.texCoordAtlas[2].y += 64;
            t.texCoordAtlas[3].x += 64;
            t.texCoordAtlas[3].y += 64;
        }

        for (int i = 0; i < level.spriteTexturesCount; i++) {
            TR::TextureInfo &t = level.spriteTextures[i];

            t.texCoordAtlas[0].x <<= 7;
            t.texCoordAtlas[0].y <<= 7;
            t.texCoordAtlas[1].x <<= 7;
            t.texCoordAtlas[1].y <<= 7;
            #ifdef _GAPI_GS
                // sample texel centers (like object textures above), otherwise bilinear filtering
                // bleeds the neighbouring atlas row/column into sprites and font glyphs
                t.texCoordAtlas[0].x += 64;
                t.texCoordAtlas[0].y += 64;
                t.texCoordAtlas[1].x += 64;
                t.texCoordAtlas[1].y += 64;
            #endif
            /*
            t.texCoord[0].x += 16;
            t.texCoord[0].y += 16;
            t.texCoord[1].x += 16;
            t.texCoord[1].y += 16;
            */
        }
        #endif
    #endif
    }

    void initReflections() {
        for (int i = 0; i < level.entitiesBaseCount; i++) {
            TR::Entity &e = level.entities[i];
            if (e.type == TR::Entity::CRYSTAL) {
                Crystal *c = (Crystal*)e.controller;
                if (c->environment) { // already initialized and baked
                    continue;
                }
                c->bake();

            #ifdef _GAPI_C3D
                // C3D has a limit of GX commands for buffers clearing (GX_MemoryFill), so we limit render to one cubemap per frame
                return;
            #endif
            }
        }
        needRedrawReflections = false;
    }

    void setMainLight(Controller *controller) {
        Core::lightPos[0]   = controller->mainLightPos;
        Core::lightColor[0] = vec4(controller->mainLightColor.xyz(), 1.0f / controller->mainLightColor.w);
    }

    void renderSky() {
        #if !defined(_GAPI_GL) && !defined(_GAPI_D3D11)
            return;
        #endif
        ASSERT(mesh->transparent == 0);

        Shader::Type type;
        TR::SkyParams skyParams;

        if (level.version & TR::VER_TR1) {
            if (Core::settings.detail.lighting < Core::Settings::HIGH || !Core::support.tex3D || !TR::getSkyParams(level.id, skyParams))
                return;
            type = Shader::SKY_AZURE;
        } else { // TR2, TR3
            if (level.extra.sky == -1)
                return;

            if (Core::settings.detail.lighting < Core::Settings::HIGH || !Core::support.tex3D) {
                type = Shader::DEFAULT;
            } else {
                type = Shader::SKY_CLOUDS;
                if (!TR::getSkyParams(level.id, skyParams)) {
                    type = Shader::DEFAULT;
                }
            }
        }

        if (type != Shader::DEFAULT && !Core::perlinTex) {
            type = Shader::DEFAULT;
            if (level.version & TR::VER_TR1) {
                return;
            }
        }

        Core::Pass pass = Core::pass;
        mat4 mView = Core::mView;
        mat4 mProj = Core::mProj;

        Core::mView.setPos(vec3(0));
        Core::setViewProj(Core::mView, Core::mProj);

        setShader(Core::passSky, type, false, false);

        if (type != Shader::DEFAULT) {
            float time = Core::params.x;
            if (time > SKY_TIME_PERIOD) {
                time /= SKY_TIME_PERIOD;
                time = (time - int(time)) * SKY_TIME_PERIOD;
            }

            Core::active.shader->setParam(uParam,     vec4(skyParams.wind * time * 2.0f, 1.0));
            Core::active.shader->setParam(uLightProj, *(mat4*)&skyParams);
            Core::active.shader->setParam(uPosScale,  skyParams.cloudDownColor, 2);

            Core::perlinTex->bind(sNormal);
            Core::ditherTex->bind(sMask);
        }

        if (level.version & TR::VER_TR1) {
            Core::setCullMode(cmNone);
            mesh->renderBox();
            Core::setCullMode(cmFront);
        } else {
            Basis b;
            Core::setBasis(&b, 1); // unused
            mesh->renderModel(level.extra.sky);
        }

        Core::setViewProj(mView, mProj);
        Core::pass = pass;
    }

    void prepareRooms(RoomDesc *roomsList, int roomsCount) {
        skyIsVisible = (level.version & TR::VER_TR1);

        for (int i = 0; i < level.roomsCount; i++)
            level.rooms[i].flags.visible = false;

        for (int i = 0; i < roomsCount; i++) {
            TR::Room &r = level.rooms[roomsList[i].index];
            skyIsVisible |= r.flags.sky;
            r.flags.visible = true;
        }

        if (Core::pass == Core::passShadow)
            return;

        if (Core::settings.detail.shadows > Core::Settings::MEDIUM) {
            Sphere spheres[MAX_CONTACTS];
            int count = player->getSpheres(spheres);

            for (int i = 0; i < MAX_CONTACTS; i++)
                if (i < count)
                    Core::contacts[i] = vec4(spheres[i].center, PI * spheres[i].radius * spheres[i].radius * 0.25f);
                else
                    Core::contacts[i] = vec4(0.0f);
        }

        setMainLight(player);
    }

    short4 getPortalRect(const vec4 &v, short4 vp) {
        //vec4 s = vec4(v.x, -v.w, v.z, -v.y);
        vec4 sp = (v * 0.5 + 0.5) * vec4(float(vp.z), float(vp.w), float(vp.z), float(vp.w));

        short4 s(short(sp.x) + vp.x, short(sp.y) + vp.y, short(sp.z)+ vp.x, short(sp.w) + vp.y);

        // expand
        s.x -= 2;
        s.y -= 2;
        s.z += 2;
        s.w += 2;

        // clamp
        s.x = max(s.x, vp.x);
        s.y = max(s.y, vp.y);
        s.z = min(s.z, short(vp.x + vp.z));
        s.w = min(s.w, short(vp.y + vp.w));

        // convert from bounds to x,y,w,h
        s.z -= s.x;
        s.w -= s.y;

        // Use the viewport rect if one of the dimensions is the same size
        // as the viewport. This may fix clipping bugs while still allowing
        // impossible geometry tricks.
        if (s.z - s.x >= vp.z - vp.x || s.w - s.y >= vp.w - vp.y) {
           return vp;
        }

        return s;
    }

    void renderRooms(RoomDesc *roomsList, int roomsCount, int transp) {
        PROFILE_MARKER("ROOMS");

        if (Core::pass == Core::passShadow)
            return;

        Basis basis;
        basis.identity();

        Core::mModel.identity();

        switch (transp) {
            case 0 : Core::setBlendMode(bmNone);    break;
            case 1 : Core::setBlendMode(bmPremult); break;
            case 2 : Core::setBlendMode(bmAdd);   Core::setDepthWrite(false); break;
        }

        int i     = 0;
        int end   = roomsCount;
        int dir   = 1;

        if (transp) {
            i   = roomsCount - 1;
            end = -1;
            dir = -1;
        }

        atlasRooms->bind(sDiffuse);

        short4 vp = Core::scissor;

        while (i != end) {
            int roomIndex = roomsList[i].index;
            MeshBuilder::RoomRange &range = mesh->rooms[roomIndex];

            if (!range.geometry[transp].count && !range.dynamic[transp].count) {
                i += dir;
                continue;
            }

            Core::setScissor(getPortalRect(roomsList[i].portal, vp));

            const TR::Room &room = level.rooms[roomIndex];

            vec3 center = room.getCenter();
            int ambient = room.getAmbient(int(center.x), int(center.y), int(center.z));

            setRoomParams(roomIndex, Shader::ROOM, 1.0f, intensityf(ambient), 0.0f, 1.0f, transp == 1);

            basis.pos = room.getOffset();
            Core::setBasis(&basis, 1);

            Core::mModel.setPos(basis.pos);

            mesh->transparent = transp;
            mesh->renderRoomGeometry(roomIndex);

            i += dir;
        }

        Core::setDepthWrite(true);

        if (transp == 1) {
            atlasSprites->bind(sDiffuse);

            Core::setBlendMode(bmPremult);

            #ifdef MERGE_SPRITES
                basis.rot = Core::mViewInv.getRot();
            #else
                basis.rot = quat(0, 0, 0, 1);
            #endif

            for (int i = 0; i < roomsCount; i++) {
                int roomIndex = roomsList[i].index;
                level.rooms[roomIndex].flags.visible = true;

                MeshBuilder::RoomRange &range = mesh->rooms[roomIndex];

                if (!range.sprites.iCount)
                    continue;

                Core::setScissor(getPortalRect(roomsList[i].portal, vp));

                setRoomParams(roomIndex, Shader::SPRITE, 1.0f, 1.0f, 0.0f, 1.0f, true);

                basis.pos = level.rooms[roomIndex].getOffset();
                Core::setBasis(&basis, 1);

                mesh->renderRoomSprites(roomIndex);
            }
        }

        Core::setScissor(vp);
        Core::setBlendMode(bmNone);
    }

    void renderEntity(const TR::Entity &entity) {
        //if (entity.room != lara->getRoomIndex()) return;
        if (Core::pass == Core::passShadow && !entity.castShadow()) return;

        Controller *controller = (Controller*)entity.controller;
        int roomIndex = controller->getRoomIndex();
        TR::Room &room = level.rooms[roomIndex];

        if (controller->flags.invisible)
            return;

        if (!entity.isLara() && !entity.isActor() && !room.flags.visible)
            return;

        bool isModel;

        if (entity.type != TR::Entity::TRAP_LAVA_EMITTER) {
            isModel = entity.modelIndex > 0;
            if (isModel) {
                if (!mesh->models[controller->getModel()->index].geometry[mesh->transparent].count) return;
            } else {
                if (level.spriteSequences[-(entity.modelIndex + 1)].transp != mesh->transparent) return;
            }
        } else {
            if (mesh->transparent != 2) {
                return;
            }
            isModel = false;
        }

        Shader::Type type = isModel ? Shader::ENTITY : Shader::SPRITE;
        if (entity.type == TR::Entity::CRYSTAL)
            type = Shader::MIRROR;

        if (isModel) { // model
            ASSERT(controller->intensity >= 0.0f);

            setMainLight(level.isCutsceneLevel() ? player : controller);
            setRoomParams(roomIndex, type, 1.0f, controller->intensity, controller->specular, 1.0f, mesh->transparent == 1);

            vec3 pos = controller->getPos();
            if (ambientCache) {
                if (!entity.isDoor() && !entity.isBlock() && !entity.isPickup()) { // no advanced ambient lighting for secret (all) doors and blocks
                    AmbientCache::Cube cube;
                    ambientCache->getAmbient(roomIndex, pos, cube);
                    if (cube.status == AmbientCache::Cube::READY) {
                        memcpy(controller->ambient, cube.colors, sizeof(cube.colors)); // store last calculated ambient into controller
                    }
                } else {
                    if (entity.isPickup()) {
                        controller->ambient[0] =
                        controller->ambient[1] =
                        controller->ambient[5] =
                        controller->ambient[4] = vec4(Core::active.material.y * 0.8f);
                        controller->ambient[2] = vec4(Core::active.material.y * 0.1f);
                        controller->ambient[3] = vec4(Core::active.material.y);
                    } else {
                        controller->ambient[0] =
                        controller->ambient[1] =
                        controller->ambient[2] =
                        controller->ambient[3] =
                        controller->ambient[4] =
                        controller->ambient[5] = vec4(Core::active.material.y);
                    }
                }
                Core::active.shader->setParam(uAmbient, controller->ambient[0], 6);
            }
        }

        controller->render(camera->frustum, mesh, type, room.flags.water);
    }

    void loadNextLevelData() {
        isEnded = true;
        char buf[64];
        TR::getGameLevelFile(buf, level.version, nextLevel);
        nextLevel = TR::LVL_MAX;
        new Stream(buf, loadLevelAsync);
    }

    void update() {
        if (isEnded) return;

        bool invRing = inventory->phaseRing != 0.0f && inventory->phaseRing != 1.0f;
        if (inventory->video || inventory->titleTimer >= 1.0f || level.isCutsceneLevel() || invRing) {
            memset(Input::btnEnable, 0, sizeof(Input::btnEnable));
            Input::btnEnable[Input::bInventory] = !invRing;
        } else
            memset(Input::btnEnable, 1, sizeof(Input::btnEnable));

        Input::btnEnable[Input::bWeapon] &= players[0] && players[0]->canDrawWeapon();

        if (inventory->isActive())
            Input::btnEnable[Input::bWalk] = Input::btnEnable[Input::bJump] = Input::btnEnable[Input::bWeapon] = false;

        if (inventory->video) {
            inventory->update();
            UI::update();
            return;
        }

        if (level.isCutsceneLevel() && waitTrack) {
            if (!sndTrack && TR::LEVEL_INFO[level.id].track != TR::NO_TRACK) {
                if (camera->timer > 0.0f) // for the case that audio stops before animation ends
                    loadNextLevel();
                return;
            }

            if (cutsceneWaitTimer > 0.0f) {
                cutsceneWaitTimer -= Core::deltaTime;
                if (cutsceneWaitTimer > 0.0f)
                    return;
                if (sndTrack)
                    sndTrack->setVolume(1.0f, 0.0f);
                cutsceneWaitTimer = 0.0f;
                Core::resetTime();
                LOG("reset timer - %d\n", Core::getTime());
                return;
            }
        }

        if ((Input::lastState[0] == cInventory || Input::lastState[1] == cInventory) && !level.isTitle() && inventory->titleTimer < 1.0f && !inventory->active) {
            int playerIndex = (Input::lastState[0] == cInventory) ? 0 : 1;

            if (getLara(playerIndex)) {
                if (level.isCutsceneLevel()) { // skip cutscene level
                    loadNextLevel();
                    return;
                }

                if (player->health <= 0.0f)
                    inventory->toggle(playerIndex, Inventory::PAGE_OPTION, TR::Entity::INV_PASSPORT);
                else
                    inventory->toggle(playerIndex);
            }
        }

        bool invActive = inventory->isActive();

        inventory->update();

        if (inventory->titleTimer > 1.0f)
            return;

        if (nextLevel != TR::LVL_MAX && !inventory->isActive()) {
            if (showStats) {
                inventory->toggle(0, Inventory::PAGE_LEVEL_STATS);
                showStats = false;
                return;
            }
            loadNextLevelData();
            return;
        }

        if (loadSlot > -1 && nextLevel == TR::LVL_MAX) {
            if (inventory->isActive())
                return;

            loadLevel(saveSlots[loadSlot].getLevelID());
            return;
        }

        if (!inventory->isActive()) {
            UI::update();
        }

        float volWater, volTrack;

        if (invActive || level.isTitle()) {
            Sound::reverb.setRoomSize(vec3(1.0f));
            Sound::listener[0].underwater = false;
            volWater = 0.0f;
            volTrack = level.isTitle() ? 0.9f : 0.0f;
        } else {
            
            if (!level.isCutsceneLevel()) {
                statsTimeDelta += Core::deltaTime;
                while (statsTimeDelta >= 1.0f) {
                    statsTimeDelta -= 1.0f;
                    saveStats.time++;
                }
            }

            if (camera->spectator && Input::lastState[0] == cStart) {
                paused = !paused;
            }

            if (!paused) {
                params->time += Core::deltaTime;
                animTexTimer += Core::deltaTime;

                float timeStep = ANIM_TEX_TIMESTEP;
                if (level.version & TR::VER_TR1)
                    timeStep *= 0.5f;

                if (animTexTimer > timeStep) {
                    level.shiftAnimTex();
                    animTexTimer -= timeStep;
                }

                updateEffect();

                Controller *c = Controller::first;
                while (c) {
                    Controller *next = c->next;
                    c->update();
                    c = next;
                }
            } else {
                if (camera->spectator) {
                    camera->update();
                }
            }

            if (waterCache) 
                waterCache->update();

            Controller::clearInactive();

        // underwater ambient sound volume control
            if (camera->isUnderwater()) {
                if (!sndWater && !level.isCutsceneLevel()) {
                    sndWater = playSound(TR::SND_UNDERWATER, vec3(0.0f), Sound::LOOP | Sound::MUSIC);
                    if (sndWater)
                        sndWater->volume = sndWater->volumeTarget = 0.0f;
                }
                volWater = 1.0f;
            } else {
                volWater = 0.0f;
            }

            Sound::listener[0].underwater = (volWater == 1.0f);

            volTrack = 1.0f;
        }

        if (sndWater && sndWater->volumeTarget != volWater)
            sndWater->setVolume(volWater, 0.2f);
        if (sndTrack && sndTrack->volumeTarget != volTrack)
            sndTrack->setVolume(volTrack, 0.2f);

    #ifdef _DEBUG
    #ifdef _NAPI_SOCKET
        if (Input::down[ikJ]) {
            //Network::sayHello();
            NAPI::Peer peer;
            peer.ip = ((93 << 0) | (92 << 8) | (200 << 16) | (163 << 24));
            peer.port = htons(10994);
            LOG("join %s:%d\n", inet_ntoa(*(in_addr*)&peer.ip), ntohs(peer.port));
            Network::joinGame(peer);
            Input::down[ikJ] = false;
        }

        if (Input::down[ikY]) {
            NAPI::requestAddress();
            Input::down[ikY] = false;
        }
    #endif        
    #endif

    #ifdef GEOMETRY_EXPORT
        if (Input::down[ikF1]) {
            Extension::exportGeometry(this, atlasRooms, atlasObjects, atlasSprites);
            Input::down[ikF1] = false;
        }
    #endif
    }

    void updateEffect() {
        if (effect == TR::Effect::NONE)
            return;

        effectTimer += Core::deltaTime;

        switch (effect) {
            case TR::Effect::TR1_FLICKER : {
                int idx = effectIdx;
                switch (effectIdx) {
                    case 0 : if (effectTimer > 3.0f) effectIdx++; break;
                    case 1 : if (effectTimer > 3.1f) effectIdx++; break;
                    case 2 : if (effectTimer > 3.5f) effectIdx++; break;
                    case 3 : if (effectTimer > 3.6f) effectIdx++; break;
                    case 4 : if (effectTimer > 4.1f) { effectIdx++; effect = TR::Effect::NONE; } break;
                }
                if (idx != effectIdx)
                    flipMap();
                break;
            }
            case TR::Effect::EARTHQUAKE : {
                switch (effectIdx) {
                    case 0 : if (effectTimer > 0.0f) { playSound(TR::SND_ROCK);     effectIdx++; shakeCamera(1.0f); } break;
                    case 1 : if (effectTimer > 0.1f) { playSound(TR::SND_STOMP);    effectIdx++; } break;
                    case 2 : if (effectTimer > 0.6f) { playSound(TR::SND_BOULDER);  effectIdx++; shakeCamera(0.5f, true); } break;
                    case 3 : if (effectTimer > 1.1f) { playSound(TR::SND_ROCK);     effectIdx++; } break;
                    case 4 : if (effectTimer > 1.6f) { playSound(TR::SND_BOULDER);  effectIdx++; shakeCamera(0.5f, true); } break;
                    case 5 : if (effectTimer > 2.3f) { playSound(TR::SND_BOULDER);  shakeCamera(0.5f, true); effect = TR::Effect::NONE; } break;
                }
                break;
            }
            default : effect = TR::Effect::NONE; return;
        }
    }

    void setup() {
        camera->setup(Core::pass == Core::passCompose);
        setupBinding();
    }

    void renderEntitiesTransp(int transp) {
    #ifdef FFP
        // FFP: sprite entities (bubbles, splashes, smoke, sparkles, lava particles...) are batched with
        // camera-relative positions; the shader path adds the camera via uBasis, here the model matrix
        // must be set *before* the first sprite is added (the batch may flush on texture tile changes).
        // So draw all models first, then set the sprite state, then add the sprites.
        mesh->transparent = transp;

        atlasObjects->bind(sDiffuse);
        for (int i = 0; i < level.entitiesCount; i++) {
            TR::Entity &e = level.entities[i];
            if (!e.controller || e.modelIndex == 0) continue;
            if (e.modelIndex < 0 || e.type == TR::Entity::TRAP_LAVA_EMITTER) continue;   // sprites below
            renderEntity(e);
        }

        {
            PROFILE_MARKER("ENTITY_SPRITES");

            atlasSprites->bind(sDiffuse);
            Core::lightPos[0]   = vec4(0, 0, 0, 0);
            Core::lightColor[0] = vec4(0, 0, 0, 1);
            setRoomParams(getLara()->getRoomIndex(), Shader::SPRITE, 1.0f, 1.0f, 0.0f, 1.0f, mesh->transparent == 1);

            Basis b;
            b.w   = 1.0f;
            b.pos = Core::viewPos.xyz();
            b.rot = quat(0, 0, 0, 1);
            Core::setBasis(&b, 1);
            Core::mModel.identity();
            Core::mModel.setPos(b.pos);

            mesh->dynBegin();
            for (int i = 0; i < level.entitiesCount; i++) {
                TR::Entity &e = level.entities[i];
                if (!e.controller || e.modelIndex == 0) continue;
                if (!(e.modelIndex < 0 || e.type == TR::Entity::TRAP_LAVA_EMITTER)) continue;
                renderEntity(e);
            }
            mesh->dynEnd();
        }
        return;
    #endif

        mesh->dynBegin();
        mesh->transparent = transp;

        atlasObjects->bind(sDiffuse);
        for (int i = 0; i < level.entitiesCount; i++) {
            TR::Entity &e = level.entities[i];
            if (!e.controller || e.modelIndex == 0) continue;
            renderEntity(e);
        }

        {
            PROFILE_MARKER("ENTITY_SPRITES");

            if (mesh->dynICount) {
                atlasSprites->bind(sDiffuse);
                Core::lightPos[0]   = vec4(0, 0, 0, 0);
                Core::lightColor[0] = vec4(0, 0, 0, 1);
                setRoomParams(getLara()->getRoomIndex(), Shader::SPRITE, 1.0f, 1.0f, 0.0f, 1.0f, mesh->transparent == 1);

                Basis b;
                b.w   = 1.0f;
                b.pos = Core::viewPos.xyz();
                #ifdef MERGE_SPRITES
                    b.rot = Core::mViewInv.getRot();
                #else
                    b.rot = quat(0, 0, 0, 1);
                #endif
                Core::setBasis(&b, 1);
            }

            mesh->dynEnd();
        }
    }

    void renderEntities(int transp) {
        if (Core::pass == Core::passAmbient) // TODO allow static entities
            return;

        PROFILE_MARKER("ENTITIES");

        if (transp == 0) {
            Core::setBlendMode(bmNone);
            renderEntitiesTransp(transp);
        }

        if (transp == 1) {
            Core::setBlendMode(bmPremult);
            renderEntitiesTransp(transp);

        #ifdef _GAPI_GS
            #ifndef GS_SHADOWS
                #define GS_SHADOWS 1            // shadow map shadows (debug switch)
            #endif
            // PS2: shadow map shadows (see renderShadowGS)
            Core::setFog(FOG_NONE);
            for (int i = 0; i < level.entitiesCount && GS_SHADOWS; i++) {
                TR::Entity &entity = level.entities[i];
                Controller *controller = (Controller*)entity.controller;
                if (controller && controller->flags.rendered && controller->getEntity().castShadow())
                    renderShadowGS(controller);
            }
            Core::setBlendMode(bmNone);
        #elif !defined(FFP)
            Core::setFog(FOG_NONE);
            Core::whiteTex->bind(sDiffuse);
            Core::setBlendMode(bmMult);
            for (int i = 0; i < level.entitiesCount; i++) {
                TR::Entity &entity = level.entities[i];
                Controller *controller = (Controller*)entity.controller;
                if (controller && controller->flags.rendered && controller->getEntity().castShadow())
                    controller->renderShadow(mesh);
            }
            Core::setBlendMode(bmNone);
        #endif
        }

        if (transp == 2) {
            Core::setDepthWrite(false);
            Core::setBlendMode(bmAdd);
            renderEntitiesTransp(transp);
            Core::setDepthWrite(true);
        }
    }

    virtual vec4 projectPoint(const vec4 &p) {
        vec4 res;
        res = Core::mViewProj * p;

        #ifdef _OS_3DS
            if (GAPI::rotate90) {
                res.y = -res.y;
                swap(res.x, res.y);
            }
        #endif

        return res;
    }

    bool checkPortal(const TR::Room &room, const TR::Room::Portal &portal, const vec4 &viewPort, vec4 &clipPort) {
        vec3 n = portal.normal;
        vec3 v = Core::viewPos.xyz() - (room.getOffset() + portal.vertices[0]);

        if (n.dot(v) <= 0.0f)
            return false;

        int  zClip = 0;
        vec4 p[4];

        clipPort = vec4(INF, INF, -INF, -INF);

        for (int i = 0; i < 4; i++) {
            p[i] = projectPoint(vec4(vec3(portal.vertices[i]) + room.getOffset(), 1.0f));

            if (p[i].w > 0.0f) {
                p[i].xy() *= (1.0f / p[i].w);

                clipPort.x = min(clipPort.x, p[i].x);
                clipPort.y = min(clipPort.y, p[i].y);
                clipPort.z = max(clipPort.z, p[i].x);
                clipPort.w = max(clipPort.w, p[i].y);
            } else
                zClip++;
        }

        if (zClip == 4)
            return false;

        if (zClip > 0) {
            for (int i = 0; i < 4; i++) {
                vec4 &a = p[i];
                vec4 &b = p[(i + 1) % 4];

                if ((a.w > 0.0f) ^ (b.w > 0.0f)) {

                    if (a.x < 0.0f && b.x < 0.0f)
                        clipPort.x = -1.0f;
                    else
                        if (a.x > 0.0f && b.x > 0.0f)
                            clipPort.z = 1.0f;
                        else {
                            clipPort.x = -1.0f;
                            clipPort.z =  1.0f;
                        }

                    if (a.y < 0.0f && b.y < 0.0f)
                        clipPort.y = -1.0f;
                    else
                        if (a.y > 0.0f && b.y > 0.0f)
                            clipPort.w = 1.0f;
                        else {
                            clipPort.y = -1.0f;
                            clipPort.w =  1.0f;
                        }

                }
            }
        }

        if (clipPort.x > viewPort.z || clipPort.y > viewPort.w || clipPort.z < viewPort.x || clipPort.w < viewPort.y)
            return false;

        clipPort.x = max(clipPort.x, viewPort.x);
        clipPort.y = max(clipPort.y, viewPort.y);
        clipPort.z = min(clipPort.z, viewPort.z);
        clipPort.w = min(clipPort.w, viewPort.w);

        return true;
    }

    virtual void getVisibleRooms(RoomDesc *roomsList, int &roomsCount, int from, int to, const vec4 &viewPort, bool water, int count = 0) {
        if (roomsCount >= 255 || count > 16) {
            //ASSERT(false);
            return;
        }

        TR::Room &room = level.rooms[to];

        if (Core::pass == Core::passCompose && water && waterCache && from != TR::NO_ROOM && (level.rooms[from].flags.water ^ level.rooms[to].flags.water))
            waterCache->setVisible(from, to);

        vec4 rect = viewPort;

    #ifdef _GAPI_GS
        // one entry per room: the recursion reaches a room once per portal path, and each duplicate
        // re-renders the whole room (scissor only clips pixels, the geometry is still transformed + sent).
        // Merge into the existing entry (union of portal rects) and only recurse when the rect grew;
        // recursing with the union is conservative (never hides a room the per-path walk would show).
        int found = -1;
        if (room.flags.visible) {
            for (int k = 0; k < roomsCount; k++) {
                if (roomsList[k].index == to) {
                    found = k;
                    break;
                }
            }
        }

        if (found >= 0) {
            vec4 &r = roomsList[found].portal;
            if (viewPort.x >= r.x && viewPort.y >= r.y && viewPort.z <= r.z && viewPort.w <= r.w)
                return;     // nothing new visible through this path
            r.x = min(r.x, viewPort.x);
            r.y = min(r.y, viewPort.y);
            r.z = max(r.z, viewPort.z);
            r.w = max(r.w, viewPort.w);
            rect = r;
        } else {
            room.flags.visible = true;
            roomsList[roomsCount++] = RoomDesc(to, viewPort);
        }
    #else
        room.flags.visible = true;
        roomsList[roomsCount++] = RoomDesc(to, viewPort);
    #endif

        vec4 clipPort;
        for (int i = 0; i < room.portalsCount; i++) {
            TR::Room::Portal &p = room.portals[i];

            if (Core::pass == Core::passCompose && water && waterCache && (level.rooms[to].flags.water ^ level.rooms[p.roomIndex].flags.water))
                waterCache->setVisible(to, p.roomIndex);

            if (from != room.portals[i].roomIndex && checkPortal(room, p, rect, clipPort))
                getVisibleRooms(roomsList, roomsCount, to, p.roomIndex, clipPort, water, count + 1);
        }
    }

    void renderOpaque(RoomDesc *roomsList, int roomsCount) {
        renderRooms(roomsList, roomsCount, 0);
        renderEntities(0);
        if (Core::pass != Core::passShadow && skyIsVisible) {
            renderSky();
        }
    }

    void renderTransparent(RoomDesc *roomsList, int roomsCount) {
        renderRooms(roomsList, roomsCount, 1);
        renderEntities(1);
    }

    void renderAdditive(RoomDesc *roomsList, int roomsCount) {
        // don't apply fog for additive geometry
        vec4 oldLevelFogParams = levelFogParams;
        vec4 oldUnderwaterFogParams = underwaterFogParams;
        levelFogParams = FOG_NONE;
        underwaterFogParams = FOG_NONE;

        renderRooms(roomsList, roomsCount, 2);
        renderEntities(2);

        levelFogParams = oldLevelFogParams;
        underwaterFogParams = oldUnderwaterFogParams;
    }

#ifdef _GAPI_GS
    #define GS_WATER_FACE_ALPHA 0.5f    // opacity of the original (PS1) water surface texture

    // PS2: water surfaces without WaterCache (wave fields + GAPI::drawWaterMesh)
    int16 gsWaterRooms[32];
    int   gsWaterRoomsCount;

    void gsAddWaterSurface(int underwaterRoom) {
        TR::Room &r = level.rooms[underwaterRoom];
        if (!r.flags.water) return;
        for (int i = 0; i < gsWaterRoomsCount; i++) {
            if (gsWaterRooms[i] == underwaterRoom) return;
        }
        if (gsWaterRoomsCount < COUNT(gsWaterRooms)) {
            gsWaterRooms[gsWaterRoomsCount++] = int16(underwaterRoom);
        }
    }

    void renderWaterSurfacesGS(RoomDesc *roomsList, int roomsCount) {
        // water rooms from the final visible list (not from portal crossings: a room is visited once,
        // so the water/dry portal is not always the one that reaches it -> layer would come and go)
        gsWaterRoomsCount = 0;
        for (int i = 0; i < roomsCount; i++) {
            gsAddWaterSurface(roomsList[i].index);
        }
        renderWaterTintGS();

    }

    // ---- wave fields: one height field per visible pool (4 cells per sector, 60 Hz wave equation) ----
    #define GS_WAVE_CELL        256.0f    // world units per grid cell
    #define GS_WAVE_HEIGHT      7.0f      // world units per height unit
    #define GS_WAVE_MAX         11.0f     // height clamp (+-42 world units)
    #define GS_WAVE_SPEED2      0.003f    // c^2 in cells^2 per step: ~0.055 cells/step = ~840 units/s at 60 Hz
    #define GS_WAVE_DAMPING     0.997f
    #define GS_WAVE_DROP        20.0f     // waterDrop strength -> height units
    #define GS_WAVE_SHADE       3.0f      // slope exaggeration for normals (Fresnel, sheen)
    #define GS_WAVE_UV          0.08f     // ripple texture distortion by wave slope
    #define GS_WAVE_FIELDS      8
    #define GS_WAVE_STEP        (1.0f / 60.0f)   // every frame at 60 fps (30 Hz looked choppy)
    #define GS_WAVE_CALM_RADIUS 1024.0f
    #define GS_WAVE_CALM_AMOUNT 0.1f

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

    GSWaterField gsFields[GS_WAVE_FIELDS];
    int          gsFieldsCount = 0;
    float        gsWaveTimer   = 0.0f;
    int          gsLaraStand   = -1;     // Lara's previous Character::Stand (water transitions)
    float        gsLaraWake    = 0.0f;
    vec3         gsLaraWakePos;
    float        gsLaraSplash  = 0.0f;   // follow-up splash drops after jumping in

    bool gsIsSurfaceSector(const TR::Room &r, int x, int z) {
        if (x < 0 || z < 0 || x >= r.xSectors || z >= r.zSectors) return false;
        const TR::Room::Sector &s = r.sectors[x * r.zSectors + z];
        return s.roomAbove != TR::NO_ROOM && !level.rooms[s.roomAbove].flags.water;
    }

    void gsFreeWaterFields() {
        for (int i = 0; i < gsFieldsCount; i++) {
            delete[] gsFields[i].cur;
            delete[] gsFields[i].prev;
            delete[] gsFields[i].mask;
            delete[] gsFields[i].faceTex;
            delete[] gsFields[i].faceMap;
            delete[] gsFields[i].faceRect;
            delete[] gsFields[i].faceTexNext;
        }
        gsFieldsCount = 0;
    }

    GSWaterField* gsGetWaterField(int roomIndex) {
        for (int i = 0; i < gsFieldsCount; i++) {
            if (gsFields[i].room == roomIndex) return &gsFields[i];
        }
        if (gsFieldsCount >= GS_WAVE_FIELDS) return NULL;

        TR::Room &r = level.rooms[roomIndex];
        // waterLevelSurface is only filled when water quality > LOW (MeshBuilder::roomRemoveWaterSurfaces),
        // waterLevel[] is always computed (MeshBuilder::calcWaterLevel) and is the surface height of a water room
        int posY = r.waterLevel[level.state.flags.flipped];
        if (posY == TR::NO_WATER) posY = r.waterLevelSurface;
        if (posY == TR::NO_WATER) return NULL;

        bool any = false;
        for (int z = 0; z < r.zSectors && !any; z++)
            for (int x = 0; x < r.xSectors && !any; x++)
                any = gsIsSurfaceSector(r, x, z);
        if (!any) return NULL;

        GSWaterField &f = gsFields[gsFieldsCount++];
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
                        if (gsIsSurfaceSector(r, nx, nz)) {
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

    void gsAddDrop(GSWaterField &f, float x, float z, float radius, float amount) {
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
    void gsDropAt(float x, float z, float radius, float amount) {
        for (int i = 0; i < gsFieldsCount; i++) {
            GSWaterField &f = gsFields[i];
            if (x < f.x0 || z < f.z0 || x > f.x0 + (f.w - 1) * GS_WAVE_CELL || z > f.z0 + (f.h - 1) * GS_WAVE_CELL) continue;
            gsAddDrop(f, x, z, radius, amount);
        }
    }

    // The engine only reports diving (Lara::goUnderwater) and swim bubbles through waterDrop; detect
    // the other water transitions from Lara's stand: jump in, climb out, surface, plus a swimming wake
    void gsLaraWaterEvents() {
        Lara *lara = players[0];
        if (!lara) return;

        int stand = int(lara->stand);
        bool inWater  = (stand == Character::STAND_ONWATER || stand == Character::STAND_UNDERWATER);
        bool wasWater = (gsLaraStand == Character::STAND_ONWATER || gsLaraStand == Character::STAND_UNDERWATER);
        vec3 p = lara->pos;

        if (gsLaraStand >= 0 && stand != gsLaraStand) {
            if (inWater && !wasWater) {
                gsDropAt(p.x, p.z, 900.0f, 10.0f);   // jump / fall into the water
                gsLaraSplash = 0.3f;                // + a short burst of follow-up drops
            } else if (!inWater && wasWater && stand != Character::STAND_AIR) {
                gsDropAt(p.x, p.z, 768.0f, 4.5f);   // pull out of the pool
            } else if (wasWater && !inWater) {
                gsDropAt(p.x, p.z, 768.0f, 4.0f);   // jump out of the water
            } else if (gsLaraStand == Character::STAND_UNDERWATER && stand == Character::STAND_ONWATER) {
                gsDropAt(p.x, p.z, 768.0f, 4.0f);   // surfacing
            } else if (gsLaraStand == Character::STAND_ONWATER && stand == Character::STAND_UNDERWATER) {
                gsDropAt(p.x, p.z, 768.0f, 4.0f);   // diving (stronger than the engine's own drop)
            }
        }
        gsLaraStand = stand;

        if (gsLaraSplash > 0.0f) {
            float prev = gsLaraSplash;
            gsLaraSplash -= Core::deltaTime;
            // drops at 0.2, 0.1 and 0.0 s left, scattered around the entry point
            for (int k = 0; k < 3; k++) {
                float at = 0.2f - k * 0.1f;
                if (prev > at && gsLaraSplash <= at) {
                    gsDropAt(p.x + (randf() - 0.5f) * 512.0f, p.z + (randf() - 0.5f) * 512.0f, 512.0f, 3.0f);
                }
            }
        }

        // wake while swimming on the surface
        // (surface swimming is animation driven, so use the distance moved, not velocity)
        if (stand == Character::STAND_ONWATER) {
            gsLaraWake += Core::deltaTime;
            if (gsLaraWake > 0.1f) {
                float dx = p.x - gsLaraWakePos.x, dz = p.z - gsLaraWakePos.z;
                if (dx * dx + dz * dz > 16.0f * 16.0f) {
                    gsDropAt(p.x, p.z, 512.0f, 1.6f);   // swimming wake
                } else if (rand() % 4 == 0) {
                    gsDropAt(p.x, p.z, 384.0f, 0.6f);   // treading water
                }
                gsLaraWake = 0.0f;
                gsLaraWakePos = p;
            }
        } else {
            gsLaraWakePos = p;
        }
    }

    // Level::waterDrop (Lara swimming / entering, bubbles popping, enemies, falling objects)
    void gsWaterDrop(const vec3 &pos, float radius, float strength) {
        for (int i = 0; i < gsFieldsCount; i++) {
            GSWaterField &f = gsFields[i];
            if (fabsf(pos.y - f.y) > 512.0f) continue;
            if (pos.x < f.x0 || pos.z < f.z0 || pos.x > f.x0 + (f.w - 1) * GS_WAVE_CELL || pos.z > f.z0 + (f.h - 1) * GS_WAVE_CELL) continue;
            gsAddDrop(f, pos.x, pos.z, radius, strength * GS_WAVE_DROP);
        }
    }

    void gsStepWaterField(GSWaterField &f) {
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

    void gsUpdateWaterFields() {
        gsLaraWaterEvents();
        gsWaveTimer = min(gsWaveTimer + Core::deltaTime, GS_WAVE_STEP * 3.0f);
        int steps = 0;
        while (gsWaveTimer >= GS_WAVE_STEP && steps < 3) {
            gsWaveTimer -= GS_WAVE_STEP;
            steps++;
        }
        for (int r = 0; r < gsWaterRoomsCount; r++) {
            GSWaterField *f = gsGetWaterField(gsWaterRooms[r]);
            if (!f) continue;
            // idle motion: weak random drops so calm water still moves
            f->idleTimer += Core::deltaTime;
            if (f->idleTimer > 0.35f) {
                f->idleTimer = 0.0f;
                for (int tries = 0; tries < 8; tries++) {
                    int i = 1 + rand() % max(1, f->w - 2);
                    int j = 1 + rand() % max(1, f->h - 2);
                    if (f->mask[j * f->w + i] == 2) {
                        gsAddDrop(*f, f->x0 + i * GS_WAVE_CELL, f->z0 + j * GS_WAVE_CELL, GS_WAVE_CALM_RADIUS, GS_WAVE_CALM_AMOUNT);
                        break;
                    }
                }
            }
            for (int s = 0; s < steps; s++) {
                gsStepWaterField(*f);
            }
        }
    }

    // ---- surface grid: wave height, Fresnel opacity + fake reflection, specular, shore foam ----
    inline float gsFieldH(const GSWaterField &f, int i, int j) {
        i = clamp(i, 0, f.w - 1);
        j = clamp(j, 0, f.h - 1);
        return f.cur[j * f.w + i];
    }

    #define GS_WATER_SPECULAR 0.60f   // specular strength (masks the original water texture)
    #define GS_WATER_ALPHA_MIN 0.20f // tint layer opacity looking straight down
    #define GS_WATER_FRESNEL  1.1f   // Fresnel strength (opacity + reflection color towards grazing angles)
    #define GS_WATER_SPEC_MIN 0.2f   // original texture visibility without specular

    // Wave specular (0..1) at a grid point. Only the part caused by the waves: the reflection of a fixed
    // light for the wave normal minus the reflection a flat surface would give from the same view. The
    // flat part depends on the camera only (a big blob that drifts with the view); without it the
    // highlights sit on the wave flanks facing the light and move with the ripples. Plus a crest term.
    #define GS_SPEC_GAIN    4.0f
    #define GS_SPEC_CREST   0.35f

    inline float gsSpecTerm(const vec3 &N, const vec3 &V, const vec3 &L) {
        vec3 R = N * (2.0f * N.dot(V)) - V;
        float sp = max(0.0f, R.dot(L));
        sp *= sp; sp *= sp; sp *= sp;   // ^8
        return sp;
    }

    float gsWaterSpecular(const GSWaterField &f, int i, int j) {
        float h  = gsFieldH(f, i, j);
        float hx = (gsFieldH(f, i + 1, j) - gsFieldH(f, i - 1, j)) * 0.5f;
        float hz = (gsFieldH(f, i, j + 1) - gsFieldH(f, i, j - 1)) * 0.5f;
        vec3 P = vec3(f.x0 + i * GS_WAVE_CELL, f.y - h * GS_WAVE_HEIGHT, f.z0 + j * GS_WAVE_CELL);
        vec3 N = vec3(-hx * GS_WAVE_SHADE, -1.0f, -hz * GS_WAVE_SHADE).normal();
        vec3 V = (Core::viewPos.xyz() - P).normal();
        static const vec3 L = vec3(0.3f, -1.0f, 0.25f).normal();   // TR y down
        static const vec3 F = vec3(0.0f, -1.0f, 0.0f);             // flat water normal

        float wave  = gsSpecTerm(N, V, L) - gsSpecTerm(F, V, L);    // reflection added by the waves
        float crest = max(0.0f, h) * (1.0f / GS_WAVE_MAX);          // wave tops
        float sp = max(0.0f, wave) * GS_SPEC_GAIN + crest * GS_SPEC_CREST;
        return (sp > 1.0f) ? 1.0f : sp;
    }

    void gsWaterVertex(GAPI::Vertex &v, const GSWaterField &f, int i, int j, float su, float sv) {
        const float s = 1.0f / 32767.0f;
        float h  = gsFieldH(f, i, j);
        float hx = (gsFieldH(f, i + 1, j) - gsFieldH(f, i - 1, j)) * 0.5f;
        float hz = (gsFieldH(f, i, j + 1) - gsFieldH(f, i, j - 1)) * 0.5f;

        vec3 P = vec3(f.x0 + i * GS_WAVE_CELL, f.y - h * GS_WAVE_HEIGHT, f.z0 + j * GS_WAVE_CELL);
        // up normal of y(x,z) = Y - A h (TR y points down)
        const float k = GS_WAVE_SHADE;   // exaggerated slope for shading
        vec3 N = vec3(-hx * k, -1.0f, -hz * k).normal();
        vec3 V = (Core::viewPos.xyz() - P).normal();
        // Fresnel from the flat water normal (view angle only): smooth, no white streaks along the waves;
        // the only highlight is the wave specular through the original texture (renderWaterFacesGS)
        float c = fabsf(V.y);

        // Fresnel: clear looking down, more opaque + darker teal "reflection" at grazing angles
        float fr = (1.0f - c) * (1.0f - c);    // ^2: Fresnel kicks in earlier than ^3
        fr = min(1.0f, fr * GS_WATER_FRESNEL);
        vec3 col = vec3(0.6f, 0.9f, 0.9f).lerp(vec3(0.45f, 0.7f, 0.78f), fr);
        float alpha = GS_WATER_ALPHA_MIN + fr * 0.7f;



        // specular from a fixed light above (TR y down)
        static const vec3 L = vec3(0.3f, -1.0f, 0.25f).normal();
        vec3 R = N * (2.0f * N.dot(V)) - V;
        float sp = max(0.0f, R.dot(L));
        (void)sp;   // the specular is applied through the original texture (renderWaterFacesGS)

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

    void renderWaterTintGS() {
        if (!gsWaterRoomsCount) return;

        gsUpdateWaterFields();

        GAPI::initWaterSurface();
        GAPI::Vertex *vtx = GAPI::waterVtx;
        Index        *idx = GAPI::waterIdx;
        int vCount = 0, iCount = 0;

        const float su = Core::params.x * 0.05f, sv = Core::params.x * 0.03f;

        for (int r = 0; r < gsWaterRoomsCount; r++) {
            GSWaterField *f = gsGetWaterField(gsWaterRooms[r]);
            if (!f) continue;
            TR::Room &room = level.rooms[f->room];

            // 4x4 quads per surface sector, 5x5 vertices per sector (edges duplicated, same values)
            for (int sz = 0; sz < room.zSectors; sz++) {
                for (int sx = 0; sx < room.xSectors; sx++) {
                    if (!gsIsSurfaceSector(room, sx, sz)) continue;
                    if (vCount + 25 > GS_WATER_MAX_VERTS || iCount + 96 > GS_WATER_MAX_INDICES) break;

                    int base = vCount;
                    for (int j = 0; j <= 4; j++) {
                        for (int i = 0; i <= 4; i++) {
                            gsWaterVertex(vtx[vCount++], *f, sx * 4 + i, sz * 4 + j, su, sv);
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
        Core::setFog(camera->isUnderwater() ? underwaterFogParams : levelFogParams);
        Core::setBlendMode(bmAlpha);
        Core::setDepthWrite(false);
        Core::setCullMode(cmNone);
        Core::setAlphaTest(false);
        Core::validateRenderState();

        GAPI::drawWaterMesh(vCount, iCount, GAPI::waterTex);

        renderWaterFacesGS();

        Core::setDepthWrite(true);
        Core::setCullMode(cmFront);
        Core::setBlendMode(bmNone);
    }

    // original PS1 (animated) water texture on the same displaced wave grid, translucent
    void renderWaterFacesGS() {
        GAPI::Vertex *vtx = GAPI::waterVtx;
        Index        *idx = GAPI::waterIdx;
        int vCount = 0, iCount = 0;
        int curTile = -1, curClut = -1;

        const float s = 1.0f / 32767.0f;

        // crossfade the texture animation: current frame at (1 - t), next frame at t
        float blend = clamp(animTexTimer / ANIM_TEX_TIMESTEP, 0.0f, 1.0f);

        for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < gsWaterRoomsCount; r++) {
            GSWaterField *f = gsGetWaterField(gsWaterRooms[r]);
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
                        atlasRooms->bindTile(curTile, curClut);
                        GAPI::drawWaterMesh(vCount, iCount, atlasRooms);
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
                            float h = gsFieldH(*f, gi, gj);
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
                            float m = GS_WATER_SPEC_MIN + gsWaterSpecular(*f, gi, gj) * GS_WATER_SPECULAR;
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
            atlasRooms->bindTile(curTile, curClut);
            GAPI::drawWaterMesh(vCount, iCount, atlasRooms);
        }
    }

#endif

    virtual void renderView(int roomIndex, bool water, bool showUI, int roomsCount = 0, RoomDesc *roomsList = NULL) {
        PROFILE_MARKER("VIEW");

    #ifdef _GAPI_GS
        mesh->animBlend = clamp(animTexTimer / ANIM_TEX_TIMESTEP, 0.0f, 1.0f);   // texture animation crossfade
        GAPI::setUnderwaterView(Core::pass == Core::passCompose && camera->isUnderwater());
        gsWaterRoomsCount = 0;
    #endif

        if (water && waterCache)
            waterCache->reset();

        RoomDesc rList[256];

        if (!roomsList) {
            roomsList = rList;

            // mark all rooms as invisible
            for (int i = 0; i < level.roomsCount; i++)
                level.rooms[i].flags.visible = false;

            if (level.isCutsceneLevel()) { // render all rooms except flipped
            // hide alternative rooms from getVisibleRooms
                for (int i = 0; i < level.roomsCount; i++) {
                    int flipIndex = level.rooms[i].alternateRoom;
                    if (flipIndex > -1)
                        level.rooms[flipIndex].flags.visible = true;
                }

            // get room list through portals (it will change room visible flag)
                getVisibleRooms(roomsList, roomsCount, TR::NO_ROOM, roomIndex, vec4(-1.0f, -1.0f, 1.0f, 1.0f), water);

            // add other non-alternative rooms
                for (int i = 0; i < level.roomsCount; i++)
                    if (!level.rooms[i].flags.visible)
                        roomsList[roomsCount++] = RoomDesc(i, vec4(-1.0f, -1.0f, 1.0f, 1.0f));

            // refresh visible flag
                for (int i = 0; i < level.roomsCount; i++)
                    level.rooms[i].flags.visible = false;

                for (int i = 0; i < roomsCount; i++)
                    level.rooms[roomsList[i].index].flags.visible = true;
            } else
                getVisibleRooms(roomsList, roomsCount, TR::NO_ROOM, roomIndex, vec4(-1.0f, -1.0f, 1.0f, 1.0f), water);
        }

        if (water && waterCache) {
            for (int i = 0; i < roomsCount; i++)
                waterCache->setVisible(roomsList[i].index);

            waterCache->renderReflection();

            Core::Pass pass = Core::pass;
            waterCache->simulate();
            Core::pass = pass;
        }

        // clear entity rendered flag (used for blob shadows)
        if (Core::pass != Core::passAmbient)
            for (int i = 0; i < level.entitiesCount; i++) {
                Controller *controller = (Controller*)level.entities[i].controller;
                if (controller)
                    controller->flags.rendered = false;
            }

        Texture *screen = NULL;
        if (water) {
            screen = (waterCache && waterCache->visible) ? waterCache->getScreenTex() : NULL;

            int clearFlags = RT_STORE_COLOR;

            if (screen) {
                clearFlags |= RT_CLEAR_COLOR | RT_CLEAR_DEPTH | RT_STORE_DEPTH;
            }

            #ifndef EARLY_CLEAR
                clearFlags |= RT_CLEAR_COLOR | RT_CLEAR_DEPTH;
            #endif

            Core::setTarget(screen, NULL, clearFlags); // render to screen texture or back buffer
            Core::validateRenderState();
            setupBinding();
        }

        prepareRooms(roomsList, roomsCount);


        renderOpaque(roomsList, roomsCount);
        renderTransparent(roomsList, roomsCount);

    #ifdef _GAPI_GS
        // water surface after the transparent pass: it writes no depth, so drawing it earlier would let
        // sprites behind it (bubbles seen from below) show on top of it
        if (water && !waterCache && GS_WATER_WAVES) {
            renderWaterSurfacesGS(roomsList, roomsCount);
        }
    #endif

        if (camera->isUnderwater())
            renderAdditive(roomsList, roomsCount);

        Core::setFog(FOG_NONE);

        Core::setBlendMode(bmNone);
        if (water && waterCache && waterCache->visible) {
            Core::Pass pass = Core::pass;
            if (!camera->isUnderwater())
                waterCache->renderRays();
            waterCache->renderMask();
            waterCache->copyScreenToRefraction();
            setMainLight(player);
            waterCache->compose();
            if (camera->isUnderwater())
                waterCache->renderRays();

            Core::pass = pass;
            setupBinding();
        }

        if (!camera->isUnderwater())
            renderAdditive(roomsList, roomsCount);
    
        Core::setFog(FOG_NONE);

        Core::setBlendMode(bmNone);

        Core::Pass pass = Core::pass;

        if (water && waterCache && waterCache->visible && screen) {
            Core::setTarget(NULL, NULL, RT_CLEAR_DEPTH | RT_STORE_COLOR);
            Core::validateRenderState();
            waterCache->blitTexture(screen);
        }

    #ifdef _GAPI_GS
        if (GAPI::underwaterView) {
            GAPI::underwaterRipple(Core::params.x);
        }
        GAPI::setUnderwaterView(false);
    #endif

        if (showUI) {
            renderUI();
        }

        Core::pass = pass;
    }

    void setupCubeCamera(const vec3 &pos, int face) {
        vec3 up  = vec3(0, -1, 0);
        vec3 dir;
        switch (face) {
            case 0 : dir = vec3( 1,  0,  0); break;
            case 1 : dir = vec3(-1,  0,  0); break;
            case 2 : dir = vec3( 0,  1,  0); up = vec3(0, 0,  1); break;
            case 3 : dir = vec3( 0, -1,  0); up = vec3(0, 0, -1); break;
            case 4 : dir = vec3( 0,  0,  1); break;
            case 5 : dir = vec3( 0,  0, -1); break;
        }

        Core::mViewInv  = mat4(pos, pos + dir, up);
        Core::mView     = Core::mViewInv.inverseOrtho();
        Core::mProj     = GAPI::perspective(90, 1.0f, 32.0f, 45.0f * 1024.0f, 0.0f);

        #ifdef _GAPI_D3D8
            Core::mProj.scale(vec3(1.0f, -1.0f, 1.0f));
        #endif

        Core::mViewProj = Core::mProj * Core::mView;
        Core::viewPos   = Core::mViewInv.offset().xyz();

        camera->setup(false);
    }

    void renderShadowView(int roomIndex) {
        vec3 pos = player->getBoundingBox().center();

        float znear = camera->znear;
        float zfar  = player->mainLightColor.w * 1.5f;

        Core::mViewInv = mat4(player->mainLightPos, pos, vec3(0, -1, 0));
        Core::mView    = Core::mViewInv.inverseOrtho();
        Core::mProj    = GAPI::perspective(90.0f, 1.0f, znear, zfar, 0.0f);

        mat4 &m = mLightProj[player->camera->cameraIndex];
        m = Core::mProj * Core::mView;

        mat4 bias;

        if (GAPI::getProjRange() == mat4::PROJ_ZERO_POS)
            bias = mat4(
                0.5f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.5f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.5f, 0.5f, 0.0f, 1.0f
            );
        else {
            bias = mat4(
                0.5f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.5f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.5f, 0.0f,
                0.5f, 0.5f, 0.5f, 1.0f
            );
        }

    #if defined(_GAPI_D3D8) || defined(_GAPI_D3D9) || defined(_GAPI_D3D11) || defined(_GAPI_GXM)
        bias.e11 = -bias.e11; // vertical flip for UVs
    #endif

        m = bias * m;

        Core::mLightProj = m;

        camera->frustum->pos = Core::viewPos.xyz();
        camera->frustum->calcPlanes(Core::mViewProj);

        setup();
        renderView(roomIndex, false, false);
    }
/*
    void renderShadowEntity(int index, Controller *controller, Controller *player) {
        Box box = controller->getSpheresBox(true);
        mat4 m = controller->getMatrix();

        vec3 pos = m * box.center();
        Core::mViewInv = mat4(player->mainLightPos, pos, vec3(0, -1, 0));
        Core::mView    = Core::mViewInv.inverseOrtho();
        Core::mProj    = mat4(90.0f, 1.0f, 1.0f, 2.0f);

        Box crop = box * (Core::mProj * Core::mView * m);
        crop.min.z = max(0.0f, crop.min.z);

        float sx =  2.0f / (crop.max.x - crop.min.x);
        float sy =  2.0f / (crop.max.y - crop.min.y);
        float sz =  2.0f / (crop.max.z - crop.min.z);
        float ox = -0.5f * (crop.max.x + crop.min.x) * sx;
        float oy = -0.5f * (crop.max.y + crop.min.y) * sy;
        float oz = -0.5f * (crop.max.z + crop.min.z) * sz;

        Core::mProj = mat4(sx,  0,    0,  0,
                            0,  sy,   0,  0,
                            0,   0,  sz,  0,
                            ox,  oy,  oz,  1) * Core::mProj;

        Core::setViewProj(Core::mView, Core::mProj);

        mat4 bias;
        bias.identity();
        bias.e00 = bias.e11 = bias.e22 = bias.e03 = bias.e13 = bias.e23 = 0.5f;
        Core::mLightProj[index] = bias * (Core::mProj * Core::mView);
        
        Core::setBlendMode(bmNone);

        mesh->transparent = 0;
        if (mesh->models[controller->getEntity().modelIndex - 1].geometry[mesh->transparent].count) {
            setShader(Core::pass, Shader::ENTITY, false, false);
            controller->render(NULL, mesh, Shader::ENTITY, false);
        }
        mesh->transparent = 1;
        if (mesh->models[controller->getEntity().modelIndex - 1].geometry[mesh->transparent].count) {
            setShader(Core::pass, Shader::ENTITY, false, true);
            controller->render(NULL, mesh, Shader::ENTITY, false);
        }
        mesh->transparent = 0;
    }

    struct NearObj {
        int   player;
        int   index;
        float dist;
        NearObj() {}
        NearObj(int player, int index, float dist) : player(player), index(index), dist(dist) {}
    };

    int getNearObjects(NearObj *nearObj, int maxCount) {
        int count = 0;

        nearObj[count++] = NearObj(0, players[0]->entity, 0.0f);
        if (players[1])
            nearObj[count++] = NearObj(1, players[1]->entity, 0.0f);
        int base = count;

        for (int i = 0; i < level.entitiesCount; i++) {
            TR::Entity &e = level.entities[i];
            Controller *controller = (Controller*)e.controller;
            if (controller && e.castShadow() && controller != players[0] && controller != players[1]) {
                int pIndex = 0;
                float dist = (players[0]->getPos() - controller->getPos()).length2();
                if (players[1]) {
                    float dist2 = (players[1]->getPos() - controller->getPos()).length2();
                    if (dist2 < dist) {
                        dist = dist2;
                        pIndex = 1;
                    }
                }

                if (dist > 8192 * 8192) continue;

            // get index to insert
                int index = base;
                while (index < count) {
                    if (dist < nearObj[index].dist)
                        break;
                    index++;
                }
            // insertion
                if (index < maxCount) {
                    if (count < maxCount)
                        count++;
                    for (int j = count - 1; j > index; j--)
                        nearObj[j] = nearObj[j - 1];
                    nearObj[index] = NearObj(pIndex, controller->entity, dist);
                }
            }
        }

        return count;
    }
*/
    void renderShadows(int roomIndex, Texture *shadowMap) {
        PROFILE_MARKER("PASS_SHADOW");

        if (Core::settings.detail.shadows == Core::Settings::LOW)
            return;
        ASSERT(shadow);

    // render to shadow map
        float oldEye = Core::eye;
        Core::eye = 0.0f;

        Core::pass = Core::passShadow;
        shadowMap->unbind(sShadow);
        bool colorShadow = shadowMap->fmt == FMT_RGBA ? true : false;
        if (colorShadow)
            Core::setClearColor(vec4(1.0f));
        Core::setTarget(shadowMap, NULL, RT_CLEAR_DEPTH | (colorShadow ? (RT_CLEAR_COLOR | RT_STORE_COLOR) : RT_STORE_DEPTH));
        //Core::setCullMode(cmBack);
        Core::validateRenderState();

        /*
        if (Core::settings.detail.shadows > Core::Settings::MEDIUM) { // per-object shadow map (atlas)
            NearObj nearObj[SHADOW_OBJ_MAX];
            int nearCount = getNearObjects(nearObj, SHADOW_OBJ_MAX);

            for (int i = 0; i < nearCount; i++) {
                Core::setViewport((i % SHADOW_OBJ_COLS) * SHADOW_TEX_TILE, (i / SHADOW_OBJ_COLS) * SHADOW_TEX_TILE, SHADOW_TEX_TILE, SHADOW_TEX_TILE);
                renderShadowEntity(i, (Controller*)level.entities[nearObj[i].index].controller, players[nearObj[i].player]);
            }

            for (int i = nearCount; i < SHADOW_OBJ_MAX; i++)
                Core::mLightProj[i].identity();
        } else // all-in-one shadow map
        */
        renderShadowView(roomIndex);

        //Core::setCullMode(cmFront);
        if (colorShadow)
            Core::setClearColor(vec4(0.0f));

        #ifdef _GAPI_D3D11 // TODO render pass
            Core::setTarget(NULL, NULL, RT_CLEAR_DEPTH | RT_CLEAR_COLOR | RT_STORE_COLOR | RT_STORE_DEPTH);
            Core::validateRenderState();
        #endif

        Core::eye = oldEye;
    }

    #ifdef DEBUG_RENDER
    void renderDebug() {
        if (level.isTitle() || inventory->titleTimer > 1.0f) return;

        Core::setViewport(Core::x, Core::y, Core::width, Core::height);
        camera->setup(true);

        Debug::begin();
        /*
        lara->updateEntity(); // TODO clip angle while rotating

        int q = int(normalizeAngle(lara->angleExt + PI * 0.25f) / (PI * 0.5f));
        float radius = 256.0f;

        const vec2 v[] = {
            { -radius,  radius },
            {  radius,  radius },
            {  radius, -radius },
            { -radius, -radius },
        };

        const vec2 &l = v[q],
                   &r = v[(q + 1) % 4],
                   &f = (q %= 2) ? vec2(l.x, radius * cosf(lara->angleExt)) : vec2(radius * sinf(lara->angleExt), l.y);

        vec3 F = vec3(f.x, 0.0f, f.y);
        vec3 L = vec3(l.x, 0.0f, l.y);
        vec3 R = vec3(r.x, 0.0f, r.y);

        vec3 p, n = lara->pos + vec3(0.0f, -512.0f, 0.0f);
        
        Core::setDepthTest(false);
        glBegin(GL_LINES);
            glColor3f(0, 0, 1); p = n; glVertex3fv((GLfloat*)&p); p += F; glVertex3fv((GLfloat*)&p);
            glColor3f(1, 0, 0); p = n; glVertex3fv((GLfloat*)&p); p += L; glVertex3fv((GLfloat*)&p);
            glColor3f(0, 1, 0); p = n; glVertex3fv((GLfloat*)&p); p += R; glVertex3fv((GLfloat*)&p);
            glColor3f(1, 1, 0); p = lara->pos; glVertex3fv((GLfloat*)&p); p -= vec3(0.0f, LARA_HANG_OFFSET, 0.0f); glVertex3fv((GLfloat*)&p);
        glEnd();
        Core::setDepthTest(true);
        */

        /*
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glOrtho(0, Core::width, 0, Core::height, 0, 1);

            if (shadow)
                shadow->bind(sDiffuse);
            else
                atlas->bind(sDiffuse);
            glEnable(GL_TEXTURE_2D);
            Core::setCullMode(cmNone);
            Core::setDepthTest(false);
            Core::setBlendMode(bmNone);
            Core::validateRenderState();

            glColor3f(10, 10, 10);
            float w = float(Core::active.textures[sDiffuse]->width);
            float h = float(Core::active.textures[sDiffuse]->height);
            glBegin(GL_QUADS);
                glTexCoord2f(0, 0); glVertex2f(0, 0);
                glTexCoord2f(1, 0); glVertex2f(w, 0);
                glTexCoord2f(1, 1); glVertex2f(w, h);
                glTexCoord2f(0, 1); glVertex2f(0, h);
            glEnd();
            glColor3f(1, 1, 1);

            glDisable(GL_TEXTURE_2D);

            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            glPopMatrix();
        */

        /*
        Core::setDepthTest(false);
        glBegin(GL_LINES);
            glColor3f(1, 1, 1);
            glVertex3fv((GLfloat*)&lara->pos);
            glVertex3fv((GLfloat*)&lara->mainLightPos);
        glEnd();
        Core::setDepthTest(true);
        */
        //    Debug::Draw::sphere(lara->mainLightPos, lara->mainLightColor.w, vec4(1, 1, 0, 1));

        //    Box bbox = lara->getBoundingBox();
        //    Debug::Draw::box(bbox.min, bbox.max, vec4(1, 0, 1, 1));

            Core::setBlendMode(bmAlpha);
            Core::setDepthTest(false);
            Core::validateRenderState();
        //    Debug::Level::rooms(level, lara->pos, lara->getEntity().room);
        //    Debug::Level::lights(level, player->getRoomIndex(), player);
        //    Debug::Level::sectors(this, players[0]->getRoomIndex(), (int)players[0]->pos.y);
        //    Core::setDepthTest(false);
        //    Debug::Level::portals(level);
        //    Core::setDepthTest(true);
        //    Debug::Level::meshes(level);
        //    Debug::Level::entities(level);
        //    Debug::Level::zones(this, players[0]);
        //    Debug::Level::blocks(level);
        //    Debug::Level::path(level, (Enemy*)level.entities[21].controller);
        //    Debug::Level::debugOverlaps(level, players[0]->box);
        //    Debug::Level::debugBoxes(level, lara->dbgBoxes, lara->dbgBoxesCount);
            Core::setDepthTest(true);
            Core::setBlendMode(bmNone);
        /*// render ambient cube
            Core::validateRenderState();

            static int dbg_ambient = 0;
            dbg_ambient = int(params->time * 2) % 4;

            shadow->unbind(sShadow);
            Core::whiteCube->unbind(sEnvironment);

            glActiveTexture(GL_TEXTURE0);
            glEnable(GL_TEXTURE_2D);
            glDisable(GL_CULL_FACE);
            glColor3f(1, 1, 1);
            for (int j = 0; j < 6; j++) {
                glPushMatrix();
                glTranslatef(player->pos.x, player->pos.y - 1024, player->pos.z);
                switch (j) {
                    case 0 : glRotatef( 90, 0, 1, 0); break;
                    case 1 : glRotatef(-90, 0, 1, 0); break;
                    case 2 : glRotatef(-90, 1, 0, 0); break;
                    case 3 : glRotatef( 90, 1, 0, 0); break;
                    case 4 : glRotatef(  0, 0, 1, 0); break;
                    case 5 : glRotatef(180, 0, 1, 0); break;
                }
                glTranslatef(0, 0, 256);
                
                ambientCache->textures[j * 4 + dbg_ambient]->bind(sDiffuse);
                glBegin(GL_QUADS);
                    glTexCoord2f(0, 0); glVertex3f(-256,  256, 0);
                    glTexCoord2f(1, 0); glVertex3f( 256,  256, 0);
                    glTexCoord2f(1, 1); glVertex3f( 256, -256, 0);
                    glTexCoord2f(0, 1); glVertex3f(-256, -256, 0);
                glEnd();
                glPopMatrix();
            }
            glEnable(GL_CULL_FACE);
            glDisable(GL_TEXTURE_2D);


            glLineWidth(4);
            glBegin(GL_LINES);
            float S = 64.0f;
            for (int i = 0; i < level.roomsCount; i++) {
                if (i != players[0]->getRoomIndex()) continue;
                TR::Room &r = level.rooms[i];                
                for (int j = 0; j < r.xSectors * r.zSectors; j++) {
                    TR::Room::Sector &s = r.sectors[j];
                    vec3 p = vec3(float((j / r.zSectors) * 1024 + 512 + r.info.x),
                                  float(max((s.floor - 2) * 256, (s.floor + s.ceiling) * 256 / 2)),
                                  float((j % r.zSectors) * 1024 + 512 + r.info.z));

                    AmbientCache::Cube &cube = ambientCache->items[ambientCache->offsets[i] + j];
                    if (cube.status == AmbientCache::Cube::READY) {
                        glColor3fv((GLfloat*)&cube.colors[0]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x + S, p.y + 0, p.z + 0);

                        glColor3fv((GLfloat*)&cube.colors[1]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x - S, p.y + 0, p.z + 0);

                        glColor3fv((GLfloat*)&cube.colors[2]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x + 0, p.y + S, p.z + 0);

                        glColor3fv((GLfloat*)&cube.colors[3]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x + 0, p.y - S, p.z + 0);

                        glColor3fv((GLfloat*)&cube.colors[4]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x + 0, p.y + 0, p.z + S);

                        glColor3fv((GLfloat*)&cube.colors[5]);
                        glVertex3f(p.x + 0, p.y + 0, p.z + 0);
                        glVertex3f(p.x + 0, p.y + 0, p.z - S);
                    }
                }
            }
            glEnd();
            glLineWidth(1);           
        */

            Debug::Level::info(this, player, player->animation);


        Debug::end();
    }
    #endif

    float setViewport(int view, int eye) {
        int vX = Core::x;
        int vY = Core::y;
        int vW = Core::width;
        int vH = Core::height;

        float aspect = float(vW) / float(vH) * Core::aspectFix;

        if (Core::defaultTarget) {
            vX = 0;
            vY = 0;
            vW = Core::defaultTarget->width;
            vH = Core::defaultTarget->height;
        }

        short4 &vp = Core::viewportDef;
        vp = short4(vX, vY, vW, vH);

        if (players[1] != NULL && view >= 0) {
            vp = short4(vX + vW / 2 * view, vY, vW / 2, vH);

            if (Core::settings.detail.stereo != Core::Settings::STEREO_SPLIT) {
                aspect *= 0.5f;
            }
        }

        if (Core::settings.detail.stereo == Core::Settings::STEREO_SBS) {
            switch (eye) {
                case -1 : vp = short4(vX + vp.x - vp.x / 2, vY + vp.y, vp.z / 2, vp.w);   break;
                case +1 : vp = short4(vX + vW / 2 + vp.x / 2, vY + vp.y, vp.z / 2, vp.w); break;
            }
        }

        Core::setViewport(vp.x, vp.y, vp.z, vp.w);

        return aspect;
    }

    void renderPrepare() {
        setupBinding();

        #ifdef _OS_3DS
            static float sliderState = -1.0f;

            float slider = osGet3DSliderState();
            bool isStereo = slider > 0.0f && !inventory->video;

            if (gfxIs3D() != isStereo) {
                gfxSet3D(isStereo);
                needRedrawTitleBG = inventory->active && !level.isTitle();
            }

            Core::settings.detail.stereo = isStereo ? Core::Settings::STEREO_ANAGLYPH : Core::Settings::STEREO_OFF;

            if (slider != sliderState) {
                sliderState = slider;
                needRedrawTitleBG = inventory->active && !level.isTitle();
            }
        #else
            if (Core::settings.detail.stereo == Core::Settings::STEREO_ANAGLYPH) {
                for (int i = 0; i < 2; i++) {
                    Texture *&tex = Core::eyeTex[i];
                    if (!tex || tex->origWidth != Core::width || tex->origHeight != Core::height) {
                        delete tex;
                        tex = new Texture(Core::width, Core::height, 1, FMT_RGBA, OPT_TARGET | OPT_NEAREST);
                    }
                }
            }
        #endif

        needRenderGame = !inventory->video && !level.isTitle() && ((inventory->phaseRing < 1.0f && inventory->titleTimer <= 1.0f) || needRedrawTitleBG);
        needRenderInventory = inventory->video || level.isTitle() || inventory->phaseRing > 0.0f || inventory->titleTimer > 0.0f;

        bool title  = inventory->isActive() || level.isTitle();
        bool copyBg = title && (lastTitle != title || needRedrawTitleBG);
        lastTitle = title;
        needRedrawTitleBG = false;

        if (!needRenderGame && !copyBg)
            return;

        if (needRedrawReflections) {
            initReflections();
        }

        if (ambientCache) {
            ambientCache->processQueue();
        }

    #ifndef FFP
        if (shadow[0] && players[0]) {
            player = players[0];
            renderShadows(player->getRoomIndex(), shadow[0]);

            if (players[1]) {
                if (!shadow[1]) {
                    shadow[1] = new Texture(shadow[0]->origWidth, shadow[0]->origHeight, 1, shadow[0]->fmt, shadow[0]->opt);
                }

                player = players[1];
                renderShadows(player->getRoomIndex(), shadow[1]);
            }
        }
    #endif

        if (copyBg) {
            inventory->prepareBackground();
        }
    }

    void setDefaultTarget(int eye, int view, bool invBG) {
        int texIndex = eye <= 0 ? 0 : 1;

        if (invBG) {
            if (Core::settings.detail.stereo == Core::Settings::STEREO_SPLIT) {
                Core::defaultTarget = inventory->getBackgroundTarget(view);
            } else {
                Core::defaultTarget = inventory->getBackgroundTarget(texIndex);
            }
        } else {
            if (Core::settings.detail.stereo == Core::Settings::STEREO_ANAGLYPH || Core::settings.detail.stereo == Core::Settings::STEREO_VR) {
                Core::defaultTarget = Core::eyeTex[texIndex];
            } else {
            #ifdef _OS_3DS
                Core::defaultTarget = Core::eyeTex[0];
            #endif
            }
        }

        if (Core::defaultTarget) {
            Core::viewportDef = short4(0, 0, Core::defaultTarget->origWidth, Core::defaultTarget->origHeight);
        } else {
            Core::viewportDef = short4(0, 0, Core::width, Core::height);
        }

        #ifdef EARLY_CLEAR
            if (view == 0 && eye <= 0) {
                Core::setTarget(NULL, NULL, RT_CLEAR_COLOR | RT_CLEAR_DEPTH | RT_STORE_COLOR | RT_STORE_DEPTH);
                Core::validateRenderState();
            }
        #endif
    }

    void renderEye(int eye, bool showUI, bool invBG) {
        float          oldEye      = Core::eye;
        GAPI::Texture *oldTarget   = Core::defaultTarget;

        Core::eye = float(eye);

        #ifdef _OS_3DS
            Core::eye *= osGet3DSliderState() * 3.25f;
        #endif

        if (needRenderGame || invBG) {
            int viewsCount = players[1] ? 2 : 1;
            for (int view = 0; view < viewsCount; view++) {
                player = players[view];
                camera = player->camera;

                Core::mLightProj = mLightProj[view];

                Core::pass = Core::passCompose;

                short4 oldViewport = Core::viewportDef;

                setDefaultTarget(eye, view, invBG);

                if (Core::settings.detail.stereo == Core::Settings::STEREO_SPLIT) {
                    camera->aspect = setViewport(invBG ? -1 : view, invBG ? 0 : eye);
                } else {
                    camera->aspect = setViewport(view, invBG ? 0 : eye);
                }

                setup();
                renderView(camera->getRoomIndex(), true, showUI);

                Core::viewportDef = oldViewport;
            }
        }

        if (needRenderInventory && !invBG) {
            if (players[1] && Core::settings.detail.stereo == Core::Settings::STEREO_SPLIT) {
                renderInventoryEye(eye, 0);
                renderInventoryEye(eye, 1);
            } else {
                renderInventoryEye(eye, -1);
            }
        }

        Core::defaultTarget = oldTarget;
        Core::eye           = oldEye;

        Core::setViewport(Core::viewportDef);
        Core::setScissor(Core::viewportDef);

        player = players[0];
        if (player) {
            camera = player->camera;
        }
    }

    void renderGame(bool showUI, bool invBG) {
        short4         oldViewport = Core::viewportDef;
        GAPI::Texture *oldTarget   = Core::defaultTarget;

        bool upscale = !invBG && Core::settings.detail.scale != Core::Settings::SCALE_100;

        if (upscale) {
            int scale = (Core::settings.detail.scale + 1) * 25;
            int w = Core::width  * scale / 100;
            int h = Core::height * scale / 100;
            if (!scaleTex || scaleTex->width != w || scaleTex->height != h) {
                delete scaleTex;
                scaleTex = new Texture(w, h, 1, FMT_RGBA, OPT_TARGET);
            }
            Core::defaultTarget = scaleTex;
            Core::viewportDef   = short4(0, 0, w, h);
        }

        if (Core::eye == 0.0f && Core::settings.detail.isStereo()) {
            renderEye(-1, showUI, invBG);
            renderEye(+1, showUI, invBG);
        }  else {
            renderEye(int(Core::eye), showUI, invBG);
        }

    #ifndef _OS_3DS
        if (!invBG && Core::settings.detail.stereo == Core::Settings::STEREO_ANAGLYPH) {
            mat4 mProj, mView;
            mView.identity();
            mProj = GAPI::ortho(-1, +1, -1, +1, 0, 1);
            mProj.scale(vec3(1.0f / 32767.0f));
            Core::setViewProj(mView, mProj);

            Core::setDepthTest(false);
            Core::setDepthWrite(false);

            Core::setTarget(NULL, NULL, RT_STORE_COLOR);
            Core::validateRenderState();
            setShader(Core::passFilter, Shader::FILTER_ANAGLYPH, false, false);
            Core::eyeTex[0]->bind(sDiffuse);
            Core::eyeTex[1]->bind(sNormal);
            Core::setDepthTest(false);
            mesh->renderQuad();

            Core::setDepthTest(true);
            Core::setDepthWrite(true);
        }
    #endif

        Core::defaultTarget = oldTarget;
        Core::viewportDef   = oldViewport;

        if (upscale) {
            mat4 mProj, mView;
            mView.identity();
            mProj = GAPI::ortho(-1, +1, -1, +1, 0, 1);
            mProj.scale(vec3(1.0f / 32767.0f));
            Core::setViewProj(mView, mProj);

            Core::setTarget(NULL, NULL, RT_STORE_COLOR);
            setShader(Core::passFilter, Shader::FILTER_UPSCALE, false, false);
            Core::active.shader->setParam(uParam, vec4(float(scaleTex->width), float(scaleTex->height), 0.0f, 0.0f));
            scaleTex->bind(sDiffuse);
            Core::setDepthTest(false);
            mesh->renderQuad();
            Core::setDepthTest(true);
            Core::setDepthWrite(true);
        }

        // TODO render all UI with native resolution here
    }

    void renderUI() {
        if (inventory->titleTimer > 1.0f || level.isTitle()) return;

        #ifdef _GAPI_SW
            GAPI::setPalette(GAPI::swPaletteColor);
            GAPI::setShading(false);
        #endif

        Core::pushLights();

        UI::begin(camera->aspect);

        atlasObjects->bind(sDiffuse);
        UI::renderPickups();

        atlasGlyphs->bind(sDiffuse);

        Core::resetLights();

        if (!level.isCutsceneLevel() && !camera->spectator) {
        // render health & oxygen bars
            vec2 size = vec2(180, 10);

            float health = player->health / float(LARA_MAX_HEALTH);
            float oxygen = player->oxygen / float(LARA_MAX_OXYGEN);

            if ((params->time - int(params->time)) < 0.5f) { // blinking
                if (health <= 0.2f) health = 0.0f;
                if (oxygen <= 0.2f) oxygen = 0.0f;
            }

            vec2 pos;
            if (Core::settings.detail.stereo == Core::Settings::STEREO_VR)
                pos = vec2((UI::width - size.x) * 0.5f, 96);
            else
                pos = vec2(UI::width - 32 - size.x, 32);

            if (!player->dozy && (player->stand == Lara::STAND_ONWATER || player->stand == Character::STAND_UNDERWATER)) {
                UI::renderBar(CTEX_OXYGEN, pos, size, oxygen);
                pos.y += 16.0f;
            }

            if ((!inventory->active && ((player->wpnReady() && !player->emptyHands()) || player->damageTime > 0.0f || health <= 0.2f))) {
                UI::renderBar(CTEX_HEALTH, pos, size, health);
                pos.y += 32.0f;

                if (!inventory->active && !player->emptyHands()) { // ammo
                    int index = inventory->contains(player->wpnCurrent);
                    if (index > -1)
                        inventory->renderItemCount(inventory->items[index], pos, size.x);
                }
            }

            UI::renderHelp();
        }

        if (!camera->spectator) {
            UI::renderSubs();
        }

        UI::end();

        Core::popLights();
    }

    void renderInventoryEye(int eye, int view) {
        short4 oldViewport = Core::viewportDef;

        setDefaultTarget(eye, view, false);
	#ifdef FFP //fixme: psp framebuffer error?
		Core::setTarget(NULL, NULL, RT_CLEAR_DEPTH | RT_CLEAR_COLOR | RT_STORE_COLOR);
	#else
		Core::setTarget(NULL, NULL, RT_CLEAR_DEPTH | RT_STORE_COLOR);
	#endif

        float aspect = setViewport(view, eye);

        Core::pushLights();
        Core::resetLights();

        if (inventory->video) {
            inventory->render(1.0);

            if (UI::subsStr != STR_EMPTY) {
                UI::begin(float(Core::width) / float(Core::height));
                atlasGlyphs->bind(sDiffuse);
                UI::renderSubs();
                UI::end();
            }
        }

        if (!inventory->video) {
            inventory->renderBackground(max(0, view));
        }

        setupBinding();
        atlasObjects->bind(sDiffuse);
        inventory->render(aspect);

        UI::begin(aspect);
        atlasGlyphs->bind(sDiffuse);
        if (!inventory->video) {
            inventory->renderUI();
        } else {
            UI::renderSubs();
        }
        UI::end();

        Core::popLights();

        Core::viewportDef = oldViewport;
    }

    void render() {
        if (isEnded && !inventory->video) {
            Core::setTarget(NULL, NULL, RT_CLEAR_COLOR | RT_STORE_COLOR);
            UI::begin(float(Core::width) / float(Core::height));
            atlasGlyphs->bind(sDiffuse);
            UI::textOut(vec2(0, 480 - 16), STR_LOADING, UI::aCenter, UI::width);
            UI::end();
            return;
        }

        renderGame(true, false);
    }

};

#endif
