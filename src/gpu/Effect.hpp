// wxl-modern-graphic: the post-process effect interface and the per-frame context handed to it.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <d3d12.h>
#include <cstdint>

namespace wxl::scripts::render_modern
{
    class Framework;

    /** @brief Shared quality tier an effect maps to its own settings (Low / Medium / High / Ultra).
     *  Effects that only define three tiers clamp Ultra to High. */
    enum class Quality { Low, Medium, High, Ultra };

    /** @brief Inputs handed to an effect for one frame. */
    struct FrameContext
    {
        ID3D12GraphicsCommandList* cmd;
        ID3D12Resource*            srcTex;     // scene resource behind srcSrv (for effects building own SRV tables)
        D3D12_GPU_DESCRIPTOR_HANDLE srcSrv;    // scene to read (SRV table base)
        D3D12_GPU_DESCRIPTOR_HANDLE depthSrv;  // scene depth, .ptr == 0 when unavailable
        D3D12_GPU_DESCRIPTOR_HANDLE waterMask; // stencil SRV (water tagged = bit 0), .ptr == 0 when unavailable
        D3D12_CPU_DESCRIPTOR_HANDLE dstRtv;    // target to write
        uint32_t w, h;
        DXGI_FORMAT fmt;
        const float* proj;                     // projection (16 floats, D3D row-major); identity/null when not in-world
        const float* view;                     // view matrix (16 floats); used to find world-up in view space
    };

    /** @brief A post-process effect: one-time init, then a per-frame pass over the scene. */
    class IEffect
    {
    public:
        virtual ~IEffect() = default;
        virtual const char* Name() const = 0;
        virtual bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) = 0;
        virtual bool Enabled() const { return m_enabled; }
        void SetEnabled(bool on) { m_enabled = on; }
        virtual void SetQuality(Quality q) { m_quality = q; }
        Quality GetQuality() const { return m_quality; }
        /** @brief Quality as a 0..2 index for effects with only three tiers (Ultra clamps to High). */
        int Tier3() const { int q = (int)m_quality; return q > 2 ? 2 : q; }
        /** @brief Number of quality tiers the overlay should offer (3 = up to High, 4 = adds Ultra). */
        virtual int QualityLevels() const { return 3; }
        /** @brief True for anti-aliasing effects; the pipeline skips them when the backbuffer is MSAA. */
        virtual bool IsAntiAliasing() const { return false; }
        /** @brief True when the effect samples the world depth; the core then produces a readable INTZ depth. */
        virtual bool NeedsDepth() const { return false; }
        /** @brief Draws effect-specific live tuning controls in the overlay (ImGui). No-op by default. */
        virtual void DrawTuning() {}
        virtual void Render(Framework& gpu, const FrameContext& ctx) = 0;

    protected:
        bool m_enabled = true;            // generic on/off the overlay flips; effects may still override Enabled()
        Quality m_quality = Quality::Medium;
    };
}
