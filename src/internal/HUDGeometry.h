// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Pure geometry for the head-locked HUD panel. No runtime dependency —
// unit-testable headless.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ImGuiVRHelper::InSceneOverlay
{

/// Shape of the HUD panel.
enum class HUDShape
{
    Flat,      ///< Original flat quad (2 triangles).
    Cylinder,  ///< Curved cylinder arc wrapping horizontally around the player.
};

/// Size + vertical centre of the HUD panel in head space (metres).
struct HUDQuad
{
    float width   = 0.0f;
    float height  = 0.0f;
    float centerY = 0.0f;
};

/// A single vertex for the HUD mesh (position + UV).
struct HUDVertex
{
    float x, y, z;   ///< position in head space (metres)
    float u, v;       ///< texture coords [0,1]
};

/// Build an EYE-INDEPENDENT HUD panel so it stereo-converges.
/// See original comment for full explanation.
inline HUDQuad ComputeHUDQuad(const float projLeft[4], const float projRight[4],
    float hudDepth, float coverage)
{
    const float lL = projLeft[0],  rL = projLeft[1],  bL = projLeft[2],  tL = projLeft[3];
    const float lR = projRight[0], rR = projRight[1], bR = projRight[2], tR = projRight[3];

    const float tanHalfX  = std::max({ -lL, rL, -lR, rR });
    const float tanBottom = std::max(-bL, -bR);
    const float tanTop    = std::max(tL, tR);

    HUDQuad quad;
    quad.width   = (2.0f * tanHalfX  * hudDepth) * coverage;
    quad.height  = ((tanTop + tanBottom) * hudDepth) * coverage;
    quad.centerY = 0.5f * (tanTop - tanBottom) * hudDepth;
    return quad;
}

/// Build a tessellated cylinder mesh for the HUD.
///
/// The cylinder has radius `hudDepth`, centred on the player's head (X=0,
/// Y=centerY, Z=0).  The arc spans the same horizontal angle as the flat
/// quad would subtend at that depth, so the perceived width is identical.
/// `segments` controls tessellation quality (16–32 is plenty for VR).
///
/// Returns interleaved HUDVertex data and a flat index list (triangle list).
inline void ComputeCylinderMesh(
    const float projLeft[4], const float projRight[4],
    float hudDepth, float coverage, int segments,
    std::vector<HUDVertex>& outVertices,
    std::vector<uint32_t>&  outIndices)
{
    segments = std::max(segments, 2);

    const HUDQuad quad = ComputeHUDQuad(projLeft, projRight, hudDepth, coverage);

    // Half-angle subtended by the flat quad at hudDepth.
    // arc = 2 * atan(halfWidth / depth)
    const float halfWidth  = quad.width  * 0.5f;
    const float halfHeight = quad.height * 0.5f;
    const float halfAngle  = std::atan2(halfWidth, hudDepth);

    const float topY    = quad.centerY + halfHeight;
    const float bottomY = quad.centerY - halfHeight;

    outVertices.clear();
    outIndices.clear();

    // (segments+1) columns x 2 rows (top + bottom).
    for (int col = 0; col <= segments; ++col)
    {
        const float t     = static_cast<float>(col) / static_cast<float>(segments);
        const float angle = -halfAngle + t * 2.0f * halfAngle;  // left → right

        const float px = hudDepth * std::sin(angle);
        const float pz = -hudDepth * std::cos(angle);  // negative = in front

        // Top vertex
        HUDVertex top;
        top.x = px; top.y = topY;    top.z = pz;
        top.u = t;  top.v = 0.0f;
        outVertices.push_back(top);

        // Bottom vertex
        HUDVertex bot;
        bot.x = px; bot.y = bottomY; bot.z = pz;
        bot.u = t;  bot.v = 1.0f;
        outVertices.push_back(bot);
    }

    // Each column pair forms 2 triangles.
    for (int col = 0; col < segments; ++col)
    {
        const uint32_t tl = static_cast<uint32_t>(col * 2 + 0);  // top-left
        const uint32_t bl = static_cast<uint32_t>(col * 2 + 1);  // bottom-left
        const uint32_t tr = static_cast<uint32_t>(col * 2 + 2);  // top-right
        const uint32_t br = static_cast<uint32_t>(col * 2 + 3);  // bottom-right

        // Triangle 1: tl, tr, bl
        outIndices.push_back(tl);
        outIndices.push_back(tr);
        outIndices.push_back(bl);

        // Triangle 2: tr, br, bl
        outIndices.push_back(tr);
        outIndices.push_back(br);
        outIndices.push_back(bl);
    }
}

}  // namespace ImGuiVRHelper::InSceneOverlay
