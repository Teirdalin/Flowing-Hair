#pragma once
#include "ClothSolver.h"
#include <cfloat>
#include <algorithm>
#include <vector>

namespace flowing_hair {
    struct MainViewDrawState {
        unsigned long long lastDraw;
        MainViewDrawState() : lastDraw(0) {}
        void observe(bool mainView,unsigned long long frame) { if (mainView) lastDraw=frame; }
        bool recent(unsigned long long frame) const
        { return lastDraw!=0 && frame>=lastDraw && frame-lastDraw<=2; }
    };
    inline bool hairVisibleInView(bool characterVisible,bool bodyVisible,unsigned int bodyFlags,unsigned int viewMask)
    { return characterVisible && bodyVisible && (bodyFlags & viewMask)!=0; }
    inline bool finiteDistanceValue(float value) { return value>=-FLT_MAX && value<=FLT_MAX; }
    inline float clampHairDistance(float value)
    { return finiteDistanceValue(value) ? std::max(0.0f,std::min(2000.0f,value)) : 500.0f; }
    inline unsigned int clampHairCharacterLimit(float value)
    { return finiteDistanceValue(value) ? static_cast<unsigned int>(std::max(1.0f,std::min(100.0f,value))+0.5f) : 10; }
    struct CameraPriority {
        size_t index,stableId;
        double distanceSquared;
        CameraPriority(size_t i,size_t id,double d) : index(i),stableId(id),distanceSquared(d) {}
        bool operator<(const CameraPriority& other) const
        { return distanceSquared==other.distanceSquared ? stableId<other.stableId : distanceSquared<other.distanceSquared; }
    };
    inline double cameraDistanceSquared(const Vec3& camera,const Vec3& actor)
    {
        if (!finiteDistanceValue(camera.x) || !finiteDistanceValue(camera.y) || !finiteDistanceValue(camera.z) ||
            !finiteDistanceValue(actor.x) || !finiteDistanceValue(actor.y) || !finiteDistanceValue(actor.z)) return DBL_MAX;
        const double x=static_cast<double>(actor.x)-camera.x,y=static_cast<double>(actor.y)-camera.y,z=static_cast<double>(actor.z)-camera.z;
        return x*x+y*y+z*z;
    }
    struct HairCharacterBudget {
        unsigned int limit,used;
        explicit HairCharacterBudget(float configured) : limit(clampHairCharacterLimit(configured)),used(0) {}
        bool admit() { if (used>=limit) return false; ++used; return true; }
        void release() { if (used) --used; }
    };
    inline bool hairWithinCameraRange(const Vec3& camera,const Vec3& actor,float distance,bool active)
    {
        if (!finiteDistanceValue(camera.x) || !finiteDistanceValue(camera.y) || !finiteDistanceValue(camera.z) ||
            !finiteDistanceValue(actor.x) || !finiteDistanceValue(actor.y) || !finiteDistanceValue(actor.z)) return false;
        const double limit=clampHairDistance(distance)*(active ? 1.0 : 0.9);
        if (limit<=0) return false;
        const double x=static_cast<double>(actor.x)-camera.x;
        const double y=static_cast<double>(actor.y)-camera.y;
        const double z=static_cast<double>(actor.z)-camera.z;
        return x*x+y*y+z*z<=limit*limit;
    }
}
