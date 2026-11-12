#pragma once
#include "ClothSolver.h"
#include <algorithm>
#include <cmath>
#include <cfloat>

namespace flowing_hair {
    inline bool weatherFinite(float f) { return f >= -FLT_MAX && f <= FLT_MAX; }
    inline float unitWeather(float f) { return weatherFinite(f) ? std::max(0.0f,std::min(1.0f,f)) : 0.0f; }
    inline Vec3 sampleWeatherWind(const Vec3& direction,float speed,double time,
        float multiplier,float gustFraction,float frequency,float maximum)
    {
        if (!weatherFinite(speed) || speed<=0 || !weatherFinite(direction.x) ||
            !weatherFinite(direction.z) || !weatherFinite(multiplier) || multiplier<=0 ||
            !weatherFinite(maximum) || maximum<=0 || !(time>=0 && time<1.0e12)) return Vec3();
        const double length=std::sqrt(static_cast<double>(direction.x)*direction.x+
            static_cast<double>(direction.z)*direction.z);
        if (length<0.000001) return Vec3();
        if (!weatherFinite(frequency)) frequency=0.04f;
        const double phase=time*std::max(0.001f,std::min(1.0f,frequency))*6.283185307179586;
        const double modulation=1.0+unitWeather(gustFraction)*(0.7*std::sin(phase)+0.3*std::sin(phase*2.17+1.3));
        const double magnitude=std::min(static_cast<double>(maximum),static_cast<double>(speed)*multiplier*modulation);
        return Vec3(static_cast<float>(direction.x/length*magnitude),0,static_cast<float>(direction.z/length*magnitude));
    }
    inline float advanceHairWetness(float wet,float rain,float dt,float soakSeconds,float drySeconds,bool swimming=false)
    {
        if (swimming) return 1.0f;
        wet=unitWeather(wet); rain=unitWeather(rain);
        if (!weatherFinite(dt) || dt<=0) return wet;
        dt=std::min(dt,0.25f); // never instantly soak/dry after a stall
        if (!weatherFinite(soakSeconds) || soakSeconds<1) soakSeconds=20;
        if (!weatherFinite(drySeconds) || drySeconds<1) drySeconds=120;
        return unitWeather(wet+(rain>0 ? rain*dt/soakSeconds : -dt/drySeconds));
    }
    inline float wetHairWeight(float wet,float extraWeight)
    {
        if (!weatherFinite(extraWeight)) extraWeight=0.75f;
        return 1.0f+unitWeather(wet)*std::max(0.0f,std::min(2.0f,extraWeight));
    }
}
