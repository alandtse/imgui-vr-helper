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

In your `kPostLoad` SKSE message handler:

```cpp
#include "ImGuiVRHelperAPI.h"

namespace API = ImGuiVRHelperPluginAPI;

API::IImGuiVRHelperInterface001* g_helper = nullptr;

// kPostLoad
g_helper = API::GetImGuiVRHelperInterface001();
if (!g_helper) {
    // Helper not installed — fall back to flatscreen-only behavior.
    return;
}

uint32_t client_id = g_helper->RegisterClient("MyMod", &OnFrame, /*user*/ nullptr, /*flags*/ 0);
```

## Credits

- Handshake pattern derived from
  [SkyrimVRESL](https://github.com/alandtse/SkyrimVRESL), itself credited to
  [HIGGS](https://github.com/adamhynek/higgs).
- xmake plugin scaffolding adapted from
  [Intellightent](https://github.com/alandtse/Intellightent).
- VR overlay/input infrastructure ported from
  [Skyrim Community Shaders](https://github.com/doodlum/skyrim-community-shaders).
