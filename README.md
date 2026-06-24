# ImGuiVRHelper

A SKSE plugin that bridges ImGui-based mods to Skyrim VR.

ImGuiVRHelper owns the OpenVR overlay submission, controller input
collection, button-combo handling, and laser-pointer raycasting that other
SKSE mods need to make their ImGui interfaces interactive in the headset.
Client mods register at startup, render their ImGui frames into a
helper-owned render target, and consume raw VR input via callback. The
helper does not link against any client's ImGui — clients keep their own
version forever.

## Status

**Pre-alpha — design phase.** This repository currently contains a
non-functional skeleton that compiles and exports the public API
handshake. Real overlay submission, input handling, and combo
machinery will be ported from
[Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders)
in subsequent PRs. See
[`docs/development/vr-imgui-helper-plan.md`](https://github.com/doodlum/skyrim-community-shaders/blob/main/docs/development/vr-imgui-helper-plan.md)
in SCS for the full design and migration plan.

## Repository layout

```
api/                    LGPL-3.0 — public header surface that clients vendor
src/                    GPL-3.0 with modding exception — helper internals
lib/commonlibsse-ng/    submodule — VR-targeted CommonLibSSE-NG runtime
```

Clients only ever consume `api/`. The DLL is loaded at runtime by SKSE;
clients have no link-time dependency on the helper.

## Licensing

Split license, designed so non-GPL clients can integrate without GPL
infection:

- **`src/`** — GPL-3.0-or-later WITH a Skyrim modding exception (lifted
  verbatim from Skyrim Community Shaders). See [COPYING](./COPYING) and
  [EXCEPTIONS.md](./EXCEPTIONS.md).
- **`api/`** — LGPL-3.0-or-later. Clients vendor `api/*.h` and
  `api/ImGuiVRHelperAPI.cpp` under permissive terms; their own code stays
  under their own license. The LGPL "must allow relinking" requirement is
  satisfied trivially because the messaging handshake makes the helper DLL
  replaceable at runtime. See [COPYING.LESSER](./COPYING.LESSER).

Each source file carries an SPDX identifier in its header.

## Build

Requires [xmake](https://xmake.io/) and Visual Studio 2022 with the C++
desktop workload.

```sh
git clone --recurse-submodules <url>
cd imgui-vr-helper
xmake
```

Set the `SkyrimVRPluginTargets` environment variable to a semicolon-separated
list of Skyrim VR Data directories (or mod-manager mod folders) to auto-deploy
the built DLL on every build. This is intentionally distinct from the more
general `SkyrimPluginTargets` convention — the helper is VR-only, so its
deploy targets shouldn't collide with non-VR plugins' deploy paths.

## Using ImGuiVRHelper from a client mod

Vendor four files into your client's source tree, or pull this repo as a
submodule and add `api/` to your include path:

- `api/ImGuiVRHelperAPI.h`
- `api/ImGuiVRHelperAPI.cpp`
- `api/ImGuiVRHelperTypes.h`
- `api/ImGuiVRHelperInput.h`

In your `kPostPostLoad` SKSE message handler (`kPostPostLoad`, not `kPostLoad` —
it fires after every plugin's `kPostLoad`, so the helper's messaging listener is
registered regardless of load order; the handshake is also retryable if you call
earlier):

```cpp
#include "ImGuiVRHelperAPI.h"

namespace API = ImGuiVRHelperPluginAPI;

API::IImGuiVRHelperInterface001* g_helper = nullptr;

// kPostPostLoad
g_helper = API::GetImGuiVRHelperInterface001();
if (!g_helper) {
    // Helper not installed — fall back to flatscreen-only behavior.
    return;
}

uint32_t client_id = g_helper->RegisterClient("MyMod", &OnFrame, /*user*/ nullptr, /*flags*/ 0);
```

### Rendering: feed the panel, not the game's HUD

In VR, the helper composites your menu as a flat 3D panel from a texture you
render into (see `RenderToPanel` in the SDK). **When connected, that panel is your
only headset output — do not _also_ run your normal flat-screen ImGui draw into
the game's frame.** A flat menu drawn into Skyrim's HUD/menu target (`kHUDMENU`)
gets wrapped onto the engine's **curved world HUD** and comes out sheared and
mispositioned — a second, mangled copy floating next to the correct flat panel.
This is the core reason the helper exists: a flat-screen ImGui mod's instinct
("draw into the game's UI") is exactly what looks broken in VR.

Gate your final draw on the connection:

```cpp
ImGui::Render();
if (g_vr.IsConnected()) {
    g_vr.RenderToPanel(myD3DContext);   // VR: flat panel is the sole output
} else {
    ImGui_ImplDX11_RenderDrawData(...); // flat screen / helper absent
}
```

Hooking at `IDXGISwapChain::Present` (the desktop swapchain/mirror) instead of the
game's menu render is also safe, since the mirror window isn't shown in the headset
— that's how Community Shaders avoids the issue.

## Credits

- Handshake pattern derived from
  [SkyrimVRESL](https://github.com/alandtse/SkyrimVRESL), itself credited to
  [HIGGS](https://github.com/adamhynek/higgs).
- xmake plugin scaffolding adapted from
  [Intellightent](https://github.com/alandtse/Intellightent).
- VR overlay/input infrastructure ported from
  [Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders).
