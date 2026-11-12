#pragma once

#include "ClothSolver.h"

namespace flowing_hair
{
    struct WindSettings
    {
        bool enabled;
        float strength;
        float gustStrength;
        float gustFrequency;
        float directionDegrees;
        float directionVariationDegrees;
        float verticalStrength;
        float spatialScale;

        WindSettings();
    };

    Vec3 sampleWind(
        const WindSettings& settings,
        double timeSeconds,
        float worldX,
        float worldZ);

    // Relative airflow persists at constant speed; acceleration-only inertia does not.
    Vec3 sampleMovementAirflow(const Vec3& velocity, float drag, float maximumAcceleration);
}
