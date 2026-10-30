# ImGuiVRHelper

A SKSE plugin that enables ImGui-based mods in VR. [Nexus](https://www.nexusmods.com/skyrimspecialedition/mods/183466)

## Users

ImGuiVRHelper is a framework other mods depend on and support must be added by
the mod author.

- **Open / close** a mod's overlay with that mod's binding, or the helper
  defaults: open with **A/X** or **B/Y** on your off hand, close with **both
  grips**. With no overlay up, the open combo (or **Shift+F4**) opens the
  helper's own settings.
- **Point** a controller at the panel to drive the cursor; **trigger** clicks,
  **thumbstick** scrolls.
- **Cycle & Quick Select** between open overlays with a **stick click** (left
  goes back, right goes forward while pointing off the panel), or **long-hold
  stick click** to open an interactive quick select list. Scroll the list with
  either thumbstick, pull **trigger** or click the stick to select, and press
  **grip** to cancel.
- **Reposition** by holding **grip** off the panel and moving your hand; push
  that hand's thumbstick up/down to move it farther/closer. (On by default;
  toggle under Settings > Interaction.)
- All binds are rebindable in the settings, saved to
  `Data/SKSE/Plugins/ImGuiVRHelper.toml`.

### Native overlay (experimental)

`useRuntimeOverlay = true` composites the focused menu as a SteamVR runtime
overlay layer instead of drawing it into the eye buffers, so temporal upscalers
and frame generation don't smear it. One thread owns the layer: the
IVRCompositor::Submit detour thread that already draws into the submitted eye
texture with the game's D3D11 immediate context. That thread copies the panel
into a helper-owned texture, draws the helper cursor into that copy only, and
makes every overlay call; a tick on any other thread is refused. The in-scene
panel stands down only after every requested anchor accepts its width,
transform and texture for consecutive frames (all anchors for
`attachMode = Both`), so a half-hosted panel never replaces the menu.

Hosting is limited to SteamVR. The helper classifies the module that implements
the clean texture-submission overlay interface, and accepts only the stock
`<runtime>\bin\vrclient_x64.dll` under a runtime root listed in
`openvrpaths.vrpath` whose Win32 version identity is Valve Corporation's VR
Client. A matching file name or registered location alone is not accepted. A
game proxy, an unregistered runtime, a wrapper under a registered root, and the
null/headless driver all keep the in-scene menu with a status that says why.
OpenComposite has no readiness/capability query this helper can verify, so it
also stays in-scene rather than treating a possibly no-op call as presentation.
The two-frame confirmation is bound to the first accepted frame's client,
configuration, tracking origin, anchor mask and texture generation, so a change
cannot inherit a partial streak; both anchors of `attachMode = Both` must
accept every call. A runtime accepting those calls is not proof that the
compositor painted pixels. Failed width/transform/texture/show/hide/clear/destroy
calls keep the in-scene menu, leave cleanup scheduled, and are retried while
graphics ownership is verified; a Present/Submit thread conflict retains the
pending cleanup without calling the runtime and says so in the status.
Repeated refusals latch native hosting off until the setting is toggled while no
handles are pending, and an off/on toggle grants a fresh backend probe. If the
submit tick stops, the helper cannot guarantee removing what the runtime still
shows: it resumes the in-scene menu, says so in the status and log, and keeps
cleanup pending when ownership cannot be verified.
SteamVR presentation on this owner thread still needs real-headset validation;
the null driver does not exercise it.

Helper graphics passes require D3D11.1 context-state isolation. If unavailable,
the helper skips the affected pass rather than modifying unprotected game state.

## Developers

Bring an existing ImGui menu into VR in four steps.

### 1. Depend on the API (`api/`, LGPL-3.0)

CMake — fetch it pinned (recommended; nothing vendored to keep in sync):

```cmake
include(FetchContent)
FetchContent_Declare(
  ImGuiVRHelper
  GIT_REPOSITORY https://github.com/alandtse/imgui-vr-helper.git
  GIT_TAG        v1.0.0          # a release tag or commit
  GIT_SUBMODULES ""              # api/ needs none of the helper's submodules
  SOURCE_SUBDIR  api             # configure only the api/ CMake target
)
FetchContent_MakeAvailable(ImGuiVRHelper)
target_link_libraries(MyMod PRIVATE ImGuiVRHelper::api)
```

`ImGuiVRHelper::api` is an INTERFACE target: it compiles the handshake stub into
your plugin and adds the `api/` headers, inheriting your CommonLibSSE / Dear ImGui /
DX11 backend / nlohmann_json. (You can also vendor `api/*` directly — it's LGPL and
self-contained — but FetchContent keeps the dependency and its version explicit.)

### 2. Connect from your kPostPostLoad handler

```cpp
#include "ImGuiVRHelperClientSDK.h"
ImGuiVRHelperPluginAPI::Client g_vr;

// in your own SKSE message handler, at kPostPostLoad:
g_vr.Connect("MyMod", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus);
```

Call `Connect` from your own SKSE lifecycle handler at **kPostPostLoad** — the
helper's listener is up by then regardless of load order. If the helper isn't
installed, `Connect` returns false and you keep your flat-screen path.

### 3. Drive it each frame

```cpp
g_vr.Update(menuOpen);   // focus reconcile + wand input, in one call
// ... your NewFrame / windows / ImGui::Render() ...
g_vr.RenderFrame();      // VR: flat panel only; flat screen / no helper: your normal draw
```

- **Focus Sync Note:** `Update(menuOpen)` accepts `menuOpen` by reference. The helper will modify this boolean to `false` if the user closes the panel or switches focus to another mod. If your mod runs internal state machine triggers on menu open/close (e.g. pausing the game or locking keyboard/mouse input), you must check if `menuOpen` was mutated by `Update()` and synchronize your managers accordingly (e.g., call `CloseMenu()`).

- **Always-On HUD overlays (HUD Mode):** For rendering a transparent HUD overlay (like crosshairs, widgets, or stats) instead of an interactive menu, connect using the `kClientFlag_HUDMode` flag and call `RenderHud`:

  ```cpp
  // Connect:
  g_vr.Connect("MyHUD", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_HUDMode);

  // Each frame:
  g_vr.RenderHud(device, context, displaySize, []() {
      // standard ImGui HUD draw calls here
  });
  ```

  HUD-mode clients bypass focus gating and are composited at a fixed, HMD-relative depth (~1.5m forward). A client that needs a nearer or wider plane (a visor or helmet frame) can call `SetHudPlane(depthMeters, coverage)` once after `Connect`; coverage above 1 over-fills each eye's view. It is a no-op against a pre-006 helper (`HasHudPlane()` returns false), which keeps the global plane.

- **World-anchored overlays (World-Quad Mode):** For 2D content that should stay pinned in the scene (subtitles, damage numbers, nameplates) rather than ride the head-locked HUD plane, connect with `kClientFlag_WorldQuad`. Draw your text into the client panel as usual, then each frame submit billboards that sample sub-rects of that panel at world positions:

  ```cpp
  // Connect:
  g_vr.Connect("MySubtitles", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_WorldQuad);

  // Each frame, after drawing your panel text (RenderHud / RenderToPanel):
  std::vector<ImGuiVRHelperPluginAPI::WorldQuad> quads;
  // fill u0/v0/u1/v1 (panel UV), pos[3] (Skyrim world-space, game units), height_m
  g_vr.SubmitWorldQuads(quads.data(), quads.size());
  ```

  Each billboard is camera-facing, `height_m` tall (width follows the sub-rect aspect), and per-pixel occluded against the game's scene depth. `pos` is your anchor's Skyrim world-space position (game units, e.g. a node's `world.translate`) — submit it as-is; the helper converts to OpenVR tracking space itself at Submit time, using the same fresh pose it builds the eye projection from. Don't convert to tracking space yourself: a client-side conversion snapshots the play-space origin at an earlier point in the frame, which desyncs from the eye projection's pose once the player is moving and shows up as visible jitter. World-quad clients aren't user-cyclable overlays (no interactive panel). The helper caps the list at 4096 quads per client; submit `count == 0` to clear. `SubmitWorldQuads` is a no-op against a pre-004 helper (`HasWorldQuads()` returns false) — connect as `kClientFlag_HUDMode` instead if you need a head-locked fallback for older helpers.

`RenderFrame()` is the drop-in replacement for a bare
`ImGui_ImplDX11_RenderDrawData(...)`. **Do not also draw your menu into the game's
frame in VR:** a flat menu painted into Skyrim's `kHUDMENU` is wrapped onto the
engine's curved world HUD and comes out sheared and mispositioned. `RenderFrame()`
hides that branch. (If you instead hook at `IDXGISwapChain::Present` — so your
in-game draw lands on the desktop mirror, not the headset — keep your normal draw
_and_ add `RenderToPanel()`; that's how Open Shaders integrates.)

### 4. Combos + a bindings UI (optional)

```cpp
auto open = g_vr.AddCombo("Open menu", keys, [](const auto* k, auto n){ /* persist */ });
if (g_vr.Fired(open)) menuOpen = true;
g_vr.DrawBindingsTable();   // drop-in, rebindable controller-map table for your settings UI
```

The full SDK is `api/ImGuiVRHelperClientSDK.h`. [**Open Shaders**](https://github.com/alandtse/open-shaders) and [**SKSE
Menu Framework**](https://github.com/alandtse/SKSE-Menu-Framework-3) are working reference integrations.

## How it works

- Each client renders into a **helper-owned panel texture** (RTV). The helper
  composites that texture as a flat quad, per eye, with proper stereo convergence —
  never onto the game's curved HUD.
- **Separate ImGui instances.** The helper composites _pixels_, not ABI, so a
  client's ImGui version is irrelevant.
- **Handshake:** the client dispatches one SKSE message to `ImGuiVRHelper`; the
  helper hands back a `GetApiFunction` vtable, and every later call goes through it.
- The helper owns the Present hook and OpenVR access on the correct thread; clients
  never touch OpenVR.

## Build

Requires [xmake](https://xmake.io/) and Visual Studio 2022 (C++ desktop workload).

```sh
git clone --recurse-submodules https://github.com/alandtse/imgui-vr-helper.git
cd imgui-vr-helper
xmake
```

Set `SkyrimVRPluginTargets` (a `;`-separated list of Skyrim VR `Data` dirs or
mod-manager folders) to auto-deploy on every build.

## Licensing

Split so non-GPL client mods can integrate without GPL infection:

- **`src/`** — [GPL-3.0-or-later](COPYING) WITH a [Modding Exception and a
  GPL-3.0 Linking Exception (with Corresponding Source)](EXCEPTIONS.md), where:
  - **Modded Code** — Skyrim and its variants
  - **Modding Libraries** — [SKSE](https://skse.silverlock.org/), CommonLib and variants
- **`api/`** — LGPL-3.0-or-later; the only files a client consumes. Every `api/`
  file carries an LGPL SPDX header, and the directory ships its own
  [COPYING](./api/COPYING) (GPL-3.0, which LGPL-3.0 incorporates) and
  [COPYING.LESSER](./api/COPYING.LESSER) (the LGPL additional permissions) so a
  vendored copy is self-sufficient. The LGPL relinking requirement is satisfied
  because the messaging handshake makes the helper DLL replaceable at runtime.

## Credits

- Handshake pattern from [SkyrimVRESL](https://github.com/Nightfallstorm/SkyrimVRESL),
  itself crediting [HIGGS](https://github.com/adamhynek/higgs).
- VR overlay/input infrastructure ported from my earlier work in
  [Community Shaders](https://github.com/community-shaders/skyrim-community-shaders)
  and [Open Shaders](https://github.com/alandtse/open-shaders).
