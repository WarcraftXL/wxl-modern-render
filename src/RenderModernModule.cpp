// wxl-render-modern: modern graphics pipeline (post-process FX) for the Client.
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

#include "core/Logger.hpp"
#include "events/EventScript.hpp"
#include "game/camera/Camera.hpp"
#include "gpu/Proxy.hpp"
#include "runtime/RenderHooks.hpp"

#include "gpu/Pipeline.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3d9on12.h>

// The live-engine half of the graphics module. The proxy (d3d9.dll) owns the shared D3D12 device + queue and
// runs On12; this module drives a D3D12 post-process pass on its own queue. It runs at the world -> UI
// boundary (OnWorldRenderEnd): the 3D scene is done but the UI has not drawn. The core hands it the finished
// world (in the backbuffer, or in a render-size offscreen surface when supersampling is on) plus a readable
// depth when a depth-using effect asked for one; the module runs the enabled effects and writes the result
// onto the native backbuffer, and the UI then draws crisp on top.
namespace wxl::scripts::render_modern
{
    namespace ev  = wxl::events;
    namespace cam = wxl::game::camera;

    /** @brief Drives the post-process pipeline once per frame from the live device. */
    class RenderModernModule : public ev::EventScript
    {
    public:
        RenderModernModule()
        {
            on<&RenderModernModule::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd);
            WLOG_INFO("wxl-render-modern: loaded (D3D12 post-process pipeline)");
        }

    private:
        IDirect3DDevice9On12* on12_ = nullptr;
        IDirect3DDevice9*     dev9_  = nullptr;

        /**
         * @brief Caches the On12 interface from the live device on first sight.
         * @param device  live D3D9 device from the args.
         * @return true when the On12 interface is available.
         */
        bool EnsureOn12(IDirect3DDevice9* device)
        {
            if (on12_ && dev9_ == device) return true;
            if (on12_) { on12_->Release(); on12_ = nullptr; }
            dev9_ = device;
            if (!device) return false;
            if (FAILED(device->QueryInterface(__uuidof(IDirect3DDevice9On12), (void**)&on12_)) || !on12_)
            {
                WLOG_WARN("wxl-render-modern: QueryInterface(IDirect3DDevice9On12) failed");
                on12_ = nullptr;
                return false;
            }
            WLOG_INFO("wxl-render-modern: On12 acquired (on12=%p)", on12_);
            return true;
        }

        /**
         * @brief Runs the post-process at the world -> UI boundary, on the finished world.
         * @param a  world-render-end args: the live device, the supersample source, and the readable depth.
         */
        void OnWorldRenderEnd(const ev::WorldRenderEndArgs& a)
        {
            // Tell the core whether any enabled effect samples the world depth, so it binds a readable INTZ
            // depth next frame. Independent of supersampling: SSAO gets its depth whether or not SSAA is on.
            bool needDepth = false;
            for (const auto& e : Pipeline::Get().Effects())
                if (e->Enabled() && e->NeedsDepth()) { needDepth = true; break; }
            wxl::runtime::render::SetReadableDepthNeeded(needDepth);

            IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(a.device);
            if (!EnsureOn12(device)) return;
            if (!WxlD3D12Device()) return;

            IDirect3DResource9* superSample = static_cast<IDirect3DResource9*>(a.superSampleSource);

            // With no supersampling and nothing enabled the world is already on the backbuffer: skip the pass
            // entirely. A supersample source must always be downsampled, so it forces the pass on.
            if (!superSample && !Pipeline::Get().HasEnabledEffect()) { WxlD3D12DrainDebug(); return; }

            IDirect3DSurface9* bb = nullptr;
            if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;

            // The pipeline runs on its own queue, separate from the On12 internal queue. depthSource is the
            // sampleable world depth or null. The projection drives the depth-using effects (SSAO): in-world the
            // live world camera global is valid, but on the glue screens it sits at identity, so the glue
            // boundary passes its own projection through a.proj -- use it when provided, else the world global.
            IDirect3DResource9* depth = static_cast<IDirect3DResource9*>(a.depthSource);
            Pipeline::Get().Frame(on12_, WxlD3D12Device(),
                                  static_cast<IDirect3DResource9*>(bb), superSample, depth,
                                  a.proj ? a.proj : cam::Projection(), cam::View());

            bb->Release();
            WxlD3D12DrainDebug();
        }
    };

    // File-scope instance self-registers its handlers at DLL load via the EventScript ctor.
    RenderModernModule g_renderModernModule;
}
