# imgui-vr-helper roadmap

Status: **stable** — a standalone SKSE plugin that lets ImGui-based mods become
VR-interactive without each mod rebuilding the overlay/input plumbing. Clients register over an
SKSE-messaging handshake (`GetImGuiVRHelperInterface001`), render their ImGui frame into a
helper-owned panel RTV, and the helper composites it as a free-floating 3D quad in front of the
player (per-eye projection so the eyes converge).

Skyrim Community Shaders / Open Shaders is the first client (see its `VR` feature's helper methods);
the SKSE Menu Framework is the second.

## Working today

- **Handshake + versioned interface** (`IImGuiVRHelperInterface001`), one SKSE messaging dispatch
  then a stable vtable. `RegisterClient(name, version, on_frame, user, flags)`.
- **Free-floating in-scene panel** — the default surface. Helper-owned 1920×1080 RGBA8 RTV,
  composited via the `IVRCompositor::Submit` hook (slot 5) as a billboarded quad at HMD-relative
  depth, with grip-to-drag repositioning, wand pointing → ImGui cursor, and thumbstick scroll.
- **HUD mode** (`kClientFlag_HUDMode`) — always-on full-FOV layer for subtitle/nameplate-style
  content, at a client-readable depth/coverage (surfaced on the per-frame `Frame`).
- **Focus-render contract** (`kClientFlag_RendersOnFocus`) — clients that ack it render on focus;
  clients that don't get a "trigger manually" banner.
- **Overlay cycle + quick-select** — stick-click cycles between open overlays; a long-hold stick
  click opens an interactive picker (thumbstick to scroll, trigger/stick to choose, grip to cancel).
- **Settings panel** — desktop mouse/keyboard via a WndProc hook, sortable Registered Clients
  table, per-client diagnostics, combo recording for a controller toggle.
- **TOML settings** at `Data/SKSE/Plugins/ImGuiVRHelper.toml`.

## Near-term

### Host-independent unit tests

Add a `imgui-vr-helper-tests` xmake target (mirror devbench's) covering the pure-logic seams that
don't need a running game: combo matching (`MatchCombo`), HUD-quad geometry, and the overlay
cycle/quick-select ordering. Gate CI on it.

### vcpkg port for the API pack

CMake clients consume the API via `FetchContent` against `api/` (the `ImGuiVRHelper::api` INTERFACE
target); Community Shaders and the SKSE Menu Framework both use it. A `cmake/ports/imgui-vr-helper-api`
vcpkg port (mirror devbench's `devbench-api`) would additionally let vcpkg-based clients pull the
API surface through their manifest.

## Later

- **Per-eye matrices in the Frame** were prototyped and reverted — the helper's remit is "flat
  ImGui for VR," not new per-VR features. Revisit only if a concrete client needs it.
