// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// Wire-stable data structures for the ImGuiVRHelper public API.
//
// IMPORTANT: this header must NOT include ImGui or OpenVR headers, must NOT
// reference any ImGuiKey/IVR* type, and must NOT change layout once
// shipped. Add new fields by bumping kInputAbiVersion and extending the
// trailing portion of structs, never by reordering existing fields.

#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D11RenderTargetView;

namespace ImGuiVRHelperPluginAPI
{
	inline constexpr uint32_t kInputAbiVersion = 1;

	/// 3D pose in OpenVR standing-universe space, meters and quaternions.
	struct Pose
	{
		float pos[3];     ///< meters; x right, y up, z toward user
		float orient[4];  ///< quaternion w, x, y, z
		float vel[3];
		float angvel[3];
		uint32_t valid;  ///< bit0 tracked, bit1 visible, bit2 has_velocity
	};

	/// Wire-stable button identifiers. Owned by the helper; never renumbered.
	/// The helper translates from RE::BSOpenVRControllerDevice::Keys at the
	/// boundary so this enum is independent of the OpenVR/SkyrimVR mapping.
	enum class Button : uint32_t
	{
		AX = 0,            ///< RE Keys::kXA              (7)  — X (left) / A (right) face button
		BY = 1,            ///< RE Keys::kBY              (1)  — Y (left) / B (right) face button
		Menu = 2,          ///< system menu / app menu   (varies by controller)
		System = 3,        ///< system / home button     (varies)
		TriggerClick = 4,  ///< RE Keys::kTrigger         (33) — analog trigger pulled past click threshold
		GripClick = 5,     ///< RE Keys::kGrip            (2)  — grip pressed; kGripAlt (34) folded in
		StickClick = 6,    ///< RE Keys::kJoystickTrigger (32) — thumbstick pressed in
		PadClick = 7,      ///< RE Keys::kTouchpadClick   (35) — touchpad clicked; kTouchpadAlt (36) folded in
		Shoulder = 8,      ///< reserved for future controllers with shoulder buttons
		Reserved9 = 9,
		Reserved10 = 10,
		Reserved11 = 11,
		Reserved12 = 12,
		Reserved13 = 13,
		Reserved14 = 14,
		Reserved15 = 15,
	};

	/// Per-controller per-frame state.
	struct Hand
	{
		uint32_t connected;
		uint32_t controller_kind;  ///< 0 unknown, 1 index, 2 oculus_touch, 3 wmr, 4 vive, 5 cosmos, ...
		Pose pose;
		uint32_t buttons_held;      ///< bitmask: (1u << static_cast<uint32_t>(Button::X))
		uint32_t buttons_pressed;   ///< edges this frame
		uint32_t buttons_released;  ///< edges this frame
		uint32_t buttons_touched;   ///< capacitive
		float trigger;              ///< 0..1 analog
		float grip;                 ///< 0..1 analog
		float stick_x, stick_y;     ///< -1..1
		float pad_x, pad_y;         ///< -1..1
		uint32_t haptic_token;      ///< opaque; pass back to TriggerHaptic()
	};

	/// Per-frame snapshot delivered to each registered client via OnFrameFn.
	struct Frame
	{
		uint32_t abi_version;  ///< == kInputAbiVersion
		uint32_t struct_size;  ///< == sizeof(Frame); allows forward-extension
		float dt;              ///< seconds since previous frame
		Pose hmd;
		Hand left;
		Hand right;
		uint32_t flags;      ///< bit0 client_has_focus
							 ///< bit1 overlay_visible
							 ///< bit2 client_pointer_in_panel
		float hud_depth;     ///< HUD plane distance, meters (kClientFlag_HUDMode)
		float hud_coverage;  ///< fraction of each eye's view the HUD plane fills
	};

	/// Named bits for Frame::flags.
	enum FrameFlags : uint32_t
	{
		kFrameFlag_HasFocus = 1u << 0,        ///< the helper routed in-scene focus to this client
		kFrameFlag_OverlayVisible = 1u << 1,  ///< any client overlay is currently shown
		kFrameFlag_PointerInPanel = 1u << 2,  ///< this client's wand pointer is on its panel
		/// A helper-owned modal gesture (e.g. an overlay reposition drag) is in progress. The client
		/// SDK's PumpInput uses this to skip forwarding wand buttons into ImGui as clicks/keys for the
		/// duration -- raw controller state (buttons_held/trigger/grip/stick) is UNCHANGED and still
		/// genuine, so anything reading it directly (IsButtonHeld, a client's own off-panel shortcuts)
		/// keeps working normally. This only suppresses the ImGui-forwarding step, not input delivery.
		kFrameFlag_SuppressInputForwarding = 1u << 3,
	};

	/// Helper-owned render target the client renders its ImGui frame into.
	/// Valid until UnregisterClient. Width/height may change between frames
	/// if the user resizes the panel; clients should re-check each frame.
	struct PanelHandle
	{
		uint32_t width;
		uint32_t height;
		ID3D11RenderTargetView* rtv;
	};

	using ComboId = uint32_t;

	/// Forward declaration; defined in ImGuiVRHelperInput.h.
	struct InputCombo;

	using OnFrameFn = void (*)(const Frame*, void* user);
	using ComboRecordedFn = void (*)(const InputCombo*, std::size_t n, void* user);

	/// Called when the user rebinds a registered combo from the helper's
	/// controller-map UI. The helper has already updated the combo's live keys;
	/// this delivers the new chord so the client can persist it to its settings.
	using ComboRebindFn = void (*)(const InputCombo* keys, std::size_t n, void* user);

	/// Bit flags for RegisterClient().
	enum ClientFlags : uint32_t
	{
		kClientFlag_None = 0,
		kClientFlag_RequiresFocus = 1u << 0,  ///< only invoke on_frame when this client holds focus

		/// Render this client's panel as a fully transparent, always-on
		/// overlay anchored to the HMD at a comfortable viewing depth —
		/// like Skyrim's vanilla HUD plane. The client doesn't have to
		/// reason about positioning; the helper picks a depth + size
		/// that fills most of the user's FOV.
		///
		/// Use cases: subtitle text, damage numbers, nameplates,
		/// always-on debug overlays — any flat 2D content you'd put on
		/// a HUD layer in flat-screen Skyrim.
		///
		/// Behavior:
		///   - The panel RTV is the same 1920×1080 RGBA8 the helper
		///     hands every client. Clear it transparent (ImGui windows
		///     with NoBackground / alpha 0) and draw text / shapes at
		///     the panel coordinates you want.
		///   - The helper renders the RTV as a 3D quad at fixed
		///     HMD-relative depth (~1.5m forward, ~2.5m wide), with
		///     proper per-eye projection so the eyes converge on it.
		///     Transparent pixels pass through to the underlying scene.
		///   - HUD clients are NOT subject to focus or attachMode
		///     gating — they render every frame they exist. The
		///     focused panel-mode client (if any) renders separately
		///     at its own (movable, draggable) position.
		///   - Multiple HUD clients stack in registration order;
		///     last-registered draws on top.
		kClientFlag_HUDMode = 1u << 1,

		/// DEPRECATED, no-op. The SteamVR Dashboard surface was removed; the
		/// helper strips this flag at registration and treats the client as
		/// in-scene only. The value is reserved so it isn't reused and so old
		/// clients still link. Don't set it in new code.
		kClientFlag_Dashboard = 1u << 2,

		/// Acknowledge the focus-render contract: when this client's
		/// per-frame Frame.flags has bit0 (client_has_focus) set, the
		/// client WILL render its menu UI into the panel RTV that
		/// frame, regardless of whatever internal "is my menu open"
		/// state the client tracks.
		///
		/// This is the wire signal the helper uses to tell a client
		/// "you're being shown right now — please draw something."
		/// It fires when the helper's focus model picks this client
		/// (e.g. the user opened it, or dismissed another panel, or
		/// chose it from the quick-select list).
		///
		/// Without this flag, the helper still tracks focus on the
		/// client (so RequestFocus / ReleaseFocus still work), but
		/// assumes the client renders only when its own internal
		/// trigger fires (e.g. a TAB hotkey); such clients get a
		/// "trigger this manually" banner instead.
		///
		/// New code SHOULD set this flag. Pre-existing clients that
		/// can't easily move their render to focus-driven (legacy
		/// menu state machines, etc.) can omit it and the helper
		/// degrades gracefully.
		kClientFlag_RendersOnFocus = 1u << 3,

		/// Declare this client a "live tool": an interactive overlay that runs
		/// while the game is UNPAUSED and the player keeps using the world
		/// (e.g. a VR photo mode flying a free camera). Opt-in per client at
		/// RegisterClient().
		///
		/// The problem it solves: by default, while any client holds focus the
		/// helper SWALLOWS all VR controller input from the game (so a trigger
		/// pull that clicks a menu button doesn't also fire your bow, and a
		/// thumbstick used for menu nav doesn't walk your character). That's
		/// correct for a pause-time settings panel, but wrong for a live tool
		/// whose whole point is to keep driving the world (fly the camera,
		/// move) while you poke its panel.
		///
		/// Behavior when a live-tool client holds focus:
		///   - LOCOMOTION PASSES THROUGH. Thumbstick events are forwarded to
		///     the game, so the player's own bindings/deadzones/handedness
		///     drive movement (e.g. the free camera) normally. The client does
		///     NOT need to read raw controller state itself.
		///   - The OVERLAY IS LASER-DRIVEN. Button events (trigger, etc.) are
		///     still captured for the panel UI (point-and-click with the wand),
		///     so the same stick can fly the world while the laser works the
		///     widgets — no fighting over the stick.
		///
		/// Pair with kClientFlag_RendersOnFocus. Enter via RequestFocus(),
		/// exit via ReleaseFocus().
		kClientFlag_LiveTool = 1u << 4,

		/// Declare this client a "pointer-focus" panel: a focused interactive
		/// overlay that COEXISTS with the game's own UI instead of taking the
		/// wand exclusively. Opt-in per client at RegisterClient().
		///
		/// The problem it solves: a normal focused panel swallows ALL VR
		/// controller input while shown, which is correct for a standalone menu
		/// but wrong for an overlay meant to sit alongside a live game menu (e.g.
		/// a dialogue-history panel shown during a conversation — you still need
		/// to pick dialogue options). LiveTool passes locomotion through but still
		/// reserves buttons for the panel; this is for the menu-coexistence case.
		///
		/// Behavior when a pointer-focus client holds focus:
		///   - INPUT IS SWALLOWED ONLY WHILE THE WAND IS ON THE PANEL. On frames
		///     where the wand laser intersects this client's panel
		///     (kFrameFlag_PointerInPanel) the helper captures the wand for the
		///     panel UI as usual; on every other frame the input passes straight
		///     to the game, so the underlying menu keeps working.
		///   - The panel composites and receives OnFrame input exactly like any
		///     focused client; only the swallow decision is pointer-gated.
		///
		/// Pair with kClientFlag_RendersOnFocus. Enter via RequestFocus(),
		/// exit via ReleaseFocus().
		kClientFlag_PointerFocus = 1u << 5,

		/// Declare this client a "world-quad" client. Instead of the head-locked
		/// HUD plane (which swims for world-anchored content as the head turns),
		/// the helper draws one or more of the client's panel sub-rects as
		/// billboards anchored at caller-supplied WORLD positions, rendered with
		/// the in-scene world-space projection so they stay fixed in the world.
		///
		/// Submit the per-frame billboard list via SubmitWorldQuads(). Positions are Skyrim
		/// world-space (game units) — the helper converts to OpenVR tracking space itself at
		/// Submit time, so don't convert client-side. The client still renders into its normal
		/// panel RTV; each WorldQuad references a sub-rect of that panel as its texture.
		///
		/// Drawn at the OpenVR Submit hook like every other helper surface — no
		/// game-render-pipeline injection — but the helper binds the game's scene
		/// depth for this pass, so billboards are per-pixel occluded by world geometry.
		kClientFlag_WorldQuad = 1u << 6,
	};

	/// One world-anchored billboard for a kClientFlag_WorldQuad client. The helper
	/// samples the client's panel texture over [u0,v0]..[u1,v1] (0..1 panel UV) and
	/// draws it as a camera-facing quad centered at `pos`, `height_m` meters tall;
	/// width follows the sub-rect's aspect ratio.
	struct WorldQuad
	{
		float u0, v0, u1, v1;  ///< source sub-rect in the client's panel, 0..1 UV
		/// Billboard center, Skyrim world-space (game units), NOT OpenVR tracking space. The
		/// helper converts this itself at Submit time (using the same fresh pose it builds the
		/// eye projection from), so the client shouldn't do its own world->tracking conversion —
		/// doing so from an earlier point in the frame is a source of visible jitter while the
		/// player is moving.
		float pos[3];
		float height_m;  ///< billboard height in meters (width = height * uvAspect)
	};

}  // namespace ImGuiVRHelperPluginAPI
