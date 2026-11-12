#include "ClothSolver.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace flowing_hair
{
    namespace
    {
        const float kEpsilon = 0.000001f;

        typedef std::pair<unsigned int, unsigned int> Edge;

        Edge makeEdge(unsigned int first, unsigned int second)
        {
            return first < second ? Edge(first, second) : Edge(second, first);
        }

        float clampFloat(float value, float minimum, float maximum)
        {
            return std::max(minimum, std::min(maximum, value));
        }

        bool finiteFloat(float value)
        {
            return value == value && value >= -FLT_MAX && value <= FLT_MAX;
        }

        bool finiteVector(const Vec3& value)
        {
            return finiteFloat(value.x) && finiteFloat(value.y) && finiteFloat(value.z);
        }

        unsigned int findRoot(std::vector<unsigned int>& parents, unsigned int index)
        {
            unsigned int root = index;
            while (parents[root] != root) root = parents[root];
            while (parents[index] != index)
            {
                const unsigned int next = parents[index];
                parents[index] = root;
                index = next;
            }
            return root;
        }

        void joinComponents(std::vector<unsigned int>& parents, unsigned int first, unsigned int second)
        {
            const unsigned int firstRoot = findRoot(parents, first);
            const unsigned int secondRoot = findRoot(parents, second);
            if (firstRoot != secondRoot) parents[secondRoot] = firstRoot;
        }

        bool containsIndex(const std::vector<unsigned int>& values, unsigned int value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        float distanceFromLineSquared(const Vec3& point, const Vec3& first, const Vec3& second)
        {
            const Vec3 line = second - first;
            const float lineLengthSquared = lengthSquared(line);
            if (lineLengthSquared <= kEpsilon * kEpsilon)
                return lengthSquared(point - first);
            const Vec3 relative = point - first;
            const float projection = dot(relative, line) / lineLengthSquared;
            return lengthSquared(relative - line * projection);
        }

        unsigned int farthestFromAnchors(
            const std::vector<Vec3>& positions,
            const std::vector<unsigned int>& candidates,
            const std::vector<unsigned int>& anchors)
        {
            unsigned int best = static_cast<unsigned int>(positions.size());
            float bestDistance = -1.0f;
            for (unsigned int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex)
            {
                const unsigned int candidate = candidates[candidateIndex];
                if (containsIndex(anchors, candidate)) continue;
                float nearestDistance = FLT_MAX;
                for (unsigned int anchorIndex = 0; anchorIndex < anchors.size(); ++anchorIndex)
                {
                    nearestDistance = std::min(nearestDistance,
                        lengthSquared(positions[candidate] - positions[anchors[anchorIndex]]));
                }
                if (best == positions.size() || nearestDistance > bestDistance)
                {
                    best = candidate;
                    bestDistance = nearestDistance;
                }
            }
            return best;
        }

        unsigned int pinStableRootBand(
            const std::vector<Vec3>& positions,
            const std::vector<unsigned int>& vertices,
            unsigned int minimumPins,
            std::vector<bool>& pinnedValues)
        {
            if (vertices.empty()) return 0;
            const unsigned int desired = std::min(
                std::max(minimumPins, 6u), static_cast<unsigned int>(vertices.size()));

            Vec3 low = positions[vertices[0]];
            Vec3 high = low;
            unsigned int vertexIndex = 0;
            for (vertexIndex = 1; vertexIndex < vertices.size(); ++vertexIndex)
            {
                const Vec3& position = positions[vertices[vertexIndex]];
                low.x = std::min(low.x, position.x);
                low.y = std::min(low.y, position.y);
                low.z = std::min(low.z, position.z);
                high.x = std::max(high.x, position.x);
                high.y = std::max(high.y, position.y);
                high.z = std::max(high.z, position.z);
            }

            // Start with only the upper 15 percent. Sparse cards expand downward by
            // whole vertex height order just far enough to obtain six stable roots.
            const float rootFloor = high.y - (high.y - low.y) * 0.15f;
            std::vector<unsigned int> candidates;
            for (vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
            {
                const unsigned int candidate = vertices[vertexIndex];
                if (positions[candidate].y >= rootFloor)
                    candidates.push_back(candidate);
            }

            // Coarse cards may not have enough vertices in the fractional band.
            // Expand only as far downward as needed, always taking the next-highest
            // vertex so the inferred root remains local to the top of the card.
            while (candidates.size() < desired)
            {
                unsigned int best = static_cast<unsigned int>(positions.size());
                for (vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
                {
                    const unsigned int candidate = vertices[vertexIndex];
                    if (containsIndex(candidates, candidate)) continue;
                    if (best == positions.size() || positions[candidate].y > positions[best].y)
                        best = candidate;
                }
                if (best == positions.size()) break;
                candidates.push_back(best);
            }

            std::vector<unsigned int> anchors;
            unsigned int highest = candidates[0];
            for (vertexIndex = 1; vertexIndex < candidates.size(); ++vertexIndex)
            {
                const unsigned int candidate = candidates[vertexIndex];
                if (positions[candidate].y > positions[highest].y)
                    highest = candidate;
            }
            anchors.push_back(highest);

            if (anchors.size() < desired)
            {
                const unsigned int second = farthestFromAnchors(positions, candidates, anchors);
                if (second != positions.size()) anchors.push_back(second);
            }

            if (anchors.size() < desired && anchors.size() >= 2)
            {
                const float componentSpanSquared = lengthSquared(high - low);
                const float nonCollinearTolerance = std::max(
                    kEpsilon * kEpsilon, componentSpanSquared * 0.000001f);
                unsigned int third = static_cast<unsigned int>(positions.size());
                float bestDistance = -1.0f;
                for (vertexIndex = 0; vertexIndex < candidates.size(); ++vertexIndex)
                {
                    const unsigned int candidate = candidates[vertexIndex];
                    if (containsIndex(anchors, candidate)) continue;
                    const float distance = distanceFromLineSquared(positions[candidate],
                        positions[anchors[0]], positions[anchors[1]]);
                    if (third == positions.size() || distance > bestDistance)
                    {
                        third = candidate;
                        bestDistance = distance;
                    }
                }

                // If the top row is a straight hinge, take the highest available
                // off-axis vertex from the next row rather than anchoring that axis.
                if (third == positions.size() || bestDistance <= nonCollinearTolerance)
                {
                    unsigned int expandedThird = static_cast<unsigned int>(positions.size());
                    float expandedDistance = -1.0f;
                    for (vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex)
                    {
                        const unsigned int candidate = vertices[vertexIndex];
                        if (containsIndex(anchors, candidate)) continue;
                        const float distance = distanceFromLineSquared(positions[candidate],
                            positions[anchors[0]], positions[anchors[1]]);
                        if (distance <= nonCollinearTolerance) continue;
                        if (expandedThird == positions.size() ||
                            positions[candidate].y > positions[expandedThird].y ||
                            (positions[candidate].y == positions[expandedThird].y &&
                             distance > expandedDistance))
                        {
                            expandedThird = candidate;
                            expandedDistance = distance;
                        }
                    }
                    if (expandedThird != positions.size()) third = expandedThird;
                }
                if (third != positions.size()) anchors.push_back(third);
            }

            while (anchors.size() < desired)
            {
                unsigned int next = farthestFromAnchors(positions, candidates, anchors);
                if (next == positions.size())
                    next = farthestFromAnchors(positions, vertices, anchors);
                if (next == positions.size()) break;
                anchors.push_back(next);
            }

            unsigned int added = 0;
            for (vertexIndex = 0; vertexIndex < anchors.size(); ++vertexIndex)
            {
                const unsigned int anchor = anchors[vertexIndex];
                if (pinnedValues[anchor]) continue;
                pinnedValues[anchor] = true;
                ++added;
            }
            return added;
        }
    }

    Vec3::Vec3() : x(0.0f), y(0.0f), z(0.0f) {}

    Vec3::Vec3(float xValue, float yValue, float zValue)
        : x(xValue), y(yValue), z(zValue) {}

    Vec3 Vec3::operator+(const Vec3& rhs) const
    {
        return Vec3(x + rhs.x, y + rhs.y, z + rhs.z);
    }

    Vec3 Vec3::operator-(const Vec3& rhs) const
    {
        return Vec3(x - rhs.x, y - rhs.y, z - rhs.z);
    }

    Vec3 Vec3::operator*(float scalar) const
    {
        return Vec3(x * scalar, y * scalar, z * scalar);
    }

    Vec3 Vec3::operator/(float scalar) const
    {
        return Vec3(x / scalar, y / scalar, z / scalar);
    }

    Vec3& Vec3::operator+=(const Vec3& rhs)
    {
        x += rhs.x;
        y += rhs.y;
        z += rhs.z;
        return *this;
    }

    Vec3& Vec3::operator-=(const Vec3& rhs)
    {
        x -= rhs.x;
        y -= rhs.y;
        z -= rhs.z;
        return *this;
    }

    float dot(const Vec3& lhs, const Vec3& rhs)
    {
        return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
    }

    float lengthSquared(const Vec3& value)
    {
        return dot(value, value);
    }

    float length(const Vec3& value)
    {
        return std::sqrt(lengthSquared(value));
    }

    SolverSettings::SolverSettings()
        : fixedTimeStep(1.0f / 120.0f),
          damping(0.02f),
          structuralStiffness(1.0f),
          bendingStiffness(0.25f),
          shapeStiffness(0.018f),
          maximumTargetDistance(0.0f),
          constraintIterations(8),
          maximumSubsteps(4),
          maximumFrameTime(1.0f / 15.0f)
    {
    }

    Ellipsoid::Ellipsoid()
        : center(), radii(1.0f, 1.0f, 1.0f), enabled(false)
    {
    }

    PinSelection::PinSelection()
        : headCenter(),
          headRadii(0.13f, 0.17f, 0.12f),
          scalpShell(0.12f),
          minimumScalpHeight(0.0f),
          crownBand(0.02f)
    {
    }

    ClothSolver::ClothSolver() : mAccumulator(0.0f), mFaceContacts(0), mRigidityScale(1.0f),mWetness(0.0f) {}

    bool ClothSolver::initialise(
        const std::vector<Vec3>& positions,
        const std::vector<unsigned int>& triangleIndices,
        const std::vector<bool>& pinnedValues,
        const SolverSettings& settings)
    {
        mParticles.clear();
        mRestPositions.clear();
        mOutputPositions.clear();
        mPinned.clear();
        mConstraints.clear();
        mAccumulator = 0.0f;
        mRigidityScale = 1.0f;
        mWetness = 0.0f;

        if (positions.empty() || triangleIndices.empty() ||
            triangleIndices.size() % 3 != 0 || pinnedValues.size() != positions.size())
        {
            return false;
        }

        mSettings = settings;
        unsigned int index = 0;
        if (mSettings.fixedTimeStep <= 0.0f || mSettings.constraintIterations < 1 ||
            mSettings.maximumSubsteps < 1 || mSettings.maximumFrameTime <= 0.0f ||
            !finiteFloat(mSettings.fixedTimeStep) || !finiteFloat(mSettings.damping) ||
            !finiteFloat(mSettings.structuralStiffness) ||
            !finiteFloat(mSettings.bendingStiffness) ||
            !finiteFloat(mSettings.shapeStiffness) ||
            !finiteFloat(mSettings.maximumTargetDistance) ||
            mSettings.maximumTargetDistance < 0.0f ||
            !finiteFloat(mSettings.maximumFrameTime))
        {
            return false;
        }

        for (index = 0; index < positions.size(); ++index)
        {
            if (!finiteVector(positions[index]))
                return false;
        }
        for (index = 0; index < triangleIndices.size(); ++index)
        {
            if (triangleIndices[index] >= positions.size())
                return false;
        }

        mParticles.resize(positions.size());
        mRestPositions = positions;
        mOutputPositions = positions;
        mPinned = pinnedValues;
        for (index = 0; index < positions.size(); ++index)
        {
            mParticles[index].position = positions[index];
            mParticles[index].previousPosition = positions[index];
            mParticles[index].isPinned = pinnedValues[index];
        }

        std::set<Edge> structuralEdges;
        std::map<Edge, unsigned int> firstOppositeVertex;
        std::set<Edge> bendingEdges;

        for (index = 0; index < triangleIndices.size(); index += 3)
        {
            const unsigned int a = triangleIndices[index];
            const unsigned int b = triangleIndices[index + 1];
            const unsigned int c = triangleIndices[index + 2];
            const Edge edges[3] = { makeEdge(a, b), makeEdge(b, c), makeEdge(c, a) };
            const unsigned int opposite[3] = { c, a, b };

            for (int edgeIndex = 0; edgeIndex < 3; ++edgeIndex)
            {
                const Edge& edge = edges[edgeIndex];
                structuralEdges.insert(edge);

                std::map<Edge, unsigned int>::iterator found = firstOppositeVertex.find(edge);
                if (found == firstOppositeVertex.end())
                {
                    firstOppositeVertex.insert(std::make_pair(edge, opposite[edgeIndex]));
                }
                else if (found->second != opposite[edgeIndex])
                {
                    bendingEdges.insert(makeEdge(found->second, opposite[edgeIndex]));
                }
            }
        }

        std::set<Edge>::const_iterator edge = structuralEdges.begin();
        for (; edge != structuralEdges.end(); ++edge)
            addConstraint(edge->first, edge->second, mSettings.structuralStiffness);

        edge = bendingEdges.begin();
        for (; edge != bendingEdges.end(); ++edge)
        {
            if (structuralEdges.find(*edge) == structuralEdges.end())
                addConstraint(edge->first, edge->second, mSettings.bendingStiffness);
        }

        return !mConstraints.empty();
    }

    void ClothSolver::reset(const std::vector<Vec3>& positions)
    {
        if (positions.size() != mParticles.size())
            return;

        for (unsigned int index = 0; index < positions.size(); ++index)
        {
            mParticles[index].position = positions[index];
            mParticles[index].previousPosition = positions[index];
        }
        mRestPositions = positions;
        mAccumulator = 0.0f;
        updateOutput();
    }

    void ClothSolver::advance(
        float frameTime,
        const std::vector<Vec3>& pinTargets,
        const Vec3& externalAcceleration,
        const Ellipsoid& collider)
    {
        if (mParticles.empty() || pinTargets.size() != mParticles.size())
            return;

        if (!finiteFloat(frameTime))
            return;

        const float safeFrameTime = clampFloat(frameTime, 0.0f, mSettings.maximumFrameTime);
        const Vec3 safeAcceleration = finiteVector(externalAcceleration) ? externalAcceleration : Vec3();
        mAccumulator += safeFrameTime;

        int stepCount = 0;
        while (mAccumulator >= mSettings.fixedTimeStep && stepCount < mSettings.maximumSubsteps)
        {
            substep(mSettings.fixedTimeStep, pinTargets, safeAcceleration, collider);
            mAccumulator -= mSettings.fixedTimeStep;
            ++stepCount;
        }

        if (stepCount == mSettings.maximumSubsteps && mAccumulator >= mSettings.fixedTimeStep)
            mAccumulator = 0.0f;

        applyPins(pinTargets);
        updateOutput();
    }

    const std::vector<Vec3>& ClothSolver::positions() const
    {
        return mOutputPositions;
    }

    const std::vector<bool>& ClothSolver::pinned() const
    {
        return mPinned;
    }

    bool ClothSolver::empty() const
    {
        return mParticles.empty();
    }

    void ClothSolver::setFaceCollider(const Ellipsoid& collider)
    {
        mFaceCollider = collider;
        mFaceCollider.enabled = collider.enabled && finiteVector(collider.center) &&
            finiteVector(collider.radii) && collider.radii.x > kEpsilon &&
            collider.radii.y > kEpsilon && collider.radii.z > kEpsilon;
        mFaceContacts = 0;
    }

    unsigned int ClothSolver::faceContacts() const { return mFaceContacts; }

    bool ClothSolver::constrainFaceMotion(const Ellipsoid& collider, const Vec3& rest,
        const Vec3& candidate, Vec3& corrected)
    {
        corrected = candidate;
        if (!collider.enabled || !finiteVector(collider.center) || !finiteVector(collider.radii) ||
            collider.radii.x <= kEpsilon || collider.radii.y <= kEpsilon || collider.radii.z <= kEpsilon ||
            !finiteVector(rest) || !finiteVector(candidate)) return false;
        const Vec3 relative = rest - collider.center, motion = candidate - rest;
        const Vec3 start(relative.x / collider.radii.x, relative.y / collider.radii.y, relative.z / collider.radii.z);
        const Vec3 delta(motion.x / collider.radii.x, motion.y / collider.radii.y, motion.z / collider.radii.z);
        const float startSquared = lengthSquared(start), a = lengthSquared(delta);
        if (!finiteFloat(startSquared) || !finiteFloat(a) || startSquared <= kEpsilon || a <= kEpsilon * kEpsilon)
            return false;
        // Authored interior vertices keep their own depth instead of being ejected.
        // Sweep from the immutable rest target, not a potentially penetrated state.
        // An approximate face cannot classify authored interior hair as contact.
        // Giving each interior point a private radial boundary made virtual pins
        // inside the cheeks, producing caught/folded cards in the v2.8 live test.
        if (startSquared <= 1.0f + kEpsilon) return false;
        const float radiusSquared = 1.0f;
        const float b = dot(start, delta), c = startSquared - radiusSquared;
        const float discriminant = b * b - a * c;
        if (!finiteFloat(discriminant) || discriminant <= 0.0f || b >= 0.0f) return false;
        const float entry = (-b - std::sqrt(discriminant)) / a;
        if (entry < -kEpsilon || entry >= 1.0f) return false;
        corrected = rest + motion * std::max(0.0f, entry);
        return true;
    }

    void ClothSolver::solveFaceCollision(const std::vector<Vec3>& shapeTargets)
    {
        if (!mFaceCollider.enabled || shapeTargets.size() != mParticles.size()) return;
        for (size_t i = 0; i < mParticles.size(); ++i)
        {
            Particle& particle = mParticles[i];
            if (particle.isPinned) continue;
            Vec3 corrected;
            if (!constrainFaceMotion(mFaceCollider, shapeTargets[i], particle.position, corrected)) continue;
            particle.position = corrected;
            particle.previousPosition = corrected; // no velocity injected by contact
            if (mFaceContacts < 0xffffffffu) ++mFaceContacts;
        }
    }

    std::vector<bool> ClothSolver::selectBeardPins(const std::vector<Vec3>& bindPositions,
        const std::vector<unsigned int>& triangleIndices, float rootFraction)
    {
        std::vector<bool> pins(bindPositions.size(), false);
        if (bindPositions.empty()) return pins;
        if (triangleIndices.size() % 3 || !finiteFloat(rootFraction)) return pins;
        std::vector<unsigned int> parents(bindPositions.size());
        for (unsigned int p = 0; p < parents.size(); ++p) parents[p] = p;
        for (size_t t = 0; t < triangleIndices.size(); t += 3)
        {
            const unsigned int a = triangleIndices[t], b = triangleIndices[t + 1], c = triangleIndices[t + 2];
            if (a >= parents.size() || b >= parents.size() || c >= parents.size()) return pins;
            joinComponents(parents, a, b); joinComponents(parents, b, c);
        }
        std::map<unsigned int, std::vector<unsigned int> > components;
        for (unsigned int p = 0; p < parents.size(); ++p) components[findRoot(parents, p)].push_back(p);
        for (std::map<unsigned int, std::vector<unsigned int> >::const_iterator it = components.begin();
            it != components.end(); ++it)
        {
            const std::vector<unsigned int>& vertices = it->second;
            float low = bindPositions[vertices[0]].y, high = low;
            for (size_t p = 1; p < vertices.size(); ++p)
            { low = std::min(low, bindPositions[vertices[p]].y); high = std::max(high, bindPositions[vertices[p]].y); }
            const float floor = high - std::max(kEpsilon, (high - low) * clampFloat(rootFraction, 0.005f, 0.2f));
            // Each disconnected braid/card keeps its own attachment. Never expand
            // down a sparse strand merely to satisfy the scalp's six-root rule.
            for (size_t p = 0; p < vertices.size(); ++p)
                pins[vertices[p]] = bindPositions[vertices[p]].y >= floor;
        }
        return pins;
    }

    bool ClothSolver::buildBeardAttachments(const std::vector<Vec3>& positions,
        const std::vector<unsigned int>& triangles, float rootFraction,
        std::vector<bool>& pins, std::vector<unsigned int>& links)
    {
        pins.assign(positions.size(), false); links.clear();
        if (positions.empty() || positions.size() > 8192 || triangles.size() % 3) return false;
        std::vector<unsigned int> parents(positions.size());
        float low = positions[0].y, high = low;
        for (unsigned int i = 0; i < positions.size(); ++i)
        {
            if (!finiteVector(positions[i])) return false;
            parents[i] = i; low = std::min(low, positions[i].y); high = std::max(high, positions[i].y);
        }
        for (size_t t = 0; t < triangles.size(); t += 3)
        {
            const unsigned int a = triangles[t], b = triangles[t+1], c = triangles[t+2];
            if (a >= positions.size() || b >= positions.size() || c >= positions.size()) return false;
            joinComponents(parents, a, b); joinComponents(parents, b, c);
        }
        std::map<unsigned int, std::vector<unsigned int> > components;
        std::map<unsigned int, float> tops;
        for (unsigned int i = 0; i < positions.size(); ++i)
        {
            parents[i] = findRoot(parents, i);
            if (!tops.count(parents[i])) tops[parents[i]] = positions[i].y;
            tops[parents[i]] = std::max(tops[parents[i]], positions[i].y);
            components[parents[i]].push_back(i);
        }
        if (components.size() > 128) return false;
        // Keep the lip/cheek attachment region intact. The lower hanging length
        // is simulated as connected sections, not separately world-pinned bulbs.
        const float attachmentFloor = high - (high - low) * 0.45f;
        for (size_t i = 0; i < positions.size(); ++i) pins[i] = positions[i].y >= attachmentFloor;
        const float linkLimit = std::max(0.05f, (high-low) * 0.35f);
        for (std::map<unsigned int, std::vector<unsigned int> >::const_iterator it = components.begin(); it != components.end(); ++it)
        {
            const std::vector<unsigned int>& vertices = it->second;
            bool rooted = false;
            float bottom = tops[it->first];
            for (size_t i = 0; i < vertices.size(); ++i)
            { rooted = rooted || pins[vertices[i]]; bottom = std::min(bottom, positions[vertices[i]].y); }
            if (rooted) continue;
            const size_t before = links.size();
            const float rootFloor = tops[it->first] - std::max(kEpsilon,
                (tops[it->first]-bottom) * clampFloat(rootFraction, 0.005f, 0.2f));
            for (size_t i = 0; i < vertices.size() && links.size() - before < 64; ++i)
            {
                const unsigned int a = vertices[i];
                if (positions[a].y < rootFloor) continue;
                unsigned int best = static_cast<unsigned int>(positions.size());
                float distance = linkLimit * linkLimit;
                for (unsigned int b = 0; b < positions.size(); ++b)
                {
                    // Strictly increasing component tops prevents attachment cycles.
                    if (parents[b] == it->first || tops[parents[b]] <= tops[it->first] + kEpsilon ||
                        positions[b].y < positions[a].y - linkLimit * 0.2f) continue;
                    const float d = lengthSquared(positions[b] - positions[a]);
                    if (d < distance) { best = b; distance = d; }
                }
                if (best != positions.size()) { links.push_back(a); links.push_back(best); }
            }
            if (links.size() == before) return false; // retain original, never leave floating geometry
        }
        return true;
    }

    bool ClothSolver::buildScalpAttachmentLinks(const std::vector<Vec3>& positions,
        const std::vector<unsigned int>& triangles, const std::vector<bool>& pins,
        std::vector<unsigned int>& links)
    {
        links.clear();
        if (positions.empty() || positions.size() > 8192 || pins.size() != positions.size() ||
            triangles.empty() || triangles.size() % 3) return false;
        std::vector<unsigned int> parents(positions.size());
        float low = positions[0].y, high = low;
        for (unsigned int i = 0; i < positions.size(); ++i)
        {
            if (!finiteVector(positions[i])) return false;
            parents[i] = i; low = std::min(low, positions[i].y); high = std::max(high, positions[i].y);
        }
        for (size_t t = 0; t < triangles.size(); t += 3)
        {
            const unsigned int a = triangles[t], b = triangles[t+1], c = triangles[t+2];
            if (a >= positions.size() || b >= positions.size() || c >= positions.size()) return false;
            joinComponents(parents,a,b); joinComponents(parents,b,c);
        }
        std::map<unsigned int, std::vector<unsigned int> > components;
        std::map<unsigned int, float> tops;
        bool anchored = false;
        for (unsigned int i = 0; i < positions.size(); ++i)
        {
            parents[i] = findRoot(parents,i);
            if (!tops.count(parents[i])) tops[parents[i]] = positions[i].y;
            tops[parents[i]] = std::max(tops[parents[i]],positions[i].y);
            components[parents[i]].push_back(i); anchored = anchored || pins[i];
        }
        if (!anchored || components.size() > 128) return false;
        const float reach = std::max(0.05f,(high-low)*0.35f);
        std::vector<unsigned int> pending;
        for (std::map<unsigned int, std::vector<unsigned int> >::const_iterator it=components.begin(); it!=components.end(); ++it)
        {
            const std::vector<unsigned int>& vertices=it->second;
            bool rooted=false;
            for (size_t i=0;i<vertices.size();++i) rooted=rooted || pins[vertices[i]];
            if (rooted) continue;
            // Reuse the distributed six-root selection, but only as seam locations.
            // No new fixed pins: the tip must travel with the moving braid above it.
            std::vector<bool> roots(positions.size(),false);
            pinStableRootBand(positions,vertices,3,roots);
            for (size_t i=0;i<vertices.size();++i)
            {
                const unsigned int a=vertices[i];
                if (!roots[a]) continue;
                std::vector<unsigned int> chosen;
                // Multiple parent points avoid turning the entire tip into a
                // freely rotating single-distance hinge. Six roots x three links.
                for (unsigned int n=0;n<3;++n)
                {
                    unsigned int best=static_cast<unsigned int>(positions.size());
                    float nearest=reach*reach;
                    for (unsigned int b=0;b<positions.size();++b)
                    {
                        if (parents[b]==it->first || tops[parents[b]]<=tops[it->first]+kEpsilon ||
                            positions[b].y<positions[a].y-reach*0.2f || containsIndex(chosen,b)) continue;
                        const float distance=lengthSquared(positions[b]-positions[a]);
                        if (distance>kEpsilon*kEpsilon && distance<nearest)
                        {best=b; nearest=distance;}
                    }
                    if (best==positions.size()) return false;
                    chosen.push_back(best); pending.push_back(a); pending.push_back(best);
                }
            }
        }
        links.swap(pending);
        return true;
    }

    void ClothSolver::addAttachmentLinks(const std::vector<unsigned int>& links)
    {
        for (size_t i = 0; i + 1 < links.size(); i += 2)
            if (links[i] < mParticles.size() && links[i+1] < mParticles.size())
                addConstraint(links[i], links[i+1], 1.0f);
    }

    std::vector<bool> ClothSolver::selectPins(
        const std::vector<Vec3>& bindPositions,
        const PinSelection& selection)
    {
        std::vector<bool> result(bindPositions.size(), false);
        if (bindPositions.empty())
            return result;

        float maximumY = bindPositions[0].y;
        for (unsigned int index = 1; index < bindPositions.size(); ++index)
            maximumY = std::max(maximumY, bindPositions[index].y);

        const float radiusX = std::max(selection.headRadii.x, kEpsilon);
        const float radiusY = std::max(selection.headRadii.y, kEpsilon);
        const float radiusZ = std::max(selection.headRadii.z, kEpsilon);
        const float shellLimit = 1.0f + std::max(selection.scalpShell, 0.0f);
        const float shellLimitSquared = shellLimit * shellLimit;

        for (unsigned int index = 0; index < bindPositions.size(); ++index)
        {
            const Vec3 relative = bindPositions[index] - selection.headCenter;
            const float nx = relative.x / radiusX;
            const float ny = relative.y / radiusY;
            const float nz = relative.z / radiusZ;
            const float ellipsoidDistanceSquared = nx * nx + ny * ny + nz * nz;

            const bool onScalp =
                bindPositions[index].y >= selection.minimumScalpHeight &&
                ellipsoidDistanceSquared <= shellLimitSquared;
            const bool inCrownBand =
                bindPositions[index].y >= maximumY - std::max(selection.crownBand, 0.0f);

            result[index] = onScalp || inCrownBand;
        }

        return result;
    }

    unsigned int ClothSolver::ensureComponentPins(
        const std::vector<Vec3>& bindPositions,
        const std::vector<unsigned int>& triangleIndices,
        unsigned int minimumPinsPerComponent,
        std::vector<bool>& pinnedValues,
        bool stabilizeWhollyUnanchoredComponents,
        unsigned int* unanchoredComponentCount,
        unsigned int* rootBandPinsAdded)
    {
        if (unanchoredComponentCount) *unanchoredComponentCount = 0;
        if (rootBandPinsAdded) *rootBandPinsAdded = 0;
        if (bindPositions.empty() || pinnedValues.size() != bindPositions.size() ||
            triangleIndices.size() % 3 != 0 || minimumPinsPerComponent == 0)
            return 0;

        std::vector<unsigned int> parents(bindPositions.size());
        unsigned int index = 0;
        for (index = 0; index < parents.size(); ++index) parents[index] = index;
        for (index = 0; index < triangleIndices.size(); index += 3)
        {
            const unsigned int a = triangleIndices[index];
            const unsigned int b = triangleIndices[index + 1];
            const unsigned int c = triangleIndices[index + 2];
            if (a >= parents.size() || b >= parents.size() || c >= parents.size()) return 0;
            joinComponents(parents, a, b);
            joinComponents(parents, b, c);
        }

        std::map<unsigned int, std::vector<unsigned int> > components;
        for (index = 0; index < parents.size(); ++index)
            components[findRoot(parents, index)].push_back(index);

        unsigned int added = 0;
        std::map<unsigned int, std::vector<unsigned int> >::const_iterator component = components.begin();
        for (; component != components.end(); ++component)
        {
            const std::vector<unsigned int>& vertices = component->second;
            const unsigned int target = std::min(minimumPinsPerComponent,
                static_cast<unsigned int>(vertices.size()));
            unsigned int pinCount = 0;
            for (index = 0; index < vertices.size(); ++index)
                if (pinnedValues[vertices[index]]) ++pinCount;
            if (pinCount == 0 && unanchoredComponentCount)
                ++(*unanchoredComponentCount);

            // A disconnected card with no natural scalp/crown contact still needs a
            // stable root. Select a small, spatially distributed upper band instead
            // of freezing the whole component or creating a two-point hinge axis.
            if (stabilizeWhollyUnanchoredComponents && pinCount == 0)
            {
                const unsigned int rootPins = pinStableRootBand(bindPositions, vertices,
                    minimumPinsPerComponent, pinnedValues);
                added += rootPins;
                if (rootBandPinsAdded) *rootBandPinsAdded += rootPins;
                continue;
            }

            while (pinCount < target)
            {
                unsigned int best = static_cast<unsigned int>(bindPositions.size());
                for (index = 0; index < vertices.size(); ++index)
                {
                    const unsigned int candidate = vertices[index];
                    if (pinnedValues[candidate]) continue;
                    if (best == bindPositions.size() ||
                        bindPositions[candidate].y > bindPositions[best].y)
                        best = candidate;
                }
                if (best == bindPositions.size()) break;
                pinnedValues[best] = true;
                ++pinCount;
                ++added;
            }
        }
        return added;
    }

    void ClothSolver::addConstraint(unsigned int first, unsigned int second, float stiffness)
    {
        if (first == second)
            return;

        const float restLength = length(mParticles[second].position - mParticles[first].position);
        if (restLength <= kEpsilon)
            return;

        Constraint constraint;
        constraint.first = first;
        constraint.second = second;
        constraint.restLength = restLength;
        constraint.stiffness = clampFloat(stiffness, 0.0f, 1.0f);
        mConstraints.push_back(constraint);
    }

    void ClothSolver::substep(
        float timeStep,
        const std::vector<Vec3>& pinTargets,
        const Vec3& acceleration,
        const Ellipsoid& collider)
    {
        const float velocityScale = clampFloat(1.0f - mSettings.damping - mWetness*0.05f, 0.0f, 1.0f);
        const float timeStepSquared = timeStep * timeStep;

        for (unsigned int index = 0; index < mParticles.size(); ++index)
        {
            Particle& particle = mParticles[index];
            if (particle.isPinned)
                continue;

            const Vec3 velocity = (particle.position - particle.previousPosition) * velocityScale;
            const Vec3 oldPosition = particle.position;
            particle.position += velocity + acceleration * timeStepSquared;
            particle.previousPosition = oldPosition;
            if (!finiteVector(particle.position))
            {
                particle.position = mRestPositions[index];
                particle.previousPosition = mRestPositions[index];
            }
        }

        applyPins(pinTargets);
        for (int iteration = 0; iteration < mSettings.constraintIterations; ++iteration)
        {
            solveConstraints();
            solveCollision(collider, pinTargets);
            applyPins(pinTargets);
        }
        // Apply shape memory last. Applying it before the distance iterations lets
        // those iterations immediately undo most of the authored-silhouette repair.
        solveShape(pinTargets);
        solveCollision(collider, pinTargets);
        limitDistanceFromTargets(pinTargets);
        // Last projection lies on the rest-to-candidate segment, so it cannot
        // violate the displacement cap or tunnel through to the opposite side.
        solveFaceCollision(pinTargets);
        applyPins(pinTargets);
    }

    void ClothSolver::applyPins(const std::vector<Vec3>& pinTargets)
    {
        for (unsigned int index = 0; index < mParticles.size(); ++index)
        {
            if (!mParticles[index].isPinned)
                continue;

            mParticles[index].position = pinTargets[index];
            mParticles[index].previousPosition = pinTargets[index];
        }
    }

    void ClothSolver::setWetness(float amount)
    {
        mWetness=finiteFloat(amount) ? clampFloat(amount,0.0f,1.0f) : 0.0f;
    }

    void ClothSolver::setRigidity(float percent)
    {
        if (!finiteFloat(percent)) percent = 50.0f;
        // Smooth bounded 0.25x..4x shape resistance. The midpoint is exactly
        // the established baseline, including the separately softer beard.
        mRigidityScale = std::pow(4.0f,(clampFloat(percent,0.0f,100.0f)-50.0f)/50.0f);
    }

    void ClothSolver::solveShape(const std::vector<Vec3>& shapeTargets)
    {
        const float stiffness = clampFloat(mSettings.shapeStiffness * mRigidityScale, 0.0f, 1.0f);
        const std::vector<Vec3>& targets = shapeTargets.size() == mParticles.size() ?
            shapeTargets : mRestPositions;
        if (stiffness <= 0.0f || targets.size() != mParticles.size()) return;
        for (unsigned int index = 0; index < mParticles.size(); ++index)
        {
            if (mParticles[index].isPinned) continue;
            mParticles[index].position +=
                (targets[index] - mParticles[index].position) * stiffness;
        }
    }

    void ClothSolver::solveConstraints()
    {
        for (unsigned int index = 0; index < mConstraints.size(); ++index)
        {
            const Constraint& constraint = mConstraints[index];
            Particle& first = mParticles[constraint.first];
            Particle& second = mParticles[constraint.second];
            const Vec3 delta = second.position - first.position;
            const float currentLengthSquared = lengthSquared(delta);
            if (!finiteVector(delta) || !finiteFloat(currentLengthSquared) ||
                currentLengthSquared <= kEpsilon * kEpsilon)
                continue;
            const float currentLength = std::sqrt(currentLengthSquared);

            const float firstWeight = first.isPinned ? 0.0f : 1.0f;
            const float secondWeight = second.isPinned ? 0.0f : 1.0f;
            const float weightSum = firstWeight + secondWeight;
            if (weightSum <= 0.0f)
                continue;

            const Vec3 correction = delta *
                (((currentLength - constraint.restLength) / currentLength) * constraint.stiffness);
            first.position += correction * (firstWeight / weightSum);
            second.position -= correction * (secondWeight / weightSum);
        }
    }

    void ClothSolver::solveCollision(
        const Ellipsoid& collider,
        const std::vector<Vec3>& shapeTargets)
    {
        if (!collider.enabled || collider.radii.x <= kEpsilon ||
            collider.radii.y <= kEpsilon || collider.radii.z <= kEpsilon)
        {
            return;
        }

        for (unsigned int index = 0; index < mParticles.size(); ++index)
        {
            Particle& particle = mParticles[index];
            if (particle.isPinned)
                continue;

            // Hair meshes legitimately contain authored vertices inside a coarse head
            // ellipsoid. Ejecting those points creates a radial blast at rest. Collision
            // is therefore one-sided: only a particle whose authored/current target is
            // outside the head may be prevented from crossing inward.
            if (shapeTargets.size() == mParticles.size())
            {
                const Vec3 targetRelative = shapeTargets[index] - collider.center;
                const Vec3 targetScaled(
                    targetRelative.x / collider.radii.x,
                    targetRelative.y / collider.radii.y,
                    targetRelative.z / collider.radii.z);
                if (!finiteVector(targetScaled) || lengthSquared(targetScaled) <= 1.0f)
                    continue;
            }

            const Vec3 relative = particle.position - collider.center;
            Vec3 scaled(
                relative.x / collider.radii.x,
                relative.y / collider.radii.y,
                relative.z / collider.radii.z);
            if (!finiteVector(scaled))
                continue;
            const float scaledLength = length(scaled);
            if (scaledLength >= 1.0f)
                continue;

            if (scaledLength <= kEpsilon)
                scaled = Vec3(0.0f, 1.0f, 0.0f);
            else
                scaled = scaled / scaledLength;

            const Vec3 corrected = collider.center + Vec3(
                scaled.x * collider.radii.x,
                scaled.y * collider.radii.y,
                scaled.z * collider.radii.z);
            particle.position = corrected;
            // A positional projection must not become artificial Verlet velocity on
            // the next step. Zero contact velocity; this also damps card twirling at
            // the coarse head boundary.
            particle.previousPosition = corrected;
        }
    }

    void ClothSolver::limitDistanceFromTargets(const std::vector<Vec3>& shapeTargets)
    {
        const float maximum = mSettings.maximumTargetDistance;
        if (maximum <= 0.0f || shapeTargets.size() != mParticles.size()) return;
        const float maximumSquared = maximum * maximum;
        for (unsigned int index = 0; index < mParticles.size(); ++index)
        {
            Particle& particle = mParticles[index];
            if (particle.isPinned) continue;
            const Vec3 delta = particle.position - shapeTargets[index];
            const float distanceSquared = lengthSquared(delta);
            if (!finiteFloat(distanceSquared) || distanceSquared <= maximumSquared ||
                distanceSquared <= kEpsilon * kEpsilon) continue;
            const Vec3 corrected = shapeTargets[index] +
                delta * (maximum / std::sqrt(distanceSquared));
            const Vec3 correction = corrected - particle.position;
            particle.position = corrected;
            particle.previousPosition += correction;
        }
    }

    void ClothSolver::updateOutput()
    {
        mOutputPositions.resize(mParticles.size());
        for (unsigned int index = 0; index < mParticles.size(); ++index)
            mOutputPositions[index] = mParticles[index].position;
    }
}
