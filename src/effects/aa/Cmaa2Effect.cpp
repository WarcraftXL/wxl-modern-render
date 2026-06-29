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

#include "Cmaa2Effect.hpp"

#include "gpu/Framework.hpp"
#include "core/Logger.hpp"

// Vendored CMAA2 (Intel, Apache-2.0), embedded as compiler-safe chunks.
#include "../../../vendor/cmaa2/CMAA2_embed.hpp"

#include <string>

namespace wxl::scripts::render_modern
{
    // The working color is RGBA8 typeless so it can be an RGBA8 SRV/RTV and an R32_UINT UAV at once. CMAA2's
    // untyped store path (CMAA2_UAV_STORE_UNTYPED_FORMAT=1) packs RGBA8 through that R32_UINT UAV.
    static const DXGI_FORMAT kColorTypeless = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    static const DXGI_FORMAT kColorUNorm    = DXGI_FORMAT_R8G8B8A8_UNORM;
    static const DXGI_FORMAT kColorUAV      = DXGI_FORMAT_R32_UINT;

    // CMAA2 fixed kernel geometry (CMAA2.hlsl): 16x16 input groups cover a 14x14 output kernel, two pixels per
    // thread, so the edge dispatch tiles the screen in steps of 28.
    static const uint32_t kEdgeDispatchStep = 28;

    // Format bridge between the scene targets and the working color.
    static const char* k_blitPS =
        "Texture2D    src : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
        "  return float4(src.Sample(smp, uv).rgb, 1);\n"
        "}\n";

    // Compute root signature: one table holding the input SRV (t0) then the eight UAVs (u0..u7), plus the
    // point-clamp sampler CMAA2 expects at s0. Matches the heap order filled each frame in Render.
    static ID3D12RootSignature* makeComputeRootSig(Framework& gpu)
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 8;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        param.DescriptorTable.NumDescriptorRanges = 2;
        param.DescriptorTable.pDescriptorRanges = ranges;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        samp.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        return gpu.CreateRootSig(rs);
    }

    bool Cmaa2Effect::Init(Framework& gpu, DXGI_FORMAT rtvFmt)
    {
        m_sceneFmt = rtvFmt;

        // Blits that bridge the scene format (BGRX) and the working color (RGBA8), both directions.
        if (!gpu.CreateTextureFx(k_blitPS, kColorUNorm, 1, &m_blitInRS, &m_blitInPso) ||
            !gpu.CreateTextureFx(k_blitPS, m_sceneFmt, 1, &m_blitOutRS, &m_blitOutPso))
        {
            WLOG_ERROR("wxl-modern-graphic: CMAA2 blit init failed");
            return false;
        }

        m_rootSig = makeComputeRootSig(gpu);
        if (!m_rootSig) { WLOG_ERROR("wxl-modern-graphic: CMAA2 root signature failed"); return false; }

        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC csd = {};
        csd.pArgumentDescs = &arg;
        csd.NumArgumentDescs = 1;
        csd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        if (FAILED(gpu.Device()->CreateCommandSignature(&csd, nullptr, __uuidof(ID3D12CommandSignature), (void**)&m_cmdSig)))
        {
            WLOG_ERROR("wxl-modern-graphic: CMAA2 command signature failed");
            return false;
        }

        // Reassemble the CMAA2 shader once; the quality preset only changes the edge pass.
        std::string header;
        for (int i = 0; i < k_cmaa2_partCount; ++i)
            header += k_cmaa2_parts[i];

        // Untyped RGBA8 store path (works without typed-UAV-store caps), no sRGB conversion.
        auto sourceFor = [&](int preset)
        {
            std::string s;
            s += "#define CMAA2_STATIC_QUALITY_PRESET " + std::to_string(preset) + "\n";
            s += "#define CMAA2_UAV_STORE_TYPED 0\n";
            s += "#define CMAA2_UAV_STORE_CONVERT_TO_SRGB 0\n";
            s += "#define CMAA2_UAV_STORE_TYPED_UNORM_FLOAT 1\n";
            s += "#define CMAA2_UAV_STORE_UNTYPED_FORMAT 1\n";
            s += header;
            return s;
        };

        // Tiers map to presets: Low = LOW(0), Medium = MEDIUM(1), High = HIGH(2). Only edge detection uses
        // the threshold, so the other three passes are compiled once (at the HIGH preset).
        static const int kPreset[3] = { 0, 1, 2 };
        for (int tier = 0; tier < 3; ++tier)
        {
            std::string src = sourceFor(kPreset[tier]);
            ID3DBlob* cs = gpu.Compile(src.c_str(), "EdgesColor2x2CS", "cs_5_0");
            if (!cs) return false;
            m_edgesPso[tier] = gpu.CreateComputePSO(m_rootSig, cs);
            cs->Release();
            if (!m_edgesPso[tier]) { WLOG_ERROR("wxl-modern-graphic: CMAA2 edge PSO failed (tier %d)", tier); return false; }
        }

        std::string shared = sourceFor(2);
        struct { const char* entry; ID3D12PipelineState** pso; } passes[] = {
            { "ComputeDispatchArgsCS",   &m_argsPso },
            { "ProcessCandidatesCS",     &m_processPso },
            { "DeferredColorApply2x2CS", &m_deferredPso },
        };
        for (auto& p : passes)
        {
            ID3DBlob* cs = gpu.Compile(shared.c_str(), p.entry, "cs_5_0");
            if (!cs) return false;
            *p.pso = gpu.CreateComputePSO(m_rootSig, cs);
            cs->Release();
            if (!*p.pso) { WLOG_ERROR("wxl-modern-graphic: CMAA2 PSO failed (%s)", p.entry); return false; }
        }

        // 64 zero bytes for the one-time control-buffer clear (a fresh default-heap buffer is uninitialized).
        D3D12_HEAP_PROPERTIES uhp = {};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ub = {};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ub.Width = 64; ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1;
        ub.Format = DXGI_FORMAT_UNKNOWN; ub.SampleDesc.Count = 1; ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(gpu.Device()->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&m_zeroUpload)))
        {
            WLOG_ERROR("wxl-modern-graphic: CMAA2 zero-upload alloc failed");
            return false;
        }
        void* mapped = nullptr;
        m_zeroUpload->Map(0, nullptr, &mapped);
        memset(mapped, 0, 64);
        m_zeroUpload->Unmap(0, nullptr);

        WLOG_INFO("wxl-modern-graphic: CMAA2 effect ready (3 quality tiers)");
        return true;
    }

    void Cmaa2Effect::ensureTargets(Framework& gpu, uint32_t w, uint32_t h)
    {
        if (m_color && m_w == w && m_h == h) return;

        ID3D12Resource* old[] = { m_color, m_edges, m_heads, m_shapeCandidates, m_blendLocs, m_blendItems, m_control, m_indirect };
        for (ID3D12Resource* r : old) if (r) r->Release();
        m_color = m_edges = m_heads = m_shapeCandidates = m_blendLocs = m_blendItems = m_control = m_indirect = nullptr;

        const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        m_color = gpu.CreateTex2D(w, h, kColorTypeless,
                                  (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | uav),
                                  D3D12_RESOURCE_STATE_COMMON);
        m_edges = gpu.CreateTex2D((w + 1) / 2, h, DXGI_FORMAT_R8_UINT, uav, ua);
        m_heads = gpu.CreateTex2D((w + 1) / 2, (h + 1) / 2, DXGI_FORMAT_R32_UINT, uav, ua);

        const uint32_t candidates = w * h / 4;
        const uint32_t blendLocs  = (w * h + 3) / 6;
        const uint32_t blendItems = w * h / 2;
        m_shapeCandidates = gpu.CreateBuffer((uint64_t)candidates * 4, uav, ua);
        m_blendLocs       = gpu.CreateBuffer((uint64_t)blendLocs * 4, uav, ua);
        m_blendItems      = gpu.CreateBuffer((uint64_t)blendItems * 8, uav, ua);
        m_control  = gpu.CreateBuffer(16 * 4, uav, ua);   // 16 uints
        m_indirect = gpu.CreateBuffer(4 * 4, uav, ua);    // 4 uints (dispatch args)

        m_w = w; m_h = h;
        m_controlCleared = false;   // a freshly created control buffer needs the one-time zero
    }

    void Cmaa2Effect::Render(Framework& gpu, const FrameContext& ctx)
    {
        ensureTargets(gpu, ctx.w, ctx.h);
        if (!m_color || !m_control || !m_indirect) return;

        ID3D12GraphicsCommandList* cmd = ctx.cmd;
        const uint32_t w = ctx.w, h = ctx.h;
        const int tier = Tier3();

        // --- Blit the scene into the working color (BGRX -> RGBA8). ---
        gpu.Barrier(m_color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gpu.DrawFullscreen(cmd, m_blitInRS, m_blitInPso, ctx.srcSrv, gpu.Rtv(m_color, kColorUNorm), w, h);
        gpu.Barrier(m_color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // --- Build the CMAA2 descriptor table: [t0, u0..u7] contiguous. ---
        const uint32_t base = gpu.AllocDescriptors(9);
        gpu.SetSRV(base + 0, m_color, kColorUNorm);                                  // t0 input color
        gpu.SetUAV(base + 1, m_color, kColorUAV);                                    // u0 output color (packed)
        gpu.SetUAV(base + 2, m_edges, DXGI_FORMAT_R8_UINT);                          // u1 edges
        gpu.SetStructuredUAV(base + 3, m_shapeCandidates, 4, w * h / 4);             // u2
        gpu.SetStructuredUAV(base + 4, m_blendLocs, 4, (w * h + 3) / 6);             // u3
        gpu.SetStructuredUAV(base + 5, m_blendItems, 8, w * h / 2);                  // u4
        gpu.SetUAV(base + 6, m_heads, DXGI_FORMAT_R32_UINT);                         // u5
        gpu.SetRawUAV(base + 7, m_control, 16);                                      // u6
        gpu.SetRawUAV(base + 8, m_indirect, 4);                                      // u7

        // Descriptor heap is bound once per frame in BeginFrame.
        cmd->SetComputeRootSignature(m_rootSig);
        cmd->SetComputeRootDescriptorTable(0, gpu.GpuHandle(base));

        // --- One-time control-buffer zero (steady-state self-clears via the second args dispatch). ---
        if (!m_controlCleared)
        {
            gpu.Barrier(m_control, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(m_control, 0, m_zeroUpload, 0, 64);
            gpu.Barrier(m_control, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_controlCleared = true;
        }

        // --- Pass 1: edge detection. ---
        cmd->SetPipelineState(m_edgesPso[tier]);
        cmd->Dispatch((w + kEdgeDispatchStep - 1) / kEdgeDispatchStep, (h + kEdgeDispatchStep - 1) / kEdgeDispatchStep, 1);
        gpu.UavBarrier(nullptr);

        // --- Pass 2: indirect args for candidate processing. ---
        cmd->SetPipelineState(m_argsPso);
        cmd->Dispatch(2, 1, 1);
        gpu.Barrier(m_indirect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        gpu.UavBarrier(m_control);

        // --- Pass 3: process candidates (indirect dispatch). ---
        cmd->SetPipelineState(m_processPso);
        cmd->ExecuteIndirect(m_cmdSig, 1, m_indirect, 0, nullptr, 0);
        gpu.Barrier(m_indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.UavBarrier(nullptr);

        // --- Pass 4: indirect args for deferred color apply (also clears counters for next frame). ---
        cmd->SetPipelineState(m_argsPso);
        cmd->Dispatch(1, 2, 1);
        gpu.Barrier(m_indirect, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        gpu.UavBarrier(m_control);

        // --- Pass 5: deferred color apply writes the color UAV (indirect dispatch). ---
        gpu.Barrier(m_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetPipelineState(m_deferredPso);
        cmd->ExecuteIndirect(m_cmdSig, 1, m_indirect, 0, nullptr, 0);
        gpu.Barrier(m_indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.UavBarrier(m_color);

        // --- Blit the antialiased color back into the scene target (RGBA8 -> BGRX). ---
        gpu.Barrier(m_color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const uint32_t outSlot = gpu.AllocDescriptors(1);
        gpu.SetSRV(outSlot, m_color, kColorUNorm);
        gpu.DrawFullscreen(cmd, m_blitOutRS, m_blitOutPso, gpu.GpuHandle(outSlot), ctx.dstRtv, w, h);
        gpu.Barrier(m_color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
}
