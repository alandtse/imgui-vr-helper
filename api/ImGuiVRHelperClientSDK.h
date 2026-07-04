// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2025 ImGuiVRHelper contributors. See api/COPYING.LESSER.
//
// ImGuiVRHelper client SDK — a header-only convenience layer over the raw
// IImGuiVRHelperInterface001 vtable. It absorbs the boilerplate every ImGui
// mod needs to gain VR support: the handshake, the per-frame controller
// snapshot, focus sync, the wand-cursor / button / thumbstick input pump, the
// panel-RTV blit, and combo registration/rebinding. Adopting VR for an
// existing ImGui menu then looks like:
//
//   ImGuiVRHelperPluginAPI::Client g_vr;
//   // From YOUR SKSE kPostPostLoad handler (the helper's listener is up by then,
//   // regardless of load order — mods own their own SKSE lifecycle):
//   g_vr.Connect("MyMod", versionStr, ImGuiVRHelperPluginAPI::kClientFlag_RendersOnFocus);
//   g_vr.AddCombo("Open menu", myOpenKeys, [](auto* k, auto n){ /* persist */ });
//   // each render frame:
//   if (g_vr.Fired(openCombo)) menuOpen = true;
//   g_vr.Update(menuOpen);  // ReconcileFocus + PumpInput (focus sync + wand input)
//   // build your ImGui frame + ImGui::Render(), then ONE output call:
//   g_vr.RenderFrame();     // VR -> flat panel; flat screen -> your normal draw
//
// For an always-on HUD layer, the whole context lifecycle is one call:
//   g_vr.RenderHud(device, ctx, displaySize, []{ /* your ImGui draws */ });
//
// Plus a drop-in, sortable/filterable bindings table for your own settings UI:
//
//   g_vr.DrawBindingsTable();
//
// Off-panel input contract: while your client holds focus, the helper turns
// controller buttons into UI input ONLY when the wand laser is on your panel
// (IsPointerInPanel() == true) — trigger/grip/pad/stick → mouse clicks, A/X →
// Enter, B/Y → Tab. Off the panel those buttons are the client's: register actions
// with AddCombo (so they surface in the helper's controller map) and gate the
// handler on !IsPointerInPanel(). The helper's own system gestures are designed
// not to collide — the open-menu chord is a no-op while any client is focused,
// overlay cycle is a stick-click, and close/exit is grip — so a focused client
// effectively owns the off-panel face buttons.
//
// Hard dependencies (include from a TU that has them): Dear ImGui (<imgui.h> +
// <imgui_internal.h>) and the official DX11 backend (<imgui_impl_dx11.h>),
// <d3d11.h>, and nlohmann/json (pulled by ImGuiVRHelperInput.h). RenderHud /
// RenderToPanel call ImGui_ImplDX11_* directly, so a mod shipping a custom
// (non-stock) ImGui backend can't use those methods. It does NOT depend on
// CommonLibSSE/RE — button names use OpenVR key codes — so it is safe to
// vendor alongside the other api/ headers.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_internal.h>  // RenderHud re-allows window SetWindowPos*AllowFlags

#include "ImGuiVRHelperAPI.h"
#include "ImGuiVRHelperInput.h"
#include "ImGuiVRHelperTypes.h"

namespace ImGuiVRHelperPluginAPI
{
	/// ImGui's InputText rework (docking, ~1.91) renamed
	/// ImGuiInputTextState::CurLenA to TextLen. This header compiles inside
	/// each client's own TU against that client's ImGui, so pick whichever
	/// member exists rather than hard-coding one vintage.
	template <typename T>
	concept HasTextLenMember = requires(T& t) { t.TextLen; };

	template <typename T>
	[[nodiscard]] inline int InputTextStateLen(T& state)
	{
		if constexpr (HasTextLenMember<T>)
			return state.TextLen;
		else
			return state.CurLenA;
	}

	/// Per-controller color encoding, matching the helper's controller-map UI:
	/// Primary = yellow, Secondary = blue, Both = green, anything else = white.
	inline ImVec4 DeviceColor(InputDeviceType device)
	{
		switch (device) {
		case InputDeviceType::Primary:
			return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // yellow
		case InputDeviceType::Secondary:
			return ImVec4(0.0f, 0.5f, 1.0f, 1.0f);  // blue
		case InputDeviceType::Both:
			return ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // green
		default:
			return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);  // white
		}
	}

	/// Human-readable name for a packed key code. VR codes are
	/// RE::BSOpenVRControllerDevice::Keys values; keyboard/mouse/gamepad
	/// devices fall through to a generic "Key N" so the table never shows a
	/// bare number with no label.
	inline std::string ButtonName(InputDeviceType device, uint32_t key)
	{
		if (device == InputDeviceType::Primary || device == InputDeviceType::Secondary ||
			device == InputDeviceType::Both) {
			switch (key) {
			case 1:
				return "B/Y";
			case 2:
				return "Grip";
			case 7:
				return "A/X";
			case 32:
				return "Stick Click";
			case 33:
				return "Trigger";
			case 34:
				return "Grip";  // kGripAlt — same physical button as kGrip
			case 35:
				return "Touchpad";
			case 36:
				return "Touchpad Alt";
			default:
				break;
			}
		}
		return "Key " + std::to_string(key);
	}

	/// RAII guard: captures the current ImGui context and restores it on scope
	/// exit. Use around any code that switches to a private context — leaving a
	/// foreign context current crashes the host's ImGui_ImplWin32_NewFrame.
	struct ScopedImGuiContext
	{
		ImGuiContext* prev = ImGui::GetCurrentContext();
		~ScopedImGuiContext() { ImGui::SetCurrentContext(prev); }
		ScopedImGuiContext() = default;
		ScopedImGuiContext(const ScopedImGuiContext&) = delete;
		ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
	};

	/// True if the helper plugin is installed and its interface is available,
	/// without registering. Lets a mod decide up front whether to adopt the VR
	/// path or fall back to desktop-only, and tell "not installed" from a failed
	/// Connect() (a false Connect after this returned true means registration was
	/// rejected, e.g. a duplicate client name).
	[[nodiscard]] inline bool IsHelperInstalled() { return GetImGuiVRHelperInterface001() != nullptr; }

	/// Convenience wrapper around the helper interface. One instance per mod;
	/// not thread-safe except for the OnFrame snapshot, which is mutex-guarded
	/// so the helper's frame thread and your render thread can share it.
	///
	/// ImGui ownership: YOU bring your own statically-linked ImGui + DX11 backend.
	/// The helper only owns the panel RTV and the controller input source; it never
	/// shares a GImGui with you. PumpInput/RenderToPanel act on your current
	/// context; RenderHud owns a private context of its own. Call PumpInput and
	/// RenderToPanel on your render thread (after your NewFrame / Render
	/// respectively). The OnFrame callback runs on the helper's frame thread — the
	/// SDK already mutex-guards the bit of state it shares with your render thread.
	class Client
	{
	public:
		/// Called with the new chord when a registered combo is rebound (from
		/// the helper's controller map or this SDK's bindings table), so the
		/// owner can persist it. The live keys are already updated.
		using RebindCallback = std::function<void(const InputCombo* keys, std::size_t n)>;

		Client() = default;
		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		// ---- Lifecycle -------------------------------------------------

		/// Perform the handshake and register as a client. Call at kPostPostLoad
		/// (so the helper's listener is up regardless of load order); retryable if
		/// you call earlier. Returns true once the helper is found and registration
		/// succeeds. `flags` is a bitmask of kClientFlag_*.
		bool Connect(const char* name, const char* version, uint32_t flags)
		{
			if (m_id != 0)
				return true;
			m_helper = GetImGuiVRHelperInterface001();
			if (!m_helper)
				return false;
			m_id = m_helper->RegisterClient(name, version, &OnFrameThunk, this, flags);
			if (m_id == 0) {
				m_helper = nullptr;
				return false;
			}
			// Optional: VR text entry (interface 002). Null against an older helper,
			// in which case PumpKeyboard is a no-op (desktop keyboard only).
			m_helper002 = GetImGuiVRHelperInterface002();
			m_helper003 = GetImGuiVRHelperInterface003();
			m_helper004 = GetImGuiVRHelperInterface004();
			m_helper005 = GetImGuiVRHelperInterface005();
			return true;
		}

		void Disconnect()
		{
			// Release any latched keyboard before we drop the interface, or the helper
			// keeps this client's keyboard state active after we're gone.
			if (m_helper002 && m_id && m_kbShown)
				m_helper002->SetKeyboardActive(m_id, false, "");
			if (m_helper && m_id)
				m_helper->UnregisterClient(m_id);
			m_helper = nullptr;
			m_helper002 = nullptr;
			m_helper003 = nullptr;
			m_helper004 = nullptr;
			m_helper005 = nullptr;
			m_kbShown = false;
			m_kbDelivered.clear();
			m_kbDismissCooldown = 0;
			m_id = 0;
		}

		[[nodiscard]] bool IsConnected() const { return m_helper != nullptr && m_id != 0; }
		[[nodiscard]] uint32_t Id() const { return m_id; }
		[[nodiscard]] IImGuiVRHelperInterface001* Helper() const { return m_helper; }

		/// True if the helper supports VR text entry (interface 002 acquired). When
		/// false, PumpKeyboard is a no-op.
		[[nodiscard]] bool HasKeyboard() const { return m_helper002 != nullptr; }

		/// True if the helper supports world-anchored quads (interface 004 acquired).
		/// When false, SubmitWorldQuads is a no-op (connect as kClientFlag_HUDMode for
		/// a head-locked fallback if you need one).
		[[nodiscard]] bool HasWorldQuads() const { return m_helper004 != nullptr; }

		/// Submit this frame's world-billboard list (kClientFlag_WorldQuad clients).
		/// Replaces the previous frame's list; pass count==0 to clear. Each WorldQuad
		/// names a sub-rect of THIS client's panel (render it via RenderToPanel /
		/// GetPanel as usual) and a Skyrim world-space position (game units, e.g. a
		/// node's world.translate) — the helper converts to OpenVR tracking space
		/// itself at Submit time; don't do that conversion client-side (an earlier,
		/// client-side conversion desyncs from the pose the eye projection uses once
		/// the player is moving, and shows up as jitter). No-op when disconnected or
		/// the helper predates interface 004.
		void SubmitWorldQuads(const ImGuiVRHelperPluginAPI::WorldQuad* quads, std::size_t count)
		{
			if (!m_helper004 || !IsConnected())
				return;
			m_helper004->SubmitWorldQuads(m_id, quads, count);
		}

		/// True if the helper supports a client-driven reposition drag (interface 005
		/// acquired). When false, RequestReposition is a no-op -- the panel can still be
		/// repositioned via the helper's own off-panel grip gesture (Settings ->
		/// enableDragToReposition), just not from an on-panel "Move" button.
		[[nodiscard]] bool HasReposition() const { return m_helper005 != nullptr; }

		/// Call every frame you want a reposition drag active -- e.g. while your own
		/// wand-clickable "Move" button is held with the trigger (check via ImGui's
		/// IsItemActive() after drawing it, which stays true for the whole hold even once the
		/// wand ray drifts off the button/panel as the user physically moves their hand to
		/// drag -- don't gate this on GetPointer/IsPointerInPanel). No-op when disconnected,
		/// the helper predates interface 005, this client doesn't currently hold focus, or
		/// drag-to-reposition is disabled in the helper's settings. This is a heartbeat, not a
		/// toggle: stop calling to end the drag and persist the new position.
		void RequestReposition()
		{
			if (!m_helper005 || !IsConnected())
				return;
			m_helper005->RequestReposition(m_id);
		}

		/// True when the helper routed in-scene focus to this client this frame
		/// (it expects fresh pixels in the panel RTV even if your own menu flag
		/// is closed — e.g. the user cycled overlays to you).
		[[nodiscard]] bool HasFocus() const { return m_requestsRender; }

		/// True when this client's wand laser is on its panel this frame
		/// (kFrameFlag_PointerInPanel). Lets a client route input by where the
		/// wand points — e.g. trigger = click on the panel, trigger = a tool
		/// action when off it — without owning a PollInputDevices hook.
		[[nodiscard]] bool IsPointerInPanel() const { return m_pointerInPanel; }

		/// True while the given controller button is held this frame (either hand),
		/// from the same snapshot that drives the laser UI. Edge-detect client-side.
		[[nodiscard]] bool IsButtonHeld(Button a_button) const
		{
			std::scoped_lock lk{ m_mutex };
			return (m_heldMask & (1u << static_cast<uint32_t>(a_button))) != 0;
		}

		// ---- Combos ----------------------------------------------------

		/// Register a chord. `onRebind` (optional) fires with the new chord
		/// whenever the user rebinds, clears, or resets it, so you can persist
		/// your bindings (an empty chord means unbound). `defaultKeys` (optional)
		/// is the factory default for this binding — pass it to enable the
		/// per-row "Reset" button in DrawBindingsTable. Returns 0 for an empty
		/// initial chord or if not connected.
		/// `offPanel`: when true, Fired() only reports the combo while the wand is OFF this client's
		/// panel — the on-panel buttons drive the UI instead. Lets a focused client own the off-panel
		/// face buttons without each caller re-checking IsPointerInPanel().
		ComboId AddCombo(const char* label, const std::vector<InputCombo>& keys,
			RebindCallback onRebind = {}, const std::vector<InputCombo>& defaultKeys = {},
			bool offPanel = false)
		{
			if (!IsConnected() || keys.empty())
				return 0;
			ComboEntry entry;
			entry.owner = this;
			entry.label = label ? label : "";
			entry.keys = keys;
			entry.onRebind = std::move(onRebind);
			entry.defaults = defaultKeys;
			entry.offPanel = offPanel;
			m_combos.push_back(std::move(entry));
			ComboEntry& e = m_combos.back();
			e.id = m_helper->RegisterCombo(m_id, e.keys.data(), e.keys.size(), 0.0f,
				e.label.c_str(), &RebindThunk, &e);
			// Tell the helper the context too (003+), so the controller map shows it and conflict
			// flagging stays per-context. Older helpers still get the local Fired() gate above.
			if (offPanel && m_helper003 && e.id != 0)
				m_helper003->SetComboOffPanel(e.id, true);
			return e.id;
		}

		/// Edge-triggered: true exactly once per activation. For an off-panel combo, returns false
		/// while the wand is on the panel (the edge is still consumed, so an on-panel press is dropped
		/// rather than deferred to when you next look off-panel).
		bool Fired(ComboId id)
		{
			if (!IsConnected() || id == 0 || !m_helper->ComboFired(id))
				return false;
			for (const auto& e : m_combos)
				if (e.id == id)
					return !e.offPanel || !m_pointerInPanel;
			return true;
		}

		// ---- Text entry ------------------------------------------------

		/// Automatic VR text entry. Call once per frame, before your NewFrame, on
		/// your menu's ImGui context. Whenever ANY text field is focused
		/// (io.WantTextInput) it shows the VR keyboard and replays the runtime's
		/// text into the active field — no per-field wrapping. No-op when the helper
		/// predates 002 or the runtime has no keyboard.
		void PumpKeyboard()
		{
			if (!m_helper002 || !IsConnected()) {
				if (m_kbShown && m_helper002)
					m_helper002->SetKeyboardActive(m_id, false, "");
				m_kbShown = false;
				if (m_kbDismissCooldown > 0)
					--m_kbDismissCooldown;
				return;
			}
			ImGuiIO& io = ImGui::GetIO();
			if (m_kbDismissCooldown > 0)
				--m_kbDismissCooldown;

			if (!m_kbShown) {
				// Open when a text field is focused, seeding the runtime keyboard with
				// the field's current text. Without the seed the diff below only manages
				// characters we ourselves typed, so backspace can't delete text that was
				// already in the field. ImGui keeps the live UTF-8 buffer of the active
				// InputText in InputTextState (imgui_internal.h). The cooldown after a
				// dismiss keeps the OK-trigger release (and its wand click) from
				// re-opening the keyboard on the same field a frame later.
				if (io.WantTextInput && m_kbDismissCooldown == 0) {
					std::string seed;
					if (ImGuiContext* g = ImGui::GetCurrentContext();
						g && g->InputTextState.ID != 0 && g->InputTextState.ID == g->ActiveId) {
						if (const int len = InputTextStateLen(g->InputTextState); len > 0)
							seed.assign(g->InputTextState.TextA.Data, static_cast<std::size_t>(len));
					}
					m_kbShown = true;
					m_kbDelivered = seed;
					m_helper002->SetKeyboardActive(m_id, true, seed.c_str());
				}
				return;
			}

			// Shown: honor a real dismiss first. On Done/X (or the menu overlay being
			// dismissed) drop the field's focus so it stays closed — leaving the field
			// focused makes WantTextInput re-open the keyboard the very next frame, so
			// the user would have to press OK twice. The cooldown guards the same frame
			// against the OK-trigger's wand click re-focusing the field.
			if (m_helper002->ConsumeKeyboardClosed(m_id) || !m_helper->IsOverlayVisible()) {
				m_helper002->SetKeyboardActive(m_id, false, "");
				m_kbShown = false;
				m_kbDismissCooldown = kKbDismissCooldown;
				ImGui::ClearActiveID();
				return;
			}

			// Replay the runtime keyboard's text into the focused field as edits. Wand
			// input is suppressed while shown, so a transient WantTextInput=false is
			// just focus jitter, not a dismiss — keep it latched.
			char kb[512];
			const uint32_t n = m_helper002->GetKeyboardText(m_id, kb, sizeof(kb));
			const std::string cur(kb, n);
			if (cur != m_kbDelivered) {
				// Diff on UTF-8 code-point boundaries. A byte-wise prefix could stop
				// inside a multibyte character, which would emit the wrong number of
				// backspaces (ImGui deletes whole code points, not bytes) and hand a
				// continuation byte to AddInputCharactersUTF8.
				std::size_t p = 0;
				const std::size_t maxp = cur.size() < m_kbDelivered.size() ? cur.size() : m_kbDelivered.size();
				while (p < maxp && cur[p] == m_kbDelivered[p])
					++p;
				// [0, p) is shared, so a continuation byte at p on either side means the
				// prefix split a code point — back up to its lead byte.
				while (p > 0 &&
					   ((p < cur.size() && (static_cast<unsigned char>(cur[p]) & 0xC0) == 0x80) ||
						   (p < m_kbDelivered.size() && (static_cast<unsigned char>(m_kbDelivered[p]) & 0xC0) == 0x80)))
					--p;
				// Backspace once per code point removed (skip continuation bytes).
				int backspaces = 0;
				for (std::size_t i = p; i < m_kbDelivered.size(); ++i)
					if ((static_cast<unsigned char>(m_kbDelivered[i]) & 0xC0) != 0x80)
						++backspaces;
				for (int i = 0; i < backspaces; ++i) {
					io.AddKeyEvent(ImGuiKey_Backspace, true);
					io.AddKeyEvent(ImGuiKey_Backspace, false);
				}
				if (p < cur.size())
					io.AddInputCharactersUTF8(cur.c_str() + p);
				m_kbDelivered = cur;
			}
		}

		// ---- Per-frame plumbing ----------------------------------------

		/// Keep helper focus in lockstep with your menu-open flag (edge-based,
		/// so it doesn't fight the helper's own UI for focus every frame).
		/// Deprecated: ReconcileFocus does this plus the helper->client direction;
		/// don't mix the two (they track separate latches).
		[[deprecated("use ReconcileFocus")]] void SyncFocus(bool menuOpen)
		{
			if (!IsConnected())
				return;
			if (menuOpen && !m_focusRequested) {
				m_helper->RequestFocus(m_id);
				m_focusRequested = true;
			} else if (!menuOpen && m_focusRequested) {
				m_helper->ReleaseFocus(m_id);
				m_focusRequested = false;
			}
		}

		/// Unconditionally request helper focus (show this client's overlay). Use
		/// when the client opens its menu itself (e.g. a keyboard shortcut) so the
		/// helper composites it. Pair with ReleaseFocus() to close.
		void RequestFocus()
		{
			if (!IsConnected())
				return;
			m_helper->RequestFocus(m_id);
			m_focusRequested = true;
		}

		/// Unconditionally release helper focus and reset focus latches. Use
		/// when closing in response to a hotkey: covers the swap case where the
		/// helper granted focus without an explicit RequestFocus (so SyncFocus
		/// alone wouldn't release it).
		void ReleaseFocus()
		{
			if (!IsConnected())
				return;
			m_helper->ReleaseFocus(m_id);
			m_focusRequested = false;
			m_hadFocus = false;
		}

		/// Returns true exactly once when the helper took focus away while you
		/// held it (the user opened another overlay) — close your menu so the
		/// new overlay replaces it (single-window swap).
		/// Deprecated: ReconcileFocus folds this in; don't mix the two.
		[[deprecated("use ReconcileFocus")]] bool ConsumeFocusLost()
		{
			if (m_requestsRender) {
				m_hadFocus = true;
				return false;
			}
			if (m_hadFocus) {
				m_hadFocus = false;
				return true;
			}
			return false;
		}

		/// One-call focus reconciliation — replaces SyncFocus + ConsumeFocusLost +
		/// the manual edge logic. Pass your menu-open flag by reference: when you
		/// toggle it, helper focus follows; when the helper changes focus (the user
		/// cycled overlays to/from you), your flag follows. Returns the resolved
		/// shown state — feed it to PumpInput and use it to gate NewFrame/Render.
		bool ReconcileFocus(bool& menuOpen)
		{
			if (!IsConnected())
				return menuOpen;
			const bool focused = HasFocus();
			if (menuOpen != m_prevMenuOpen) {
				if (menuOpen)
					RequestFocus();
				else
					ReleaseFocus();
			} else if (focused != menuOpen) {
				menuOpen = focused;  // helper changed focus (overlay swap) — mirror it
			}
			m_prevMenuOpen = menuOpen;
			return menuOpen;
		}

		/// Pump the wand cursor, controller buttons, and thumbstick scroll into
		/// the current ImGui context's IO. Call with `active` true while your
		/// menu is shown (your own open OR HasFocus()); pass it false otherwise
		/// so button edges stay current without poking IO.
		void PumpInput(bool active, float scrollDeadzone = 0.15f)
		{
			if (!IsConnected())
				return;

			uint32_t held = 0;
			float stickX = 0.0f, stickY = 0.0f;
			bool suppressForwarding = false;
			{
				std::scoped_lock lk{ m_mutex };
				held = m_heldMask;
				stickX = m_stickX;
				stickY = m_stickY;
				suppressForwarding = m_suppressForwarding;
			}

			// Releases that happen to land while inactive or while the VR keyboard owns the wand used to
			// be lost outright: these early-outs advanced m_prevHeld without forwarding anything, so a
			// button that let go during one of these windows left ImGui's io.MouseDown/KeysDown stuck
			// true forever (the exact same "swallowed edge" class as the off-panel case, just gated by
			// a different condition). Forward release-only edges here before bailing so a button can
			// never end up logically stuck down just because it happened to release at the wrong moment.
			const auto forceReleases = [this, held]() {
				ImGuiIO& io = ImGui::GetIO();
				const uint32_t released = m_prevHeld & ~held;
				if (!released)
					return;
				const auto onRelease = [&](Button b, auto&& fn) {
					if (released & (1u << static_cast<uint32_t>(b)))
						fn(false);
				};
				onRelease(Button::TriggerClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, d); });
				onRelease(Button::GripClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Right, d); });
				onRelease(Button::PadClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
				onRelease(Button::StickClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
				onRelease(Button::BY, [&](bool d) { io.AddKeyEvent(ImGuiKey_Tab, d); });
				onRelease(Button::AX, [&](bool d) { io.AddKeyEvent(ImGuiKey_Enter, d); });
			};

			if (!active) {
				forceReleases();
				m_prevHeld = held;  // stay current so the next open edge-detects cleanly
				return;
			}

			ImGuiIO& io = ImGui::GetIO();

			// While the VR keyboard is up, don't let the wand move the cursor or
			// click — pointing at the keyboard would otherwise deselect the focused
			// text field and close the keyboard. The keyboard has its own laser. The
			// dismiss cooldown extends this so the OK-trigger release can't click the
			// field and immediately re-open the keyboard.
			if (m_kbShown || m_kbDismissCooldown > 0) {
				forceReleases();
				m_pointerInPanel = true;  // the keyboard owns the wand; suppress off-panel client shortcuts
				m_prevHeld = held;        // keep edges current for when it closes
				return;
			}

			// Wand-laser intersection → ImGui cursor. WantSetMousePos warps the
			// OS cursor so the platform backend reads our position back instead
			// of the (off-screen, in VR) desktop cursor.
			float u = 0.0f, v = 0.0f;
			const bool onPanel = m_helper->GetPointer(m_id, &u, &v, nullptr);
			m_pointerInPanel = onPanel;
			if (onPanel) {
				const float x = std::clamp(u * io.DisplaySize.x, 0.0f, io.DisplaySize.x);
				const float y = std::clamp(v * io.DisplaySize.y, 0.0f, io.DisplaySize.y);
				io.MousePos = ImVec2(x, y);
				io.AddMousePosEvent(x, y);
				io.MouseDrawCursor = true;
				io.WantSetMousePos = true;
			} else if (ImGui::GetActiveID() == 0) {
				// Wand off the panel and nothing is active: park the cursor off-screen so a click
				// can't land on the last-hovered widget, and skip the pointer-click synthesis below
				// — off-panel input belongs to the client (e.g. a tool shortcut, read via
				// IsButtonHeld).
				//
				// But NOT while something IS active (ActiveId != 0 -- a button held, a slider or
				// drag-float being dragged, custom click-and-drag tracking, etc.): teleporting the
				// cursor to an extreme off-screen position mid-gesture is fine for a plain button
				// (ActiveId persists independent of mouse position until release) but corrupts any
				// widget that computes its value from continuous mouse position, and can leave that
				// widget's own internal drag state stuck once the ray drifts back on/off panel.
				// Hold the cursor at its last known (on-panel) position instead -- the same
				// "capture" semantics a desktop OS gives a window mid-drag, and the active widget
				// still ends normally on release regardless (button-release forwarding doesn't
				// depend on cursor position).
				io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
				io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
				io.MouseDrawCursor = false;
			}

			const uint32_t changed = held ^ m_prevHeld;
			const auto onEdge = [&](Button b, auto&& fn) {
				const uint32_t bit = 1u << static_cast<uint32_t>(b);
				if (!(changed & bit))
					return;
				const bool down = (held & bit) != 0;
				// Presses drive the UI only while the wand is on the panel (off-panel belongs to the
				// client's own shortcuts, read via IsButtonHeld) -- but releases always forward
				// regardless of onPanel. A press that starts on the panel and ends after the wand
				// drifts off it (e.g. a drag that moves the hand far enough to slip the ray off the
				// panel edge) used to have its release edge gated the same as the press: skipped
				// entirely when onPanel was false that frame, with m_prevHeld still updated to match,
				// so the edge was lost forever. ImGui then believed the button was held down with no
				// way to un-stick it short of an unrelated nav event clearing ActiveId.
				if (down && !onPanel)
					return;
				fn(down);
			};
			// Suppressed while a helper-owned modal gesture (e.g. an overlay reposition drag) is in
			// progress -- grip both drives that drag and maps to a client right-click below, so without
			// this a wand ray sweeping onto the panel mid-drag would forward the still-held grip as an
			// unintended click. m_prevHeld still advances every frame regardless (see below), so a
			// button already held when suppression lifts requires a fresh release+press to register as
			// a genuine new click, rather than firing a phantom edge the instant forwarding resumes.
			if (!suppressForwarding) {
				onEdge(Button::TriggerClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Left, d); });
				onEdge(Button::GripClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Right, d); });
				onEdge(Button::PadClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
				onEdge(Button::StickClick, [&](bool d) { io.AddMouseButtonEvent(ImGuiMouseButton_Middle, d); });
				onEdge(Button::BY, [&](bool d) { io.AddKeyEvent(ImGuiKey_Tab, d); });
				onEdge(Button::AX, [&](bool d) { io.AddKeyEvent(ImGuiKey_Enter, d); });
			}
			m_prevHeld = held;

			// Recovery watchdog: a wand click's press/release edges are inherently noisier than a real
			// mouse's (analog trigger threshold jitter, the ray drifting off/onto the panel mid-drag),
			// and something in that noise can occasionally leave ImGui's ActiveId pinned to a widget
			// after the client's own MouseDown has already and correctly gone false -- observed as a
			// drag slider that can never be clicked away from until an unrelated nav event (e.g. Tab)
			// clears focus. Rather than chase the exact noise pattern (a fixed release-debounce window
			// just traded this bug for "never releases" under sustained chatter), force it loose once
			// MouseDown has been confirmed false for longer than any real release ever takes.
			//
			// Gated on !io.WantTextInput: an active InputText (or a drag slider's temp numeric-entry
			// mode) legitimately holds ActiveId with MouseDown false for as long as the user is typing,
			// which isn't chatter -- without this the watchdog would yank focus out from under someone
			// mid-edit every 300ms. WantTextInput is exactly the signal ImGui itself already uses to
			// mean "a text-editing widget wants keyboard focus right now".
			if (const auto activeId = ImGui::GetActiveID()) {
				constexpr float kStuckActiveIdSeconds = 0.3f;
				if (!io.MouseDown[ImGuiMouseButton_Left] && !io.WantTextInput) {
					m_stuckActiveIdTimer += io.DeltaTime;
					if (m_stuckActiveIdTimer >= kStuckActiveIdSeconds) {
						ImGui::ClearActiveID();
						m_stuckActiveIdTimer = 0.0f;
					}
				} else {
					m_stuckActiveIdTimer = 0.0f;
				}
			} else {
				m_stuckActiveIdTimer = 0.0f;
			}

			// Thumbstick → discrete wheel ticks.
			constexpr float kScrollAccumRate = 0.1f;
			constexpr float kScrollTickThreshold = 0.3f;
			if (std::abs(stickX) > scrollDeadzone || std::abs(stickY) > scrollDeadzone) {
				m_accumX += stickX * kScrollAccumRate;
				m_accumY += stickY * kScrollAccumRate;
				float wheelX = 0.0f, wheelY = 0.0f;
				if (std::abs(m_accumX) > kScrollTickThreshold) {
					wheelX = m_accumX > 0.0f ? 1.0f : -1.0f;
					m_accumX = 0.0f;
				}
				if (std::abs(m_accumY) > kScrollTickThreshold) {
					wheelY = m_accumY > 0.0f ? 1.0f : -1.0f;
					m_accumY = 0.0f;
				}
				if (wheelX != 0.0f || wheelY != 0.0f)
					io.AddMouseWheelEvent(-wheelX, wheelY);
			}
		}

		/// Blit the current ImGui draw data into the helper-owned panel RTV.
		/// Call after ImGui::Render(). Saves and restores the bound render
		/// target / viewport so the rest of your frame is undisturbed.
		///
		/// VR: this panel is your ONLY headset output. When connected, render to
		/// the panel and do NOT also run your normal in-game draw
		/// (ImGui_ImplDX11_RenderDrawData) into the game's frame for the VR view.
		/// If your menu hook draws into the game's HUD/menu target (e.g. Skyrim's
		/// kHUDMENU), the engine wraps that flat texture onto its CURVED world HUD
		/// and your menu comes out sheared/deformed — a second, mangled copy
		/// alongside this flat panel. Gate it: `if (IsConnected()) RenderToPanel();
		/// else ImGui_ImplDX11_RenderDrawData();`. Hooking at Present (the desktop
		/// swapchain/mirror) instead is harmless, since the mirror isn't shown in
		/// the headset.
		void RenderToPanel(ID3D11DeviceContext* ctx = nullptr)
		{
			if (!IsConnected())
				return;

			PanelHandle panel{};
			if (!m_helper->GetPanel(m_id, &panel) || panel.rtv == nullptr)
				return;

			// ctx is optional: default to the process immediate context (the one
			// ImGui_ImplDX11 was initialized with), derived from the panel RTV's
			// device. Pass an explicit ctx only for a deferred-context renderer.
			if (!ctx)
				ctx = ResolveImmediateContext(panel);
			if (!ctx)
				return;

			ImDrawData* drawData = ImGui::GetDrawData();
			if (drawData && drawData->Valid && drawData->CmdListsCount > 0) {
				BlitDrawData(ctx, panel, drawData);
				m_panelWasShowing = true;
			} else if (m_panelWasShowing) {
				// Nothing to draw this frame — clear once so the helper doesn't keep
				// compositing the last frame's pixels during an overlay swap.
				const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx->ClearRenderTargetView(panel.rtv, clear);
				m_panelWasShowing = false;
			}
		}

		/// Your single per-frame output call — the drop-in replacement for a bare
		/// `ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData())`. Call after
		/// ImGui::Render(). When connected (VR), renders ONLY to the flat panel; on
		/// flat screen / helper absent, runs your normal in-game draw into whatever
		/// RT your hook has bound. Use this and you never write the VR/desktop branch
		/// yourself — so you can't accidentally paint a second, curved-HUD copy of
		/// your menu (see RenderToPanel). `ctx` is optional (see RenderToPanel).
		void RenderFrame(ID3D11DeviceContext* ctx = nullptr)
		{
			if (IsConnected())
				RenderToPanel(ctx);
			else
				ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		}

		/// Per-frame convenience: ReconcileFocus + PumpInput in one call. Pass your
		/// menu-open flag by reference; on return it reflects the resolved shown
		/// state (the helper's open/close/cycle combos can flip it) and the wand has
		/// been pumped into ImGui. Returns the shown state. Call before NewFrame.
		bool Update(bool& menuOpen, float scrollDeadzone = 0.15f)
		{
			ReconcileFocus(menuOpen);
			PumpInput(menuOpen, scrollDeadzone);
			return menuOpen;
		}

		/// One-call interactive menu for a VR mod that brings its own ImGui context
		/// + DX11 backend (the common case). Reconciles focus against `menuOpen`,
		/// pumps controller input, and — every frame — runs NewFrame -> your draw()
		/// -> Render on the CURRENT context and blits to the panel (clearing once
		/// when hidden). `draw` only runs while shown. Returns the resolved shown
		/// state. It owns NewFrame/Render, so don't also drive them yourself that
		/// frame. For desktop mouse/keyboard you still feed ImGui yourself before
		/// this (e.g. a Win32 backend); this adds only the VR controller input —
		/// which is why a desktop+VR client with a custom frame (like Community
		/// Shaders) drives the steps individually instead of using this.
		bool RenderMenu(ID3D11DeviceContext* ctx, bool& menuOpen, const std::function<void()>& draw)
		{
			if (!IsConnected() || !ctx || !draw)
				return menuOpen;
			ReconcileFocus(menuOpen);
			const bool shown = menuOpen;
			PumpInput(shown);
			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();
			if (shown)
				draw();
			ImGui::Render();
			RenderToPanel(ctx);  // blits when drawn, clears once when hidden
			return menuOpen;
		}

		/// Set once: loads your fonts + style into the private HUD context, called
		/// right after it's created, so RenderHud matches your desktop look.
		void SetHudStyleCallback(std::function<void()> styleSetup) { m_hudStyle = std::move(styleSetup); }

		[[nodiscard]] float GetHudCoverage() const { return m_hudCoverage; }
		[[nodiscard]] float GetHudDepth() const { return m_hudDepth; }

		/// Render an always-on HUD layer (a kClientFlag_HUDMode client) in one
		/// call — the whole context lifecycle the flag promises, done for you.
		/// Owns a private, non-interactive ImGui context + DX11 backend, sizes it
		/// to the (supersampled) panel so text stays crisp, re-honors window
		/// positions every frame (a passive mirror can't be dragged), runs `draw`
		/// to build the UI in your own style, blits to the panel, and clears it
		/// once when nothing is drawn. `logicalSize` is your UI's coordinate space
		/// (typically your desktop DisplaySize) so layout matches the flat screen.
		void RenderHud(ID3D11Device* device, ID3D11DeviceContext* ctx, ImVec2 logicalSize,
			const std::function<void()>& draw)
		{
			if (!IsConnected() || !device || !ctx || !draw)
				return;
			if (logicalSize.x <= 0.0f || logicalSize.y <= 0.0f)
				return;  // a zero DisplaySize would clip everything to a blank HUD

			ScopedImGuiContext guard;  // restores the caller's context on every exit

			if (!m_hudCtx) {
				m_hudCtx = ImGui::CreateContext();
				ImGui::SetCurrentContext(m_hudCtx);
				ImGuiIO& io = ImGui::GetIO();
				io.IniFilename = nullptr;
				io.LogFilename = nullptr;
				if (!ImGui_ImplDX11_Init(device, ctx)) {
					ImGui::DestroyContext(m_hudCtx);
					m_hudCtx = nullptr;
					return;
				}
				if (m_hudStyle)
					m_hudStyle();
			}

			PanelHandle panel{};
			if (!m_helper->GetPanel(m_id, &panel) || panel.rtv == nullptr)
				return;

			ImGui::SetCurrentContext(m_hudCtx);
			ImGuiIO& io = ImGui::GetIO();
			io.DisplaySize = logicalSize;
			if (panel.width && panel.height && logicalSize.x > 0.0f && logicalSize.y > 0.0f)
				io.DisplayFramebufferScale = ImVec2(static_cast<float>(panel.width) / logicalSize.x,
					static_cast<float>(panel.height) / logicalSize.y);
			io.DeltaTime = 1.0f / 60.0f;

			ImGui_ImplDX11_NewFrame();
			ImGui::NewFrame();

			// Passive mirror: re-allow the positional Cond flags so FirstUseEver/Once
			// act like Always, and every window tracks its source position each frame.
			for (ImGuiWindow* w : m_hudCtx->Windows) {
				w->SetWindowPosAllowFlags |=
					ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
				w->SetWindowSizeAllowFlags |=
					ImGuiCond_Always | ImGuiCond_Once | ImGuiCond_FirstUseEver | ImGuiCond_Appearing;
			}

			draw();

			ImGui::Render();
			ImDrawData* drawData = ImGui::GetDrawData();
			if (drawData && drawData->Valid && drawData->CmdListsCount > 0) {
				BlitDrawData(ctx, panel, drawData);
				m_hudWasShowing = true;
			} else if (m_hudWasShowing) {
				const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				ctx->ClearRenderTargetView(panel.rtv, clear);
				m_hudWasShowing = false;
			}
		}

		/// Destroy the private HUD context + DX11 backend (call on shutdown). Safe
		/// if RenderHud was never used.
		void ShutdownHud()
		{
			if (!m_hudCtx)
				return;
			ScopedImGuiContext guard;
			ImGui::SetCurrentContext(m_hudCtx);
			ImGui_ImplDX11_Shutdown();
			ImGui::DestroyContext(m_hudCtx);
			m_hudCtx = nullptr;
		}

		/// Forward a raw VR controller event into the helper (for clients that
		/// own a PollInputDevices-style hook). Optional.
		void FeedVREvent(uint32_t device, uint32_t key, bool pressed, float stickX, float stickY)
		{
			if (IsConnected())
				m_helper->FeedVREvent(device, key, pressed, stickX, stickY);
		}

		// ---- Bindings table widget -------------------------------------

		/// Draw a sortable / filterable table of every combo registered via
		/// AddCombo, color-coded per controller, with per-row Rebind (live
		/// capture), Clear (unbind), and — when a default was supplied to
		/// AddCombo and the binding differs from it — Reset. All three persist
		/// via your onRebind. Drop into your own settings window. `strId` must be
		/// unique if you draw more than one.
		void DrawBindingsTable(const char* strId = "##vrhelper_bindings")
		{
			if (m_combos.empty()) {
				ImGui::TextDisabled("No VR bindings registered.");
				return;
			}

			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##vrhelper_filter", "Filter bindings...", m_filter,
				sizeof(m_filter));

			const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg |
			                              ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
			if (!ImGui::BeginTable(strId, 3, flags))
				return;

			ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_DefaultSort, 0.45f);
			ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_NoSort, 0.45f);
			ImGui::TableSetupColumn("##rebind", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			// Filtered view (pointers into m_combos — stable list nodes).
			std::vector<ComboEntry*> rows;
			rows.reserve(m_combos.size());
			for (ComboEntry& e : m_combos) {
				if (m_filter[0] == '\0' || ContainsCI(e.label, m_filter))
					rows.push_back(&e);
			}

			if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
				specs && specs->SpecsCount > 0) {
				const bool ascending = specs->Specs[0].SortDirection != ImGuiSortDirection_Descending;
				std::sort(rows.begin(), rows.end(), [ascending](ComboEntry* a, ComboEntry* b) {
					const int cmp = CompareCI(a->label, b->label);
					return ascending ? (cmp < 0) : (cmp > 0);
				});
			}

			for (ComboEntry* e : rows) {
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(e->id));

				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(e->label.empty() ? "(unnamed)" : e->label.c_str());

				ImGui::TableSetColumnIndex(1);
				if (e->keys.empty()) {
					ImGui::TextDisabled("(unbound)");
				} else {
					for (std::size_t i = 0; i < e->keys.size(); ++i) {
						if (i != 0) {
							ImGui::SameLine(0.0f, 0.0f);
							ImGui::TextDisabled(" + ");
							ImGui::SameLine(0.0f, 0.0f);
						}
						const InputCombo& k = e->keys[i];
						ImGui::TextColored(DeviceColor(k.GetDevice()), "%s",
							ButtonName(k.GetDevice(), k.GetKey()).c_str());
					}
				}

				ImGui::TableSetColumnIndex(2);
				if (ImGui::SmallButton("Rebind"))
					m_helper->StartComboRecording(m_id, e->label.c_str(), &RecordThunk, e, 5.0f);
				if (!e->keys.empty()) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear"))
						m_helper->RebindCombo(e->id, nullptr, 0);  // unbind (empty chord)
				}
				// Reset shown only when a factory default was provided and the
				// current binding differs from it.
				if (!e->defaults.empty() && e->keys != e->defaults) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset"))
						m_helper->RebindCombo(e->id, e->defaults.data(), e->defaults.size());
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

	private:
		struct ComboEntry
		{
			Client* owner = nullptr;
			ComboId id = 0;
			std::string label;
			std::vector<InputCombo> keys;
			std::vector<InputCombo> defaults;  ///< factory default, for the Reset button
			RebindCallback onRebind;
			bool offPanel = false;  ///< only fires while the wand is off this client's panel
		};

		static void OnFrameThunk(const Frame* f, void* user)
		{
			auto* self = static_cast<Client*>(user);
			if (!self)
				return;
			self->m_requestsRender = f && (f->flags & kFrameFlag_HasFocus) != 0;
			if (!f)
				return;
			// Pick the larger-magnitude stick axis across both hands so either
			// controller can scroll.
			const float sx = (std::abs(f->left.stick_x) >= std::abs(f->right.stick_x)) ? f->left.stick_x : f->right.stick_x;
			const float sy = (std::abs(f->left.stick_y) >= std::abs(f->right.stick_y)) ? f->left.stick_y : f->right.stick_y;
			std::scoped_lock lk{ self->m_mutex };
			self->m_heldMask = f->left.buttons_held | f->right.buttons_held;
			self->m_suppressForwarding = (f->flags & kFrameFlag_SuppressInputForwarding) != 0;
			self->m_stickX = sx;
			self->m_stickY = sy;
			if (f->struct_size >= offsetof(Frame, hud_coverage) + sizeof(float)) {
				self->m_hudDepth = f->hud_depth;
				self->m_hudCoverage = f->hud_coverage;
			}
		}

		// on_rebind from the helper: live keys already updated; mirror them into
		// our entry (so the table refreshes) and notify the owner to persist.
		static void RebindThunk(const InputCombo* keys, std::size_t n, void* user)
		{
			auto* e = static_cast<ComboEntry*>(user);
			if (!e)
				return;
			e->keys.assign(keys, keys + n);  // n == 0 clears (unbind)
			if (e->onRebind)
				e->onRebind(keys, n);
		}

		// Recording finished for a bindings-table Rebind button: commit via the
		// helper, which updates the live keys and fires RebindThunk.
		static void RecordThunk(const InputCombo* keys, std::size_t n, void* user)
		{
			auto* e = static_cast<ComboEntry*>(user);
			if (!e || !e->owner || !e->owner->m_helper || n == 0)
				return;
			e->owner->m_helper->RebindCombo(e->id, keys, n);
		}

		static int CompareCI(const std::string& a, const std::string& b)
		{
			const std::size_t n = std::min(a.size(), b.size());
			for (std::size_t i = 0; i < n; ++i) {
				const int ca = std::tolower(static_cast<unsigned char>(a[i]));
				const int cb = std::tolower(static_cast<unsigned char>(b[i]));
				if (ca != cb)
					return ca - cb;
			}
			return static_cast<int>(a.size()) - static_cast<int>(b.size());
		}

		static bool ContainsCI(const std::string& haystack, const char* needle)
		{
			std::string h, n;
			h.reserve(haystack.size());
			for (char c : haystack)
				h.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			for (const char* p = needle; *p; ++p)
				n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
			return h.find(n) != std::string::npos;
		}

		// Blit the current draw data into `panel`, restoring the prior RT + viewport.
		// Shared by RenderToPanel and RenderHud.
		void BlitDrawData(ID3D11DeviceContext* ctx, const PanelHandle& panel, ImDrawData* drawData)
		{
			ID3D11RenderTargetView* prevRTV = nullptr;
			ID3D11DepthStencilView* prevDSV = nullptr;
			ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);

			D3D11_VIEWPORT prevViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			UINT prevViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
			ctx->RSGetViewports(&prevViewportCount, prevViewports);

			D3D11_VIEWPORT vp{};
			vp.Width = static_cast<float>(panel.width);
			vp.Height = static_cast<float>(panel.height);
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			ctx->RSSetViewports(1, &vp);

			ID3D11RenderTargetView* rtv = panel.rtv;
			ctx->OMSetRenderTargets(1, &rtv, nullptr);
			const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			ctx->ClearRenderTargetView(rtv, clearColor);

			ImGui_ImplDX11_RenderDrawData(drawData);

			ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
			if (prevViewportCount > 0)
				ctx->RSSetViewports(prevViewportCount, prevViewports);
			if (prevRTV)
				prevRTV->Release();
			if (prevDSV)
				prevDSV->Release();
		}

		// Derive (and cache) the process immediate device context from the panel
		// RTV's device. There is one ID3D11Device in the game, so this is the same
		// context ImGui_ImplDX11 was initialized with. GetImmediateContext returns
		// an AddRef'd pointer, but the device owns its immediate context for its
		// whole life, so we drop our ref and keep the raw pointer.
		ID3D11DeviceContext* ResolveImmediateContext(const PanelHandle& panel)
		{
			if (m_cachedContext)
				return m_cachedContext;
			ID3D11RenderTargetView* rtv = panel.rtv;
			if (!rtv)
				return nullptr;
			ID3D11Device* device = nullptr;
			rtv->GetDevice(&device);
			if (!device)
				return nullptr;
			device->GetImmediateContext(&m_cachedContext);
			if (m_cachedContext)
				m_cachedContext->Release();  // device retains ownership
			device->Release();
			return m_cachedContext;
		}

		IImGuiVRHelperInterface001* m_helper = nullptr;
		IImGuiVRHelperInterface002* m_helper002 = nullptr;  // null if helper predates 002 (no VR keyboard)
		IImGuiVRHelperInterface003* m_helper003 = nullptr;  // null if helper predates 003 (no off-panel combos)
		IImGuiVRHelperInterface004* m_helper004 = nullptr;  // null if helper predates 004 (no world quads)
		IImGuiVRHelperInterface005* m_helper005 = nullptr;  // null if helper predates 005 (no client-driven reposition)
		uint32_t m_id = 0;
		ID3D11DeviceContext* m_cachedContext = nullptr;  // ctx resolved lazily, device-owned
		bool m_requestsRender = false;

		bool m_kbShown = false;     // PumpKeyboard: keyboard latched open (suppresses wand input)
		std::string m_kbDelivered;  // PumpKeyboard: text already replayed into the field
		// Frames after a dismiss during which the keyboard won't re-open and the wand
		// stays suppressed, so the OK-trigger release can't re-focus the field.
		static constexpr int kKbDismissCooldown = 30;
		int m_kbDismissCooldown = 0;

		// OnFrame snapshot (frame thread → render thread).
		mutable std::mutex m_mutex;
		uint32_t m_heldMask = 0;
		// kFrameFlag_SuppressInputForwarding mirror -- a helper-owned modal gesture (e.g. an overlay
		// drag) is in progress. buttons_held above still reflects genuine raw state; only the
		// ImGui-forwarding step in PumpInput stands down while this is set.
		bool m_suppressForwarding = false;
		float m_stickX = 0.0f;
		float m_stickY = 0.0f;
		float m_hudDepth = 1.0f;
		float m_hudCoverage = 0.5f;

		// Render-thread-only state.
		uint32_t m_prevHeld = 0;
		// Last GetPointer result from PumpInput. Atomic because the public IsPointerInPanel() may be
		// read off the render thread (e.g. from a client's on_frame callback).
		std::atomic<bool> m_pointerInPanel = false;
		bool m_focusRequested = false;
		bool m_hadFocus = false;
		bool m_prevMenuOpen = false;  // ReconcileFocus edge state
		float m_accumX = 0.0f;
		float m_accumY = 0.0f;
		// Seconds ImGui's ActiveId has been non-zero while this client's own MouseDown reads false --
		// see the recovery-watchdog comment in PumpInput.
		float m_stuckActiveIdTimer = 0.0f;

		bool m_panelWasShowing = false;  // RenderToPanel clear-once

		// RenderHud private context: its own ImGui context + DX11 backend + style.
		ImGuiContext* m_hudCtx = nullptr;
		bool m_hudWasShowing = false;
		std::function<void()> m_hudStyle;

		std::list<ComboEntry> m_combos;  // stable node addresses back the thunk user pointers
		char m_filter[64] = {};
	};

}  // namespace ImGuiVRHelperPluginAPI
