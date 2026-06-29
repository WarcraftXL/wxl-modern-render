// wxl-modern-graphic: screen-space ambient occlusion (SAO + GTAO) over the world depth.
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

#include "effects/ao/AoEffect.hpp"
#include "gpu/Framework.hpp"
#include "core/Logger.hpp"

#include "imgui.h"

#include <cmath>

namespace wxl::scripts::render_modern
{
    namespace
    {
        // SAO: view-space position + normal reconstructed from depth, spiral occlusion sampling.
        const char* k_sao =
            "Texture2D<float> depth : register(t0);\n"
            "RWTexture2D<float> ao : register(u0);\n"
            "SamplerState smp : register(s0);\n"
            "cbuffer C : register(b0) {\n"
            "  float4 c0;\n"   // resX, resY, invX, invY
            "  float4 c1;\n"   // radius, intensity, bias, pad
            "  float4 c2;\n"   // proj00, proj11, proj22, proj32
            "  uint4  c3;\n"   // numSamples
            "};\n"
            "float linz(float d){ return c2.w/(d - c2.z); }\n"
            "float3 viewPos(float2 uv, float d){\n"
            "  float ze = linz(d);\n"
            "  float2 ndc = uv*2.0-1.0; ndc.y = -ndc.y;\n"
            "  return float3(ndc.x*ze/c2.x, ndc.y*ze/c2.y, ze);\n"
            "}\n"
            "float hash12(float2 p){ return frac(sin(dot(p, float2(12.9898,78.233)))*43758.5453); }\n"
            "[numthreads(8,8,1)]\n"
            "void main(uint3 tid : SV_DispatchThreadID){\n"
            "  uint2 px = tid.xy;\n"
            "  if (px.x >= (uint)c0.x || px.y >= (uint)c0.y) return;\n"
            "  float2 uv = ((float2)px+0.5)*c0.zw;\n"
            "  float dc = depth.SampleLevel(smp, uv, 0);\n"
            "  if (dc >= 0.9995){ ao[px] = 1.0; return; }\n"
            "  float3 P = viewPos(uv, dc);\n"
            "  float3 Px = viewPos(uv+float2(c0.z,0), depth.SampleLevel(smp, uv+float2(c0.z,0), 0));\n"
            "  float3 Py = viewPos(uv+float2(0,c0.w), depth.SampleLevel(smp, uv+float2(0,c0.w), 0));\n"
            "  float3 N = normalize(cross(Px-P, Py-P));\n"
            "  if (dot(N, -P) < 0.0) N = -N;\n"
            "  float radius = c1.x, intensity = c1.y, bias = c1.z; uint ns = c3.x;\n"
            "  float uvR = min(0.5*radius*c2.y/P.z, 0.08);\n"
            "  float rnd = hash12(uv*c0.xy)*6.2831853;\n"
            "  float sum = 0.0;\n"
            "  [loop] for (uint i=0;i<ns;i++){\n"
            "    float ang = rnd + (float)i*2.39996323;\n"
            "    float r = uvR*sqrt(((float)i+0.5)/(float)ns);\n"
            "    float2 off = float2(cos(ang)*c2.x/c2.y, sin(ang))*r;\n"
            "    float2 suv = uv+off;\n"
            "    float3 Ps = viewPos(suv, depth.SampleLevel(smp, suv, 0));\n"
            "    float3 v = Ps-P;\n"
            "    float vv = dot(v,v);\n"
            "    float vn = dot(v,N);\n"
            "    float falloff = saturate(1.0 - vv/(radius*radius));\n"
            "    sum += max(0.0, vn - bias)*rcp(vv+1e-3)*falloff;\n"
            "  }\n"
            "  ao[px] = saturate(1.0 - (2.0*intensity/(float)ns)*sum);\n"
            "}\n";

        // GTAO (Ground Truth AO, horizon-based) - c3.y slices, c3.z steps.
        const char* k_gtao =
            "Texture2D<float> depth : register(t0);\n"
            "RWTexture2D<float> ao : register(u0);\n"
            "SamplerState smp : register(s0);\n"
            "cbuffer C : register(b0) { float4 c0; float4 c1; float4 c2; uint4 c3; };\n"
            "static const float PI = 3.14159265;\n"
            "static const float HPI = 1.57079633;\n"
            "float linz(float d){ return c2.w/(d - c2.z); }\n"
            "float3 viewPos(float2 uv, float d){\n"
            "  float ze = linz(d); float2 n = uv*2.0-1.0; n.y = -n.y;\n"
            "  return float3(n.x*ze/c2.x, n.y*ze/c2.y, ze);\n"
            "}\n"
            "float hash12(float2 p){ return frac(sin(dot(p, float2(12.9898,78.233)))*43758.5453); }\n"
            "float arc(float h, float n){ return 0.25*(-cos(2.0*h - n) + cos(n) + 2.0*h*sin(n)); }\n"
            "[numthreads(8,8,1)]\n"
            "void main(uint3 tid : SV_DispatchThreadID){\n"
            "  uint2 px = tid.xy;\n"
            "  if (px.x >= (uint)c0.x || px.y >= (uint)c0.y) return;\n"
            "  float2 uv = ((float2)px+0.5)*c0.zw;\n"
            "  float dc = depth.SampleLevel(smp, uv, 0);\n"
            "  if (dc >= 0.9995){ ao[px] = 1.0; return; }\n"
            "  float3 P = viewPos(uv, dc);\n"
            "  float3 Px = viewPos(uv+float2(c0.z,0), depth.SampleLevel(smp, uv+float2(c0.z,0), 0));\n"
            "  float3 Py = viewPos(uv+float2(0,c0.w), depth.SampleLevel(smp, uv+float2(0,c0.w), 0));\n"
            "  float3 N = normalize(cross(Px-P, Py-P)); if (dot(N,-P) < 0.0) N = -N;\n"
            "  float3 V = normalize(-P);\n"
            "  float radius = c1.x, intensity = c1.y;\n"
            "  uint slices = max(c3.y, 1u); uint steps = max(c3.z, 1u);\n"
            "  float uvR = min(0.5*radius*c2.y/P.z, 0.12);\n"
            "  float noise = hash12(uv*c0.xy);\n"
            "  float vis = 0.0;\n"
            "  [loop] for (uint s=0; s<slices; s++){\n"
            "    float phi = (PI/(float)slices)*((float)s + noise);\n"
            "    float2 omega = float2(cos(phi), sin(phi));\n"
            "    float3 axis = normalize(cross(float3(omega,0.0), V));\n"
            "    float3 projN = N - axis*dot(N,axis);\n"
            "    float projLen = length(projN);\n"
            "    if (projLen < 1e-4) continue;\n"
            "    float3 projNn = projN/projLen;\n"
            "    float3 tangent = cross(V, axis);\n"
            "    float nAng = atan2(dot(projNn,tangent), dot(projNn,V));\n"
            "    float cH1 = -1.0, cH2 = -1.0;\n"
            "    [loop] for (uint t=0; t<steps; t++){\n"
            "      float r = ((float)t + noise)/(float)steps;\n"
            "      float2 so = omega*uvR*r;\n"
            "      float2 u1 = uv+so, u2 = uv-so;\n"
            "      float3 d1 = viewPos(u1, depth.SampleLevel(smp,u1,0)) - P;\n"
            "      float3 d2 = viewPos(u2, depth.SampleLevel(smp,u2,0)) - P;\n"
            "      float l1 = length(d1), l2 = length(d2);\n"
            "      float f1 = saturate(1.0 - l1*l1/(radius*radius));\n"
            "      float f2 = saturate(1.0 - l2*l2/(radius*radius));\n"
            "      cH1 = max(cH1, lerp(-1.0, dot(d1,V)/max(l1,1e-4), f1));\n"
            "      cH2 = max(cH2, lerp(-1.0, dot(d2,V)/max(l2,1e-4), f2));\n"
            "    }\n"
            "    float h1 = nAng + max(-acos(clamp(cH1,-1,1)) - nAng, -HPI);\n"
            "    float h2 = nAng + min( acos(clamp(cH2,-1,1)) - nAng,  HPI);\n"
            "    vis += projLen * (arc(h1,nAng) + arc(h2,nAng));\n"
            "  }\n"
            "  vis = saturate(vis/(float)slices);\n"
            "  ao[px] = saturate(1.0 - (1.0 - vis)*intensity);\n"
            "}\n";

        // Depth-aware denoise of the raw AO.
        const char* k_blur =
            "Texture2D<float> aoIn : register(t0);\n"
            "Texture2D<float> depth : register(t1);\n"
            "RWTexture2D<float> aoOut : register(u0);\n"
            "SamplerState smp : register(s0);\n"
            "cbuffer C : register(b0) { float4 c0; }\n"   // resX, resY, blurPx, sharp
            "[numthreads(8,8,1)]\n"
            "void main(uint3 tid : SV_DispatchThreadID){\n"
            "  uint2 px = tid.xy;\n"
            "  if (px.x >= (uint)c0.x || px.y >= (uint)c0.y) return;\n"
            "  float2 invRes = float2(1.0/c0.x, 1.0/c0.y);\n"
            "  float2 uv = ((float2)px+0.5)*invRes;\n"
            "  float dc = depth.SampleLevel(smp, uv, 0);\n"
            "  float2 st = invRes*c0.z;\n"
            "  float sum = 0.0, wsum = 0.0;\n"
            "  [unroll] for (int y=-2;y<=2;y++)\n"
            "  [unroll] for (int x=-2;x<=2;x++){\n"
            "    float2 o = float2(x,y)*st;\n"
            "    float a = aoIn.SampleLevel(smp, uv+o, 0);\n"
            "    float dd = depth.SampleLevel(smp, uv+o, 0);\n"
            "    float w = exp(-abs(dd-dc)*c0.w);\n"
            "    sum += a*w; wsum += w;\n"
            "  }\n"
            "  aoOut[px] = sum/max(wsum, 1e-4);\n"
            "}\n";

        // Composite: scene * AO. c.x < -0.5 passes the scene through untouched (when no readable depth).
        const char* k_comp =
            "Texture2D       scene : register(t0);\n"
            "Texture2D<float> aoTex : register(t1);\n"
            "SamplerState    smp : register(s0);\n"
            "cbuffer C : register(b0) { float4 c; }\n"   // x: < -0.5 passthrough else composite; y: power
            "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
            "  if (c.x < -0.5) return float4(scene.Sample(smp, uv).rgb, 1);\n"
            "  float a = pow(saturate(aoTex.Sample(smp, uv).r), c.y);\n"
            "  return float4(scene.Sample(smp, uv).rgb * a, 1);\n"
            "}\n";

        D3D12_STATIC_SAMPLER_DESC LinearClamp(D3D12_SHADER_VISIBILITY vis)
        {
            D3D12_STATIC_SAMPLER_DESC s = {};
            s.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s.ShaderRegister = 0;
            s.ShaderVisibility = vis;
            s.MaxLOD = D3D12_FLOAT32_MAX;
            return s;
        }
    }

    void AoEffect::SetQuality(Quality q)
    {
        IEffect::SetQuality(q);
        // Shared tuned baseline for the SAO tiers (Low/Medium/High); they differ only in sample count.
        // Ultra (GTAO) overrides what it needs and is left at its current values pending further tuning.
        m_intensity = 0.50f;
        m_radius    = 0.30f;
        m_power     = 0.60f;
        m_bias      = 0.040f;
        m_blurPx    = 1.20f;
        m_blurSharp = 10.0f;
        switch (q)
        {
            case Quality::Low:
                m_algo = 0;
                m_samples = 6;
                break; // Low    (SAO)
            case Quality::Medium:
                m_algo = 0;
                m_samples = 12;
                break; // Medium (SAO)
            case Quality::High:
                m_algo = 0;
                m_samples = 16;
                break; // High   (SAO)

            // Ultra (GTAO): horizon-based; tuned intensity/radius/power, blur sharpness uses the shared 10.0.
            case Quality::Ultra:
                m_algo = 1;
                m_samples = 36;
                m_intensity = 0.45f;
                m_radius = 0.5f;
                m_power = 1.30f;
                m_blurPx = 2.0f;
                break;
        }
    }

    bool AoEffect::Init(Framework& gpu, DXGI_FORMAT rtvFmt)
    {
        // Compute (AO): SRV(depth) table, UAV(ao) table, 16 root constants, sampler.
        D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE uav = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER cp[3] = {};
        cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        cp[0].DescriptorTable.NumDescriptorRanges = 1;
        cp[0].DescriptorTable.pDescriptorRanges = &srv;
        cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        cp[1].DescriptorTable.NumDescriptorRanges = 1;
        cp[1].DescriptorTable.pDescriptorRanges = &uav;
        cp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        cp[2].Constants.ShaderRegister = 0;
        cp[2].Constants.Num32BitValues = 16;
        D3D12_STATIC_SAMPLER_DESC csmp = LinearClamp(D3D12_SHADER_VISIBILITY_ALL);
        D3D12_ROOT_SIGNATURE_DESC crs = {};
        crs.NumParameters = 3; crs.pParameters = cp;
        crs.NumStaticSamplers = 1; crs.pStaticSamplers = &csmp;
        m_aoRootSig = gpu.CreateRootSig(crs);
        if (!m_aoRootSig) return false;
        if (ID3DBlob* cs = gpu.Compile(k_sao, "main", "cs_5_0")) { m_saoPso = gpu.CreateComputePSO(m_aoRootSig, cs); cs->Release(); }
        if (ID3DBlob* gcs = gpu.Compile(k_gtao, "main", "cs_5_0")) { m_gtaoPso = gpu.CreateComputePSO(m_aoRootSig, gcs); gcs->Release(); }
        if (!m_saoPso || !m_gtaoPso) { WLOG_ERROR("wxl-modern-graphic: SSAO compute PSO failed"); return false; }

        // Compute (blur): SRV(ao), SRV(depth), UAV(out), 4 root constants, sampler.
        D3D12_DESCRIPTOR_RANGE bAo = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE bDepth = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_DESCRIPTOR_RANGE bOut = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
        D3D12_ROOT_PARAMETER bp[4] = {};
        bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        bp[0].DescriptorTable.NumDescriptorRanges = 1; bp[0].DescriptorTable.pDescriptorRanges = &bAo;
        bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        bp[1].DescriptorTable.NumDescriptorRanges = 1; bp[1].DescriptorTable.pDescriptorRanges = &bDepth;
        bp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        bp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        bp[2].DescriptorTable.NumDescriptorRanges = 1; bp[2].DescriptorTable.pDescriptorRanges = &bOut;
        bp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        bp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        bp[3].Constants.ShaderRegister = 0; bp[3].Constants.Num32BitValues = 4;
        D3D12_STATIC_SAMPLER_DESC bsmp = LinearClamp(D3D12_SHADER_VISIBILITY_ALL);
        D3D12_ROOT_SIGNATURE_DESC brs = {};
        brs.NumParameters = 4; brs.pParameters = bp;
        brs.NumStaticSamplers = 1; brs.pStaticSamplers = &bsmp;
        m_blurRootSig = gpu.CreateRootSig(brs);
        if (!m_blurRootSig) return false;
        if (ID3DBlob* bcs = gpu.Compile(k_blur, "main", "cs_5_0")) { m_blurPso = gpu.CreateComputePSO(m_blurRootSig, bcs); bcs->Release(); }
        if (!m_blurPso) { WLOG_ERROR("wxl-modern-graphic: SSAO blur PSO failed"); return false; }

        // Composite (pixel): SRV(scene), SRV(ao), 4 root constants, sampler.
        D3D12_DESCRIPTOR_RANGE s0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
        D3D12_DESCRIPTOR_RANGE s1 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0 };
        D3D12_ROOT_PARAMETER pp[3] = {};
        pp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        pp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        pp[0].DescriptorTable.NumDescriptorRanges = 1; pp[0].DescriptorTable.pDescriptorRanges = &s0;
        pp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        pp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        pp[1].DescriptorTable.NumDescriptorRanges = 1; pp[1].DescriptorTable.pDescriptorRanges = &s1;
        pp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        pp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        pp[2].Constants.ShaderRegister = 0; pp[2].Constants.Num32BitValues = 4;
        D3D12_STATIC_SAMPLER_DESC psmp = LinearClamp(D3D12_SHADER_VISIBILITY_PIXEL);
        D3D12_ROOT_SIGNATURE_DESC prs = {};
        prs.NumParameters = 3; prs.pParameters = pp;
        prs.NumStaticSamplers = 1; prs.pStaticSamplers = &psmp;
        prs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        m_compRootSig = gpu.CreateRootSig(prs);
        if (!m_compRootSig) return false;
        ID3DBlob* ps = gpu.Compile(k_comp, "main", "ps_5_0");
        if (!ps) return false;
        m_compPso = gpu.CreateFullscreenPSO(m_compRootSig, ps, rtvFmt, 1);
        ps->Release();
        if (!m_compPso) return false;

        SetQuality(GetQuality());
        WLOG_INFO("wxl-modern-graphic: SSAO effect ready (SAO + GTAO/Ultra)");
        return true;
    }

    void AoEffect::DrawTuning()
    {
        ImGui::Indent();
        ImGui::TextDisabled(m_algo == 1 ? "Mode: GTAO (horizon, Ultra)" : "Mode: SAO (spiral)");

        // Live tuning. Changing the quality tier resets these to that tier's baseline; tweak from there.
        ImGui::SliderFloat("Intensity", &m_intensity, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Radius",    &m_radius,    0.1f, 5.0f, "%.2f");
        ImGui::SliderFloat("Power",     &m_power,     0.5f, 4.0f, "%.2f");
        if (m_algo == 0)
            ImGui::SliderFloat("Bias",   &m_bias,     0.0f, 0.2f, "%.3f");

        int samples = (int)m_samples;
        if (ImGui::SliderInt("Samples", &samples, 4, 64))
            m_samples = (uint32_t)(samples < 1 ? 1 : samples);

        ImGui::SliderFloat("Blur radius",    &m_blurPx,   0.0f, 4.0f,  "%.2f");
        ImGui::SliderFloat("Blur sharpness", &m_blurSharp, 1.0f, 32.0f, "%.1f");
        ImGui::Unindent();
    }

    void AoEffect::ensureAo(Framework& gpu, uint32_t w, uint32_t h)
    {
        if (m_aoTex && m_w == w && m_h == h) return;
        if (m_aoTex)  { m_aoTex->Release();  m_aoTex = nullptr; }
        if (m_aoTexB) { m_aoTexB->Release(); m_aoTexB = nullptr; }
        // Ambient occlusion is low-frequency, so the raw AO is computed at HALF resolution (a quarter of the
        // pixels) -- a large GPU saving on the heaviest GTAO/Ultra tier -- and the depth-aware denoise then
        // upsamples it to full resolution while it filters (the blur samples the AO by UV, so it bilinearly
        // upscales for free). m_aoTex is the half-res raw AO; m_aoTexB is the full-res denoised + upsampled AO.
        m_aoHalfW = (w + 1) / 2;
        m_aoHalfH = (h + 1) / 2;
        m_aoTex  = gpu.CreateTex2D(m_aoHalfW, m_aoHalfH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        m_aoTexB = gpu.CreateTex2D(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        m_aoState = m_aoStateB = D3D12_RESOURCE_STATE_COMMON;
        m_w = w; m_h = h;
    }

    void AoEffect::Render(Framework& gpu, const FrameContext& ctx)
    {
        ID3D12GraphicsCommandList* cmd = ctx.cmd;
        // Descriptor heap is bound once per frame in BeginFrame.

        D3D12_VIEWPORT vp = { 0, 0, (float)ctx.w, (float)ctx.h, 0, 1 };
        D3D12_RECT sc = { 0, 0, (LONG)ctx.w, (LONG)ctx.h };

        // No readable depth (no SSAA / MSAA frame): passthrough so AO does not darken a flat scene.
        if (!ctx.depthSrv.ptr)
        {
            float passthrough[4] = { -1.0f, 1.0f, 0, 0 };
            cmd->SetGraphicsRootSignature(m_compRootSig);
            cmd->SetGraphicsRootDescriptorTable(0, ctx.srcSrv);
            cmd->SetGraphicsRootDescriptorTable(1, ctx.srcSrv);
            cmd->SetGraphicsRoot32BitConstants(2, 4, passthrough, 0);
            cmd->SetPipelineState(m_compPso);
            cmd->OMSetRenderTargets(1, &ctx.dstRtv, FALSE, nullptr);
            cmd->RSSetViewports(1, &vp);
            cmd->RSSetScissorRects(1, &sc);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->DrawInstanced(3, 1, 0, 0);
            return;
        }

        ensureAo(gpu, ctx.w, ctx.h);

        auto trans = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES s) {
            if (cur != s) { gpu.Barrier(r, cur, s); cur = s; }
        };
        const D3D12_RESOURCE_STATES NPS = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // Projection terms from WoW's live matrix (D3D row-major, perspective). proj[11] ~ 1 in-world.
        float p00 = 1.0f, p11 = 1.0f, p22 = 1.0f, p32 = -1.0f;
        if (ctx.proj && ctx.proj[11] > 0.5f)
        {
            p00 = ctx.proj[0]; p11 = ctx.proj[5]; p22 = ctx.proj[10]; p32 = ctx.proj[14];
        }

        // AO compute -> m_aoTex, at HALF resolution. The shader works in UV space (it samples the full-res
        // depth by UV and the sample radius is in UV), so running it at half res just lowers the AO's spatial
        // frequency -- imperceptible for occlusion -- while doing a quarter of the work. The denoise then
        // upsamples it back to full res. The cbuffer resolution and the dispatch use the half dimensions.
        const uint32_t aw = m_aoHalfW, ah = m_aoHalfH;
        trans(m_aoTex, m_aoState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t uavSlot = gpu.AllocDescriptors(1);
        gpu.SetUAV(uavSlot, m_aoTex, DXGI_FORMAT_R32_FLOAT);

        struct { float c0[4]; float c1[4]; float c2[4]; uint32_t c3[4]; } c;
        c.c0[0] = (float)aw; c.c0[1] = (float)ah; c.c0[2] = 1.0f/aw; c.c0[3] = 1.0f/ah;
        c.c1[0] = m_radius; c.c1[1] = m_intensity; c.c1[2] = m_bias; c.c1[3] = 0;
        c.c2[0] = p00; c.c2[1] = p11; c.c2[2] = p22; c.c2[3] = p32;
        uint32_t ns = m_samples ? m_samples : 1;
        uint32_t slices = (uint32_t)(sqrtf((float)ns) + 0.5f); if (slices < 1) slices = 1;
        uint32_t steps = (ns + slices - 1) / slices; if (steps < 1) steps = 1;
        c.c3[0] = ns; c.c3[1] = slices; c.c3[2] = steps; c.c3[3] = 0;

        cmd->SetComputeRootSignature(m_aoRootSig);
        cmd->SetComputeRootDescriptorTable(0, ctx.depthSrv);
        cmd->SetComputeRootDescriptorTable(1, gpu.GpuHandle(uavSlot));
        cmd->SetComputeRoot32BitConstants(2, 16, &c, 0);
        cmd->SetPipelineState(m_algo > 0 ? m_gtaoPso : m_saoPso);
        cmd->Dispatch((aw + 7) / 8, (ah + 7) / 8, 1);

        // Depth-aware denoise: m_aoTex -> m_aoTexB.
        trans(m_aoTex, m_aoState, NPS);
        trans(m_aoTexB, m_aoStateB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t aoInSlot = gpu.AllocDescriptors(1);
        gpu.SetSRV(aoInSlot, m_aoTex, DXGI_FORMAT_R32_FLOAT);
        uint32_t uavBSlot = gpu.AllocDescriptors(1);
        gpu.SetUAV(uavBSlot, m_aoTexB, DXGI_FORMAT_R32_FLOAT);
        float bc[4] = { (float)ctx.w, (float)ctx.h, m_blurPx, m_blurSharp };
        cmd->SetComputeRootSignature(m_blurRootSig);
        cmd->SetComputeRootDescriptorTable(0, gpu.GpuHandle(aoInSlot));
        cmd->SetComputeRootDescriptorTable(1, ctx.depthSrv);
        cmd->SetComputeRootDescriptorTable(2, gpu.GpuHandle(uavBSlot));
        cmd->SetComputeRoot32BitConstants(3, 4, bc, 0);
        cmd->SetPipelineState(m_blurPso);
        cmd->Dispatch((ctx.w + 7) / 8, (ctx.h + 7) / 8, 1);

        trans(m_aoTexB, m_aoStateB, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Composite the denoised AO over the scene.
        uint32_t aoSlot = gpu.AllocDescriptors(1);
        gpu.SetSRV(aoSlot, m_aoTexB, DXGI_FORMAT_R32_FLOAT);
        float comp[4] = { 0.0f, m_power, 0, 0 };
        cmd->SetGraphicsRootSignature(m_compRootSig);
        cmd->SetGraphicsRootDescriptorTable(0, ctx.srcSrv);
        cmd->SetGraphicsRootDescriptorTable(1, gpu.GpuHandle(aoSlot));
        cmd->SetGraphicsRoot32BitConstants(2, 4, comp, 0);
        cmd->SetPipelineState(m_compPso);
        cmd->OMSetRenderTargets(1, &ctx.dstRtv, FALSE, nullptr);
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);

        // Leave AO buffers ready for next frame's UAV write.
        trans(m_aoTex, m_aoState, D3D12_RESOURCE_STATE_COMMON);
        trans(m_aoTexB, m_aoStateB, D3D12_RESOURCE_STATE_COMMON);
    }
}
