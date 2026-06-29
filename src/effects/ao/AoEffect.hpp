// wxl-modern-graphic: screen-space ambient occlusion (SAO for the lower tiers, horizon-based GTAO for Ultra).
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

namespace wxl::scripts::render_modern
{
    /**
     * @brief Ambient occlusion from the world depth (INTZ). Reconstructs view-space position + normal from
     *        depth, estimates occlusion, denoises it depth-aware, and multiplies it over the scene. Tiers:
     *        Low/Medium/High = SAO (spiral sampling, rising sample count); Ultra = GTAO (horizon-based,
     *        ground-truth quality). Needs ctx.depthSrv; passthrough when depth is absent.
     */
    class AoEffect : public IEffect
    {
    public:
        const char* Name() const override { return "SSAO"; }
        bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) override;
        void SetQuality(Quality q) override;
        bool IsAntiAliasing() const override { return false; }
        bool NeedsDepth() const override { return true; }
        int QualityLevels() const override { return 4; }   // Low/Medium/High (SAO) + Ultra (GTAO)
        void DrawTuning() override;
        void Render(Framework& gpu, const FrameContext& ctx) override;

    private:
        void ensureAo(Framework& gpu, uint32_t w, uint32_t h);

        ID3D12RootSignature* m_aoRootSig = nullptr;
        ID3D12PipelineState* m_saoPso    = nullptr;   // SAO (spiral) compute
        ID3D12PipelineState* m_gtaoPso   = nullptr;   // GTAO (horizon) compute, Ultra
        ID3D12RootSignature* m_blurRootSig = nullptr;
        ID3D12PipelineState* m_blurPso     = nullptr;
        ID3D12RootSignature* m_compRootSig = nullptr;
        ID3D12PipelineState* m_compPso     = nullptr;

        ID3D12Resource* m_aoTex  = nullptr;           // raw AO at HALF resolution (R32_FLOAT, UAV)
        ID3D12Resource* m_aoTexB = nullptr;           // denoised + upsampled AO at full resolution
        D3D12_RESOURCE_STATES m_aoState  = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES m_aoStateB = D3D12_RESOURCE_STATE_COMMON;
        uint32_t m_w = 0, m_h = 0;                    // full (scene) resolution
        uint32_t m_aoHalfW = 0, m_aoHalfH = 0;        // half resolution the raw AO is computed at

        // Per-tier settings (set by SetQuality). algo 0 = SAO, 1 = GTAO.
        int   m_algo      = 0;
        float m_intensity = 0.50f;
        uint32_t m_samples = 12;
        float m_radius   = 0.20f;  // occlusion radius in world units
        float m_bias     = 0.03f;  // removes self-occlusion on flat surfaces
        float m_power    = 0.60f;  // AO contrast
        float m_blurPx   = 1.20f;  // denoise kernel spread
        float m_blurSharp = 8.0f;  // edge preservation in the denoise
    };
}
