// wxl-modern-graphic: CMAA2 (Intel) as a compute-based post-process effect.
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

#include "gpu/Effect.hpp"

#include <d3d12.h>
#include <cstdint>

namespace wxl::scripts::render_modern
{
    /**
     * @brief Conservative Morphological Anti-Aliasing 2 (Intel), a compute pipeline.
     *
     * CMAA2 is compute-only and works in place on a UAV color target, so it cannot read and write the scene
     * ping-pong targets directly (those are render targets in the backbuffer's BGRX format). The effect blits
     * the scene into its own RGBA8 working texture, runs the four CMAA2 compute passes (edge detection, two
     * indirect-args computes, candidate processing and deferred color apply via ExecuteIndirect), then blits
     * the result back. Quality tiers map to CMAA2's LOW/MEDIUM/HIGH edge-threshold presets.
     */
    class Cmaa2Effect : public IEffect
    {
    public:
        const char* Name() const override { return "CMAA2"; }
        bool IsAntiAliasing() const override { return true; }
        bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) override;
        void Render(Framework& gpu, const FrameContext& ctx) override;

    private:
        void ensureTargets(Framework& gpu, uint32_t w, uint32_t h);

        // Compute pipeline shared across tiers; only the edge pass depends on the quality preset.
        ID3D12RootSignature*    m_rootSig  = nullptr;
        ID3D12CommandSignature* m_cmdSig   = nullptr;   // single DISPATCH indirect arg
        ID3D12PipelineState*    m_edgesPso[3] = { nullptr, nullptr, nullptr };
        ID3D12PipelineState*    m_argsPso     = nullptr;
        ID3D12PipelineState*    m_processPso  = nullptr;
        ID3D12PipelineState*    m_deferredPso = nullptr;

        // Format bridge between the scene targets (BGRX) and the UAV-capable working color (RGBA8).
        ID3D12RootSignature* m_blitInRS  = nullptr;  ID3D12PipelineState* m_blitInPso  = nullptr;
        ID3D12RootSignature* m_blitOutRS = nullptr;  ID3D12PipelineState* m_blitOutPso = nullptr;

        // Working resources (recreated on resize).
        ID3D12Resource* m_color  = nullptr;   // RGBA8 typeless, RT|UAV; CMAA2's in-place target
        ID3D12Resource* m_edges  = nullptr;   // u1
        ID3D12Resource* m_heads  = nullptr;   // u5
        ID3D12Resource* m_shapeCandidates = nullptr;  // u2
        ID3D12Resource* m_blendLocs       = nullptr;  // u3
        ID3D12Resource* m_blendItems      = nullptr;  // u4
        ID3D12Resource* m_control  = nullptr; // u6, raw 16 uints
        ID3D12Resource* m_indirect = nullptr; // u7, raw 4 uints (the dispatch args)
        ID3D12Resource* m_zeroUpload = nullptr; // 64 zero bytes, for the one-time control-buffer clear

        uint32_t    m_w = 0, m_h = 0;
        DXGI_FORMAT m_sceneFmt = DXGI_FORMAT_UNKNOWN;
        bool        m_controlCleared = false;
    };
}
