#define BOOST_ALL_NO_LIB 1
#include <windows.h>
class Weather;
class WeatherRegion;
class Season;
#include <kenshi/Weather.h>
#include <kenshi/Character.h>
#include "NativeWeather.h"

namespace flowing_hair {
    // C++ try/catch is not an access-violation guard. Keep these SEH boundaries
    // narrow, with only pointers, references and primitive locals inside.
    int readCameraWeather(float* directionXYZ,float* speed)
    {
        __try {
            WeatherSystem* system=WeatherSystem::getInstance();
            if (!system || !system->ActiveRegionWeather) return 0;
            WeatherInstance* instance=system->ActiveRegionWeather->getWeatherInstance();
            if (!instance || instance->getHasEnded()) return 0;
            const Ogre::Vector3& direction=instance->getWindDirection();
            directionXYZ[0]=direction.x; directionXYZ[1]=direction.y; directionXYZ[2]=direction.z;
            *speed=instance->getWindSpeed();
            return 1;
        } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return -1; }
    }
    int readRainAt(Character* character,const Ogre::Vector3* position,float* exposure,bool* swimming)
    {
        __try {
            if (!character) return 0;
            *exposure=0;
            *swimming=character->getWaterLevel()==WaterState::DEEP_WATER;
            if (*swimming) return 1; // immersion also soaks sheltered characters
            if (!character->_NV_isIndoors().isNull()) return 1;
            WeatherSystem* system=WeatherSystem::getInstance();
            if (system)
            {
                const std::vector<std::pair<EffectType::Enum,float>,Ogre::STLAllocator<std::pair<EffectType::Enum,float>,Ogre::GeneralAllocPolicy> >& effects=
                    system->getPositionGlobalEffects(*position);
                if (effects.size()>64) return 0;
                for (size_t i=0;i<effects.size();++i)
                    if ((effects[i].first==EffectType::CAMERA_RAIN || effects[i].first==EffectType::CAMERA_ACID_RAIN) &&
                        effects[i].second>*exposure) *exposure=effects[i].second;
            }
            // Character effects cover rain implementations that don't publish
            // a camera-rain entry. Never infer wetness from dust, gas or burning.
            const WeatherAffecting type=character->getCurrentWeatherAffectStatus();
            if (type==WA_RAIN || type==WA_ACID)
            {
                const float strength=character->getCurrentWeatherAffectStrength();
                if (strength>*exposure) *exposure=strength;
            }
            return 1;
        } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return -1; }
    }
    int readCharacterWetExposure(Character* character,float x,float y,float z,float* exposure,bool* swimming)
    {
        const Ogre::Vector3 position(x,y,z);
        return readRainAt(character,&position,exposure,swimming);
    }
}
