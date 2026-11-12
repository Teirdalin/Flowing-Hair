#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Appearance.h>
#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/GameData.h>
#include <kenshi/Renderer.h>
#include <kenshi/PlayerInterface.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_InputManager.h>
#include <ogre/OgreAnimationState.h>
#include <ogre/OgreAxisAlignedBox.h>
#include <ogre/OgreEntity.h>
#include <ogre/OgreCamera.h>
#include <ogre/OgreAutoParamDataSource.h>
#include <ogre/OgreViewport.h>
#include <ogre/OgreException.h>
#include <ogre/OgreHardwareBufferManager.h>
#include <ogre/OgreHardwareIndexBuffer.h>
#include <ogre/OgreMesh.h>
#include <ogre/OgreMeshManager.h>
#include <ogre/OgreMatrix4.h>
#include <ogre/OgreMaterialManager.h>
#include <ogre/OgreTechnique.h>
#include <ogre/OgrePass.h>
#include <ogre/OgreGpuProgramParams.h>
#include <ogre/OgreRenderObjectListener.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreSubEntity.h>
#include <ogre/OgreSubMesh.h>
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ClothSolver.h"
#include "WindField.h"
#include "IntegratedWind.h"
#include "SkinPalette.h"
#include "HairCandidate.h"
#include "WeatherDynamics.h"
#include "NativeWeather.h"
#include "HairDistance.h"
#include "KhajjitTail.h"
#include "TailDynamics.h"
#include "TailFollow.h"
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_TabControl.h>
#include <mygui/MyGUI_TabItem.h>

namespace
{
    // Own visible flag is safe through the native export. Scene/pass masks are
    // transient and must not be used as persistent main-view visibility.
    typedef bool (*ObjectVisibleFn)(const Ogre::MovableObject*);
    ObjectVisibleFn gObjectVisible=0;
    unsigned int gVisibilityReports=0;
    ULONGLONG gNextVisibilityReport=0;
    struct BodyWatch {
        Character* character; // identity only; checked in current native roster/update set before reuse
        Ogre::Entity* body;
        std::vector<Ogre::Renderable*> parts;
        flowing_hair::MainViewDrawState draw;
        unsigned long long touched;
        double distanceSquared;
        BodyWatch() : character(0),body(0),touched(0),distanceSquared(DBL_MAX) {}
    };
    typedef std::map<AppearanceBase*,BodyWatch> BodyWatchMap;
    BodyWatchMap gBodyWatches;
    std::map<Ogre::Renderable*,BodyWatch*> gBodyRenderables;
    const Ogre::Camera* gMainViewCamera=0; // identity only; never dereferenced later
    Ogre::SceneManager* gWatchScene=0; // identity only; revalidated renderer supplies registration target
    unsigned long long gVisibilityFrame=0;

    void clearBodyWatches()
    {
        gMainViewCamera=0; gWatchScene=0;
        gBodyRenderables.clear(); gBodyWatches.clear(); gVisibilityFrame=0;
    }
    void forgetBodyParts(BodyWatch& watch)
    {
        for (size_t i=0;i<watch.parts.size();++i)
        {
            std::map<Ogre::Renderable*,BodyWatch*>::iterator part=gBodyRenderables.find(watch.parts[i]);
            if (part!=gBodyRenderables.end() && part->second==&watch) gBodyRenderables.erase(part);
        }
        watch.parts.clear(); watch.draw=flowing_hair::MainViewDrawState();
    }
    bool bodyDrawnInMainView(Character* character,AppearanceBase* appearance,Ogre::Entity* body,double distanceSquared)
    {
        BodyWatchMap::iterator found=gBodyWatches.find(appearance);
        if (found==gBodyWatches.end())
        {
            if (gBodyWatches.size()>=512)
            {
                BodyWatchMap::iterator farthest=gBodyWatches.begin();
                for (BodyWatchMap::iterator w=gBodyWatches.begin();w!=gBodyWatches.end();++w)
                    if (w->second.distanceSquared>farthest->second.distanceSquared) farthest=w;
                if (distanceSquared>=farthest->second.distanceSquared) return false;
                forgetBodyParts(farthest->second); gBodyWatches.erase(farthest);
            }
            found=gBodyWatches.insert(std::make_pair(appearance,BodyWatch())).first;
        }
        BodyWatch& watch=found->second;
        watch.character=character;
        watch.distanceSquared=distanceSquared;
        watch.touched=gVisibilityFrame;
        if (watch.body!=body || watch.parts.empty())
        {
            forgetBodyParts(watch); watch.body=body;
            const size_t count=std::min(static_cast<size_t>(body->getNumSubEntities()),static_cast<size_t>(64));
            for (size_t i=0;i<count;++i)
            {
                Ogre::Renderable* part=body->getSubEntity(i);
                watch.parts.push_back(part); gBodyRenderables[part]=&watch;
            }
        }
        return watch.draw.recent(gVisibilityFrame);
    }

    struct Config
    {
        bool enabled, simulateNpcs, proceduralWind, useGameWind, stressWindHotkey;
        int iterations;
        float gravity, windStrength, gustStrength, gustFrequency, windDirection;
        float windDirectionVariation, windVertical, windSpatialScale, gameWindMultiplier;
        float inertia, damping, bending, shapeStiffness, maxAcceleration, stressAcceleration;
        float crownFraction, scalpShell;
        float movementDrag, maximumMovementAcceleration;
        float beardRootFraction, beardShapeStiffness;
        float hairRigidity, hairDistance, hairCharacterLimit;
        bool wetHair, khajjitTails, catTails;
        float soakSeconds,drySeconds,wetExtraWeight;
        bool faceCollision;
        Config() : enabled(true), simulateNpcs(true), proceduralWind(true), useGameWind(true),
            stressWindHotkey(true),
            iterations(8), gravity(7.5f), windStrength(3.0f),
            gustStrength(3.8f), gustFrequency(0.11f), windDirection(35.0f),
            windDirectionVariation(55.0f), windVertical(0.5f), windSpatialScale(0.012f),
            gameWindMultiplier(0.45f), inertia(0.035f), damping(0.035f),
            bending(0.45f), shapeStiffness(0.018f), maxAcceleration(40.0f), stressAcceleration(240.0f),
            crownFraction(0.04f), scalpShell(0.16f), movementDrag(12.0f), maximumMovementAcceleration(120.0f),
            beardRootFraction(0.04f), beardShapeStiffness(0.0045f), hairRigidity(50.0f), hairDistance(500.0f), hairCharacterLimit(10.0f),
            wetHair(true),khajjitTails(true),catTails(true),soakSeconds(20.0f),drySeconds(120.0f),wetExtraWeight(0.75f),faceCollision(true) {}
    };

    struct Key
    {
        int x, y, z;
        bool operator<(const Key& rhs) const
        {
            if (x != rhs.x) return x < rhs.x;
            if (y != rhs.y) return y < rhs.y;
            return z < rhs.z;
        }
    };

    struct PaletteBinding
    {
        size_t subEntity;
        size_t capacity;
        Ogre::GpuProgramParametersSharedPtr parameters;
    };

    struct HairStream
    {
        Ogre::VertexData* vertexData;
        const Ogre::VertexElement* positionElement;
        Ogre::HardwareVertexBufferSharedPtr positionBuffer;
        std::vector<unsigned char> vertexBytes;
        size_t renderOffset;
        HairStream() : vertexData(0), positionElement(0), renderOffset(0) {}
    };

    struct TailCustomBinding
    {
        Ogre::GpuProgramParametersSharedPtr parameters;
        Ogre::GpuProgramParameters::AutoConstantEntry entry;
        TailCustomBinding(const Ogre::GpuProgramParametersSharedPtr& p,const Ogre::GpuProgramParameters::AutoConstantEntry& e)
            : parameters(p),entry(e) {}
    };

    struct Hair
    {
        AppearanceBase* owner;
        GameData* hairStyle;
        Ogre::SceneNode* root;
        Ogre::Entity* original;
        Ogre::Entity* visibleClone;
        Ogre::SceneManager* manager;
        std::string cloneName;
        Ogre::MeshPtr mesh;
        std::vector<HairStream> streams;
        std::vector<flowing_hair::Vec3> bindPositions;
        std::vector<unsigned int> renderToParticle;
        flowing_hair::ClothSolver solver;
        flowing_hair::TailDynamics tailDynamics;
        flowing_hair::TailFollow tailFollow;
        float tailFollowDt;
        bool tailFollowPending;
        flowing_hair::Ellipsoid collider;
        Ogre::Vector3 lastPosition, lastVelocity;
        std::string sourceName;
        float maximumDisplacement;
        float maximumObservedDisplacement;
        float wetness,rainExposure,rainSampleTimer;
        bool swimming;
        unsigned int telemetryFrames;
        bool motionReady, stressReported, uploadReported, movementReported, faceContactReported;
        std::vector<Ogre::MaterialPtr> privateMaterials;
        std::vector<PaletteBinding> paletteBindings;
        std::vector<Ogre::Renderable*> renderables;
        bool renderPoseReported, renderPoseFailed;
        bool tailSlot;
        bool catTail;
        Ogre::SubEntity* tailBodyPart; // identity key only outside a validated appearance
        Ogre::MeshPtr tailSourceMesh;
        Ogre::HardwareIndexBufferSharedPtr tailSourceIndices;
        size_t tailSourceStart,tailSourceCount;
        Ogre::IndexData tailBodyIndices, tailEmptyIndices;
        std::string tailSourceMaterial;
        std::vector<std::pair<size_t,size_t> > tailUploadRanges;
        std::vector<TailCustomBinding> tailCustomBindings;
        float tailPoseWeights[60];
        Ogre::Quaternion tailPose;
        bool tailPoseValid;
        Hair() : owner(0), hairStyle(0), root(0), original(0), visibleClone(0), manager(0),
            tailFollowDt(0),tailFollowPending(false),maximumDisplacement(1.0f),
            maximumObservedDisplacement(0.0f), wetness(0),rainExposure(0),rainSampleTimer(0),swimming(false),telemetryFrames(0),
            motionReady(false), stressReported(false), uploadReported(false), movementReported(false), faceContactReported(false),
            renderPoseReported(false), renderPoseFailed(false),tailSlot(false),catTail(false),tailBodyPart(0),tailSourceStart(0),tailSourceCount(0),
            tailPoseValid(false) { std::memset(tailPoseWeights,0,sizeof(tailPoseWeights)); }
    };

    Config gConfig;
    volatile bool gInternalDebug = false;
    volatile bool gInternalStressWind = false;
    volatile bool gInternalShowOriginal = false;
    HMODULE gModule = 0;
    FILE* gLog = 0;
    enum AttachmentSlot { ScalpSlot=0, BeardSlot=1, TailSlot=2, SlotCount=3 };
    typedef std::pair<AppearanceBase*, int> HairKey;
    typedef std::map<HairKey, Hair*> HairMap;
    HairMap gHair;
    std::map<Ogre::Renderable*, Hair*> gRenderHair;
    std::map<Ogre::SubEntity*, Hair*> gTailBodies;
    void (*gOriginalSubEntityDraw)(Ogre::SubEntity*,Ogre::RenderOperation&) = 0;
    bool gTailDrawHookReady=false;

    // Post-hook: native draw setup exactly once; replace ONLY the returned
    // operation's index-data pointer. Never edit source SubMesh/IndexData or
    // SubEntity start/end fields (native Ogre writes those into shared data).
    // Installation and lifecycle changes occur at the existing GPU-sensitive
    // main-loop boundary, just like the private hair render registrations.
    void subEntityDraw(Ogre::SubEntity* self,Ogre::RenderOperation& operation)
    {
        gOriginalSubEntityDraw(self,operation);
        std::map<Ogre::SubEntity*,Hair*>::iterator body=gTailBodies.find(self);
        if (body!=gTailBodies.end())
        {
            Hair* hair=body->second;
            if (hair->renderPoseFailed || gInternalShowOriginal) return;
            Ogre::IndexData* expected=hair->tailSourceMesh->getSubMesh(0)->indexData;
            // Another mod, LOD, or appearance rebuild owns a different draw:
            // immediately fall back to the complete native body and tail.
            if (operation.indexData!=expected || expected->indexBuffer.get()!=hair->tailSourceIndices.get() ||
                expected->indexStart!=hair->tailSourceStart || expected->indexCount!=hair->tailSourceCount || !operation.useIndexes ||
                operation.operationType!=Ogre::RenderOperation::OT_TRIANGLE_LIST)
            { hair->renderPoseFailed=true; return; }
            operation.indexData=&hair->tailBodyIndices;
        }
        else
        {
            std::map<Ogre::Renderable*,Hair*>::iterator clone=gRenderHair.find(self);
            if (clone!=gRenderHair.end() && clone->second->tailSlot &&
                (clone->second->renderPoseFailed || gInternalShowOriginal))
            { operation.useIndexes=true; operation.indexData=&clone->second->tailEmptyIndices; }
        }
    }
    std::set<Ogre::Entity*> gClones;
    std::set<std::string> gSkipMessages;
    std::set<AppearanceBase*> gNoCandidateLogged;
    std::set<std::string> gRuntimeFailures;
    std::map<Character*, std::string> gPlayerAudit;
    Character* gNextNpc = 0;
    bool gStressWind = false;
    bool gShowOriginal = false;
    bool gF8WasDown = false;
    bool gPWasDown = false;
    float gStressWindBlend = 0.0f;
    GameWorld* gTrackedWorld = 0;
    bool gWorldReady = false;
    const char* gRuntimePhase = "idle";
    void (*gOriginalMainLoop)(GameWorld*, float) = 0;
    unsigned int gId = 1;

    std::string directory()
    {
        char value[MAX_PATH] = {0};
        if (!gModule || !GetModuleFileNameA(gModule, value, MAX_PATH)) return ".";
        std::string path(value);
        const std::string::size_type split = path.find_last_of("\\/");
        return split == std::string::npos ? "." : path.substr(0, split);
    }

    void logLine(const std::string& text, bool error)
    {
        if (!error && !gInternalDebug) return;
        const std::string line = std::string("[Flowing Hair] ") + text;
        if (error) ErrorLog(line); else DebugLog(line);
        if (gLog) { std::fprintf(gLog, "%s\n", line.c_str()); std::fflush(gLog); }
    }

    void skipOnce(const std::string& mesh, const std::string& why)
    {
        if (gSkipMessages.insert(mesh + "|" + why).second)
            logLine("Skipped '" + mesh + "': " + why, false);
    }

    bool finiteFloat(float value)
    {
        return value == value && value >= -FLT_MAX && value <= FLT_MAX;
    }

    bool finiteVector(const flowing_hair::Vec3& value)
    {
        return finiteFloat(value.x) && finiteFloat(value.y) && finiteFloat(value.z);
    }

    bool finiteVector(const Ogre::Vector3& value)
    {
        return finiteFloat(value.x) && finiteFloat(value.y) && finiteFloat(value.z);
    }

    bool finiteQuaternion(const Ogre::Quaternion& value)
    {
        if (!finiteFloat(value.w) || !finiteFloat(value.x) ||
            !finiteFloat(value.y) || !finiteFloat(value.z)) return false;
        const float norm = value.w * value.w + value.x * value.x +
            value.y * value.y + value.z * value.z;
        return finiteFloat(norm) && norm > 0.000001f;
    }

    float readFloat(const std::string& file, const char* key, float fallback, float low, float high)
    {
        char fallbackText[32], value[32];
        _snprintf(fallbackText, sizeof(fallbackText) - 1, "%.6f", fallback);
        fallbackText[sizeof(fallbackText) - 1] = 0;
        GetPrivateProfileStringA("FlowingHair", key, fallbackText, value, sizeof(value), file.c_str());
        return std::max(low, std::min(high, static_cast<float>(std::atof(value))));
    }

    void loadConfig()
    {
        const std::string file = directory() + "\\FlowingHair.ini";
        gConfig.enabled = GetPrivateProfileIntA("FlowingHair", "Enabled", 1, file.c_str()) != 0;
        gConfig.simulateNpcs = GetPrivateProfileIntA("FlowingHair", "SimulateNPCs", 1, file.c_str()) != 0;
        gConfig.hairDistance = readFloat(file, "FlowingHairDistance", 500.0f, 0.0f, 2000.0f);
        gConfig.hairCharacterLimit = static_cast<float>(flowing_hair::clampHairCharacterLimit(
            readFloat(file, "FlowingHairCharacterLimit", 10.0f, 1.0f, 100.0f)));
        gConfig.hairRigidity = readFloat(file, "HairRigidity", 50.0f, 0.0f, 100.0f);
        gConfig.wetHair=GetPrivateProfileIntA("FlowingHair","WetHair",1,file.c_str())!=0;
        gConfig.khajjitTails=GetPrivateProfileIntA("FlowingHair","KhajjitTails",1,file.c_str())!=0;
        gConfig.catTails=GetPrivateProfileIntA("FlowingHair","CatTails",1,file.c_str())!=0;
        gConfig.soakSeconds=readFloat(file,"WetHairSoakSeconds",20,1,600);
        gConfig.drySeconds=readFloat(file,"WetHairDrySeconds",120,1,1800);
        gConfig.wetExtraWeight=readFloat(file,"WetHairExtraWeight",0.75f,0,2);
        gConfig.proceduralWind = GetPrivateProfileIntA("FlowingHair", "ProceduralWind", 1, file.c_str()) != 0;
        gConfig.useGameWind = GetPrivateProfileIntA("FlowingHair", "UseGameWind", 1, file.c_str()) != 0;
        gConfig.stressWindHotkey = GetPrivateProfileIntA("FlowingHair", "EnableStressWindHotkey", 1, file.c_str()) != 0;
        gConfig.iterations = std::max(1, std::min(20,
            static_cast<int>(GetPrivateProfileIntA("FlowingHair", "ConstraintIterations", 8, file.c_str()))));
        gConfig.gravity = readFloat(file, "Gravity", 7.5f, 0.0f, 30.0f);
        gConfig.windStrength = readFloat(file, "WindStrength", 3.0f, 0.0f, 20.0f);
        gConfig.gustStrength = readFloat(file, "WindGustStrength", 3.8f, 0.0f, 20.0f);
        gConfig.gustFrequency = readFloat(file, "WindGustFrequency", 0.11f, 0.005f, 2.0f);
        gConfig.windDirection = readFloat(file, "WindDirectionDegrees", 35.0f, -360.0f, 360.0f);
        gConfig.windDirectionVariation = readFloat(file, "WindDirectionVariationDegrees", 55.0f, 0.0f, 180.0f);
        gConfig.windVertical = readFloat(file, "WindVerticalStrength", 0.5f, 0.0f, 10.0f);
        gConfig.windSpatialScale = readFloat(file, "WindSpatialScale", 0.012f, 0.0f, 1.0f);
        gConfig.gameWindMultiplier = readFloat(file, "GameWindMultiplier", 0.45f, 0.0f, 5.0f);
        gConfig.inertia = readFloat(file, "Inertia", 0.035f, 0.0f, 0.5f);
        gConfig.movementDrag = readFloat(file, "MovementDrag", 12.0f, 0.0f, 50.0f);
        gConfig.maximumMovementAcceleration = readFloat(file, "MaximumMovementAcceleration", 120.0f, 0.0f, 300.0f);
        gConfig.beardRootFraction = readFloat(file, "BeardRootFraction", 0.04f, 0.005f, 0.20f);
        gConfig.beardShapeStiffness = readFloat(file, "BeardShapeStiffness", 0.0045f, 0.001f, 0.25f);
        gConfig.faceCollision = GetPrivateProfileIntA("FlowingHair", "FaceCollision", 1, file.c_str()) != 0;
        gConfig.damping = readFloat(file, "Damping", 0.035f, 0.0f, 0.5f);
        gConfig.bending = readFloat(file, "BendingStiffness", 0.45f, 0.0f, 1.0f);
        gConfig.shapeStiffness = readFloat(file, "ShapeStiffness", 0.018f, 0.0f, 0.25f);
        gConfig.maxAcceleration = readFloat(file, "MaximumAcceleration", 40.0f, 1.0f, 200.0f);
        gConfig.stressAcceleration = readFloat(file, "StressWindAcceleration", 240.0f, 40.0f, 1000.0f);
        gConfig.crownFraction = readFloat(file, "CrownFraction", 0.04f, 0.005f, 0.25f);
        gConfig.scalpShell = readFloat(file, "ScalpShell", 0.16f, 0.0f, 0.75f);
    }

    bool containsInsensitive(const std::string& value, const char* needle)
    {
        if (!needle || !*needle || value.size() > 512) return false;
        const size_t needleLength = std::strlen(needle);
        if (needleLength > value.size()) return false;
        for (size_t start = 0; start + needleLength <= value.size(); ++start)
        {
            size_t offset = 0;
            for (; offset < needleLength; ++offset)
            {
                const unsigned char actual = static_cast<unsigned char>(value[start + offset]);
                const unsigned char wanted = static_cast<unsigned char>(needle[offset]);
                if (std::tolower(actual) != std::tolower(wanted)) break;
            }
            if (offset == needleLength) return true;
        }
        return false;
    }

    bool hairName(const std::string& value)
    {
        return containsInsensitive(value, "hair") || containsInsensitive(value, "scalp") ||
               containsInsensitive(value, "ponytail") || containsInsensitive(value, "mohawk") ||
               containsInsensitive(value, "nutfro") || containsInsensitive(value, "beard") ||
               containsInsensitive(value, "facial") || containsInsensitive(value, "moustache") ||
               containsInsensitive(value, "mustache");
    }

    bool facialHairName(const std::string& value)
    {
        return containsInsensitive(value, "beard") || containsInsensitive(value, "facial") ||
            containsInsensitive(value, "moustache") || containsInsensitive(value, "mustache");
    }

    bool below(Ogre::Node* node, Ogre::Node* parent)
    {
        for (int i = 0; node && i < 32; ++i, node = node->getParent())
            if (node == parent) return true;
        return false;
    }

    void appendEntities(Ogre::SceneNode* node, int depth, std::vector<Ogre::Entity*>& found)
    {
        if (!node || depth > 32 || found.size() >= 256) return;
        const size_t attachmentCount = std::min(node->numAttachedObjects(), static_cast<size_t>(256));
        for (size_t i = 0; i < attachmentCount && found.size() < 256; ++i)
        {
            Ogre::MovableObject* object = node->getAttachedObject(i);
            if (object && object->getMovableType() == Ogre::EntityFactory::FACTORY_TYPE_NAME)
                found.push_back(static_cast<Ogre::Entity*>(object));
        }
        const size_t childCount = std::min(node->numChildren(), static_cast<size_t>(256));
        for (size_t i = 0; i < childCount && found.size() < 256; ++i)
        {
            Ogre::SceneNode* child = dynamic_cast<Ogre::SceneNode*>(node->getChild(i));
            if (child) appendEntities(child, depth + 1, found);
        }
    }

    std::vector<Ogre::Entity*> entities(Ogre::SceneNode* root)
    {
        std::vector<Ogre::Entity*> found;
        found.reserve(64);
        appendEntities(root, 0, found);
        return found;
    }

    bool containsEntity(const std::vector<Ogre::Entity*>& all, Ogre::Entity* target)
    {
        return target && std::find(all.begin(), all.end(), target) != all.end();
    }

    std::string declaredScalpMesh(AppearanceBase* appearance)
    {
        GameData* style=appearance ? appearance->hairStyle : 0;
        if (!style || !style->isValid()) return std::string();
        // Read the already merged, live FCS record. Never enumerate mods or
        // mutate the record with operator[]; this also respects mod load order.
        const ogre_unordered_map<std::string,std::string>::type& files=style->filesdata;
        ogre_unordered_map<std::string,std::string>::type::const_iterator found=files.end();
        if (appearance->female) found=files.find("mesh female");
        if (found==files.end() || found->second.empty()) found=files.find("mesh");
        if (found==files.end() || found->second.size()>512) return std::string();
        return found->second;
    }

    Ogre::Entity* findHair(AppearanceBase* appearance, const std::vector<Ogre::Entity*>& all, bool facialSlot)
    {
        if (!appearance || !appearance->body ||
            (!facialSlot && (!appearance->hairStyle || appearance->shaved))) return 0;
        Ogre::Entity* best = 0;
        int bestScore = 0;
        bool ambiguous=false;
        std::string declared;
        try { declared=declaredScalpMesh(appearance); }
        catch (...) { logLine("Hairstyle mesh lookup failed; using legacy name discovery",true); }
        Ogre::Node* root = appearance->getSceneNode();
        for (size_t i = 0; i < all.size(); ++i)
        {
            Ogre::Entity* e = all[i];
            if (!e || e == appearance->body || gClones.count(e)) continue;
            const Ogre::MeshPtr& mesh = e->getMesh();
            if (mesh.isNull() || mesh->getName().size() > 512) continue;
            const bool sameTree = root && below(e->getParentNode(), root);
            if (!sameTree) continue;
            const flowing_hair::HairKind kind = flowing_hair::classifyHair(mesh->getName(), e->getName());
            const int recordMatch=flowing_hair::hairstyleMeshMatch(mesh->getName(),declared);
            if (facialSlot && recordMatch) continue; // never simulate one source in both slots
            if (!recordMatch && kind == flowing_hair::NotHair) continue;
            const bool facial = kind == flowing_hair::FacialHair;
            if (!recordMatch && facial != facialSlot) continue;
            const int score = recordMatch ? recordMatch*100 :
                20 + (hairName(mesh->getName()) ? 10 : 0) + (facial ? 0 : 100);
            if (score > bestScore) { best = e; bestScore = score; ambiguous=false; }
            else if (recordMatch && score==bestScore && e!=best) ambiguous=true;
        }
        // Same-named mod assets can coexist. Never choose an arbitrary attachment
        // on an ambiguous record match; leave the original rendering alone.
        return ambiguous ? 0 : best;
    }

    Ogre::Entity* findAttachment(AppearanceBase* appearance,const std::vector<Ogre::Entity*>& all,int slot)
    {
        if (slot!=TailSlot) return findHair(appearance,all,slot==BeardSlot);
        if ((!gConfig.khajjitTails && !gConfig.catTails) || !gTailDrawHookReady || !appearance || !appearance->body ||
            !containsEntity(all,appearance->body) || appearance->body->getMesh().isNull()) return 0;
        const std::string& name=appearance->body->getMesh()->getName();
        if (gRuntimeFailures.count("tail-profile|"+name) || (appearance->body->getNumSubEntities()==1 &&
            gRuntimeFailures.count("tail-setup|"+name+"|"+appearance->body->getSubEntity(0)->getMaterialName()))) return 0;
        const flowing_hair::KhajjitTailProfile profile=flowing_hair::khajjitTailProfile(name);
        return profile.vertices && (profile.cat ? gConfig.catTails : gConfig.khajjitTails) ? appearance->body : 0;
    }

    void logRelatedMeshes(AppearanceBase* appearance, const std::vector<Ogre::Entity*>& all)
    {
        if (!appearance || !gNoCandidateLogged.insert(appearance).second) return;
        Ogre::Node* root = appearance->getSceneNode();
        std::ostringstream message;
        message << "No supported hair candidate found. Related meshes:";
        int count = 0;
        for (size_t i = 0; i < all.size() && count < 16; ++i)
        {
            Ogre::Entity* e = all[i];
            if (!e || e == appearance->body || gClones.count(e) || e->getMesh().isNull()) continue;
            const bool sameTree = root && below(e->getParentNode(), root);
            if (!sameTree) continue;
            const std::string& name = e->getMesh()->getName();
            if (name.size() <= 512) { message << " [" << name << "]"; ++count; }
        }
        if (!count) message << " (none found below the appearance scene node)";
        logLine(message.str(), false);
    }

    bool collectVertexData(Ogre::Mesh* mesh, std::vector<HairStream>& streams, std::string& why)
    {
        size_t total = 0;
        if (!mesh->getNumSubMeshes() || mesh->getNumSubMeshes() > 64)
        { why = "unsupported submesh count"; return false; }
        for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i)
        {
            Ogre::SubMesh* sub = mesh->getSubMesh(i);
            Ogre::VertexData* data = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            if (!data) { why = "missing vertex data"; return false; }
            bool found = false;
            for (size_t s = 0; s < streams.size(); ++s)
                if (streams[s].vertexData == data) { found = true; break; }
            if (found) continue;
            if (data->vertexCount > 250000 || total > 250000 - data->vertexCount)
            { why = "combined vertex count is unsafe"; return false; }
            HairStream stream; stream.vertexData = data; stream.renderOffset = total;
            streams.push_back(stream); total += data->vertexCount;
        }
        return !streams.empty();
    }

    bool readPositions(HairStream* hair, std::vector<flowing_hair::Vec3>& positions,
                       Ogre::HardwareVertexBufferSharedPtr& source, std::string& why)
    {
        hair->positionElement = hair->vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!hair->positionElement || (hair->positionElement->getType() != Ogre::VET_FLOAT3 &&
            hair->positionElement->getType() != Ogre::VET_FLOAT4))
        { why = "position is not float3/float4"; return false; }
        source = hair->vertexData->vertexBufferBinding->getBuffer(hair->positionElement->getSource());
        if (source.isNull() || hair->vertexData->vertexCount < 3)
        { why = "missing position buffer"; return false; }
        const size_t stride = source->getVertexSize();
        const size_t sourceVertices = source->getNumVertices();
        if (!stride || stride > 4096 || hair->vertexData->vertexCount > 250000 ||
            hair->vertexData->vertexStart > sourceVertices ||
            hair->vertexData->vertexCount > sourceVertices - hair->vertexData->vertexStart ||
            sourceVertices > static_cast<size_t>(128 * 1024 * 1024) / stride)
        { why = "position buffer dimensions are unsafe"; return false; }
        const size_t bytes = stride * sourceVertices;
        hair->vertexBytes.resize(bytes);
        try { source->readData(0, bytes, &hair->vertexBytes[0]); }
        catch (...) { why = "position buffer is not CPU-readable"; return false; }
        positions.resize(hair->vertexData->vertexCount);
        for (size_t i = 0; i < positions.size(); ++i)
        {
            unsigned char* vertex = &hair->vertexBytes[0] +
                (hair->vertexData->vertexStart + i) * source->getVertexSize();
            float* p = 0; hair->positionElement->baseVertexPointerToElement(vertex, &p);
            positions[i] = flowing_hair::Vec3(p[0], p[1], p[2]);
            if (!finiteVector(positions[i]) || std::fabs(positions[i].x) > 100000.0f ||
                std::fabs(positions[i].y) > 100000.0f || std::fabs(positions[i].z) > 100000.0f)
            { why = "position buffer contains invalid or extreme coordinates"; return false; }
        }
        return true;
    }

    bool readTriangles(Ogre::Mesh* mesh, const std::vector<HairStream>& streams,
                       std::vector<unsigned int>& triangles, std::string& why)
    {
        for (unsigned short s = 0; s < mesh->getNumSubMeshes(); ++s)
        {
            Ogre::SubMesh* sub = mesh->getSubMesh(s);
            Ogre::VertexData* data = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
            size_t streamIndex = 0;
            while (streamIndex < streams.size() && streams[streamIndex].vertexData != data) ++streamIndex;
            if (streamIndex == streams.size()) { why = "submesh has no captured vertex stream"; return false; }
            const size_t vertexCount = data->vertexCount;
            const size_t renderOffset = streams[streamIndex].renderOffset;
            if (sub->operationType != Ogre::RenderOperation::OT_TRIANGLE_LIST || !sub->indexData ||
                !sub->indexData->indexCount || sub->indexData->indexCount % 3)
            { why = "non-indexed or non-triangle-list geometry"; return false; }
            if (sub->indexData->indexCount > 3000000 ||
                triangles.size() > 3000000 - sub->indexData->indexCount)
            { why = "index buffer dimensions are unsafe"; return false; }
            Ogre::HardwareIndexBufferSharedPtr buffer = sub->indexData->indexBuffer;
            if (buffer.isNull()) { why = "missing index buffer"; return false; }
            const size_t width = buffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT ? 2 : 4;
            if (sub->indexData->indexStart > buffer->getNumIndexes() ||
                sub->indexData->indexCount > buffer->getNumIndexes() - sub->indexData->indexStart)
            { why = "index range exceeds source buffer"; return false; }
            void* raw = 0;
            try
            {
                raw = buffer->lock(sub->indexData->indexStart * width,
                    sub->indexData->indexCount * width, Ogre::HardwareBuffer::HBL_READ_ONLY);
                for (size_t i = 0; i < sub->indexData->indexCount; ++i)
                {
                    const unsigned int index = width == 2 ? static_cast<unsigned short*>(raw)[i]
                                                          : static_cast<unsigned int*>(raw)[i];
                    if (index >= vertexCount)
                    { buffer->unlock(); why = "index outside vertex stream"; return false; }
                    triangles.push_back(static_cast<unsigned int>(renderOffset + index));
                }
                buffer->unlock(); raw = 0;
            }
            catch (...)
            {
                if (raw) try { buffer->unlock(); } catch (...) {}
                why = "index buffer is not CPU-readable"; return false;
            }
        }
        if (triangles.empty()) { why = "no triangles"; return false; }
        return true;
    }

    void weld(const std::vector<flowing_hair::Vec3>& renderPositions,
              const std::vector<unsigned int>& renderTriangles,
              std::vector<flowing_hair::Vec3>& particles,
              std::vector<unsigned int>& renderToParticle,
              std::vector<unsigned int>& particleTriangles)
    {
        flowing_hair::Vec3 low = renderPositions[0], high = renderPositions[0];
        for (size_t i = 1; i < renderPositions.size(); ++i)
        {
            low.x = std::min(low.x, renderPositions[i].x); low.y = std::min(low.y, renderPositions[i].y); low.z = std::min(low.z, renderPositions[i].z);
            high.x = std::max(high.x, renderPositions[i].x); high.y = std::max(high.y, renderPositions[i].y); high.z = std::max(high.z, renderPositions[i].z);
        }
        const float dx = high.x - low.x, dy = high.y - low.y, dz = high.z - low.z;
        const float epsilon = std::max(0.000001f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.00005f);
        const float inverse = 1.0f / epsilon;
        std::map<Key, unsigned int> lookup;
        renderToParticle.resize(renderPositions.size());
        for (size_t i = 0; i < renderPositions.size(); ++i)
        {
            Key key;
            key.x = static_cast<int>(std::floor((renderPositions[i].x - low.x) * inverse + 0.5f));
            key.y = static_cast<int>(std::floor((renderPositions[i].y - low.y) * inverse + 0.5f));
            key.z = static_cast<int>(std::floor((renderPositions[i].z - low.z) * inverse + 0.5f));
            std::map<Key, unsigned int>::iterator found = lookup.find(key);
            if (found == lookup.end())
            {
                const unsigned int index = static_cast<unsigned int>(particles.size());
                lookup[key] = index; particles.push_back(renderPositions[i]); renderToParticle[i] = index;
            }
            else renderToParticle[i] = found->second;
        }
        for (size_t i = 0; i < renderTriangles.size(); i += 3)
        {
            const unsigned int a = renderToParticle[renderTriangles[i]];
            const unsigned int b = renderToParticle[renderTriangles[i + 1]];
            const unsigned int c = renderToParticle[renderTriangles[i + 2]];
            if (a != b && b != c && c != a)
            { particleTriangles.push_back(a); particleTriangles.push_back(b); particleTriangles.push_back(c); }
        }
    }

    std::vector<bool> pinsAndCollider(const std::vector<flowing_hair::Vec3>& positions,
                                      flowing_hair::Ellipsoid& collider)
    {
        flowing_hair::Vec3 low = positions[0], high = positions[0];
        for (size_t i = 1; i < positions.size(); ++i)
        {
            low.x = std::min(low.x, positions[i].x); low.y = std::min(low.y, positions[i].y); low.z = std::min(low.z, positions[i].z);
            high.x = std::max(high.x, positions[i].x); high.y = std::max(high.y, positions[i].y); high.z = std::max(high.z, positions[i].z);
        }
        const float width = std::max(0.001f, high.x - low.x);
        const float height = std::max(0.001f, high.y - low.y);
        const float depth = std::max(0.001f, high.z - low.z);
        const float head = std::max(width, depth);
        flowing_hair::PinSelection select;
        select.headCenter = flowing_hair::Vec3((low.x + high.x) * 0.5f,
            high.y - std::min(height * 0.28f, head * 0.58f), (low.z + high.z) * 0.5f);
        select.headRadii = flowing_hair::Vec3(width * 0.46f,
            std::max(head * 0.55f, height * 0.12f), depth * 0.46f);
        select.minimumScalpHeight = select.headCenter.y - select.headRadii.y * 0.2f;
        select.scalpShell = gConfig.scalpShell;
        select.crownBand = height * gConfig.crownFraction;
        std::vector<bool> result = flowing_hair::ClothSolver::selectPins(positions, select);
        const size_t count = static_cast<size_t>(std::count(result.begin(), result.end(), true));
        if (count < 3 || count > positions.size() * 3 / 5)
            for (size_t i = 0; i < positions.size(); ++i)
                result[i] = positions[i].y >= high.y - height * std::max(0.08f, gConfig.crownFraction);
        collider.center = select.headCenter; collider.radii = select.headRadii; collider.enabled = true;
        return result;
    }

    float meshSpan(const std::vector<flowing_hair::Vec3>& positions)
    {
        flowing_hair::Vec3 low = positions[0], high = positions[0];
        for (size_t i = 1; i < positions.size(); ++i)
        {
            low.x = std::min(low.x, positions[i].x); low.y = std::min(low.y, positions[i].y); low.z = std::min(low.z, positions[i].z);
            high.x = std::max(high.x, positions[i].x); high.y = std::max(high.y, positions[i].y); high.z = std::max(high.z, positions[i].z);
        }
        return flowing_hair::length(high - low);
    }

    void removeMeshRegistration(Hair* hair)
    {
        if (hair)
        {
            if (hair->tailBodyPart) gTailBodies.erase(hair->tailBodyPart);
            for (size_t r = 0; r < hair->renderables.size(); ++r)
                gRenderHair.erase(hair->renderables[r]);
            Ogre::MaterialManager* materials = Ogre::MaterialManager::getSingletonPtr();
            if (materials)
                for (size_t i = 0; i < hair->privateMaterials.size(); ++i)
                    if (!hair->privateMaterials[i].isNull())
                        materials->remove(hair->privateMaterials[i]->getHandle());
        }
        if (!hair || hair->mesh.isNull()) return;
        Ogre::MeshManager* manager = Ogre::MeshManager::getSingletonPtr();
        if (manager) manager->remove(hair->mesh->getHandle());
    }

    void discardCreatedHair(Hair* hair)
    {
        if (!hair) return;
        if (hair->original && !hair->tailSlot)
        {
            try { hair->original->setVisible(true); } catch (...) {}
        }
        // The clone may not yet be attached to the appearance node when setup
        // fails.  Ask the owning SceneManager whether it still owns the pointer
        // before calling any Entity methods; this also makes partial setup safe.
        bool cloneRegistered = false;
        if (hair->visibleClone && hair->manager)
        {
            try
            {
                cloneRegistered = hair->manager->hasMovableObject(hair->visibleClone);
                // Guard against an allocator reusing a stale pointer for another
                // entity between appearance rebuild and cleanup.
                if (cloneRegistered && !hair->cloneName.empty() &&
                    hair->visibleClone->getName() != hair->cloneName)
                    cloneRegistered = false;
                if (cloneRegistered)
                {
                    Ogre::SceneNode* parent = hair->visibleClone->getParentSceneNode();
                    if (parent) parent->detachObject(hair->visibleClone);
                    hair->manager->destroyEntity(hair->visibleClone);
                }
            }
            catch (...) {}
        }
        gClones.erase(hair->visibleClone);
        try { removeMeshRegistration(hair); } catch (...) {}
        delete hair;
    }

    void destroyHair(Hair* hair, const std::vector<Ogre::Entity*>& live, bool restoreVisible = true)
    {
        if (!hair) return;
        const bool originalLive = containsEntity(live, hair->original);
        // A refreshed appearance can move the old clone outside the current
        // subtree, so subtree membership is not sufficient ownership proof.
        // SceneManager::hasMovableObject compares its registry pointer and lets
        // us safely clean up the plugin-owned clone without dereferencing a
        // stale Entity pointer.
        bool cloneRegistered = false;
        if (hair->visibleClone && hair->manager)
        {
            try
            {
                cloneRegistered = hair->manager->hasMovableObject(hair->visibleClone);
                if (cloneRegistered && !hair->cloneName.empty() &&
                    hair->visibleClone->getName() != hair->cloneName)
                    cloneRegistered = false;
            }
            catch (...) { cloneRegistered = false; }
        }
        if (originalLive && !hair->tailSlot)
        {
            try { hair->original->setVisible(restoreVisible); }
            catch (...) { try { logLine("Exception while restoring original hair", true); } catch (...) {} }
        }
        if (cloneRegistered && hair->manager)
        {
            try
            {
                Ogre::SceneNode* parent = hair->visibleClone->getParentSceneNode();
                if (parent) parent->detachObject(hair->visibleClone);
                hair->manager->destroyEntity(hair->visibleClone);
            }
            catch (...) { try { logLine("Exception while destroying replacement hair", true); } catch (...) {} }
        }
        // Always remove the pointer from our exclusion set, including when
        // Kenshi already destroyed the entity during an appearance rebuild.
        gClones.erase(hair->visibleClone);
        try { removeMeshRegistration(hair); } catch (...) {}
        delete hair;
    }

    void abandonAllHair(const char* reason)
    {
        if (gHair.empty()) return;
        const size_t count = gHair.size();
        for (HairMap::iterator it = gHair.begin(); it != gHair.end(); ++it)
        {
            try { removeMeshRegistration(it->second); } catch (...) {}
            delete it->second;
        }
        gHair.clear();
        gClones.clear();
        gNoCandidateLogged.clear();
        gRuntimeFailures.clear();
        gPlayerAudit.clear();
        gNextNpc = 0;
        std::ostringstream message;
        message << "Released " << count << " cached hair instance" << (count == 1 ? "" : "s")
                << " at " << reason << "; scene objects are left to Ogre's world teardown";
        logLine(message.str(), false);
    }

    bool bindPaletteParameters(Hair* hair, const Ogre::MaterialPtr& material,
                               size_t subEntity, std::string& why)
    {
        material->load();
        const size_t before = hair->paletteBindings.size();
        for (unsigned short t = 0; t < material->getNumTechniques(); ++t)
        {
            Ogre::Technique* technique = material->getTechnique(t);
            for (unsigned short p = 0; p < technique->getNumPasses(); ++p)
            {
                Ogre::Pass* pass = technique->getPass(p);
                if (hair->tailSlot)
                {
                    // Body shaders use per-renderable values (notably waterLine).
                    // Copy them through the original's native virtual callback;
                    // do not inspect Renderable's inline custom-parameter map.
                    for (int stage=0;stage<2;++stage)
                    {
                        if (stage==0 ? !pass->hasVertexProgram() : !pass->hasFragmentProgram()) continue;
                        Ogre::GpuProgramParametersSharedPtr custom=stage==0 ? pass->getVertexProgramParameters() : pass->getFragmentProgramParameters();
                        if (custom.isNull()) continue;
                        const Ogre::GpuConstantDefinitionMap& definitions=custom->getConstantDefinitions().map;
                        for (Ogre::GpuConstantDefinitionMap::const_iterator d=definitions.begin();d!=definitions.end();++d)
                        {
                            const Ogre::GpuProgramParameters::AutoConstantEntry* entry=custom->findAutoConstantEntry(d->first);
                            if (entry && entry->paramType==Ogre::GpuProgramParameters::ACT_CUSTOM)
                            {
                                hair->tailCustomBindings.push_back(TailCustomBinding(custom,*entry));
                                custom->clearNamedAutoConstant(d->first);
                            }
                        }
                    }
                }
                if (!pass->hasVertexProgram()) continue;
                Ogre::GpuProgramParametersSharedPtr params = pass->getVertexProgramParameters();
                if (params.isNull()) continue;
                const Ogre::GpuConstantDefinition* def =
                    params->_findNamedConstantDefinition("worldMatrix3x4Array", false);
                if (!def) continue;
                if (def->elementSize != 12 || !def->arraySize || def->arraySize > 60)
                { why = "unrecognised skin palette shader layout"; return false; }
                PaletteBinding binding;
                binding.subEntity = subEntity; binding.capacity = def->arraySize;
                binding.parameters = params;
                // Only change the private material. Otherwise Ogre overwrites our
                // complete palette with the replacement Entity's single matrix.
                params->clearNamedAutoConstant("worldMatrix3x4Array");
                hair->paletteBindings.push_back(binding);
            }
        }
        if (hair->paletteBindings.size() == before)
        { why = "material has no supported skin palette; preserving original"; return false; }
        return true;
    }

    bool createPrivateMaterials(Hair* hair, std::string& why)
    {
        for (size_t s = 0; s < hair->original->getNumSubEntities(); ++s)
        {
            const Ogre::MaterialPtr& source = hair->original->getSubEntity(s)->getMaterial();
            if (source.isNull()) { why = "source material missing"; return false; }
            std::ostringstream name; name << hair->cloneName << "_material_" << s;
            Ogre::MaterialPtr material = source->clone(name.str());
            hair->privateMaterials.push_back(material);
            if (!bindPaletteParameters(hair, material, s, why)) return false;
            for (unsigned short t = 0; t < material->getNumTechniques(); ++t)
            {
                Ogre::Technique* technique = material->getTechnique(t);
                Ogre::MaterialPtr shadow = technique->getShadowCasterMaterial();
                if (shadow.isNull()) continue;
                std::ostringstream shadowName; shadowName << name.str() << "_shadow_" << t;
                Ogre::MaterialPtr privateShadow = shadow->clone(shadowName.str());
                hair->privateMaterials.push_back(privateShadow);
                if (!bindPaletteParameters(hair, privateShadow, s, why)) return false;
                technique->setShadowCasterMaterial(privateShadow);
            }
            hair->visibleClone->getSubEntity(s)->setMaterial(material);
        }
        return true;
    }

    bool readTailBlendStream(Ogre::VertexData* data,const Ogre::VertexElement* element,
        std::vector<unsigned char>& bytes,size_t& stride)
    {
        Ogre::HardwareVertexBufferSharedPtr buffer=data->vertexBufferBinding->getBuffer(element->getSource());
        if(buffer.isNull()) return false;
        stride=buffer->getVertexSize();
        if(!stride || stride>4096 || element->getOffset()+element->getSize()>stride ||
            data->vertexStart>buffer->getNumVertices() || data->vertexCount>buffer->getNumVertices()-data->vertexStart ||
            data->vertexCount>64*1024*1024/stride) return false;
        bytes.resize(data->vertexCount*stride);
        if(bytes.empty()) return false;
        buffer->readData(data->vertexStart*stride,bytes.size(),&bytes[0]);return true;
    }

    bool captureTailRootWeights(Hair* hair)
    {
        // Read compiled palette indices, NOT skeleton bone handles. Root weights
        // select the actual lower-body skinning pose without accessing a skeleton.
        Ogre::VertexData* data=hair->original->getSubEntity(0)->getVertexDataForBinding();
        if(!data || data->vertexCount!=hair->renderToParticle.size()) return false;
        const Ogre::VertexElement* indices=data->vertexDeclaration->findElementBySemantic(Ogre::VES_BLEND_INDICES);
        const Ogre::VertexElement* weights=data->vertexDeclaration->findElementBySemantic(Ogre::VES_BLEND_WEIGHTS);
        if(!indices || !weights || indices->getType()!=Ogre::VET_UBYTE4 ||
            weights->getType()<Ogre::VET_FLOAT1 || weights->getType()>Ogre::VET_FLOAT4) return false;
        std::vector<unsigned char> indexBytes,weightBytes;size_t indexStride=0,weightStride=0;
        if(!readTailBlendStream(data,indices,indexBytes,indexStride) || !readTailBlendStream(data,weights,weightBytes,weightStride)) return false;
        const size_t components=Ogre::VertexElement::getTypeCount(weights->getType());
        const std::vector<bool> pins=flowing_hair::khajjitTailPins(hair->bindPositions);
        float totals[60]={0},total=0;
        for(size_t i=0;i<data->vertexCount;++i)
        {
            const unsigned int particle=hair->renderToParticle[i];
            if(particle==UINT_MAX) continue;
            if(particle>=pins.size()) return false;
            const unsigned char* bone=&indexBytes[i*indexStride+indices->getOffset()];
            float values[4]={0};std::memcpy(values,&weightBytes[i*weightStride+weights->getOffset()],components*sizeof(float));
            float vertexTotal=0;
            for(size_t j=0;j<components;++j)
            {
                if(!finiteFloat(values[j]) || values[j]<0 || values[j]>1.01f) return false;
                if(values[j]<=0) continue;
                if(bone[j]>=60) return false;
                vertexTotal+=values[j];
                if(pins[particle]) { totals[bone[j]]+=values[j];total+=values[j]; }
            }
            // Every visible tail vertex must have normalized skin weights: all
            // its palette entries will become the same attachment transform.
            if(std::fabs(vertexTotal-1.0f)>0.001f) return false;
        }
        if(total<=0) return false;
        for(size_t i=0;i<60;++i) hair->tailPoseWeights[i]=totals[i]/total;
        return true;
    }

    void sampleTailRootPose(Hair* hair,const Ogre::Matrix4* matrices,size_t count)
    {
        Ogre::Vector3 x=Ogre::Vector3::ZERO,y=Ogre::Vector3::ZERO,z=Ogre::Vector3::ZERO;
        float total=0;
        hair->tailPoseValid=false;
        for(size_t i=0;i<60;++i)
        {
            const float weight=hair->tailPoseWeights[i];if(weight<=0) continue;
            if(i>=count) return;
            x+=Ogre::Vector3(matrices[i][0][0],matrices[i][1][0],matrices[i][2][0])*weight;
            y+=Ogre::Vector3(matrices[i][0][1],matrices[i][1][1],matrices[i][2][1])*weight;
            z+=Ogre::Vector3(matrices[i][0][2],matrices[i][1][2],matrices[i][2][2])*weight;
            total+=weight;
        }
        if(total<0.99f || !finiteVector(x)||!finiteVector(y)||!finiteVector(z) || x.squaredLength()<0.000001f) return;
        x.normalise();y-=x*x.dotProduct(y);
        if(y.squaredLength()<0.000001f) return;
        y.normalise();const Ogre::Vector3 orthogonalZ=x.crossProduct(y);
        if(orthogonalZ.dotProduct(z)<=0.000001f) return;
        Ogre::Matrix3 rotation;rotation.SetColumn(0,x);rotation.SetColumn(1,y);rotation.SetColumn(2,orthogonalZ);
        Ogre::Quaternion pose(rotation);
        if(!finiteQuaternion(pose)) return;
        pose.normalise();hair->tailPose=pose;hair->tailPoseValid=true;
    }

    bool synchroniseRenderPalette(Hair* hair, std::string& why)
    {
        if (!hair || !hair->original || !hair->visibleClone || hair->paletteBindings.empty())
        { why = "replacement render relationship is incomplete"; return false; }
        // Hidden originals are omitted from Ogre's render-driven update. Sharing
        // the body's skeleton does NOT refresh this Entity's bone-world cache.
        // During steady state this runs from the pre-draw listener, after the
        // frame's animation work and before GPU constants are uploaded. Sampling
        // in the gameplay hook caused a visible positional lag.
        hair->original->_updateAnimation();
        for (size_t i=0;i<hair->tailCustomBindings.size();++i)
        {
            TailCustomBinding& binding=hair->tailCustomBindings[i];
            hair->original->getSubEntity(0)->_updateCustomGpuParameter(binding.entry,binding.parameters.get());
        }
        for (size_t s = 0; s < hair->original->getNumSubEntities(); ++s)
        {
            Ogre::SubEntity* source = hair->original->getSubEntity(s);
            const unsigned short count = source->getNumWorldTransforms();
            if (!count || count > 60)
            { why = "source skin palette exceeds shader capacity"; return false; }
            Ogre::Matrix4 matrices[60];
            source->getWorldTransforms(matrices);
            if(hair->tailSlot && s==0) sampleTailRootPose(hair,matrices,count);
            float input[60 * 16], packed[60 * 12];
            for (size_t m = 0; m < count; ++m)
                for (size_t r = 0; r < 4; ++r)
                    for (size_t c = 0; c < 4; ++c)
                        input[m * 16 + r * 4 + c] = matrices[m][r][c];
            for (size_t b = 0; b < hair->paletteBindings.size(); ++b)
            {
                const PaletteBinding& binding = hair->paletteBindings[b];
                if (binding.subEntity != s) continue;
                const bool packedOk=hair->tailSlot ?
                    (hair->tailPoseValid && flowing_hair::packTailAttachmentPalette(input,count,hair->tailPoseWeights,packed,binding.capacity)) :
                    flowing_hair::packSkinPalette(input,count,packed,binding.capacity);
                if (!packedOk)
                { why = "invalid or oversized source skin palette"; return false; }
                if(hair->tailSlot && (hair->tailFollowPending || hair->tailFollow.positions().empty()))
                {
                    // Exactly one simulation/upload for each queued gameplay tick,
                    // using THIS draw's attachment, not last frame's palette.
                    const float dt=hair->tailFollowPending ? hair->tailFollowDt : 0.0f;
                    float catFrame[12];const float* followFrame=packed;
                    if(hair->catTail) { flowing_hair::catTailSolverFrame(packed,catFrame);followFrame=catFrame; }
                    if(!hair->tailFollow.advance(hair->tailDynamics,hair->bindPositions,followFrame,dt,gConfig.hairRigidity))
                    { why="invalid tail follow attachment"; return false; }
                    bool recovered=false;
                    if(!hair->tailFollow.recoverOutput(hair->tailDynamics,hair->bindPositions,followFrame,hair->maximumDisplacement,recovered))
                    { why="tail follow could not recover valid geometry"; return false; }
                    if(recovered && gRuntimeFailures.insert("tail-follow-recovery|"+hair->sourceName).second)
                        logLine("Recovered tail simulation locally; physics remains active for '"+hair->sourceName+"'",true);
                    const std::vector<flowing_hair::Vec3>& result=hair->tailFollow.positions();
                    for(size_t si=0;si<hair->streams.size();++si)
                    {
                        HairStream& stream=hair->streams[si];const size_t stride=stream.positionBuffer->getVertexSize();
                        for(size_t range=0;range<hair->tailUploadRanges.size();++range)
                        {
                            const size_t first=hair->tailUploadRanges[range].first,last=hair->tailUploadRanges[range].second;
                            for(size_t v=first;v<last;++v)
                            {
                                const unsigned int particle=hair->renderToParticle[stream.renderOffset+v];
                                if(particle==UINT_MAX) continue;
                                unsigned char* vertex=&stream.vertexBytes[(stream.vertexData->vertexStart+v)*stride];
                                float* output=0;stream.positionElement->baseVertexPointerToElement(vertex,&output);
                                const flowing_hair::Vec3 p=hair->catTail ? flowing_hair::catTailToMesh(result[particle]) : result[particle];
                                output[0]=p.x;output[1]=p.y;output[2]=p.z;
                            }
                            const size_t offset=(stream.vertexData->vertexStart+first)*stride;
                            stream.positionBuffer->writeData(offset,(last-first)*stride,&stream.vertexBytes[offset],false);
                        }
                    }
                    hair->tailFollowPending=false;
                }
                binding.parameters->setNamedConstant("worldMatrix3x4Array", packed,
                    binding.capacity * 3, 4);
            }
        }
        return true;
    }

    bool updateRenderBounds(Hair* hair, std::string& why)
    {
        Ogre::Aabb bounds = hair->original->getLocalAabb();
        if (!finiteVector(bounds.mCenter) || !finiteVector(bounds.mHalfSize))
        { why = "source hair has invalid culling bounds"; return false; }
        bounds.mHalfSize += Ogre::Vector3(hair->maximumDisplacement,
            hair->maximumDisplacement, hair->maximumDisplacement);
        hair->visibleClone->setLocalAabb(bounds);
        return true;
    }

    class HairPoseListener : public Ogre::RenderObjectListener
    {
    public:
        virtual void notifyRenderSingleObject(Ogre::Renderable* renderable, const Ogre::Pass*,
            const Ogre::AutoParamDataSource* source, const Ogre::LightList*, bool)
        {
            // Registered before any cloth exists: vanilla body draws bootstrap
            // eligibility. Portraits/shadows cannot mark a body main-view visible.
            if (source && gMainViewCamera && source->getCurrentCamera()==gMainViewCamera)
            {
                std::map<Ogre::Renderable*,BodyWatch*>::iterator body=gBodyRenderables.find(renderable);
                if (body!=gBodyRenderables.end()) body->second->draw.observe(true,gVisibilityFrame);
            }
            std::map<Ogre::Renderable*, Hair*>::iterator found = gRenderHair.find(renderable);
            if (found == gRenderHair.end()) return;
            Hair* hair = found->second;
            if (hair->renderPoseFailed) return;
            try
            {
                std::string why;
                if (!synchroniseRenderPalette(hair, why))
                {
                    hair->renderPoseFailed = true;
                    logLine("Pre-draw pose rejected for '" + hair->sourceName + "': " + why, true);
                    return;
                }
                if (!hair->renderPoseReported)
                {
                    hair->renderPoseReported = true;
                    logLine("Pre-draw pose refresh active for '" + hair->sourceName + "'", false);
                }
            }
            catch (...)
            {
                hair->renderPoseFailed = true;
                try { logLine("Pre-draw pose exception; original will be restored", true); } catch (...) {}
            }
        }
    };

    HairPoseListener gHairPoseListener;

    void auditRenderable(Ogre::SubEntity* sub, const char* label)
    {
        Ogre::RenderOperation op;
        sub->getRenderOperation(op);
        std::ostringstream info;
        info << "DRAW AUDIT " << label << " vertices=" << (op.vertexData ? op.vertexData->vertexCount : 0)
             << " indices=" << (op.indexData ? op.indexData->indexCount : 0);
        if (op.vertexData)
        {
            const Ogre::VertexDeclaration::VertexElementList& elements = op.vertexData->vertexDeclaration->getElements();
            for (Ogre::VertexDeclaration::VertexElementList::const_iterator e = elements.begin(); e != elements.end(); ++e)
            {
                info << " [semantic=" << e->getSemantic() << " type=" << e->getType()
                     << " stream=" << e->getSource() << " offset=" << e->getOffset();
                Ogre::HardwareVertexBufferSharedPtr buffer = op.vertexData->vertexBufferBinding->getBuffer(e->getSource());
                const size_t stride = buffer->getVertexSize();
                std::vector<unsigned char> bytes(stride);
                buffer->readData(op.vertexData->vertexStart * stride, stride, &bytes[0]);
                if (e->getType() == Ogre::VET_FLOAT3 || e->getType() == Ogre::VET_FLOAT4)
                {
                    float* values = 0; e->baseVertexPointerToElement(&bytes[0], &values);
                    info << " value=" << values[0] << "," << values[1] << "," << values[2];
                }
                if (e->getSemantic() == Ogre::VES_BLEND_INDICES)
                {
                    unsigned char* values = 0; e->baseVertexPointerToElement(&bytes[0], &values);
                    info << " indexBytes=" << static_cast<int>(values[0]) << ","
                         << static_cast<int>(values[1]) << "," << static_cast<int>(values[2]);
                }
                info << "]";
            }
        }
        logLine(info.str(), false);
    }

    Hair* createHair(AppearanceBase* owner, Ogre::SceneNode* root, Ogre::Entity* original, int slot)
    {
        const bool facialSlot=slot==BeardSlot,tailSlot=slot==TailSlot;
        if (!original || original->getMesh().isNull()) return 0;
        const std::string& rawSourceName = original->getMesh()->getName();
        if (rawSourceName.empty() || rawSourceName.size() > 512)
        { skipOnce("unnamed", "mesh resource name is unsafe"); return 0; }
        const std::string sourceName(rawSourceName);
        Hair* hair = new Hair();
        hair->tailSlot=tailSlot;
        hair->owner = owner; hair->hairStyle = owner->hairStyle; hair->root = root;
        hair->original = original; hair->manager = original->_getManager(); hair->sourceName = sourceName;
        try
        {
            if (tailSlot && (!gTailDrawHookReady || original->getNumSubEntities()!=1 ||
                original->getMesh()->getNumSubMeshes()!=1))
            { skipOnce(sourceName,"tail requires the verified one-submesh draw path"); discardCreatedHair(hair); return 0; }
            // A rejected native body/material must not trigger a full-body mesh
            // allocation every frame. Successful setup removes this sentinel.
            if (tailSlot) gRuntimeFailures.insert("tail-setup|"+sourceName+"|"+original->getSubEntity(0)->getMaterialName());
            std::ostringstream name; name << "FlowingHair_" << gId++ << "_" << sourceName;
            hair->mesh = original->getMesh()->clone(name.str());
            std::string why;
            // Mesh::clone duplicates the vertex/index data but the installed
            // Ogre build does not reliably carry the source skeleton resource
            // name into the private mesh.  A skinned hair entity with no
            // skeleton link can still accept our position-buffer updates, yet
            // contributes no visible geometry.  Copy the resource link before
            // creating the replacement Entity so Ogre builds its own safe
            // skeleton instance for the clone.
            const Ogre::String& sourceSkeleton = original->getMesh()->getSkeletonName();
            if (hair->mesh.isNull())
            { skipOnce(sourceName, "mesh clone failed"); discardCreatedHair(hair); return 0; }
            if (!sourceSkeleton.empty())
                hair->mesh->setSkeletonName(sourceSkeleton);
            if (hair->mesh.get() == original->getMesh().get() ||
                !collectVertexData(hair->mesh.get(), hair->streams, why))
            { skipOnce(sourceName, why.empty() ? "mesh clone failed" : why); discardCreatedHair(hair); return 0; }

            std::vector<flowing_hair::Vec3> renderPositions;
            size_t totalBytes = 0;
            for (size_t s = 0; s < hair->streams.size(); ++s)
            {
                HairStream& stream = hair->streams[s];
                std::vector<flowing_hair::Vec3> positions;
                Ogre::HardwareVertexBufferSharedPtr sourceBuffer;
                if (!readPositions(&stream, positions, sourceBuffer, why))
                { skipOnce(sourceName, why); discardCreatedHair(hair); return 0; }
                totalBytes += stream.vertexBytes.size();
                if (totalBytes > 128 * 1024 * 1024)
                { skipOnce(sourceName, "combined vertex buffers exceed memory cap"); discardCreatedHair(hair); return 0; }
                renderPositions.insert(renderPositions.end(), positions.begin(), positions.end());
                Ogre::HardwareBufferManagerBase* bufferManager = sourceBuffer->getManager();
                if (!bufferManager)
                { skipOnce(sourceName, "no buffer manager"); discardCreatedHair(hair); return 0; }
                stream.positionBuffer = bufferManager->createVertexBuffer(sourceBuffer->getVertexSize(),
                    sourceBuffer->getNumVertices(), tailSlot ? Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY : Ogre::HardwareBuffer::HBU_DYNAMIC_WRITE_ONLY_DISCARDABLE, false);
                if (stream.positionBuffer.isNull() || stream.positionBuffer.get() == sourceBuffer.get())
                { skipOnce(sourceName, "private dynamic position buffer was not created"); discardCreatedHair(hair); return 0; }
                stream.positionBuffer->writeData(0, stream.vertexBytes.size(), &stream.vertexBytes[0], true);
                stream.vertexData->vertexBufferBinding->setBinding(stream.positionElement->getSource(), stream.positionBuffer);
            }
            std::vector<unsigned int> renderTriangles, particleTriangles;
            if (!readTriangles(hair->mesh.get(), hair->streams, renderTriangles, why))
            { skipOnce(sourceName, why); discardCreatedHair(hair); return 0; }
            if (tailSlot)
            {
                hair->catTail=flowing_hair::khajjitTailProfile(sourceName).cat;
                std::vector<unsigned int> bodyIndices,tailIndices;
                if (hair->streams.size()!=1 || !flowing_hair::splitKhajjitTail(
                    flowing_hair::khajjitTailProfile(sourceName),renderPositions,renderTriangles,bodyIndices,tailIndices))
                { gRuntimeFailures.insert("tail-profile|"+sourceName); skipOnce(sourceName,"Topology differs from the verified tail profile; body untouched"); discardCreatedHair(hair); return 0; }
                Ogre::HardwareBufferManagerBase* manager=hair->streams[0].positionBuffer->getManager();
                hair->tailBodyIndices.indexBuffer=manager->createIndexBuffer(Ogre::HardwareIndexBuffer::IT_32BIT,
                    bodyIndices.size(),Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY,false);
                hair->tailBodyIndices.indexCount=bodyIndices.size(); hair->tailBodyIndices.indexStart=0;
                hair->tailBodyIndices.indexBuffer->writeData(0,bodyIndices.size()*4,&bodyIndices[0],true);
                hair->tailEmptyIndices.indexBuffer=hair->tailBodyIndices.indexBuffer;
                hair->tailEmptyIndices.indexStart=0; hair->tailEmptyIndices.indexCount=0;
                Ogre::IndexData* cloneIndices=hair->mesh->getSubMesh(0)->indexData;
                if (cloneIndices==original->getMesh()->getSubMesh(0)->indexData)
                { skipOnce(sourceName,"tail clone index data is not private"); discardCreatedHair(hair); return 0; }
                cloneIndices->indexBuffer=manager->createIndexBuffer(Ogre::HardwareIndexBuffer::IT_32BIT,
                    tailIndices.size(),Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY,false);
                cloneIndices->indexCount=tailIndices.size(); cloneIndices->indexStart=0;
                cloneIndices->indexBuffer->writeData(0,tailIndices.size()*4,&tailIndices[0],true);
                hair->tailSourceMesh=original->getMesh();
                Ogre::IndexData* nativeIndices=hair->tailSourceMesh->getSubMesh(0)->indexData;
                hair->tailSourceIndices=nativeIndices->indexBuffer;
                hair->tailSourceStart=nativeIndices->indexStart; hair->tailSourceCount=nativeIndices->indexCount;
                hair->tailSourceMaterial=original->getSubEntity(0)->getMaterialName();
                // Keep authored blend-index streams and bone assignments intact.
                // Only tail vertices enter the solver or receive buffer writes.
                std::vector<unsigned int> used=tailIndices;
                std::sort(used.begin(),used.end()); used.erase(std::unique(used.begin(),used.end()),used.end());
                std::vector<unsigned int> compactIndex(renderPositions.size(),UINT_MAX),compactTriangles,compactToParticle;
                std::vector<flowing_hair::Vec3> compactPositions;
                for (size_t i=0;i<used.size();++i)
                {
                    compactIndex[used[i]]=static_cast<unsigned int>(i);
                    compactPositions.push_back(hair->catTail ? flowing_hair::catTailToSolver(renderPositions[used[i]]) : renderPositions[used[i]]);
                    if (hair->tailUploadRanges.empty() || hair->tailUploadRanges.back().second!=used[i])
                        hair->tailUploadRanges.push_back(std::make_pair(static_cast<size_t>(used[i]),static_cast<size_t>(used[i]+1)));
                    else hair->tailUploadRanges.back().second=used[i]+1;
                }
                for (size_t i=0;i<tailIndices.size();++i) compactTriangles.push_back(compactIndex[tailIndices[i]]);
                weld(compactPositions,compactTriangles,hair->bindPositions,compactToParticle,particleTriangles);
                hair->renderToParticle.assign(renderPositions.size(),UINT_MAX);
                for (size_t i=0;i<used.size();++i) hair->renderToParticle[used[i]]=compactToParticle[i];
            }
            else weld(renderPositions, renderTriangles, hair->bindPositions,
                hair->renderToParticle, particleTriangles);
            if (hair->bindPositions.size() < 3 || particleTriangles.empty())
            { skipOnce(sourceName, "empty welded topology"); discardCreatedHair(hair); return 0; }
            const float span = meshSpan(hair->bindPositions);
            hair->maximumDisplacement = std::max(0.25f, std::min(1.5f, span * 0.25f));
            if (facialSlot) hair->maximumDisplacement = std::max(0.25f, std::min(1.5f, span * 0.50f));
            if (tailSlot) hair->maximumDisplacement=span*1.2f;

            flowing_hair::SolverSettings settings;
            settings.damping = gConfig.damping;
            settings.bendingStiffness = gConfig.bending;
            settings.shapeStiffness = facialSlot ? gConfig.beardShapeStiffness : gConfig.shapeStiffness;
            if (tailSlot) settings.shapeStiffness=0.006f;
            settings.maximumTargetDistance = hair->maximumDisplacement;
            settings.constraintIterations = gConfig.iterations;
            std::vector<bool> pins = pinsAndCollider(hair->bindPositions, hair->collider);
            if (tailSlot)
            { pins=flowing_hair::khajjitTailPins(hair->bindPositions); hair->collider.enabled=false; }
            std::vector<unsigned int> beardLinks;
            if (facialSlot)
            {
                // A beard's bounding box is not a head: don't turn its lower
                // strands into an artificial scalp collision surface.
                hair->collider.enabled = false;
                if (!flowing_hair::ClothSolver::buildBeardAttachments(hair->bindPositions, particleTriangles,
                    gConfig.beardRootFraction, pins, beardLinks))
                { skipOnce(sourceName, "beard attachment graph could not be safely connected"); discardCreatedHair(hair); return 0; }
            }
            unsigned int unanchoredComponents = 0;
            unsigned int rootBandPinsAdded = 0;
            const bool linkedPony = !facialSlot && !tailSlot && flowing_hair::usesLinkedPonyTip(sourceName);
            if (linkedPony && !flowing_hair::ClothSolver::buildScalpAttachmentLinks(
                hair->bindPositions, particleTriangles, pins, beardLinks))
            { skipOnce(sourceName, "pony attachment graph could not be safely connected"); discardCreatedHair(hair); return 0; }
            const unsigned int componentPinsAdded = (facialSlot || linkedPony || tailSlot) ? 0 : flowing_hair::ClothSolver::ensureComponentPins(
                hair->bindPositions, particleTriangles, 3, pins, true,
                &unanchoredComponents, &rootBandPinsAdded);
            if (tailSlot ? !hair->tailDynamics.initialise(hair->bindPositions,static_cast<float>(gId%97)*2.399963f) :
                !hair->solver.initialise(hair->bindPositions, particleTriangles, pins, settings))
            { skipOnce(sourceName, "solver rejected topology"); discardCreatedHair(hair); return 0; }
            if(tailSlot)
            {
                hair->maximumDisplacement=flowing_hair::TailFollow::maximumReachDisplacement(hair->tailDynamics,hair->bindPositions);
                if(!finiteFloat(hair->maximumDisplacement) || hair->maximumDisplacement<=0 || hair->maximumDisplacement>span*4)
                { skipOnce(sourceName,"invalid tail reach bound"); discardCreatedHair(hair); return 0; }
            }
            hair->solver.addAttachmentLinks(beardLinks);
            if (gConfig.faceCollision && !facialSlot && !tailSlot)
            {
                // Bind-space head-following proxy. It uses the same skinning path
                // as the hair; never fit a face from a beard's hanging-strand bounds.
                flowing_hair::Ellipsoid face = hair->collider;
                face.center.y -= face.radii.y * 0.65f;
                face.radii.x *= 0.85f; face.radii.y *= 0.95f; face.radii.z *= 0.95f;
                hair->solver.setFaceCollider(face);
                std::ostringstream message;
                message << "Face collision proxy enabled for '" << sourceName << "': center="
                    << face.center.x << "," << face.center.y << "," << face.center.z
                    << " radii=" << face.radii.x << "," << face.radii.y << "," << face.radii.z
                    << " (bind-space approximation; authored interior excluded)";
                logLine(message.str(), false);
            }

            Ogre::SceneNode* parent = original->getParentSceneNode();
            if (!hair->manager || !parent)
            { skipOnce(sourceName, "entity has no scene-node parent"); discardCreatedHair(hair); return 0; }
            hair->visibleClone = hair->manager->createEntity(hair->mesh);
            if (!hair->visibleClone)
            { skipOnce(sourceName, "clone entity creation failed"); discardCreatedHair(hair); return 0; }
            hair->cloneName = hair->visibleClone->getName();
            {
                std::ostringstream diag;
                diag << "RENDER AUDIT mesh=" << sourceName << " streams=" << hair->streams.size() << " sourceSkeleton='" << sourceSkeleton
                     << "' sourceStates=" << original->getAllAnimationStates()
                     << " cloneStates=" << hair->visibleClone->getAllAnimationStates();
                for (size_t s = 0; s < original->getNumSubEntities(); ++s)
                {
                    Ogre::SubEntity* sub = original->getSubEntity(s);
                    unsigned short count = sub->getNumWorldTransforms();
                    diag << " sub=" << s << " sourceMatrices=" << count
                         << " cloneMatrices=" << hair->visibleClone->getSubEntity(s)->getNumWorldTransforms()
                         << " material=" << sub->getMaterial()->getName();
                    if (count && count <= 256)
                    {
                        std::vector<Ogre::Matrix4> matrices(count);
                        sub->getWorldTransforms(&matrices[0]);
                        diag << " palette0=" << matrices[0];
                    }
                }
                logLine(diag.str(), false);
            }
            const size_t materials = original->getNumSubEntities();
            if (materials != hair->visibleClone->getNumSubEntities())
            { skipOnce(sourceName, "replacement subentity count differs from source"); discardCreatedHair(hair); return 0; }
            if (!createPrivateMaterials(hair, why))
            { skipOnce(sourceName, why); discardCreatedHair(hair); return 0; }
            if(tailSlot)
            {
                bool captured=false;
                try { captured=captureTailRootWeights(hair); } catch(...) {}
                if(!captured)
                { skipOnce(sourceName,"Tail attachment weights invalid; retaining native tail"); discardCreatedHair(hair); return 0; }
            }
            hair->visibleClone->setRenderQueueGroup(original->getRenderQueueGroup());
            hair->visibleClone->setVisibilityFlags(original->getVisibilityFlags());
            hair->visibleClone->setQueryFlags(original->getQueryFlags());
            hair->visibleClone->setCastShadows(original->getCastShadows());
            hair->visibleClone->setVisible(original->getVisible());
            if (!synchroniseRenderPalette(hair, why))
            { skipOnce(sourceName, why); discardCreatedHair(hair); return 0; }
            parent->attachObject(hair->visibleClone);
            if (!updateRenderBounds(hair, why))
            { skipOnce(sourceName, why); discardCreatedHair(hair); return 0; }

            const Ogre::AxisAlignedBox& bounds = hair->mesh->getBounds();
            if (!bounds.isNull() && !bounds.isInfinite())
            {
                const float margin = std::max(0.1f, hair->maximumDisplacement + span * 0.05f);
                const Ogre::Vector3 pad(margin, margin, margin);
                hair->mesh->_setBounds(Ogre::AxisAlignedBox(
                    bounds.getMinimum() - pad, bounds.getMaximum() + pad), false);
                hair->mesh->_setBoundingSphereRadius(
                    hair->mesh->getBoundingSphereRadius() + margin);
            }
            gClones.insert(hair->visibleClone);
            // Rebind idempotently through a currently validated manager, including
            // when a new scene manager reuses an address after a world/load boundary.
            hair->manager->removeRenderObjectListener(&gHairPoseListener);
            hair->manager->addRenderObjectListener(&gHairPoseListener);
            for (size_t s = 0; s < hair->visibleClone->getNumSubEntities(); ++s)
            {
                Ogre::Renderable* renderable = hair->visibleClone->getSubEntity(s);
                hair->renderables.push_back(renderable);
                gRenderHair[renderable] = hair;
            }
            if (tailSlot)
            {
                hair->tailBodyPart=original->getSubEntity(0);
                gTailBodies[hair->tailBodyPart]=hair;
                gRuntimeFailures.erase("tail-setup|"+sourceName+"|"+hair->tailSourceMaterial);
                logLine(std::string(hair->catTail ? "Cat" : "Khajjit")+" tail isolated: native body retained; root band pinned; tail-only private draw",true);
            }
            else original->setVisible(gShowOriginal);
            hair->visibleClone->setVisible(!gShowOriginal);
            std::ostringstream info;
            info << "Simulating '" << sourceName << "': " << renderPositions.size()
                 << " vertices, " << hair->bindPositions.size() << " particles, "
                 << particleTriangles.size() / 3 << " triangles, "
                 << std::count(pins.begin(), pins.end(), true) << " pins ("
                 << componentPinsAdded << " component anchors added; "
                 << rootBandPinsAdded << " distributed root-band anchors across "
                 << unanchoredComponents << " initially unanchored card components), private clone buffer, "
                 << (sourceSkeleton.empty() ? "no skeleton resource" : "skeleton resource copied") << ", "
                 << "read-only source render palette, private colour and shadow materials"
                 << (tailSlot ? ", tail base pinned; no head collision, " : facialSlot ? ", beard component-root attachment; no face collision, " : ", one-sided rest-aware head collision, ")
                 << "shape stiffness=" << settings.shapeStiffness << ", "
                 << "attachment links=" << beardLinks.size() / 2 << ", "
                 << "deflection cap=" << hair->maximumDisplacement;
            logLine(info.str(), false);
            return hair;
        }
        catch (const Ogre::Exception& exception)
        {
            skipOnce(sourceName, std::string("Ogre setup exception: ") +
                exception.getFullDescription());
            discardCreatedHair(hair); return 0;
        }
        catch (const std::exception& exception)
        {
            skipOnce(sourceName, std::string("C++ setup exception: ") + exception.what());
            discardCreatedHair(hair); return 0;
        }
        catch (...)
        {
            skipOnce(sourceName, "unknown exception during setup");
            discardCreatedHair(hair); return 0;
        }
    }

    Ogre::Vector3 limited(const Ogre::Vector3& value, float maximum)
    {
        if (!finiteVector(value) || !finiteFloat(maximum) || maximum <= 0.0f)
            return Ogre::Vector3::ZERO;
        const float square = value.squaredLength();
        if (!finiteFloat(square)) return Ogre::Vector3::ZERO;
        return square > maximum * maximum ? value * (maximum / std::sqrt(square)) : value;
    }

    bool keyboardShortcutBlocked()
    {
        MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
        if (!input) return false;
        MyGUI::Widget* focus = input->getKeyFocusWidget();
        const bool typing = focus && focus->castType<MyGUI::EditBox>(false) != 0;
        return typing || input->isModalAny();
    }

    void updateStressWindHotkey()
    {
        // Developer-only memory flags. No keyboard polling in player builds.
        gStressWind = gInternalDebug && gInternalStressWind;
        gShowOriginal = gInternalDebug && gInternalShowOriginal;
    }

    bool updateHair(Hair* hair, Ogre::SceneNode* node, GameWorld* world, Character* character,float frameTime,
                    std::string& why)
    {
        if (!node) return true;
        // Kenshi may reapply visibility while rebuilding an appearance.  Keep
        // the rigid source hidden and the isolated animated replacement visible
        // so the player never sees both copies (or only the rigid source).
        try
        {
            if (hair->original && hair->original != hair->visibleClone && !hair->tailSlot)
                hair->original->setVisible(gShowOriginal);
            if (hair->visibleClone)
            {
                // Floor/interior masks can change after creation. Never retain an
                // old layer mask on our private replacement.
                hair->visibleClone->setVisibilityFlags(hair->original->getVisibilityFlags());
                hair->visibleClone->setVisible(!gShowOriginal);
            }
        }
        catch (...) { why = "hair visibility state could not be maintained"; return false; }
        if (hair->renderPoseFailed)
        {
            if (hair->tailSlot) gRuntimeFailures.insert("tail-setup|"+hair->sourceName+"|"+hair->tailSourceMaterial);
            why = "pre-draw pose refresh failed"; return false;
        }
        if (hair->tailSlot && (hair->original->getMesh().get()!=hair->tailSourceMesh.get() ||
            hair->original->getSubEntity(0)->getMaterialName()!=hair->tailSourceMaterial))
        { why="tail body appearance rebuilt; native draw restored"; return false; }
        if (!updateRenderBounds(hair, why)) return false;
        hair->solver.setRigidity(gConfig.hairRigidity);
        if (!finiteFloat(frameTime) || frameTime <= 0.0f)
        { return true; }
        const float dt = std::max(1.0f / 240.0f, std::min(frameTime, 1.0f / 15.0f));
        const Ogre::Vector3 position = node->_getDerivedPositionUpdated();
        const Ogre::Quaternion orientation = node->_getDerivedOrientationUpdated();
        if (!finiteVector(position) || !finiteQuaternion(orientation))
        { why = "appearance transform is non-finite"; return false; }
        const float wetDt=world->paused ? 0.0f : std::min(frameTime,0.25f);
        if (gConfig.wetHair && wetDt>0)
        {
            hair->rainSampleTimer-=wetDt;
            if (hair->rainSampleTimer<=0)
            {
                float rain=0;
                bool swimming=false;
                const int status=flowing_hair::readCharacterWetExposure(character,position.x,position.y,position.z,&rain,&swimming);
                hair->rainExposure=status==1 ? flowing_hair::unitWeather(rain) : 0.0f;
                hair->swimming=status==1 && swimming;
                hair->rainSampleTimer=0.25f;
                if (status<0) { static bool logged=false; if(!logged){logLine("Native rain exposure read failed; wetting disabled for invalid samples",true);logged=true;} }
            }
            hair->wetness=flowing_hair::advanceHairWetness(hair->wetness,hair->rainExposure,wetDt,
                gConfig.soakSeconds,gConfig.drySeconds,hair->swimming);
        }
        if (!gConfig.wetHair) hair->wetness=0;
        hair->solver.setWetness(hair->wetness);
        const float wetWeight=flowing_hair::wetHairWeight(hair->wetness,gConfig.wetExtraWeight);
        Ogre::Vector3 inertia = Ogre::Vector3::ZERO;
        if (!hair->motionReady || (position - hair->lastPosition).squaredLength() > 100.0f)
        {
            hair->solver.reset(hair->bindPositions); hair->tailDynamics.reset(); hair->tailFollow.reset(); hair->lastVelocity = Ogre::Vector3::ZERO; hair->motionReady = true;
        }
        else
        {
            const Ogre::Vector3 velocity = (position - hair->lastPosition) / dt;
            inertia = (hair->lastVelocity - velocity) * (gConfig.inertia / dt);
            hair->lastVelocity = velocity;
        }
        hair->lastPosition = position;
        flowing_hair::WindSettings windSettings;
        const float normalStrength = gConfig.proceduralWind ? gConfig.windStrength : 0.0f;
        const float normalGust = gConfig.proceduralWind ? gConfig.gustStrength : 0.0f;
        const float normalVertical = gConfig.proceduralWind ? gConfig.windVertical : 0.0f;
        windSettings.enabled = gStressWindBlend > 0.0f || gConfig.proceduralWind || gConfig.useGameWind;
        windSettings.strength = normalStrength +
            (gConfig.stressAcceleration - normalStrength) * gStressWindBlend;
        windSettings.gustStrength = normalGust * (1.0f - gStressWindBlend);
        windSettings.gustFrequency = gConfig.gustFrequency;
        windSettings.directionDegrees = gConfig.windDirection;
        windSettings.directionVariationDegrees =
            gConfig.windDirectionVariation * (1.0f - gStressWindBlend);
        windSettings.verticalStrength = normalVertical * (1.0f - gStressWindBlend);
        windSettings.spatialScale = gConfig.windSpatialScale;
        if (gConfig.useGameWind)
        {
            const float gameWind = world->getWindSpeed(position);
            if (gameWind >= 0.0f && gameWind < 1000.0f)
                windSettings.strength += std::min(gameWind, 20.0f) *
                    gConfig.gameWindMultiplier * (1.0f - gStressWindBlend);
        }
        const double windTime = static_cast<double>(GetTickCount()) * 0.001;
        flowing_hair::Vec3 sampledWind=flowing_hair::sampleIntegratedWind(position.x,position.z);
        if (gInternalDebug && gStressWindBlend > 0)
            sampledWind = flowing_hair::sampleWind(windSettings,windTime,position.x,position.z);
        const Ogre::Vector3 wind=Ogre::Vector3(sampledWind.x, sampledWind.y, sampledWind.z)/wetWeight;
        const flowing_hair::Vec3 airflow = flowing_hair::sampleMovementAirflow(
            flowing_hair::Vec3(hair->lastVelocity.x, hair->lastVelocity.y, hair->lastVelocity.z),
            gConfig.movementDrag, gConfig.maximumMovementAcceleration);
        const Ogre::Vector3 movement=Ogre::Vector3(airflow.x, airflow.y, airflow.z)/wetWeight;
        const float accelerationCap = gConfig.maxAcceleration +
            (std::max(gConfig.maxAcceleration, gConfig.stressAcceleration) - gConfig.maxAcceleration) * gStressWindBlend;
        // Bound the existing forces separately so adding movement doesn't
        // increase idle wind, or silently discard motion at the old 40-unit cap.
        // The whole tail is now root-attached physics, not a skin-deformed body
        // surface. Use that same attachment frame for world forces. Reduce the
        // hair-strength running drag so it trails without snapping to its limit.
        const Ogre::Quaternion forceFrame=hair->tailSlot && hair->tailPoseValid ? hair->tailPose : orientation;
        const float motionScale=hair->tailSlot ? 0.10f : 1.0f;
        const Ogre::Vector3 local = forceFrame.Inverse() *
            (limited(inertia*motionScale + Ogre::Vector3(0, -gConfig.gravity*wetWeight, 0) + wind, accelerationCap) + movement*motionScale);
        if (hair->tailSlot)
        {
            hair->tailFollowDt=world->paused ? 0.0f : frameTime;
            hair->tailFollowPending=true;
            const flowing_hair::Vec3 tailForce=hair->catTail ? flowing_hair::catTailToSolver(flowing_hair::Vec3(local.x,local.y,local.z)) : flowing_hair::Vec3(local.x,local.y,local.z);
            hair->tailDynamics.advance(hair->tailFollowDt,
                tailForce,gConfig.hairRigidity,hair->wetness,true);
        }
        else hair->solver.advance(frameTime, hair->bindPositions,
            flowing_hair::Vec3(local.x, local.y, local.z), hair->collider);
        if (!hair->faceContactReported && hair->solver.faceContacts())
        {
            logLine("Face collision contact resolved for '" + hair->sourceName + "'", false);
            hair->faceContactReported = true;
        }
        const std::vector<flowing_hair::Vec3>* result = hair->tailSlot ? &hair->tailDynamics.positions() : &hair->solver.positions();
        float maximumDisplacementSquared = 0.0f;
        bool invalidResult = result->size() != hair->bindPositions.size();
        for (size_t i = 0; !invalidResult && i < result->size(); ++i)
        {
            if (!finiteVector((*result)[i])) { invalidResult = true; break; }
            maximumDisplacementSquared = std::max(maximumDisplacementSquared,
                flowing_hair::lengthSquared((*result)[i] - hair->bindPositions[i]));
        }
        const float hardLimit = hair->maximumDisplacement * 1.02f;
        if (!finiteFloat(maximumDisplacementSquared) ||
            maximumDisplacementSquared > hardLimit * hardLimit)
            invalidResult = true;
        if (invalidResult)
        {
            hair->solver.reset(hair->bindPositions);
            hair->tailDynamics.reset();
            hair->tailFollow.reset();
            result = hair->tailSlot ? &hair->tailDynamics.positions() : &hair->solver.positions();
            hair->motionReady = false;
            const std::string key = std::string("solver-reset|") + hair->sourceName;
            if (gRuntimeFailures.insert(key).second)
                logLine("Reset unstable solver output for '" + hair->sourceName +
                    "' before it reached Ogre", true);
            maximumDisplacementSquared = 0.0f;
        }
        const float frameDisplacement = std::sqrt(maximumDisplacementSquared);
        hair->maximumObservedDisplacement = std::max(
            hair->maximumObservedDisplacement, frameDisplacement);
        for (size_t s = 0; s < hair->streams.size(); ++s)
        {
            if(hair->tailSlot) break; // TailFollow uploads with the current pre-draw attachment.
            HairStream& stream = hair->streams[s];
            const size_t stride = stream.positionBuffer->getVertexSize();
            for (size_t i = 0; i < stream.vertexData->vertexCount; ++i)
            {
                if (hair->renderToParticle[stream.renderOffset+i]==UINT_MAX) continue;
                unsigned char* vertex = &stream.vertexBytes[0] + (stream.vertexData->vertexStart + i) * stride;
                float* output = 0; stream.positionElement->baseVertexPointerToElement(vertex, &output);
                const flowing_hair::Vec3& p = (*result)[hair->renderToParticle[stream.renderOffset + i]];
                output[0] = p.x; output[1] = p.y; output[2] = p.z;
            }
            stream.positionBuffer->writeData(0, stream.vertexBytes.size(), &stream.vertexBytes[0], true);
        }
        if (hair->lastVelocity.squaredLength() < 0.25f) hair->movementReported = false;
        if (!hair->movementReported && movement.squaredLength() > 100.0f)
        {
            std::ostringstream message;
            message << "movement airflow reached '" << hair->sourceName << "': speed="
                    << hair->lastVelocity.length() << " airflow=" << movement
                    << " displacement=" << frameDisplacement;
            logLine(message.str(), false); hair->movementReported = true;
        }
        ++hair->telemetryFrames;
        if (gInternalDebug && !hair->uploadReported && hair->telemetryFrames >= 120)
        {
            for (size_t s = 0; s < hair->visibleClone->getNumSubEntities(); ++s)
            {
                auditRenderable(hair->original->getSubEntity(s), "source");
                auditRenderable(hair->visibleClone->getSubEntity(s), "clone");
                Ogre::RenderOperation op;
                hair->visibleClone->getSubEntity(s)->getRenderOperation(op);
                const Ogre::VertexElement* element = op.vertexData ?
                    op.vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION) : 0;
                bool privateBuffer = false;
                if (element)
                {
                    Ogre::HardwareVertexBufferSharedPtr buffer = op.vertexData->vertexBufferBinding->getBuffer(element->getSource());
                    for (size_t v = 0; v < hair->streams.size(); ++v)
                        privateBuffer = privateBuffer || buffer.get() == hair->streams[v].positionBuffer.get();
                }
                std::ostringstream draw;
                draw << "Draw buffer check '" << hair->sourceName << "' sub=" << s << " privatePositionBuffer=" << privateBuffer;
                logLine(draw.str(), !privateBuffer);
            }
            std::ostringstream message;
            message << "bounds source=" << hair->original->getWorldAabbUpdated().mCenter
                    << " +/- " << hair->original->getWorldAabbUpdated().mHalfSize
                    << " clone=" << hair->visibleClone->getWorldAabbUpdated().mCenter
                    << " +/- " << hair->visibleClone->getWorldAabbUpdated().mHalfSize
                    << " root=" << position << " ";
            message << "render-buffer telemetry for '" << hair->sourceName
                    << "': 120 updates uploaded, maximum particle displacement="
                    << hair->maximumObservedDisplacement;
            logLine(message.str(), false);
            hair->uploadReported = true;
        }
        if (!gStressWind) hair->stressReported = false;
        if (gStressWind && gStressWindBlend >= 0.99f && !hair->stressReported)
        {
            std::ostringstream message;
            message << "stress wind reached hair '" << hair->sourceName
                    << "': local acceleration=" << local.length()
                    << ", maximum particle displacement=" << frameDisplacement;
            for (size_t s = 0; s < hair->streams.size(); ++s)
            {
                float maximum = 0.0f;
                for (size_t v = 0; v < hair->streams[s].vertexData->vertexCount; ++v)
                {
                    const size_t particle = hair->renderToParticle[hair->streams[s].renderOffset + v];
                    if (particle==UINT_MAX) continue;
                    maximum = std::max(maximum, flowing_hair::length((*result)[particle]-hair->bindPositions[particle]));
                }
                message << " stream" << s << "=" << maximum;
            }
            logLine(message.str(), false);
            hair->stressReported = true;
        }
        return true;
    }

    bool readHairCameraPosition(GameWorld* world, Ogre::Camera** camera, Ogre::SceneManager** scene, Ogre::Vector3* position)
    {
        __try
        {
            if (!world->render || !gObjectVisible) return false;
            Renderer* renderer=static_cast<Renderer*>(world->render);
            *camera=renderer->getCamera();
            *scene=renderer->getSceneManager();
            if (!*camera || !*scene) return false;
            // Use the rendering camera's scene coordinates, just like scene nodes
            // and world AABBs; do not mix global positions with floating origins.
            *position=(*camera)->getDerivedPosition();
            return true;
        }
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        { return false; }
    }

    bool characterVisibleForHair(Character* character, AppearanceBase* appearance)
    {
        __try { return appearance && appearance->getVisible() && character->_NV_getVisible(); }
        __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        { return false; }
    }

    void reportHairVisibility(bool player, bool cameraValid, float distance, bool nativeVisible,
        bool inRange, bool inView, bool bodyDrawn)
    {
        if (!player || gVisibilityReports>=24 || GetTickCount64()<gNextVisibilityReport) return;
        ++gVisibilityReports; gNextVisibilityReport=GetTickCount64()+5000;
        std::ostringstream message;
        message << "[Flowing Hair] Visibility gate camera=" << cameraValid << " distance=" << distance
            << " limit=" << gConfig.hairDistance << " nativeVisible=" << nativeVisible
            << " range=" << inRange << " frustum=" << inView << " mainBodyDraw=" << bodyDrawn
            << " watches=" << gBodyWatches.size() << " activeAttachments=" << gHair.size()
            << " characterLimit=" << flowing_hair::clampHairCharacterLimit(gConfig.hairCharacterLimit);
        DebugLog(message.str());
    }

    void updateAll(GameWorld* world, float frameTime)
    {
        gRuntimePhase = "world validation";
        if (world != gTrackedWorld)
        {
            clearBodyWatches();
            abandonAllHair("world identity change");
            gTrackedWorld = world;
            gWorldReady = false;
        }
        if (!world || !world->initialized || world->gameResetting)
        {
            clearBodyWatches();
            if (gWorldReady || !gHair.empty()) abandonAllHair("world reset/loading boundary");
            gWorldReady = false;
            gStressWind = false;
            gStressWindBlend = 0.0f;
            gPWasDown = false;
            return;
        }
        gWorldReady = true;
        Ogre::Vector3 cameraPosition=Ogre::Vector3::ZERO;
        Ogre::Camera* renderCamera=0;
        Ogre::SceneManager* viewScene=0;
        bool cameraValid=false;
        try { cameraValid=readHairCameraPosition(world,&renderCamera,&viewScene,&cameraPosition); } catch (...) {}
        cameraValid=cameraValid && finiteFloat(cameraPosition.x) && finiteFloat(cameraPosition.y) && finiteFloat(cameraPosition.z);
        gMainViewCamera=cameraValid ? renderCamera : 0;
        ++gVisibilityFrame;
        if (cameraValid && gWatchScene!=viewScene)
        {
            viewScene->removeRenderObjectListener(&gHairPoseListener);
            viewScene->addRenderObjectListener(&gHairPoseListener);
            gWatchScene=viewScene;
        }
        // Expiry touches only plugin-owned records, never stale engine pointers.
        for (BodyWatchMap::iterator w=gBodyWatches.begin();w!=gBodyWatches.end();)
        {
            if (gVisibilityFrame-w->second.touched>120)
            { forgetBodyParts(w->second); gBodyWatches.erase(w++); }
            else ++w;
        }
        if (!cameraValid)
        {
            static bool reported=false;
            if (!reported) { logLine("Camera position unavailable; keeping normal hair until range can be checked",true); reported=true; }
        }
        gRuntimePhase = "debug hotkey";
        updateStressWindHotkey();
        const float safeFrameTime = finiteFloat(frameTime) ? frameTime : 0.0f;
        const float stressStep = std::min(std::max(safeFrameTime, 0.0f), 0.1f) *
            (gStressWind ? 0.75f : 3.0f);
        if (gStressWind)
            gStressWindBlend = std::min(1.0f, gStressWindBlend + stressStep);
        else
            gStressWindBlend = std::max(0.0f, gStressWindBlend - stressStep);
        gRuntimePhase = "character update list";
        const ogre_unordered_set<Character*>::type& characters = world->getCharacterUpdateList();
        std::vector<Character*> work;
        std::set<Character*> players;
        if (world->player)
        {
            const PlayerInterface* player = world->player;
            const lektor<Character*>& roster = player->getAllPlayerCharacters();
            const size_t count = std::min(static_cast<size_t>(roster.size()), static_cast<size_t>(256));
            if (roster.valid())
                for (size_t p = 0; p < count; ++p)
                    if (roster[static_cast<unsigned int>(p)] && players.insert(roster[static_cast<unsigned int>(p)]).second)
                        work.push_back(roster[static_cast<unsigned int>(p)]);
        }
        // Player discovery must not depend on membership/order in the engine's
        // changing update set. Rotate the bounded NPC window; never retain an iterator.
        if (!characters.empty())
        {
            ogre_unordered_set<Character*>::type::const_iterator next = characters.find(gNextNpc);
            if (next == characters.end()) next = characters.begin();
            const size_t count = std::min(characters.size(), static_cast<size_t>(256));
            for (size_t n = 0; n < count; ++n)
            {
                Character* c = *next;
                if (c && !players.count(c)) work.push_back(c);
                if (++next == characters.end()) next = characters.begin();
            }
            gNextNpc = *next;
        }

        // Keep previously discovered nearby contenders in the selection even
        // when today's rotating discovery window is elsewhere. Never dereference
        // a cached identity unless the CURRENT native roster/update set validates it.
        std::set<Character*> queued(work.begin(),work.end());
        for (BodyWatchMap::const_iterator watched=gBodyWatches.begin();watched!=gBodyWatches.end();++watched)
        {
            Character* c=watched->second.character;
            if (c && (players.count(c) || characters.find(c)!=characters.end()) && queued.insert(c).second)
                work.push_back(c);
        }
        // Comparator uses captured numbers only, never engine calls or raw object
        // dereferences. Both PCs and NPCs compete solely by camera distance.
        std::vector<flowing_hair::CameraPriority> priority;
        for (size_t i=0;i<work.size();++i)
        {
            double distanceSquared=DBL_MAX;
            try
            {
                AppearanceBase* a=work[i] ? work[i]->getAppearance() : 0;
                Ogre::SceneNode* root=a && a->getAttachmentsLoaded() ? a->getSceneNode() : 0;
                if (cameraValid && root)
                {
                    const Ogre::Vector3& p=root->_getDerivedPositionUpdated();
                    distanceSquared=flowing_hair::cameraDistanceSquared(
                        flowing_hair::Vec3(cameraPosition.x,cameraPosition.y,cameraPosition.z),flowing_hair::Vec3(p.x,p.y,p.z));
                }
                BodyWatchMap::iterator watched=gBodyWatches.find(a);
                if (watched!=gBodyWatches.end()) watched->second.distanceSquared=distanceSquared;
            }
            catch (...) {} // invalid candidates sort last and retain normal validation below
            priority.push_back(flowing_hair::CameraPriority(i,reinterpret_cast<size_t>(work[i]),distanceSquared));
        }
        std::sort(priority.begin(),priority.end());
        const std::vector<Character*> unsorted=work;
        for (size_t i=0;i<priority.size();++i) work[i]=unsorted[priority[i].index];
        flowing_hair::HairCharacterBudget characterBudget(gConfig.hairCharacterLimit);
        unsigned int createdThisFrame = 0;
        for (size_t actor = 0; actor < work.size(); ++actor)
        {
            try
            {
                Character* c = work[actor];
                if (!c) continue;
                const bool simulate = players.count(c) != 0 || c->isPlayerCharacter() || gConfig.simulateNpcs;
                gRuntimePhase = "appearance validation";
                AppearanceBase* a = c->getAppearance();
                const bool loaded = a && a->getAttachmentsLoaded();
                bool visible = characterVisibleForHair(c,a);
                Ogre::SceneNode* root = loaded ? a->getSceneNode() : 0;
                if (!root) continue;
                const bool hasHair = gHair.find(HairKey(a,ScalpSlot))!=gHair.end() ||
                    gHair.find(HairKey(a,BeardSlot))!=gHair.end() || gHair.find(HairKey(a,TailSlot))!=gHair.end();
                bool inRange=false;
                float cameraDistance=-1.0f;
                if (cameraValid && simulate && visible)
                {
                    const Ogre::Vector3& actorPosition=root->_getDerivedPositionUpdated();
                    cameraDistance=(actorPosition-cameraPosition).length();
                    inRange=flowing_hair::hairWithinCameraRange(
                        flowing_hair::Vec3(cameraPosition.x,cameraPosition.y,cameraPosition.z),
                        flowing_hair::Vec3(actorPosition.x,actorPosition.y,actorPosition.z),gConfig.hairDistance,hasHair);
                }
                // Cheap range rejection BEFORE scene traversal or mesh discovery.
                // Existing replacements still need validated traversal for safe retirement.
                if ((!simulate || !visible || !inRange) && !hasHair)
                {
                    reportHairVisibility(players.count(c)!=0,cameraValid,cameraDistance,visible,inRange,false,false);
                    continue;
                }
                gRuntimePhase = "appearance scene traversal";
                std::vector<Ogre::Entity*> all = entities(root);
                bool inView=false;
                bool bodyDrawn=false;
                if (visible)
                {
                    // The unmodified body is authoritative: our source hair is
                    // deliberately hidden, so its visible flag cannot be used.
                    visible=containsEntity(all,a->body) && gObjectVisible && gObjectVisible(a->body);
                    if (visible && cameraValid && inRange)
                    {
                        const Ogre::Aabb& box=a->body->getWorldAabbUpdated();
                        if (finiteVector(box.mCenter) && finiteVector(box.mHalfSize))
                        {
                            const Ogre::Vector3 margin(2.0f,2.0f,2.0f);
                            const Ogre::AxisAlignedBox bounds(box.mCenter-box.mHalfSize-margin,
                                box.mCenter+box.mHalfSize+margin);
                            inView=renderCamera->isVisible(bounds);
                        }
                    }
                    if (visible && inRange && inView) bodyDrawn=bodyDrawnInMainView(c,a,a->body,
                        static_cast<double>(cameraDistance)*cameraDistance);
                }
                reportHairVisibility(players.count(c)!=0,cameraValid,cameraDistance,visible,inRange,inView,bodyDrawn);
                if (gInternalDebug && players.count(c))
                {
                    std::ostringstream audit;
                    audit << "PLAYER DISCOVERY actor=" << c << " appearance=" << a
                          << " loaded=" << loaded << " visible=" << visible << " root=" << root;
                    if (a) audit << " style=" << a->hairStyle << " shaved=" << a->shaved << " body=" << a->body;
                    for (size_t e = 0; e < all.size() && e < 32; ++e)
                        if (!all[e]->getMesh().isNull())
                        {
                            const std::string& meshName = all[e]->getMesh()->getName();
                            if (meshName.compare(0,12,"FlowingHair_") == 0) continue;
                            audit << " [" << meshName << " entity=" << all[e] << " name="
                                << all[e]->getName().substr(0,160) << " kind="
                                << flowing_hair::classifyHair(meshName,all[e]->getName())
                                << " cloneCache=" << gClones.count(all[e]) << " belowRoot="
                                << below(all[e]->getParentNode(),root) << "]";
                        }
                    if (gPlayerAudit[c] != audit.str())
                    { gPlayerAudit[c] = audit.str(); logLine(audit.str(), false); }
                }
                if (!root) continue;
                const bool admitted=simulate && visible && inRange && inView && bodyDrawn && characterBudget.admit();
                if (!admitted)
                {
                    for (int slot=0;slot<SlotCount;++slot)
                    {
                        HairMap::iterator old = gHair.find(HairKey(a,slot));
                        if (old != gHair.end())
                        { destroyHair(old->second,all,visible); gHair.erase(old); all=entities(root); }
                    }
                    continue;
                }
                for (int slot = 0; slot < SlotCount; ++slot)
                {
                    const bool facialSlot = slot == BeardSlot,tailSlot=slot==TailSlot;
                    const HairKey key(a, slot);
                    // The first slot can delete a clone. Refresh before traversing
                    // the second slot so 'all' never contains a destroyed entity.
                    if (slot!=ScalpSlot) all = entities(root);
                    gRuntimePhase = "hair candidate selection";
                    HairMap::iterator old = gHair.find(key);
                    const bool oldRelationshipValid = old != gHair.end() && (facialSlot || tailSlot || !a->shaved) &&
                        (tailSlot ? ((old->second->catTail ? gConfig.catTails : gConfig.khajjitTails) && a->body==old->second->original) : a->hairStyle == old->second->hairStyle) && root == old->second->root &&
                        containsEntity(all, old->second->original) &&
                        containsEntity(all, old->second->visibleClone);
                    Ogre::Entity* original = oldRelationshipValid ? old->second->original : 0;
                    bool candidateScanned = false;
                    if (old != gHair.end() && !oldRelationshipValid)
                    {
                        // Search while every pointer in 'all' is still live. The old clone is
                        // excluded by gClones; searching after destroying it would dereference
                        // a stale pointer retained in this frame's traversal vector.
                        original = findAttachment(a, all, slot);
                        candidateScanned = true;
                        if (!original && slot==ScalpSlot && a->hairStyle && !a->shaved) logRelatedMeshes(a, all);
                        destroyHair(old->second, all); gHair.erase(old); old = gHair.end();
                    }
                    if (!original && old == gHair.end())
                    {
                        if (createdThisFrame >= 4) continue;
                        if (!candidateScanned) original = findAttachment(a, all, slot);
                    }
                    if (!original)
                    {
                        if (gInternalDebug && players.count(c) && !tailSlot)
                        {
                            std::ostringstream diagnostic;
                            diagnostic << "Player " << (facialSlot ? "beard" : "scalp")
                                << " selection empty: style=" << a->hairStyle << " shaved=" << a->shaved;
                            for (size_t e=0;e<all.size();++e)
                            {
                                if (all[e]->getMesh().isNull()) continue;
                                const std::string& name = all[e]->getMesh()->getName();
                                if (name.compare(0,12,"FlowingHair_")==0 || !hairName(name)) continue;
                                diagnostic << " [" << name << " body=" << (all[e]==a->body)
                                    << " cloneCache=" << gClones.count(all[e]) << " sameTree="
                                    << below(all[e]->getParentNode(),root) << " kind="
                                    << flowing_hair::classifyHair(name,all[e]->getName()) << "]";
                            }
                            if (gRuntimeFailures.insert(diagnostic.str()).second) logLine(diagnostic.str(),true);
                        }
                        if (!candidateScanned && slot==ScalpSlot && a->hairStyle && !a->shaved) logRelatedMeshes(a, all);
                        continue;
                    }
                    gRuntimePhase = "hair instance creation";
                    const bool newlyCreated = old == gHair.end();
                    Hair* hair = newlyCreated ? createHair(a, root, original, slot) : old->second;
                    if (!hair) continue;
                    if (newlyCreated) ++createdThisFrame;
                    if (newlyCreated)
                    {
                        try { gHair[key] = hair; }
                        catch (...) { discardCreatedHair(hair); throw; }
                    }
                    gRuntimePhase = "hair simulation update";
                    std::string why;
                    if (!updateHair(hair, root, world,c, safeFrameTime, why))
                    {
                        skipOnce(hair->sourceName, why);
                        HairMap::iterator remove = gHair.find(key);
                        if (remove != gHair.end()) gHair.erase(remove);
                        if (newlyCreated) discardCreatedHair(hair);
                        else destroyHair(hair, all);
                    }
                }
                // A bald/unsupported character must not occupy one of the
                // simulation slots. Scalp, beard and tail together consume one.
                if (gHair.find(HairKey(a,ScalpSlot))==gHair.end() && gHair.find(HairKey(a,BeardSlot))==gHair.end() &&
                    gHair.find(HairKey(a,TailSlot))==gHair.end())
                    characterBudget.release();
            }
            catch (const Ogre::Exception& exception)
            {
                const std::string key = std::string(gRuntimePhase) + "|ogre|" + exception.what();
                if (gRuntimeFailures.insert(key).second)
                    logLine(std::string("Character skipped after Ogre exception during ") +
                        gRuntimePhase + ": " + exception.getFullDescription(), true);
            }
            catch (const std::exception& exception)
            {
                const std::string key = std::string(gRuntimePhase) + "|cpp|" + exception.what();
                if (gRuntimeFailures.insert(key).second)
                    logLine(std::string("Character skipped after C++ exception during ") +
                        gRuntimePhase + ": " + exception.what(), true);
            }
            catch (...)
            {
                const std::string key = std::string(gRuntimePhase) + "|unknown";
                if (gRuntimeFailures.insert(key).second)
                    logLine(std::string("Character skipped after unknown exception during ") +
                        gRuntimePhase, true);
            }
        }

        // An appearance omitted from Kenshi's update list is not proven dead. Keep its
        // replacement dormant until it is revalidated or a world boundary clears it;
        // dereferencing it here was a stale-pointer risk in earlier builds.
        gRuntimePhase = "idle";
    }

    void (*gOriginalOptionsCreate)(OptionsWindow*) = 0;
    void (*gOriginalOptionsShow)(OptionsWindow*,bool) = 0;
    void (*gOriginalOptionsSave)(OptionsWindow*) = 0;
    void injectGraphicsOption(OptionsWindow* window)
    {
        if (!window || !window->tabs) return;
        MyGUI::TabControl* tabs=window->tabs;
        const std::string label="Non PC Characters Have Hair Physics";
        for (size_t i=0;i<tabs->getItemCount() && i<32;++i)
        {
            if (!containsInsensitive(tabs->getItemNameAt(i).asUTF8(),"graphics")) continue;
            DatapanelGUI** stored=tabs->getItemDataAt<DatapanelGUI*>(i,false);
            if (!stored || !*stored) return;
            DatapanelGUI* panel=*stored;
            const int category=panel->_NV_getCurrentCategory();
            if (!panel->lineExists(label,category))
            {
                DataPanelLine_CheckBox* line=panel->setLineCheckbox(label,&gConfig.simulateNpcs,category);
                if (line) line->setToolTip("Enable hair and beard physics for loaded, visible NPCs within Flowing Hair Distance. Disable to reduce CPU use.",window->tooltip);
            }
            const std::string distanceLabel="Flowing Hair Distance";
            if (!panel->lineExists(distanceLabel,category))
            {
                DataPanelLine_SliderEditable* line=panel->setLineSliderEditable(distanceLabel,category,true,
                    0.0f,2000.0f,&gConfig.hairDistance);
                if (line) line->setToolTip("Maximum camera distance in world units for player and NPC hair/beard physics. Default 500, maximum 2000. 0 disables simulation. Distant hair renders normally. Activation starts 10% inside the limit to prevent repeated switching.",window->tooltip);
            }
            const std::string limitLabel="Flowing Hair Character Limit";
            if (!panel->lineExists(limitLabel,category))
            {
                // Native Slider delivers integer values divided by scale. Scale 1
                // keeps both the displayed selection and the live budget integral.
                DataPanelLine_Slider* line=panel->setLineSlider(limitLabel,limitLabel,
                    1.0f,100.0f,category,&gConfig.hairCharacterLimit,1.0f);
                if (line) line->setToolTip("Maximum characters running physics at once. Default 10, maximum 100. Nearest visible eligible characters take priority regardless of player/NPC status. Scalp, beard and supported tails count together as one.",window->tooltip);
            }
            const std::string rigidityLabel="Hair Rigidity";
            if (!panel->lineExists(rigidityLabel,category))
            {
                DataPanelLine_Slider* line=panel->setLineSlider(rigidityLabel,rigidityLabel,
                    0.0f,100.0f,category,&gConfig.hairRigidity,1.0f);
                if (line) line->setToolTip("Hair, beard and supported tails resistance to bending: 0 softer, 100 stiffer. 50 is the original feel. Applies while playing; roots remain attached.",window->tooltip);
            }
            const std::string gravityLabel="Hair Gravity";
            if (!panel->lineExists(gravityLabel,category))
            {
                DataPanelLine_SliderEditable* line=panel->setLineSliderEditable(gravityLabel,category,true,
                    0.0f,30.0f,&gConfig.gravity);
                if (line) line->setToolTip("Downward pull on hair and beards. Default 7.5; 0 removes gravity. Rain adds wet weight; swimming fully soaks hair. Wet hair gradually dries after exposure.",window->tooltip);
            }
            return;
        }
    }
    void optionsCreate(OptionsWindow* window)
    { gOriginalOptionsCreate(window); try { injectGraphicsOption(window); } catch (...) { logLine("Graphics option injection failed",true); } }
    void optionsShow(OptionsWindow* window,bool visible)
    { gOriginalOptionsShow(window,visible); if(visible) try { injectGraphicsOption(window); } catch (...) { logLine("Graphics option injection failed",true); } }
    void optionsSave(OptionsWindow* window)
    {
        gOriginalOptionsSave(window);
        const std::string file=directory()+"\\FlowingHair.ini";
        char rigidity[32];
        const float value=finiteFloat(gConfig.hairRigidity) ? std::max(0.0f,std::min(100.0f,gConfig.hairRigidity)) : 50.0f;
        _snprintf(rigidity,sizeof(rigidity)-1,"%.3f",value); rigidity[sizeof(rigidity)-1]=0;
        const bool npcSaved=WritePrivateProfileStringA("FlowingHair","SimulateNPCs",gConfig.simulateNpcs ? "1" : "0",file.c_str())!=0;
        const bool rigiditySaved=WritePrivateProfileStringA("FlowingHair","HairRigidity",rigidity,file.c_str())!=0;
        const float gravity=finiteFloat(gConfig.gravity) ? std::max(0.0f,std::min(30.0f,gConfig.gravity)) : 7.5f;
        _snprintf(rigidity,sizeof(rigidity)-1,"%.3f",gravity); rigidity[sizeof(rigidity)-1]=0;
        const bool gravitySaved=WritePrivateProfileStringA("FlowingHair","Gravity",rigidity,file.c_str())!=0;
        _snprintf(rigidity,sizeof(rigidity)-1,"%.3f",flowing_hair::clampHairDistance(gConfig.hairDistance)); rigidity[sizeof(rigidity)-1]=0;
        const bool distanceSaved=WritePrivateProfileStringA("FlowingHair","FlowingHairDistance",rigidity,file.c_str())!=0;
        _snprintf(rigidity,sizeof(rigidity)-1,"%u",flowing_hair::clampHairCharacterLimit(gConfig.hairCharacterLimit)); rigidity[sizeof(rigidity)-1]=0;
        const bool limitSaved=WritePrivateProfileStringA("FlowingHair","FlowingHairCharacterLimit",rigidity,file.c_str())!=0;
        if (!npcSaved || !rigiditySaved || !gravitySaved || !distanceSaved || !limitSaved) logLine("Could not save graphics settings to FlowingHair.ini",true);
    }

    void mainLoop(GameWorld* world, float frameTime)
    {
        gOriginalMainLoop(world, frameTime);
        if (!gConfig.enabled) return;
        try { flowing_hair::updateIntegratedWind(world,frameTime); updateAll(world, frameTime); }
        catch (const Ogre::Exception& exception)
        {
            static bool reported = false;
            if (!reported)
            {
                logLine(std::string("Ogre exception during ") + gRuntimePhase + ": " +
                    exception.getFullDescription(), true);
                reported = true;
            }
        }
        catch (const std::exception& exception)
        {
            static bool reported = false;
            if (!reported)
            {
                logLine(std::string("C++ exception during ") + gRuntimePhase + ": " +
                    exception.what(), true);
                reported = true;
            }
        }
        catch (...)
        {
            static bool reported = false;
            if (!reported)
            {
                logLine(std::string("Unknown exception during ") + gRuntimePhase +
                    "; frame skipped", true);
                reported = true;
            }
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { gModule = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}

__declspec(dllexport) void startPlugin()
{
    if (gInternalDebug) gLog = std::fopen((directory() + "\\FlowingHair.log").c_str(), "w");
    loadConfig();
    HMODULE ogre=GetModuleHandleA("OgreMain_x64.dll");
    if (ogre)
    {
        gObjectVisible=reinterpret_cast<ObjectVisibleFn>(GetProcAddress(ogre,"?getVisible@MovableObject@Ogre@@QEBA_NXZ"));
    }
    if (!gObjectVisible)
    { logLine("Native visibility exports missing; leaving original hair unchanged",true); return; }
    if (gConfig.enabled && (gConfig.khajjitTails || gConfig.catTails))
    {
        FARPROC draw=ogre ? GetProcAddress(ogre,"?getRenderOperation@SubEntity@Ogre@@UEAAXAEAVRenderOperation@2@@Z") : 0;
        gTailDrawHookReady=draw && KenshiLib::SUCCESS==KenshiLib::AddHook(draw,&subEntityDraw,&gOriginalSubEntityDraw);
        if (!gTailDrawHookReady) logLine("Tail draw hook unavailable; hair unchanged, native tails retained",true);
    }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&OptionsWindow::create),&optionsCreate,&gOriginalOptionsCreate) ||
        KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&OptionsWindow::_NV_show),&optionsShow,&gOriginalOptionsShow) ||
        KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&OptionsWindow::saveOptions),&optionsSave,&gOriginalOptionsSave))
        logLine("One or more graphics settings hooks could not be installed",true);
    std::ostringstream stamp;
    stamp << "[Flowing Hair] version 4.3 loaded (" << __DATE__ << " " << __TIME__ << ")";
    DebugLog(stamp.str());
    logLine("isolated clone mode: private cloth buffer and materials; original hair pose explicitly refreshed then its world skin palette copied; skeleton-sharing state is never inspected or mutated", false);
    if (gConfig.proceduralWind)
    {
        std::ostringstream windInfo;
        windInfo << "procedural wind enabled: base=" << gConfig.windStrength
                 << ", gust=" << gConfig.gustStrength
                 << ", frequency=" << gConfig.gustFrequency << " Hz"
                 << ", direction=" << gConfig.windDirection << " degrees";
        logLine(windInfo.str(), false);
    }
    if (gConfig.stressWindHotkey)
        logLine("debug hotkey enabled: P toggles maximum steady wind; suppressed while typing or in a modal", false);

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
        &mainLoop, &gOriginalMainLoop))
    {
        logLine("Failed to install main-loop hook; plugin inactive", true); return;
    }
    flowing_hair::initialiseIntegratedWind(directory()+"\\FlowingHair.ini",gConfig.enabled);
    logLine(gConfig.enabled ? "main-loop hook installed; integrated weather wind ready" : "disabled in FlowingHair.ini", false);
}

