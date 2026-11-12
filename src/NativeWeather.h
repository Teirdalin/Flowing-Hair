#pragma once
class Character;
namespace flowing_hair {
    // Short main-thread reads. No engine pointers retained. 1=valid, 0=unavailable,
    // -1=native access fault. Outputs must be treated as invalid unless 1.
    int readCameraWeather(float* directionXYZ,float* speed);
    int readCharacterWetExposure(Character* character,float x,float y,float z,float* exposure,bool* swimming);
}
