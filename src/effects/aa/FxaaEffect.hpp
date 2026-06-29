// wxl-modern-graphic: FXAA 3.11 (NVIDIA) as a single-pass post-process effect.
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

namespace wxl::scripts::render_modern
{
    /**
     * @brief Fast approximate anti-aliasing (FXAA 3.11 Quality) in one fullscreen pass.
     *
     * Wraps the vendored NVIDIA FXAA shader (BSD) into a standard texture pass: it reads the resolved
     * scene and writes the edge-smoothed result. Luma comes from the green channel (FXAA_GREEN_AS_LUMA),
     * so no luma pre-pass is needed; the inverse screen size is derived from the bound texture.
     */
    class FxaaEffect : public IEffect
    {
    public:
        const char* Name() const override { return "FXAA"; }
        bool IsAntiAliasing() const override { return true; }
        bool Init(Framework& gpu, DXGI_FORMAT rtvFmt) override;
        void Render(Framework& gpu, const FrameContext& ctx) override;

    private:
        // One PSO per quality tier (Low/Medium/High), differing only in FXAA's compile-time quality preset.
        ID3D12RootSignature* m_rootSig = nullptr;
        ID3D12PipelineState* m_pso[3] = { nullptr, nullptr, nullptr };
    };
}
