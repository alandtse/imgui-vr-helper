# imgui-vr-helper roadmap

Status: **core working, pre-release** — a standalone SKSE plugin that lets ImGui-based mods become
VR-interactive without each mod rebuilding the overlay/input plumbing. Clients register over an
SKSE-messaging handshake (`GetImGuiVRHelperInterface001`), render their ImGui frame into a
helper-owned panel RTV, and the helper composites it as a free-floating 3D quad in front of the
player (per-eye projection so the eyes converge) and — optionally — as a SteamVR Dashboard panel.

Skyrim Community Shaders / Open Shaders is the first client (see its `ImGuiVRHelperClient`).

## Working today

- **Handshake + versioned interface** (`IImGuiVRHelperInterface001`), one SKSE messaging dispatch
  then a stable vtable. `RegisterClient(name, version, on_frame, user, flags)`.
- **Free-floating in-scene panel** — the default surface. Helper-owned 1920×1080 RGBA8 RTV,
  composited via the `IVRCompositor::Submit` hook (slot 5) as a billboarded quad at HMD-relative
  depth, with grip-to-drag repositioning, wand pointing → ImGui cursor, and thumbstick scroll.
- **SteamVR Dashboard surface** (`kClientFlag_Dashboard`) — one shared dashboard overlay in the
  SteamVR rail; a picker in the helper's settings panel chooses which eligible client's RTV is
  mirrored. SteamVR's own laser drives the cursor on this path.
- **HUD mode** (`kClientFlag_HUDMode`) — always-on full-FOV layer for subtitle/nameplate-style
  content; mutually exclusive with Dashboard.
- **Focus-render contract** (`kClientFlag_RendersOnFocus`) — clients that ack it render on focus
  so the dashboard picker auto-shows them; clients that don't get a "trigger manually" banner.
- **Settings panel** — desktop mouse/keyboard via a WndProc hook, sortable Registered Clients
  table, per-client diagnostics, combo recording for a controller toggle.
- **TOML settings** at `Data/SKSE/Plugins/ImGuiVRHelper.toml`.

## Near-term

### Release wiring (blocked on a Nexus modid)

`release.yaml` is intentionally disabled — it builds + packages on manual dispatch but does not
publish. When a modid exists: re-enable the tag trigger, add `nexus-upload.yaml` (mirror
devbench's), flip the `publish` job's guard. The version lives in `xmake.lua`; there is no
semantic-release.

### Host-independent unit tests

Add a `imgui-vr-helper-tests` xmake target (mirror devbench's) covering the pure-logic seams that
don't need a running game: combo matching (`MatchCombo`), the HUD ⊕ Dashboard flag-exclusion at
registration, and the dashboard picker's active-client resolution. Gate CI on it.

### vcpkg port for the API pack

Today consumers vendor `api/*.{h,cpp}` by hand (SCS keeps a copy under
`extern/imgui-vr-helper-api/`). A `cmake/ports/imgui-vr-helper-api` vcpkg port (mirror devbench's
`devbench-api`) would let CMake-based clients consume the API surface without manual copies.

## Later

- **Per-eye matrices in the Frame** were prototyped and reverted — the helper's remit is "flat
  ImGui for VR," not new per-VR features. Revisit only if a concrete client needs it.
- **SteamVR virtual keyboard** for text inputs on the dashboard path (`ShowKeyboardForOverlay`).
- **Multiple concurrent dashboard clients** if the single-picker model proves limiting.
- **Submodule the API pack into clients** instead of vendoring, once there's a remote to point at.
