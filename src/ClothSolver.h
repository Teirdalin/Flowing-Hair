#pragma once

#include <vector>

namespace flowing_hair
{
    struct Vec3
    {
        float x;
        float y;
        float z;

        Vec3();
        Vec3(float xValue, float yValue, float zValue);

        Vec3 operator+(const Vec3& rhs) const;
        Vec3 operator-(const Vec3& rhs) const;
        Vec3 operator*(float scalar) const;
        Vec3 operator/(float scalar) const;
        Vec3& operator+=(const Vec3& rhs);
        Vec3& operator-=(const Vec3& rhs);
    };

    float dot(const Vec3& lhs, const Vec3& rhs);
    float lengthSquared(const Vec3& value);
    float length(const Vec3& value);

    struct SolverSettings
    {
        float fixedTimeStep;
        float damping;
        float structuralStiffness;
        float bendingStiffness;
        float shapeStiffness;
        float maximumTargetDistance;
        int constraintIterations;
        int maximumSubsteps;
        float maximumFrameTime;

        SolverSettings();
    };

    struct Ellipsoid
    {
        Vec3 center;
        Vec3 radii;
        bool enabled;

        Ellipsoid();
    };

    struct PinSelection
    {
        Vec3 headCenter;
        Vec3 headRadii;
        float scalpShell;
        float minimumScalpHeight;
        float crownBand;

        PinSelection();
    };

    class ClothSolver
    {
    public:
        ClothSolver();

        bool initialise(
            const std::vector<Vec3>& positions,
            const std::vector<unsigned int>& triangleIndices,
            const std::vector<bool>& pinned,
            const SolverSettings& settings);

        void reset(const std::vector<Vec3>& positions);

        void advance(
            float frameTime,
            const std::vector<Vec3>& pinTargets,
            const Vec3& externalAcceleration,
            const Ellipsoid& collider);

        const std::vector<Vec3>& positions() const;
        const std::vector<bool>& pinned() const;
        bool empty() const;

        // Live shape resistance: 50 preserves the authored solver settings.
        // Does not rebuild topology, move pins, or reset velocity.
        void setRigidity(float percent);
        void setWetness(float amount);

        void setFaceCollider(const Ellipsoid& collider);
        unsigned int faceContacts() const;
        static bool constrainFaceMotion(const Ellipsoid& collider, const Vec3& rest,
            const Vec3& candidate, Vec3& corrected);

        static std::vector<bool> selectPins(
            const std::vector<Vec3>& bindPositions,
            const PinSelection& selection);

        static std::vector<bool> selectBeardPins(const std::vector<Vec3>& bindPositions,
            const std::vector<unsigned int>& triangleIndices, float rootFraction = 0.04f);
        static bool buildBeardAttachments(const std::vector<Vec3>& positions,
            const std::vector<unsigned int>& triangles, float rootFraction,
            std::vector<bool>& pins, std::vector<unsigned int>& links);
        void addAttachmentLinks(const std::vector<unsigned int>& links);

        // Keep existing scalp pins; join wholly loose sections to higher sections
        // instead of fixing another root band in bind space. Failure is atomic.
        static bool buildScalpAttachmentLinks(const std::vector<Vec3>& positions,
            const std::vector<unsigned int>& triangles, const std::vector<bool>& pins,
            std::vector<unsigned int>& links);

        static unsigned int ensureComponentPins(
            const std::vector<Vec3>& bindPositions,
            const std::vector<unsigned int>& triangleIndices,
            unsigned int minimumPinsPerComponent,
            std::vector<bool>& pinned,
            bool stabilizeWhollyUnanchoredComponents = false,
            unsigned int* unanchoredComponentCount = 0,
            unsigned int* rootBandPinsAdded = 0);

    private:
        struct Particle
        {
            Vec3 position;
            Vec3 previousPosition;
            bool isPinned;
        };

        struct Constraint
        {
            unsigned int first;
            unsigned int second;
            float restLength;
            float stiffness;
        };

        SolverSettings mSettings;
        std::vector<Particle> mParticles;
        std::vector<Vec3> mRestPositions;
        std::vector<Vec3> mOutputPositions;
        std::vector<bool> mPinned;
        std::vector<Constraint> mConstraints;
        float mAccumulator;
        Ellipsoid mFaceCollider;
        unsigned int mFaceContacts;
        float mRigidityScale;
        float mWetness;

        void addConstraint(unsigned int first, unsigned int second, float stiffness);
        void substep(
            float timeStep,
            const std::vector<Vec3>& pinTargets,
            const Vec3& acceleration,
            const Ellipsoid& collider);
        void applyPins(const std::vector<Vec3>& pinTargets);
        void solveShape(const std::vector<Vec3>& shapeTargets);
        void solveConstraints();
        void solveCollision(const Ellipsoid& collider, const std::vector<Vec3>& shapeTargets);
        void solveFaceCollision(const std::vector<Vec3>& shapeTargets);
        void limitDistanceFromTargets(const std::vector<Vec3>& shapeTargets);
        void updateOutput();
    };
}
