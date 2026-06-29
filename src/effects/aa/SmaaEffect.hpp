// wxl-modern-graphic: SMAA (Jimenez et al.) as a three-pass post-process effect.
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
     * @brief Subpixel Morphological Anti-Aliasing (SMAA 1x), three passes.
     *
     * Edge detection (scene -> edges) then blending-weight calculation (edges + area/search LUTs -> weights)
     * then neighborhood blending (scene + weights -> output). Sharper edges and less blur than FXAA. The
     * quality tier maps to SMAA's LOW/MEDIUM/HIGH presets (search steps + diagonal/corner detection).
     */
    class SmaaEffect : public IEffect
    {
    public:
        const char* Name() const override { return "SMAA"; }
        bool IsAntiAliasing() const override { return true; }
        bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) override;
        void Render(Framework& gpu, const FrameContext& ctx) override;

    private:
        // Recreates the intermediate edge/weight targets when the scene size changes.
        void ensureTargets(Framework& gpu, uint32_t w, uint32_t h);

        // One root signature per pass (they differ only in SRV count); PSOs per tier where the preset matters.
        ID3D12RootSignature* m_edgeRS  = nullptr;   // 1 SRV (scene)
        ID3D12RootSignature* m_blendRS = nullptr;   // 3 SRV (edges, area, search)
        ID3D12RootSignature* m_neighRS = nullptr;   // 2 SRV (scene, weights)
        ID3D12PipelineState* m_edgePso[3]  = { nullptr, nullptr, nullptr };
        ID3D12PipelineState* m_blendPso[3] = { nullptr, nullptr, nullptr };
        ID3D12PipelineState* m_neighPso    = nullptr;   // preset-independent

        ID3D12Resource* m_areaTex   = nullptr;   // precomputed area LUT (RG8 160x560)
        ID3D12Resource* m_searchTex = nullptr;   // precomputed search LUT (R8 64x16)
        ID3D12Resource* m_edges = nullptr;       // pass-1 output
        ID3D12Resource* m_blend = nullptr;       // pass-2 output

        uint32_t    m_w = 0, m_h = 0;
        DXGI_FORMAT m_sceneFmt = DXGI_FORMAT_UNKNOWN;
    };
}
