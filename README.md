# wxl-render-modern

Modern post-process rendering for the Client: a D3D12 pass layered on the engine's native frame through the
d3d9-on-12 proxy. It adds supersampling (SSAA), ambient occlusion (SSAO), and post-process anti-aliasing
(FXAA / SMAA / CMAA2). No native render path is replaced; the engine draws the frame and this module
reprocesses it at the world → UI boundary, so the UI then draws crisp on top.

## What it does

| Effect | Kind | Tiers | Notes |
| --- | --- | --- | --- |
| **SSAA** | Supersampling | Off / x1.5 / x2.0 | Core renders the world into a render-size offscreen surface; the pipeline downsamples it. Driven by the proxy exports `WxlSetSsaaFactor` / `WxlGetSsaaFactor`. |
| **SSAO** | Ambient occlusion | Low / Medium / High / **Ultra** | SAO (spiral) for Low/Medium/High, GTAO (horizon-based) for Ultra. Needs a readable world depth. Works in-world **and** on the glue screens (login / character select). |
| **FXAA** | Anti-aliasing | Low / Medium / High | Cheapest AA. Default of the three. |
| **SMAA** | Anti-aliasing | Low / Medium / High | Pattern-based; High adds diagonal + corner detection. |
| **CMAA2** | Anti-aliasing | Low / Medium / High | Conservative morphological AA. |

The three AA methods are **mutually exclusive** (the overlay keeps one enabled at a time). SSAO and SSAA are
independent and stack with each other and with whichever AA is on. Every effect ships **disabled**: an empty
run is a 1:1 passthrough.

## Approach: downsample on the way in, effects at native resolution

The pipeline keeps a single rule: **the effect chain always runs at native (backbuffer) resolution, and the
supersampling resolve happens on the way *in*, not at the end.**

- **Scene targets are always native size.** With SSAA on, the world arrives in a render-size offscreen
  surface (`native * factor`); the first stage (the *fill*) is a sampled blit that box-filters that larger
  source **down** into the native scene target. With SSAA off there is no larger source — the backbuffer is
  the scene input and the first effect reads it directly (the redundant 1:1 fill copy is skipped).
- **Every effect is an independent native-resolution stage** reading one scene target and writing the next
  (ping-pong over two targets). Because supersampling is fully resolved by the fill, the effects neither know
  nor care about it; any subset stacks. The final output is a 1:1 copy of the last scene target onto the
  native backbuffer.

> The file-level rule used to be the inverse ("run the whole chain at render-size, downsample last"). The
> code now downsamples on the way in (`Pipeline::Frame`, `sceneW = nativeW`); this README and the in-code
> class docs reflect the live behaviour.

## Frame path

1. The core fires **`OnWorldRenderEnd`** (world done, UI not yet drawn) with the live device, the render-size
   supersample source (or null), the SSAA factor, a readable INTZ world depth (or null) when a depth-using
   effect asked for one, and the projection / view matrices.
2. The module reports back, via `SetReadableDepthNeeded`, whether any enabled effect samples depth, so the
   core arms a readable INTZ depth **next** frame. It then unwraps the native backbuffer (final output) and,
   when present, the render-size source and the depth through On12.
3. If SSAA is on, the fill **downsamples** the source into native `scene[0]`; otherwise the backbuffer is the
   chain source. The enabled effects run ping-pong over the two native scene targets.
4. The result is copied (1:1) onto the native backbuffer; the borrowed surfaces are returned to On12 with a
   completion fence, so the engine's later D3D9 use (UI draw, present) waits on our writes.

When the engine's own **MSAA** is on, the world lives in a multisampled backbuffer the post-process cannot
sample, so the whole pass is bypassed (the engine's multisampling is the anti-aliasing). The overlay shows a
note when this happens.

## Effect ordering

`SSAO → FXAA / SMAA / CMAA2`. Ambient occlusion runs **first** so its darkening is itself anti-aliased by the
later AA pass; the anti-aliasing runs last on the otherwise-final image.

## SSAO specifics

- **Two algorithms behind the tiers.** Low/Medium/High use SAO (spiral occlusion sampling, increasing sample
  counts); Ultra uses GTAO (Ground-Truth, horizon-based).
- **Half-resolution AO.** Ambient occlusion is low-frequency, so the raw AO is computed at *half* resolution
  (a quarter of the pixels — a large saving on the heavy GTAO/Ultra tier). The depth-aware denoise then
  upsamples it to full resolution for free while it filters (it samples the AO by UV).
- **Glue screens.** In-world the live world-camera projection is valid, but on the login / character screens
  it sits at identity, so the glue boundary passes its own projection through `OnWorldRenderEnd`; SSAO uses it
  when provided, else the world global.
- **Live tuning.** While enabled, the overlay exposes Intensity / Radius / Power / Bias / Samples / Blur
  radius / Blur sharpness. Changing the quality tier resets these to that tier's baseline.

## The D3D12 backend (`Framework`)

- **Reuses the proxy's shared D3D12 device** but creates its **own** command queue. Submitting mid-frame work
  to On12's internal queue races its translation scheduler and deadlocks under load; with a dedicated queue,
  On12 synchronizes the backbuffer hand-off through the Unwrap/Return fences instead.
- One per-frame command list + fence with full serialization (`WaitForGpu` each frame), transient
  (per-frame) shader-visible SRV and RTV descriptor rings, and the shared helpers every effect uses:
  barriers, texture/buffer creation, shader + PSO compilation, and a fullscreen-triangle draw.

## Core seam (universal vs. feature)

The world-pass redirect and the readable-depth bind are **universal primitives owned by the core** (it hooks
the engine + the D3D9 render-target / depth bind). They are exposed through `OnWorldRenderEnd`'s
`superSampleSource` / `depthSource` / `proj`; the SSAA factor is driven through the proxy exports
`WxlSetSsaaFactor` / `WxlGetSsaaFactor`. Everything above (the D3D12 backend and the effects) is this module's.

## Layout

- `src/RenderModernModule.cpp` — the `EventScript` entry: drives the pipeline once per frame from
  `OnWorldRenderEnd`, and reports the depth requirement back to the core.
- `src/OverlayPanel.cpp` — registers the dev-overlay **"Graphics"** panel: per-effect enable + quality tier,
  the SSAA selector, and (per effect) the live tuning sliders. Enforces the mutually-exclusive AA.
- `src/gpu/` — the D3D12 backend (`Framework`: dedicated queue, per-frame command list + fence, descriptor
  heaps, barriers, texture/PSO helpers, fullscreen draw), the `Pipeline` (unwrap, native chain, resolve), and
  the `IEffect` interface + per-frame `FrameContext`.
- `src/effects/ao/` — SSAO (`AoEffect`: SAO + GTAO, half-res, depth-aware denoise, composite).
- `src/effects/aa/` — the anti-aliasing effects (`FxaaEffect`, `SmaaEffect`, `Cmaa2Effect`).
- `vendor/` — upstream third-party AA sources (FXAA, SMAA, CMAA2), unmodified.
- `module.cmake` — links `d3d12` for the root-signature serializer.

## Adding an effect

1. Implement `IEffect` (`src/gpu/Effect.hpp`): `Name`, `Init` (root sigs + PSOs), `Render` (one pass over the
   scene), and as needed `SetQuality`, `QualityLevels`, `IsAntiAliasing`, `NeedsDepth`, `DrawTuning`.
2. Register it in the `Pipeline` constructor (`src/gpu/Pipeline.cpp`) in render order; ship it disabled.

It then appears in the overlay automatically (the panel is generic over the effect chain), runs at native
resolution like the rest, and — if it returns `NeedsDepth()` — gets a readable world depth supplied by the
core.

## Building

Client-side only, so the default build covers it:

```
.\build.ps1
```

(The game must be **closed** while deploying — the client locks the built DLL.)
