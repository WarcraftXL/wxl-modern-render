// wxl-modern-graphic: D3D12 helper backing the post-process effects.
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

#include "Framework.hpp"

#include "core/Logger.hpp"

#include <windows.h>
#include <d3dcompiler.h>
#include <cstring>

namespace wxl::scripts::render_modern
{
    // Fullscreen triangle: emits one oversized triangle covering the viewport, with a UV that spans 0..1.
    static const char* k_fullscreenVS =
        "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {\n"
        "  uv = float2((id << 1) & 2, id & 2);\n"
        "  pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n";

    bool Framework::Init(ID3D12Device* device)
    {
        m_device = device;

        // Our own queue: never submit mid-frame work onto On12's internal queue (it deadlocks under load).
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&m_queue))) return false;

        if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&m_alloc))) return false;
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&m_list))) return false;
        m_list->Close();

        if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&m_fence))) return false;
        m_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

        m_vs = Compile(k_fullscreenVS, "main", "vs_5_0");
        if (!m_vs) return false;

        m_srvCap = 256;
        m_srvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_DESCRIPTOR_HEAP_DESC sh = {};
        sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        sh.NumDescriptors = m_srvCap;
        sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(m_device->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&m_srvHeap))) return false;

        m_rtvCap = 64;
        m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_DESCRIPTOR_HEAP_DESC rh = {};
        rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rh.NumDescriptors = m_rtvCap;
        if (FAILED(m_device->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&m_rtvHeap))) return false;

        WLOG_INFO("wxl-modern-graphic: GPU framework init OK");
        return true;
    }

    void Framework::WaitForGpu()
    {
        if (m_fence->GetCompletedValue() < m_fenceVal)
        {
            m_fence->SetEventOnCompletion(m_fenceVal, m_event);
            // A healthy post-process frame completes in well under a second. If the wait does not return
            // promptly the GPU has likely hung or been removed (a TDR under max load -- all effects at once),
            // which presents as a frozen image while the rest of the game keeps running. Log the
            // device-removed reason once so the intermittent freeze can be diagnosed, then keep waiting so the
            // behavior is otherwise unchanged. removedReason 0 = no removal (a pure scheduling stall) vs e.g.
            // DXGI_ERROR_DEVICE_HUNG/REMOVED/RESET = a real GPU fault.
            if (WaitForSingleObject(m_event, 2000) == WAIT_TIMEOUT)
            {
                const HRESULT rr = m_device ? m_device->GetDeviceRemovedReason() : 0;
                WLOG_ERROR("wxl-modern-graphic: GPU wait > 2s (fence %llu/%llu) removedReason=0x%08X",
                           (unsigned long long)m_fence->GetCompletedValue(),
                           (unsigned long long)m_fenceVal, (unsigned)rr);
                WaitForSingleObject(m_event, INFINITE);
            }
        }
    }

    ID3D12GraphicsCommandList* Framework::BeginFrame()
    {
        WaitForGpu();
        m_alloc->Reset();
        m_list->Reset(m_alloc, nullptr);
        m_srvHead = 0;
        m_rtvHead = 0;
        // Bind the shader-visible descriptor heap once for the whole frame: it never changes within a frame
        // (there is only one CBV/SRV/UAV heap), so every pass shares this bind and the effects no longer each
        // re-issue SetDescriptorHeaps.
        ID3D12DescriptorHeap* heaps[] = { m_srvHeap };
        m_list->SetDescriptorHeaps(1, heaps);
        return m_list;
    }

    void Framework::EndFrame(ID3D12Fence** outFence, uint64_t* outValue)
    {
        m_list->Close();
        ID3D12CommandList* lists[] = { m_list };
        m_queue->ExecuteCommandLists(1, lists);
        m_fenceVal++;
        m_queue->Signal(m_fence, m_fenceVal);
        *outFence = m_fence;
        *outValue = m_fenceVal;
    }

    void Framework::Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        m_list->ResourceBarrier(1, &b);
    }

    void Framework::UavBarrier(ID3D12Resource* res)
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = res;
        m_list->ResourceBarrier(1, &b);
    }

    ID3D12Resource* Framework::CreateBuffer(uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ID3D12Resource* res = nullptr;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, __uuidof(ID3D12Resource), (void**)&res)))
            return nullptr;
        return res;
    }

    ID3D12Resource* Framework::CreateTex2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;
        ID3D12Resource* res = nullptr;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initState, nullptr, __uuidof(ID3D12Resource), (void**)&res)))
            return nullptr;
        return res;
    }

    ID3D12Resource* Framework::CreateTex2DFromMips(uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t mipCount, const void* const* mipData, const uint32_t* mipBytes)
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
        rd.MipLevels = (UINT16)mipCount; rd.Format = fmt; rd.SampleDesc.Count = 1;
        ID3D12Resource* tex = nullptr;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&tex)))
            return nullptr;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16] = {};
        UINT rows[16] = {};
        UINT64 rowSize[16] = {};
        UINT64 total = 0;
        m_device->GetCopyableFootprints(&rd, 0, mipCount, 0, fp, rows, rowSize, &total);

        D3D12_HEAP_PROPERTIES uhp = {};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ub = {};
        ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ub.Width = total; ub.Height = 1; ub.DepthOrArraySize = 1; ub.MipLevels = 1;
        ub.Format = DXGI_FORMAT_UNKNOWN; ub.SampleDesc.Count = 1; ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* upload = nullptr;
        if (FAILED(m_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ub, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&upload)))
        {
            tex->Release();
            return nullptr;
        }

        BYTE* mapped = nullptr;
        upload->Map(0, nullptr, (void**)&mapped);
        for (uint32_t m = 0; m < mipCount; ++m)
        {
            const BYTE* src = (const BYTE*)mipData[m];
            BYTE* dst = mapped + fp[m].Offset;
            for (UINT r = 0; r < rows[m]; ++r)
                memcpy(dst + r * fp[m].Footprint.RowPitch, src + r * rowSize[m], (size_t)rowSize[m]);
        }
        upload->Unmap(0, nullptr);

        m_alloc->Reset();
        m_list->Reset(m_alloc, nullptr);
        for (uint32_t m = 0; m < mipCount; ++m)
        {
            D3D12_TEXTURE_COPY_LOCATION d = {};
            d.pResource = tex; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION s = {};
            s.pResource = upload; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = fp[m];
            m_list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_list->Close();
        ID3D12CommandList* lists[] = { m_list };
        m_queue->ExecuteCommandLists(1, lists);
        m_fenceVal++;
        m_queue->Signal(m_fence, m_fenceVal);
        m_fence->SetEventOnCompletion(m_fenceVal, m_event);
        WaitForSingleObject(m_event, INFINITE);

        upload->Release();
        WLOG_INFO("wxl-modern-graphic: texture uploaded %ux%u mips=%u", w, h, mipCount);
        (void)mipBytes;
        return tex;
    }

    ID3DBlob* Framework::Compile(const char* src, const char* entry, const char* target)
    {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &err)))
        {
            if (err) WLOG_ERROR("wxl-modern-graphic: shader %s: %s", entry, (const char*)err->GetBufferPointer());
            if (err) err->Release();
            return nullptr;
        }
        if (err) err->Release();
        return code;
    }

    ID3D12RootSignature* Framework::CreateRootSig(const D3D12_ROOT_SIGNATURE_DESC& desc)
    {
        ID3DBlob* sig = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            if (err) WLOG_ERROR("wxl-modern-graphic: rootsig: %s", (const char*)err->GetBufferPointer());
            return nullptr;
        }
        ID3D12RootSignature* rs = nullptr;
        m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&rs);
        sig->Release();
        if (err) err->Release();
        return rs;
    }

    ID3D12PipelineState* Framework::CreateFullscreenPSO(ID3D12RootSignature* rootSig, ID3DBlob* ps, DXGI_FORMAT rtvFmt, uint32_t sampleCount, bool additive)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.VS = { m_vs->GetBufferPointer(), m_vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        if (additive)
        {
            pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
            pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
            pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        }
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = rtvFmt;
        pd.SampleDesc.Count = sampleCount;
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(m_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso)))
            WLOG_ERROR("wxl-modern-graphic: CreateGraphicsPipelineState failed");
        return pso;
    }

    bool Framework::CreateTextureFx(const char* psSrc, DXGI_FORMAT rtvFmt, uint32_t sampleCount, ID3D12RootSignature** outRootSig, ID3D12PipelineState** outPso)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samp.MaxLOD = D3D12_FLOAT32_MAX;

        D3D12_ROOT_SIGNATURE_DESC rs = {};
        rs.NumParameters = 1;
        rs.pParameters = &param;
        rs.NumStaticSamplers = 1;
        rs.pStaticSamplers = &samp;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        *outRootSig = CreateRootSig(rs);
        if (!*outRootSig) return false;

        ID3DBlob* ps = Compile(psSrc, "main", "ps_5_0");
        if (!ps) return false;
        *outPso = CreateFullscreenPSO(*outRootSig, ps, rtvFmt, sampleCount);
        ps->Release();
        return *outPso != nullptr;
    }

    void Framework::DrawFullscreen(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* rootSig, ID3D12PipelineState* pso, D3D12_GPU_DESCRIPTOR_HANDLE srcSrv, D3D12_CPU_DESCRIPTOR_HANDLE dstRtv, uint32_t w, uint32_t h)
    {
        // The descriptor heap is bound once per frame in BeginFrame; no per-draw SetDescriptorHeaps needed.
        cmd->SetGraphicsRootSignature(rootSig);
        cmd->SetGraphicsRootDescriptorTable(0, srcSrv);
        cmd->SetPipelineState(pso);
        cmd->OMSetRenderTargets(1, &dstRtv, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        cmd->RSSetViewports(1, &vp);
        D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
        cmd->RSSetScissorRects(1, &sc);

        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawInstanced(3, 1, 0, 0);
    }

    ID3D12PipelineState* Framework::CreateComputePSO(ID3D12RootSignature* rootSig, ID3DBlob* cs)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rootSig;
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        ID3D12PipelineState* pso = nullptr;
        if (FAILED(m_device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso)))
            WLOG_ERROR("wxl-modern-graphic: CreateComputePipelineState failed");
        return pso;
    }

    uint32_t Framework::AllocDescriptors(uint32_t count)
    {
        uint32_t base = m_srvHead;
        m_srvHead += count;
        return base;
    }

    void Framework::SetSRV(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t planeSlice)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = fmt;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = (UINT)-1;   // all mips
        d.Texture2D.PlaneSlice = planeSlice;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)slot * m_srvStride;
        m_device->CreateShaderResourceView(res, &d, h);
    }

    void Framework::SetUAV(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = fmt;
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)slot * m_srvStride;
        m_device->CreateUnorderedAccessView(res, nullptr, &d, h);
    }

    void Framework::SetStructuredUAV(uint32_t slot, ID3D12Resource* res, uint32_t stride, uint32_t numElements)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = numElements;
        d.Buffer.StructureByteStride = stride;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)slot * m_srvStride;
        m_device->CreateUnorderedAccessView(res, nullptr, &d, h);
    }

    void Framework::SetRawUAV(uint32_t slot, ID3D12Resource* res, uint32_t numUints)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = numUints;
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)slot * m_srvStride;
        m_device->CreateUnorderedAccessView(res, nullptr, &d, h);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Framework::GpuHandle(uint32_t slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += (UINT64)slot * m_srvStride;
        return h;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Framework::Rtv(ID3D12Resource* res, DXGI_FORMAT fmt)
    {
        if (m_rtvHead >= m_rtvCap) m_rtvHead = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (size_t)m_rtvHead++ * m_rtvStride;
        // Explicit format so the view works on a typeless resource (a null desc cannot infer one).
        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = fmt;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(res, &rd, h);
        return h;
    }
}
