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

#include "SmaaEffect.hpp"

#include "gpu/Framework.hpp"
#include "core/Logger.hpp"

// Vendored SMAA (Jimenez et al., MIT). The algorithm header is embedded as compiler-safe chunks; the
// precomputed lookup tables ship as plain C byte arrays and are included directly.
#include "../../../vendor/smaa/SMAA_embed.hpp"
#include "../../../vendor/smaa/AreaTex.h"
#include "../../../vendor/smaa/SearchTex.h"

#include <string>

namespace wxl::scripts::render_modern
{
    // Edge and weight targets hold packed data, not color, so a fixed RGBA8 format regardless of scene format.
    static const DXGI_FORMAT kInterFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Per-pass texture declarations and entry points. The shared fullscreen VS supplies uv; the SMAA per-pass
    // vertex math is run at the top of each pixel shader (cheap, and avoids three custom vertex shaders).
    static const char* k_edgeTex = "Texture2D colorTex : register(t0);\n";
    static const char* k_edgeMain =
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float4 offset[3];\n"
        "  SMAAEdgeDetectionVS(uv, offset);\n"
        "  return float4(SMAAColorEdgeDetectionPS(uv, offset, colorTex), 0.0, 0.0);\n"
        "}\n";

    static const char* k_blendTex =
        "Texture2D edgesTex  : register(t0);\n"
        "Texture2D areaTex   : register(t1);\n"
        "Texture2D searchTex : register(t2);\n";
    static const char* k_blendMain =
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float2 pix; float4 offset[3];\n"
        "  SMAABlendingWeightCalculationVS(uv, pix, offset);\n"
        "  return SMAABlendingWeightCalculationPS(uv, pix, offset, edgesTex, areaTex, searchTex, float4(0,0,0,0));\n"
        "}\n";

    static const char* k_neighTex =
        "Texture2D colorTex : register(t0);\n"
        "Texture2D blendTex : register(t1);\n";
    static const char* k_neighMain =
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  float4 offset;\n"
        "  SMAANeighborhoodBlendingVS(uv, offset);\n"
        "  return SMAANeighborhoodBlendingPS(uv, offset, colorTex, blendTex);\n"
        "}\n";

    // A pass's root signature: one SRV table (numSrv textures from t0), the resolution metrics as root
    // constants at b0, and SMAA's two samplers (linear at s0, point at s1).
    static ID3D12RootSignature* makeRootSig(Framework& gpu, UINT numSrv)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = numSrv;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[0].DescriptorTable.NumDescriptorRanges = 1;
        params[0].DescriptorTable.pDescriptorRanges = &range;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].Constants.ShaderRegister = 0;   // b0 = SMAA_RT_METRICS
        params[1].Constants.Num32BitValues = 4;

        D3D12_STATIC_SAMPLER_DESC samp[2] = {};
        samp[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[0].ShaderRegister = 0;   // s0 = LinearSampler
        samp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samp[0].MaxLOD = D3D12_FLOAT32_MAX;
        samp[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[1].AddressU = samp[1].AddressV = samp[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[1].ShaderRegister = 1;   // s1 = PointSampler
        samp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samp[1].MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 2;
        rs.pParameters = params;
        rs.NumStaticSamplers = 2;
        rs.pStaticSamplers = samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        return gpu.CreateRootSig(rs);
    }

    // Binds a pass and draws the fullscreen triangle.
    static void drawPass(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* rs, ID3D12PipelineState* pso,
                         D3D12_GPU_DESCRIPTOR_HANDLE srv, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                         const float metrics[4], uint32_t w, uint32_t h)
    {
        cmd->SetGraphicsRootSignature(rs);
        cmd->SetGraphicsRootDescriptorTable(0, srv);
        cmd->SetGraphicsRoot32BitConstants(1, 4, metrics, 0);
        cmd->SetPipelineState(pso);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT scr = { 0, 0, (LONG)w, (LONG)h };
        cmd->RSSetScissorRects(1, &scr);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    bool SmaaEffect::Init(Framework& gpu, DXGI_FORMAT rtvFmt)
    {
        m_sceneFmt = rtvFmt;

        // Upload the precomputed lookup tables (RG8 area, R8 search).
        const void* aData[1] = { areaTexBytes };
        const uint32_t aBytes[1] = { AREATEX_SIZE };
        m_areaTex = gpu.CreateTex2DFromMips(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, 1, aData, aBytes);
        const void* sData[1] = { searchTexBytes };
        const uint32_t sBytes[1] = { SEARCHTEX_SIZE };
        m_searchTex = gpu.CreateTex2DFromMips(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, 1, sData, sBytes);
        if (!m_areaTex || !m_searchTex)
        {
            WLOG_ERROR("wxl-modern-graphic: SMAA LUT upload failed");
            return false;
        }

        m_edgeRS  = makeRootSig(gpu, 1);
        m_blendRS = makeRootSig(gpu, 3);
        m_neighRS = makeRootSig(gpu, 2);
        if (!m_edgeRS || !m_blendRS || !m_neighRS)
        {
            WLOG_ERROR("wxl-modern-graphic: SMAA root signature failed");
            return false;
        }

        // Reassemble the SMAA header once; only the preset define changes between tiers.
        std::string header;
        for (int i = 0; i < k_smaa_partCount; ++i)
            header += k_smaa_parts[i];

        auto build = [&](const char* preset, const char* texs, const char* mainSrc)
        {
            std::string s = "#define SMAA_HLSL_4_1 1\n";
            s += preset;
            s += "cbuffer SmaaRT : register(b0) { float4 SMAA_RT_METRICS; };\n";
            s += "SamplerState LinearSampler : register(s0);\n";
            s += "SamplerState PointSampler  : register(s1);\n";
            s += header;
            s += texs;
            s += mainSrc;
            return s;
        };

        // Tiers map to SMAA presets: Low = LOW, Medium = MEDIUM, High = HIGH (HIGH adds diagonal + corner
        // detection). The edge (threshold) and blend (search steps) passes depend on the preset; the final
        // neighborhood blend does not, so it is compiled once.
        for (int tier = 0; tier < 3; ++tier)
        {
            const char* preset = tier == 0 ? "#define SMAA_PRESET_LOW 1\n"
                                : tier == 1 ? "#define SMAA_PRESET_MEDIUM 1\n"
                                            : "#define SMAA_PRESET_HIGH 1\n";

            ID3DBlob* eps = gpu.Compile(build(preset, k_edgeTex, k_edgeMain).c_str(), "main", "ps_5_0");
            if (!eps) return false;
            m_edgePso[tier] = gpu.CreateFullscreenPSO(m_edgeRS, eps, kInterFmt, 1);
            eps->Release();

            ID3DBlob* bps = gpu.Compile(build(preset, k_blendTex, k_blendMain).c_str(), "main", "ps_5_0");
            if (!bps) return false;
            m_blendPso[tier] = gpu.CreateFullscreenPSO(m_blendRS, bps, kInterFmt, 1);
            bps->Release();

            if (!m_edgePso[tier] || !m_blendPso[tier])
            {
                WLOG_ERROR("wxl-modern-graphic: SMAA PSO failed (tier %d)", tier);
                return false;
            }
        }

        ID3DBlob* nps = gpu.Compile(build("#define SMAA_PRESET_MEDIUM 1\n", k_neighTex, k_neighMain).c_str(), "main", "ps_5_0");
        if (!nps) return false;
        m_neighPso = gpu.CreateFullscreenPSO(m_neighRS, nps, m_sceneFmt, 1);
        nps->Release();
        if (!m_neighPso)
        {
            WLOG_ERROR("wxl-modern-graphic: SMAA neighborhood PSO failed");
            return false;
        }

        WLOG_INFO("wxl-modern-graphic: SMAA effect ready (3 quality tiers)");
        return true;
    }

    void SmaaEffect::ensureTargets(Framework& gpu, uint32_t w, uint32_t h)
    {
        if (m_edges && m_w == w && m_h == h) return;
        if (m_edges) { m_edges->Release(); m_edges = nullptr; }
        if (m_blend) { m_blend->Release(); m_blend = nullptr; }
        m_edges = gpu.CreateTex2D(w, h, kInterFmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        m_blend = gpu.CreateTex2D(w, h, kInterFmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        m_w = w; m_h = h;
    }

    void SmaaEffect::Render(Framework& gpu, const FrameContext& ctx)
    {
        ensureTargets(gpu, ctx.w, ctx.h);
        if (!m_edges || !m_blend) return;

        ID3D12GraphicsCommandList* cmd = ctx.cmd;
        const int tier = Tier3();
        const float metrics[4] = { 1.0f / ctx.w, 1.0f / ctx.h, (float)ctx.w, (float)ctx.h };
        const float black[4] = { 0, 0, 0, 0 };

        ID3D12DescriptorHeap* heaps[] = { gpu.SrvHeap() };
        cmd->SetDescriptorHeaps(1, heaps);

        // Pass 1: edge detection (scene -> edges). Edges are cleared first because the detection clips
        // non-edge pixels rather than writing them.
        gpu.Barrier(m_edges, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE edgesRtv = gpu.Rtv(m_edges, kInterFmt);
        cmd->ClearRenderTargetView(edgesRtv, black, 0, nullptr);
        {
            uint32_t s = gpu.AllocDescriptors(1);
            gpu.SetSRV(s, ctx.srcTex, ctx.fmt);
            drawPass(cmd, m_edgeRS, m_edgePso[tier], gpu.GpuHandle(s), edgesRtv, metrics, ctx.w, ctx.h);
        }
        gpu.Barrier(m_edges, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 2: blending-weight calculation (edges + LUTs -> weights).
        gpu.Barrier(m_blend, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_CPU_DESCRIPTOR_HANDLE blendRtv = gpu.Rtv(m_blend, kInterFmt);
        cmd->ClearRenderTargetView(blendRtv, black, 0, nullptr);
        {
            uint32_t s = gpu.AllocDescriptors(3);
            gpu.SetSRV(s + 0, m_edges, kInterFmt);
            gpu.SetSRV(s + 1, m_areaTex, DXGI_FORMAT_R8G8_UNORM);
            gpu.SetSRV(s + 2, m_searchTex, DXGI_FORMAT_R8_UNORM);
            drawPass(cmd, m_blendRS, m_blendPso[tier], gpu.GpuHandle(s), blendRtv, metrics, ctx.w, ctx.h);
        }
        gpu.Barrier(m_blend, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Pass 3: neighborhood blending (scene + weights -> the effect's destination target).
        {
            uint32_t s = gpu.AllocDescriptors(2);
            gpu.SetSRV(s + 0, ctx.srcTex, ctx.fmt);
            gpu.SetSRV(s + 1, m_blend, kInterFmt);
            drawPass(cmd, m_neighRS, m_neighPso, gpu.GpuHandle(s), ctx.dstRtv, metrics, ctx.w, ctx.h);
        }

        // Leave the intermediates in COMMON for the next frame's barriers.
        gpu.Barrier(m_edges, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        gpu.Barrier(m_blend, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}
