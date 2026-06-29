// wxl-modern-graphic: the post-process pipeline. Runs the effect chain at native resolution and resolves to
// the native backbuffer; supersampling is a render-size source the fill downsamples on the way in.
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

#include "Framework.hpp"
#include "Effect.hpp"

#include <vector>
#include <memory>

struct IDirect3DDevice9On12;
struct IDirect3DResource9;

namespace wxl::scripts::render_modern
{
    /**
     * @brief Owns the D3D12 backend and the effect chain, and resolves the result onto the backbuffer.
     *
     * The scene targets are always NATIVE size. When supersampling is on the world arrives in a larger
     * offscreen surface and the scene fill DOWNSAMPLES it into the native scene; with supersampling off the
     * fill is a 1:1 copy of the backbuffer. Either way the effects run at native resolution and the final pass
     * is a 1:1 copy onto the backbuffer. The effects therefore neither know nor care about supersampling: each
     * is an independent stage reading one scene target and writing the next, so any subset stacks (SSAO with or
     * without an anti-aliasing pass, anti-aliasing with or without supersampling).
     */
    class Pipeline
    {
    public:
        static Pipeline& Get();

        /**
         * @brief Runs the post-process for one frame and writes the result onto the native backbuffer.
         * @param on12         the device's On12 interface, for unwrap/return.
         * @param device       the shared D3D12 device.
         * @param backbuffer   the native-size backbuffer surface; always the final output.
         * @param superSample  render-size offscreen world color (supersampling on), or null. When present it
         *                     is the scene input and the fill downsamples it into the native scene.
         * @param depth        sampleable (INTZ) world depth, or null when no depth-using effect is active.
         * @param proj         projection matrix (16 floats), or null.
         * @param view         view matrix (16 floats), or null.
         */
        void Frame(IDirect3DDevice9On12* on12, ID3D12Device* device,
                   IDirect3DResource9* backbuffer, IDirect3DResource9* superSample,
                   IDirect3DResource9* depth, const float* proj, const float* view);

        /** @brief The effect chain, in render order; the overlay reads it to toggle effects by name. */
        const std::vector<std::unique_ptr<IEffect>>& Effects() const { return m_effects; }

        /** @brief True when at least one enabled effect would run this frame (drives the module's skip). */
        bool HasEnabledEffect() const;

        /** @brief True when the last frame's backbuffer was multisampled (engine MSAA on -> AA bypassed). */
        bool MsaaActive() const { return m_lastSamples > 1; }

    private:
        Pipeline();

        bool ensureInit(DXGI_FORMAT fmt, uint32_t samples);
        void ensureTargets(uint32_t w, uint32_t h, DXGI_FORMAT fmt);

        Framework m_gpu;
        bool m_gpuReady = false;   // framework (dedicated queue) is up; set before the first unwrap
        bool m_init = false;       // format-dependent setup (effects + resolve PSO) is up
        std::vector<std::unique_ptr<IEffect>> m_effects;

        // resolve stage: render/native scene -> native backbuffer (downsamples when scene > native)
        ID3D12RootSignature* m_blitRootSig = nullptr;
        ID3D12PipelineState* m_blitPso = nullptr;

        ID3D12Resource* m_scene[2] = { nullptr, nullptr };
        D3D12_RESOURCE_STATES m_sceneState[2] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
        uint32_t m_w = 0, m_h = 0;       // current scene-target size (native; the fill downsamples the input)
        DXGI_FORMAT m_fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t m_lastSamples = 1;      // backbuffer sample count seen last frame (for the overlay's MSAA note)
    };
}
