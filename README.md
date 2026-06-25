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
- **Cycle** between open overlays with a **stick click** — left goes back, right
  goes forward (while pointing off the panel).
- **Reposition** by holding **grip** off the panel and moving your hand; push
  that hand's thumbstick up/down to move it farther/closer. (On by default;
  toggle under Settings > Interaction.)
- All binds are rebindable in the settings, saved to
  `Data/SKSE/Plugins/ImGuiVRHelper.toml`.

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

  HUD-mode clients bypass focus gating and are composited at a fixed, HMD-relative depth (~1.5m forward).

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
