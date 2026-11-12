#pragma once
#include "ClothSolver.h"
#include <string>
class GameWorld;
namespace flowing_hair
{
    void initialiseIntegratedWind(const std::string& path,bool enabled);
    void updateIntegratedWind(GameWorld* world,float dt);
    Vec3 sampleIntegratedWind(float x,float z);
}
