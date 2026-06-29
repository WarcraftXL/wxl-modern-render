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

#include "FxaaEffect.hpp"

#include "gpu/Framework.hpp"
#include "core/Logger.hpp"

// Vendored FXAA 3.11 (NVIDIA, BSD). Embedded as compiler-safe chunks because the header exceeds MSVC's
// string-literal limits. Configuration macros precede it; the wrapper entry point follows it.
#include "../../../vendor/fxaa/Fxaa3_11_embed.hpp"

#include <string>

namespace wxl::scripts::render_modern
{
    // Compile-time FXAA configuration. PC Quality path, SM5, green channel as the luma estimate. The
    // per-tier knobs (preset + the three tuning values) are injected per PSO, not baked here.
    static const char* k_fxaaConfig =
        "#define FXAA_PC 1\n"
        "#define FXAA_HLSL_5 1\n"
        "#define FXAA_GREEN_AS_LUMA 1\n";

    // Per-tier settings (Low / Medium / High). Beyond the preset (edge-search step count, barely visible),
    // the tiers differ in the values that DO show: subpix = sub-pixel aliasing removal (higher = smoother but
    // softer), edge = local contrast needed to anti-alias (lower = catches more edges), edgeMin = dark-region
    // floor. So Low is sharp/selective, High is smooth/aggressive.
    struct Tier { int preset; const char* subpix; const char* edge; const char* edgeMin; };
    static const Tier k_tiers[3] = {
        { 12, "0.50", "0.250", "0.0833" },   // Low
        { 29, "0.75", "0.166", "0.0625" },   // Medium
        { 39, "1.00", "0.125", "0.0312" },   // High
    };

    // Wrapper entry: derives 1/screen from the bound texture and calls the Quality path. Console-only
    // arguments are passed as zero. The tuning values come from the per-tier defines injected at compile.
    static const char* k_fxaaMain =
        "\n"
        "Texture2D    scene : register(t0);\n"
        "SamplerState smp   : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target\n"
        "{\n"
        "    uint sw, sh; scene.GetDimensions(sw, sh);\n"
        "    float2 rcp = float2(1.0 / sw, 1.0 / sh);\n"
        "    FxaaTex t; t.smpl = smp; t.tex = scene;\n"
        "    return FxaaPixelShader(\n"
        "        uv, float4(0,0,0,0), t, t, t, t, rcp,\n"
        "        float4(0,0,0,0), float4(0,0,0,0), float4(0,0,0,0),\n"
        "        WX_FXAA_SUBPIX, WX_FXAA_EDGE, WX_FXAA_EDGEMIN, 8.0, 0.125, 0.05,\n"
        "        float4(1.0, -1.0, 0.25, -0.25));\n"
        "}\n";

    bool FxaaEffect::Init(Framework& gpu, DXGI_FORMAT rtvFmt)
    {
        // Reassemble the embedded FXAA header once; only the leading preset define changes per tier.
        std::string header;
        for (int i = 0; i < k_fxaa3_11_partCount; ++i)
            header += k_fxaa3_11_parts[i];

        // One PSO per tier. The root signature (1 SRV + linear sampler) is identical, so reuse the first.
        for (int tier = 0; tier < 3; ++tier)
        {
            const Tier& t = k_tiers[tier];
            std::string src = k_fxaaConfig;
            src += "#define FXAA_QUALITY__PRESET " + std::to_string(t.preset) + "\n";
            src += std::string("#define WX_FXAA_SUBPIX ")  + t.subpix  + "\n";
            src += std::string("#define WX_FXAA_EDGE ")    + t.edge    + "\n";
            src += std::string("#define WX_FXAA_EDGEMIN ") + t.edgeMin + "\n";
            src += header;
            src += k_fxaaMain;

            ID3D12RootSignature* rs = nullptr;
            // Scene targets are single-sample, so the pass is always sampleCount 1.
            if (!gpu.CreateTextureFx(src.c_str(), rtvFmt, 1, &rs, &m_pso[tier]))
            {
                WLOG_ERROR("wxl-modern-graphic: FXAA pipeline init failed (tier %d)", tier);
                return false;
            }
            if (tier == 0) m_rootSig = rs;
            else rs->Release();
        }
        WLOG_INFO("wxl-modern-graphic: FXAA effect ready (3 quality tiers)");
        return true;
    }

    void FxaaEffect::Render(Framework& gpu, const FrameContext& ctx)
    {
        gpu.DrawFullscreen(ctx.cmd, m_rootSig, m_pso[Tier3()], ctx.srcSrv, ctx.dstRtv, ctx.w, ctx.h);
    }
}
