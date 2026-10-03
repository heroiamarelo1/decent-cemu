#pragma once

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Contract: columns are the GamePad body axes expressed in the game world.
// IMU quaternion rotates body to world (world gravity reference is +Z).
// VPAD world swaps Y/Z; VPAD body reverses Z. Both changes have determinant -1,
// so their composition preserves a proper rotation. Axial rates consequently
// map as (-wx,-wy,+wz), while acceleration maps as (-ax,-ay,+az).
namespace vpad_motion
{
inline glm::mat3 direction(const glm::quat& attitude)
{
    const glm::mat3 r = glm::mat3_cast(glm::normalize(attitude));
    return {{r[0].x, r[0].z, r[0].y},
            {r[1].x, r[1].z, r[1].y},
            {-r[2].x, -r[2].z, -r[2].y}};
}

// Android DCM2 uses screen-relative IMU coordinates. Convert both world and
// body bases together; an isolated sign change breaks attitude/rate agreement.
// Relative to the legacy adapter D'=S*D*S, S=diag(-1,1,1).
// Polar acceleration transforms by S; axial rates by det(S)*S.
// This is a candidate Android adapter, validated separately from legacy input.
inline glm::mat3 androidDirection(const glm::quat& attitude)
{
    auto d = direction(attitude);
    constexpr float signs[3]{-1.0f, 1.0f, 1.0f};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) d[c][r] *= signs[c] * signs[r];
    return d;
}

inline glm::vec3 androidAxialFromLegacy(const glm::vec3& value)
{
    return {value.x, -value.y, -value.z};
}

inline glm::vec2 verticality(const glm::vec3& acceleration)
{
    const float length = glm::length(acceleration);
    if (!std::isfinite(length) || length < 1e-5f)
        return {0.0f, 0.0f};
    // Flat screen up/down => |Y|=1. Upright top up => Z=-1.
    return {std::clamp(std::abs(acceleration.y) / length, 0.0f, 1.0f),
            std::clamp(-acceleration.z / length, -1.0f, 1.0f)};
}

inline bool isRotation(const glm::mat3& matrix)
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!std::isfinite(matrix[c][r])) return false;
    const glm::mat3 gram = glm::transpose(matrix) * matrix;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (std::abs(gram[c][r] - (c == r ? 1.0f : 0.0f)) > 0.05f) return false;
    return std::abs(glm::determinant(matrix) - 1.0f) < 0.05f;
}

// A change of game reference is a WORLD transform. Post-multiplying would
// instead rotate the body axes, coupling steering to the calibration pose.
class Reference
{
public:
    // acceleration is the current VPAD accelerometer, in g. When it is a real
    // gravity reading and does not match the pose the game is declaring, the
    // call is ignored. Nintendo Land asks for the flat pose again on minigame
    // entry; honouring that while the pad is in the player's hand relabels
    // the held pose as position 0. A pad with no accelerometer still rebases.
    bool setDirection(const glm::mat3& requested, const glm::vec3& acceleration, bool haveAcceleration)
    {
        if (!isRotation(requested)) return false;
        if (haveAcceleration && !declaredPoseMatchesGravity(requested, acceleration))
            return false;
        m_requested = glm::mat3_cast(glm::normalize(glm::quat_cast(requested)));
        m_pending = true;
        if (m_hasRaw) rebase();
        return true;
    }

    glm::mat3 apply(const glm::mat3& raw)
    {
        m_raw = raw;
        m_hasRaw = true;
        if (m_pending) rebase();
        return m_reference * raw;
    }

    bool setAngles(const glm::vec3& requested)
    {
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(requested[i])) return false;
        m_requestedAngles = requested;
        m_anglePending = true;
        if (m_hasAngles)
        {
            m_angleOffset = requested - m_rawAngles;
            m_anglePending = false;
        }
        return true;
    }

    glm::vec3 applyAngles(const glm::vec3& raw)
    {
        m_rawAngles = raw;
        m_hasAngles = true;
        if (m_anglePending)
        {
            m_angleOffset = m_requestedAngles - raw;
            m_anglePending = false;
        }
        return raw + m_angleOffset;
    }

private:
    // Identity is the flat, screen-up pose, whose accelerometer is (0, -1, 0).
    // For any declared direction the same world-up gives -(column y components).
    static bool declaredPoseMatchesGravity(const glm::mat3& requested, const glm::vec3& acceleration)
    {
        const float magnitude = glm::length(acceleration);
        if (!std::isfinite(magnitude) || magnitude < 0.2f)
            return true;
        if (magnitude < 0.75f || magnitude > 1.25f)
            return false;
        const glm::vec3 expected(-requested[0].y, -requested[1].y, -requested[2].y);
        const float expectedLength = glm::length(expected);
        if (expectedLength < 0.5f)
            return false;
        const float agreement = glm::dot(acceleration / magnitude, expected / expectedLength);
        return agreement > 0.85f;
    }

    void rebase()
    {
        m_reference = m_requested * glm::transpose(m_raw);
        m_pending = false;
    }
    glm::mat3 m_reference{1.0f}, m_raw{1.0f}, m_requested{1.0f};
    glm::vec3 m_angleOffset{}, m_rawAngles{}, m_requestedAngles{};
    bool m_hasRaw = false, m_pending = false, m_hasAngles = false, m_anglePending = false;
};
}
