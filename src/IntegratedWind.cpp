#define BOOST_ALL_NO_LIB 1
#include "IntegratedWind.h"
#include <windows.h>
#include <Debug.h>
#include <core/Functions.h>
#include <ogre/OgreVector3.h>
#include <kenshi/GameWorld.h>
#include "WindField.h"
#include "HairCandidate.h"
#include "NativeWeather.h"
#include "WeatherDynamics.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace {
    std::string gConfigPath;
    bool gEnabled=true, gParticles=true;
    volatile bool gInternalDebug=false;
    flowing_hair::WindSettings gSettings;
    float gParticleScale=1.0f;
    bool gFollowWeather=true;
    float gWeatherMultiplier=2.0f,gWeatherGust=0.25f,gWeatherMaximum=60.0f;
    SRWLOCK gSampleLock=SRWLOCK_INIT;
    flowing_hair::Vec3 gWeatherSample;
    bool gWeatherReady=false;
    double gWeatherTime=0;
    void weatherMainLoop(GameWorld* world,float dt)
    {
        flowing_hair::Vec3 sample;
        bool ready=false;
        if (world && world->initialized && !world->gameResetting)
        {
            if (!world->paused && flowing_hair::weatherFinite(dt) && dt>0)
                gWeatherTime+=std::min(dt,0.25f);
            float direction[3]={0},speed=0;
            int result=0;
            try { result=flowing_hair::readCameraWeather(direction,&speed); }
            catch (...) { result=-1; }
            ready=result==1 && flowing_hair::weatherFinite(speed) && speed>=0 && speed<=1000 &&
                flowing_hair::weatherFinite(direction[0]) && flowing_hair::weatherFinite(direction[2]);
            if (ready) sample=flowing_hair::sampleWeatherWind(flowing_hair::Vec3(direction[0],0,direction[2]),
                speed,gWeatherTime,gWeatherMultiplier,gWeatherGust,gSettings.gustFrequency,gWeatherMaximum);
            if (result<0) { static bool logged=false; if(!logged){ErrorLog("[Flowing Hair / Wind] Native weather read failed; wind remains inactive until a valid sample");logged=true;} }
        }
        else gWeatherTime=0;
        AcquireSRWLockExclusive(&gSampleLock);
        if (ready && gWeatherReady)
        {
            const float blend=(world && !world->paused && flowing_hair::weatherFinite(dt)) ?
                std::max(0.0f,std::min(1.0f,dt*2.0f)) : 0.0f;
            sample=gWeatherSample+(sample-gWeatherSample)*blend;
        }
        gWeatherSample=sample; gWeatherReady=ready;
        ReleaseSRWLockExclusive(&gSampleLock);
    }
    typedef const std::string& (*NameFn)(void*);
    typedef size_t (*CountFn)(void*);
    typedef void* (*IndexFn)(void*,size_t);
    typedef void* (*CreateFn)(void*,const std::string&);
    typedef void (*SetNameFn)(void*,const std::string&);
    typedef void (*SetForceFn)(void*,const Ogre::Vector3&);
    typedef void (*SetModeFn)(void*,int);
    NameFn templateName=0, affectorName=0;
    CountFn techniqueCount=0,affectorCount=0;
    IndexFn techniqueAt=0,affectorAt=0;
    CreateFn createAffector=0;
    SetNameFn setName=0;
    SetForceFn setForce=0;
    SetModeFn setMode=0;
    void (*originalUpdate)(void*,float)=0;
    const std::string marker="FlowingWind_Force_v1";
    const std::string& configPath() { return gConfigPath; }
    float configFloat(const char* name,float fallback,float low,float high)
    {
        char value[64]={0};
        GetPrivateProfileStringA("FlowingWind",name,"",value,64,configPath().c_str());
        const float f=*value ? static_cast<float>(atof(value)) : fallback;
        return f==f ? std::max(low,std::min(high,f)) : fallback;
    }
    FARPROC symbol(HMODULE module,const char* prefix,const char* suffix="")
    {
        // Read only the loaded module's export table; never guessed object offsets.
        const unsigned char* base=reinterpret_cast<const unsigned char*>(module);
        const IMAGE_DOS_HEADER* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* nt=reinterpret_cast<const IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
        DWORD rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if(!rva)return 0;
        const IMAGE_EXPORT_DIRECTORY* exports=reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base+rva);
        const DWORD* names=reinterpret_cast<const DWORD*>(base+exports->AddressOfNames);
        FARPROC result=0;
        for(DWORD i=0;i<exports->NumberOfNames;++i)
        {
            const char* name=reinterpret_cast<const char*>(base+names[i]);
            size_t n=strlen(name),tail=strlen(suffix);
            if(strncmp(name,prefix,strlen(prefix)) || n<tail || strcmp(name+n-tail,suffix))continue;
            if(result)return 0; // ambiguous overload is unsafe
            result=GetProcAddress(module,name);
        }
        return result;
    }
    flowing_hair::Vec3 sample(float x,float z)
    {
        if (!gEnabled) return flowing_hair::Vec3();
        if (!gFollowWeather) return flowing_hair::sampleWind(gSettings,GetTickCount64()*0.001,x,z);
        AcquireSRWLockShared(&gSampleLock);
        const flowing_hair::Vec3 result=gWeatherReady ? gWeatherSample : flowing_hair::Vec3();
        ReleaseSRWLockShared(&gSampleLock);
        return result;
    }
    void updateParticles(void* system,float dt)
    {
        // Native system/technique owns the affector. Reacquire on each callback;
        // no cached particle or effect pointers survive scene teardown.
        if(gEnabled && gParticles && dt>0 && dt<1.0f)
        try {
            const std::string& name=templateName(system);
            const bool rain=flowing_hair::nameContains(name,"rain");
            const bool dust=flowing_hair::nameContains(name,"dust") || flowing_hair::nameContains(name,"sandstorm");
            if(rain || dust)
            {
                flowing_hair::Vec3 w=sample(0,0);
                Ogre::Vector3 force(w.x*gParticleScale,0,w.z*gParticleScale);
                const size_t count=std::min(techniqueCount(system),static_cast<size_t>(16));
                for(size_t t=0;t<count;++t)
                {
                    void* technique=techniqueAt(system,t); if(!technique)continue;
                    void* own=0; const size_t affectors=affectorCount(technique);
                    if(affectors>64)continue;
                    for(size_t a=0;a<affectors;++a)
                    {
                        void* affector=affectorAt(technique,a);
                        if(affector && affectorName(affector)==marker){own=affector;break;}
                    }
                    if(!own)
                    {
                        own=createAffector(technique,std::string("LinearForce"));
                        if(!own)continue;
                        setName(own,marker); setMode(own,1); // ParticleUniverse FA_ADD
                        if(gInternalDebug)DebugLog("[Flowing Hair / Wind] Attached force to "+name);
                    }
                    setForce(own,force);
                }
            }
        } catch (...) { static bool logged=false; if(!logged){ErrorLog("[Flowing Hair / Wind] Particle wind update failed");logged=true;} }
        originalUpdate(system,dt); // exactly once; no raw particle-field writes
    }
}

namespace flowing_hair {
Vec3 sampleIntegratedWind(float x,float z) { return sample(x,z); }
void updateIntegratedWind(GameWorld* world,float dt)
{
    if(gEnabled && gFollowWeather) weatherMainLoop(world,dt);
}
void initialiseIntegratedWind(const std::string& path,bool enabled)
{
    gConfigPath=path;
    gEnabled=enabled && GetPrivateProfileIntA("FlowingWind","Enabled",1,configPath().c_str())!=0;
    gParticles=GetPrivateProfileIntA("FlowingWind","RainAndDust",1,configPath().c_str())!=0;
    gFollowWeather=GetPrivateProfileIntA("FlowingWind","FollowWeather",1,configPath().c_str())!=0;
    gWeatherMultiplier=configFloat("WeatherWindMultiplier",2,0,10);
    gWeatherGust=configFloat("WeatherGustFraction",0.25f,0,1);
    gWeatherMaximum=configFloat("MaximumWeatherWind",60,0,200);
    gSettings.strength=configFloat("BaseWind",15,0,100);
    gSettings.gustStrength=configFloat("GustWind",45,0,150);
    gSettings.gustFrequency=configFloat("GustFrequency",0.04f,0.005f,1);
    gSettings.directionDegrees=configFloat("DirectionDegrees",35,-360,360);
    gSettings.directionVariationDegrees=configFloat("DirectionVariation",75,0,180);
    // Shared global wind: rain, dust and hair agree regardless of emitter location.
    gSettings.spatialScale=0;
    gParticleScale=configFloat("ParticleWindScale",1,0,10);
    if(!gEnabled || !gParticles)return;
    HMODULE pu=GetModuleHandleA("Plugin_ParticleUniverse_x64.dll");
    if(!pu){ErrorLog("[Flowing Hair / Wind] ParticleUniverse unavailable; particle wind inactive");return;}
    templateName=reinterpret_cast<NameFn>(symbol(pu,"?getTemplateName@ParticleSystem@ParticleUniverse@@"));
    techniqueCount=reinterpret_cast<CountFn>(symbol(pu,"?getNumTechniques@ParticleSystem@ParticleUniverse@@"));
    techniqueAt=reinterpret_cast<IndexFn>(symbol(pu,"?getTechnique@ParticleSystem@ParticleUniverse@@","_K@Z"));
    affectorCount=reinterpret_cast<CountFn>(symbol(pu,"?getNumAffectors@ParticleTechnique@ParticleUniverse@@"));
    affectorAt=reinterpret_cast<IndexFn>(symbol(pu,"?getAffector@ParticleTechnique@ParticleUniverse@@","_K@Z"));
    affectorName=reinterpret_cast<NameFn>(symbol(pu,"?getName@ParticleAffector@ParticleUniverse@@"));
    createAffector=reinterpret_cast<CreateFn>(symbol(pu,"?createAffector@ParticleTechnique@ParticleUniverse@@"));
    setName=reinterpret_cast<SetNameFn>(symbol(pu,"?setName@ParticleAffector@ParticleUniverse@@"));
    setForce=reinterpret_cast<SetForceFn>(symbol(pu,"?setForceVector@BaseForceAffector@ParticleUniverse@@"));
    setMode=reinterpret_cast<SetModeFn>(symbol(pu,"?setForceApplication@BaseForceAffector@ParticleUniverse@@"));
    FARPROC update=symbol(pu,"?_update@ParticleSystem@ParticleUniverse@@");
    if(!templateName||!techniqueCount||!techniqueAt||!affectorCount||!affectorAt||!affectorName||!createAffector||!setName||!setForce||!setMode||!update)
    {ErrorLog("[Flowing Hair / Wind] Particle ABI exports unavailable; particle wind inactive");return;}
    if(KenshiLib::SUCCESS!=KenshiLib::AddHook(reinterpret_cast<void*>(update),&updateParticles,&originalUpdate))
        ErrorLog("[Flowing Hair / Wind] Particle update hook failed");
}

}

