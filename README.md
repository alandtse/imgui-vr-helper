# ImGuiVRHelper

A standalone SKSE plugin that makes any ImGui-based mod interactive in the Skyrim
VR headset — without each mod rebuilding the OpenVR overlay, controller input,
button-combo, and laser-pointer plumbing.

A client mod renders its existing ImGui frame into a helper-owned render target;
the helper composites it as a flat 3D panel in front of the player and feeds back
wand pointing, clicks, scroll, and button combos. The helper never links a
client's ImGui — clients keep their own version forever and talk to the helper
through a small versioned C-ABI obtained over an SKSE-messaging handshake.

## What it does for developers

You already have a working flat-screen ImGui menu. In VR it's invisible — or worse,
smeared onto the game's curved HUD. ImGuiVRHelper turns it into a proper headset
panel for a few lines of integration:

- **Bring your own ImGui.** You render into a helper panel RTV; the helper owns
  OpenVR submission and never shares an ImGui instance with you, so ImGui version
  drift can't break you — it composites pixels, not ABI.
- **VR input, mapped for you.** Wand laser → panel cursor, trigger → click, stick →
  scroll, delivered into your ImGui IO. Register button **combos** (with a built-in
  rebinding table) instead of hand-rolling controller matching.
- **Focus, overlays, HUD.** The helper owns menu open/close/cycle across all
  clients, an always-on HUD-mode layer for persistent overlays, drag-to-reposition,
  and an optional SteamVR dashboard surface.
- **One handshake, then a vtable.** No per-frame messaging — you call a normal C++
  interface.

## Add it to your mod

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

`RenderFrame()` is the drop-in replacement for a bare
`ImGui_ImplDX11_RenderDrawData(...)`. **Do not also draw your menu into the game's
frame in VR:** a flat menu painted into Skyrim's `kHUDMENU` is wrapped onto the
engine's curved world HUD and comes out sheared and mispositioned. `RenderFrame()`
hides that branch. (If you instead hook at `IDXGISwapChain::Present` — so your
in-game draw lands on the desktop mirror, not the headset — keep your normal draw
_and_ add `RenderToPanel()`; that's how Community Shaders integrates.)

### 4. Combos + a bindings UI (optional)

```cpp
auto open = g_vr.AddCombo("Open menu", keys, [](const auto* k, auto n){ /* persist */ });
if (g_vr.Fired(open)) menuOpen = true;
g_vr.DrawBindingsTable();   // drop-in, rebindable controller-map table for your settings UI
```

The full SDK is `api/ImGuiVRHelperClientSDK.h`. **Community Shaders** and **SKSE
Menu Framework** are working reference integrations.

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
git clone --recurse-submodules <url>
cd imgui-vr-helper
xmake
```

Set `SkyrimVRPluginTargets` (a `;`-separated list of Skyrim VR `Data` dirs or
mod-manager folders) to auto-deploy on every build — distinct from the general
`SkyrimPluginTargets` convention since the helper is VR-only.

## Licensing

Split so non-GPL client mods can integrate without GPL infection:

- **`src/`** — GPL-3.0-or-later WITH a Skyrim modding exception. See
  [COPYING](./COPYING) and [EXCEPTIONS.md](./EXCEPTIONS.md).
- **`api/`** — LGPL-3.0-or-later; the only files a client consumes. Every `api/`
  file carries an LGPL SPDX header, and the directory ships its own
  [COPYING](./api/COPYING) (GPL-3.0, which LGPL-3.0 incorporates) and
  [COPYING.LESSER](./api/COPYING.LESSER) (the LGPL additional permissions) so a
  vendored copy is self-sufficient. The LGPL relinking requirement is satisfied
  because the messaging handshake makes the helper DLL replaceable at runtime.

## Credits

- Handshake pattern from [SkyrimVRESL](https://github.com/alandtse/SkyrimVRESL),
  itself crediting [HIGGS](https://github.com/adamhynek/higgs).
- xmake scaffolding adapted from
  [Intellightent](https://github.com/alandtse/Intellightent).
- VR overlay/input infrastructure ported from
  [Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders).
