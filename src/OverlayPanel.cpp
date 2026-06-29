// wxl-render-modern: the dev-overlay panel exposing the post-process effects (enable + quality tier).
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

#include "overlay/Panels.hpp"
#include "gpu/Pipeline.hpp"
#include "gpu/Proxy.hpp"

#include "imgui.h"

// Registers a "Graphics" panel with the dev overlay. The panel is generic over the effect chain: each effect
// exposes an enable toggle and a quality tier, so effects added later appear here automatically. Anti-aliasing
// methods are mutually exclusive; SSAO and supersampling are independent and stack with everything.
namespace wxl::scripts::render_modern
{
    namespace
    {
        const char* const k_qualityNames[] = { "Low", "Medium", "High", "Ultra" };

        void DrawGraphicsPanel()
        {
            const auto& effects = Pipeline::Get().Effects();
            if (effects.empty())
            {
                ImGui::TextDisabled("(no effects registered)");
                return;
            }

            const bool msaa = Pipeline::Get().MsaaActive();

            // Under engine MSAA the world is in a multisampled backbuffer the post-process cannot sample, so the
            // whole pass is bypassed (the engine's own multisampling is the anti-aliasing).
            if (msaa)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Engine MSAA active: post-processing bypassed");
                ImGui::Spacing();
            }

            for (const auto& e : effects)
            {
                ImGui::PushID(e.get());

                bool on = e->Enabled();
                if (ImGui::Checkbox(e->Name(), &on))
                {
                    e->SetEnabled(on);
                    // Anti-aliasing methods are mutually exclusive: enabling one disables the other AA methods.
                    // SSAO is not anti-aliasing, so it is left alone and stacks with whichever AA is on.
                    if (on && e->IsAntiAliasing())
                        for (const auto& other : effects)
                            if (other.get() != e.get() && other->IsAntiAliasing())
                                other->SetEnabled(false);
                }

                int q = static_cast<int>(e->GetQuality());
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.0f);
                // Only effects that define the extra tier (SSAO = GTAO) show Ultra; AA effects stop at High.
                if (ImGui::Combo("##quality", &q, k_qualityNames, e->QualityLevels()))
                    e->SetQuality(static_cast<Quality>(q));

                // Effect-specific live tuning controls (SSAO sliders, ...), shown while the effect is enabled.
                if (on)
                    e->DrawTuning();

                ImGui::PopID();
            }

            // Supersampling: the world renders at a higher resolution and the pipeline downsamples it onto the
            // backbuffer. It is independent of the effects above (each runs at the supersampled resolution) and
            // applies live on the next frame.
            ImGui::Spacing();
            ImGui::TextUnformatted("Supersampling (SSAA)");
            static const char* k_ssaaLabel[]  = { "Off", "x1.5", "x2.0" };
            static const float  k_ssaaFactor[] = { 1.0f, 1.5f, 2.0f };
            const float curFactor = WxlGetSsaaFactor();
            int sel = (curFactor >= 1.99f) ? 2 : (curFactor >= 1.49f) ? 1 : 0;
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("##ssaa", &sel, k_ssaaLabel, IM_ARRAYSIZE(k_ssaaLabel)))
                WxlSetSsaaFactor(k_ssaaFactor[sel]);
        }

        // File-scope registration: adds the panel at DLL load, before the overlay first draws.
        struct PanelRegistrar
        {
            PanelRegistrar() { wxl::overlay::RegisterPanel("Graphics", &DrawGraphicsPanel); }
        } g_panelRegistrar;
    }
}
