#include "WindField.h"

#include <algorithm>
#include <cmath>
#include <cfloat>

namespace flowing_hair
{
    namespace
    {
        const double PI = 3.14159265358979323846;

        float clamp01(float value)
        {
            return std::max(0.0f, std::min(1.0f, value));
        }
    }

    Vec3 sampleMovementAirflow(const Vec3& velocity, float drag, float maximumAcceleration)
    {
        if (!(drag > 0 && drag <= FLT_MAX && maximumAcceleration > 0 && maximumAcceleration <= FLT_MAX) ||
            !(velocity.x >= -FLT_MAX && velocity.x <= FLT_MAX &&
              velocity.y >= -FLT_MAX && velocity.y <= FLT_MAX &&
              velocity.z >= -FLT_MAX && velocity.z <= FLT_MAX)) return Vec3();
        const double speed = std::sqrt(static_cast<double>(velocity.x) * velocity.x +
            static_cast<double>(velocity.y) * velocity.y + static_cast<double>(velocity.z) * velocity.z);
        if (speed < 0.000001) return Vec3();
        const double scale = -std::min(static_cast<double>(drag), maximumAcceleration / speed);
        return Vec3(static_cast<float>(velocity.x * scale), static_cast<float>(velocity.y * scale),
            static_cast<float>(velocity.z * scale));
    }

    WindSettings::WindSettings()
        : enabled(true), strength(3.0f), gustStrength(3.8f), gustFrequency(0.11f),
          directionDegrees(35.0f), directionVariationDegrees(55.0f),
          verticalStrength(0.5f), spatialScale(0.012f)
    {
    }

    Vec3 sampleWind(
        const WindSettings& settings,
        double timeSeconds,
        float worldX,
        float worldZ)
    {
        if (!settings.enabled) return Vec3();

        const float frequency = std::max(0.001f, settings.gustFrequency);
        const double phase = timeSeconds * frequency * (2.0 * PI);
        const double spatialPhase = (worldX + worldZ * 0.73f) * settings.spatialScale;
        const float directionOffset = static_cast<float>(std::sin(phase * 0.37 + spatialPhase)) *
            settings.directionVariationDegrees;
        const double direction = (settings.directionDegrees + directionOffset) * (PI / 180.0);

        const float gust = clamp01(0.5f +
            static_cast<float>(std::sin(phase + spatialPhase * 0.7)) * 0.32f +
            static_cast<float>(std::sin(phase * 2.17 + 1.3 + spatialPhase * 1.7)) * 0.18f);
        const float magnitude = std::max(0.0f, settings.strength) +
            std::max(0.0f, settings.gustStrength) * gust;
        const float vertical = settings.verticalStrength * (
            static_cast<float>(std::sin(phase * 1.41 + spatialPhase)) * 0.55f +
            static_cast<float>(std::sin(phase * 0.63 + 2.1 + spatialPhase * 0.4)) * 0.45f);

        return Vec3(static_cast<float>(std::cos(direction)) * magnitude, vertical,
            static_cast<float>(std::sin(direction)) * magnitude);
    }
}
