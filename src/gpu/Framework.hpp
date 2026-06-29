// wxl-modern-graphic: D3D12 helper backing the post-process effects (heaps, PSOs, barriers, fullscreen).
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
#include <d3dcommon.h>
#include <cstdint>

namespace wxl::scripts::render_modern
{
    /**
     * @brief Reusable D3D12 backend for the post-process effects.
     *
     * Reuses the proxy's shared device and render queue (it does not create its own), and owns the
     * per-frame command list + fence, the descriptor heaps, and the helpers every effect shares:
     * barriers, textures, shader/PSO compilation, and a fullscreen-triangle draw.
     *
     * Universal-plumbing candidate: once the effect set is proven, this layer is a promotion target for
     * the core (a reusable D3D12 helper any GPU-side module could share).
     */
    class Framework
    {
    public:
        /**
         * @brief Brings up a dedicated queue, command list, fence, fullscreen VS, and descriptor heaps.
         *
         * Creates its OWN command queue on the shared device rather than borrowing On12's internal queue:
         * submitting our mid-frame work to On12's queue races its translation scheduler and deadlocks under
         * load. With a dedicated queue, On12 synchronizes the backbuffer hand-off through the Unwrap/Return
         * fences instead (the same pattern the proxy's present path uses).
         * @param device  shared D3D12 device from the proxy.
         * @return true on success.
         */
        bool Init(ID3D12Device* device);

        ID3D12Device* Device() const { return m_device; }
        /** @brief The dedicated queue our command lists run on (passed to On12 Unwrap/Return for sync). */
        ID3D12CommandQueue* Queue() const { return m_queue; }
        ID3D12GraphicsCommandList* List() const { return m_list; }
        ID3DBlob* FullscreenVS() const { return m_vs; }

        /** @brief Waits for the previous submit, resets recording, clears the transient descriptor rings. */
        ID3D12GraphicsCommandList* BeginFrame();
        /** @brief Closes and executes the list, signals the fence, and hands the fence + value back. */
        void EndFrame(ID3D12Fence** outFence, uint64_t* outValue);
        /** @brief Blocks until the last submitted frame finished, so its referenced resources can be freed. */
        void WaitForGpu();

        void Barrier(ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);
        /** @brief UAV barrier ordering writes; pass null for a global barrier across all UAVs. */
        void UavBarrier(ID3D12Resource* res);

        ID3D12Resource* CreateTex2D(uint32_t w, uint32_t h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState);
        /** @brief Committed default-heap buffer (e.g. UAV scratch); raw/structured views set via SetRawUAV/SetStructuredUAV. */
        ID3D12Resource* CreateBuffer(uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState);
        /** @brief Creates a sampled texture and synchronously uploads its mips; leaves it in PIXEL_SHADER_RESOURCE. */
        ID3D12Resource* CreateTex2DFromMips(uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t mipCount, const void* const* mipData, const uint32_t* mipBytes);

        ID3DBlob* Compile(const char* src, const char* entry, const char* target);
        ID3D12RootSignature* CreateRootSig(const D3D12_ROOT_SIGNATURE_DESC& desc);

        /** @brief Graphics PSO using the shared fullscreen VS plus a pixel shader (additive = ONE/ONE blend). */
        ID3D12PipelineState* CreateFullscreenPSO(ID3D12RootSignature* rootSig, ID3DBlob* ps, DXGI_FORMAT rtvFmt, uint32_t sampleCount = 1, bool additive = false);
        ID3D12PipelineState* CreateComputePSO(ID3D12RootSignature* rootSig, ID3DBlob* cs);

        /** @brief Standard post-FX pass: root sig (1 SRV table + linear-clamp sampler) + fullscreen PSO from a PS source. */
        bool CreateTextureFx(const char* psSrc, DXGI_FORMAT rtvFmt, uint32_t sampleCount, ID3D12RootSignature** outRootSig, ID3D12PipelineState** outPso);
        /** @brief Binds the pass and draws a fullscreen triangle reading srcSrv into dstRtv. */
        void DrawFullscreen(ID3D12GraphicsCommandList* cmd, ID3D12RootSignature* rootSig, ID3D12PipelineState* pso, D3D12_GPU_DESCRIPTOR_HANDLE srcSrv, D3D12_CPU_DESCRIPTOR_HANDLE dstRtv, uint32_t w, uint32_t h);

        /** @brief Transient (per-frame) shader-visible descriptor allocation. */
        uint32_t AllocDescriptors(uint32_t count);
        void SetSRV(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt, uint32_t planeSlice = 0);
        void SetUAV(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt);
        /** @brief Structured-buffer UAV (StructureByteStride = stride, NumElements = count). */
        void SetStructuredUAV(uint32_t slot, ID3D12Resource* res, uint32_t stride, uint32_t numElements);
        /** @brief Raw (ByteAddress) buffer UAV viewing numUints R32 elements. */
        void SetRawUAV(uint32_t slot, ID3D12Resource* res, uint32_t numUints);
        D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t slot) const;
        ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap; }

        /** @brief Transient (per-frame) RTV. */
        D3D12_CPU_DESCRIPTOR_HANDLE Rtv(ID3D12Resource* res, DXGI_FORMAT fmt);

    private:
        ID3D12Device* m_device = nullptr;
        ID3D12CommandQueue* m_queue = nullptr;
        ID3D12CommandAllocator* m_alloc = nullptr;
        ID3D12GraphicsCommandList* m_list = nullptr;
        ID3D12Fence* m_fence = nullptr;
        uint64_t m_fenceVal = 0;
        HANDLE m_event = nullptr;
        ID3DBlob* m_vs = nullptr;

        ID3D12DescriptorHeap* m_srvHeap = nullptr;
        uint32_t m_srvStride = 0, m_srvCap = 0, m_srvHead = 0;
        ID3D12DescriptorHeap* m_rtvHeap = nullptr;
        uint32_t m_rtvStride = 0, m_rtvCap = 0, m_rtvHead = 0;
    };
}
