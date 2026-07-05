// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Route-on-press button leases.
//
// The stuck-input bug class this kills: a button's press and release edges
// being routed by DIFFERENT rules because gating state (drag suppression,
// modal settling, focus) changed while the button was held. Whichever side
// consumed the press but not the release left a latch stuck (ImGui MouseDown,
// settle flags), observed as "clicks stop registering after grip-moving the
// overlay". This module makes routing a property of the PRESS: the route is
// chosen once, on the press edge, and the hold and release follow it
// unconditionally. A half-pair is unrepresentable.
//
// Routes:
//   Client     — delivered to the focused client's Frame untouched. The
//                client SDK's own on-panel gate decides ImGui vs IsButtonHeld,
//                exactly as shipped SDKs (interface 003 off-panel contract)
//                already do.
//   HelperDrag — pressed while a helper-owned reposition drag is active
//                (including the grip that starts it, same frame): stripped
//                from client frames for the whole lifetime so a mid-drag
//                press can never leak in as a click.
//   Modal      — pressed while combo recording is active: stripped likewise,
//                which also replaces the post-recording "settle until all
//                buttons release" latch.
//
// Pure logic: no RE/vr/ImGui dependencies, unit-testable.

#pragma once

#include <array>
#include <cstdint>

namespace ImGuiVRHelper::InputLeases
{
	enum class Route : uint8_t
	{
		None = 0,    // button not held
		Client,      // deliver to the focused client
		HelperDrag,  // strip: helper reposition drag owns it
		Modal        // strip: combo recording owns it
	};

	struct Context
	{
		bool helperDragActive = false;  // drag in progress and NOT client-requested
		bool modalRecording = false;    // combo capture overlay is up
	};

	class Table
	{
	public:
		static constexpr std::size_t kHands = 2;  // 0 = left, 1 = right
		static constexpr std::size_t kBits = 32;  // wire mask width

		/// Advance one frame. Press edges claim a route from `ctx`; releases
		/// keep their route through this frame (so the release edge is stripped
		/// or delivered consistently with its press) and free afterwards.
		void Update(const std::array<uint32_t, kHands>& pressed,
			const std::array<uint32_t, kHands>& released, const Context& ctx)
		{
			for (std::size_t h = 0; h < kHands; ++h) {
				for (std::size_t b = 0; b < kBits; ++b) {
					const uint32_t bit = 1u << b;
					if (pressed[h] & bit) {
						m_route[h][b] = ctx.modalRecording   ? Route::Modal :
						                ctx.helperDragActive ? Route::HelperDrag :
						                                       Route::Client;
					}
				}
			}
			// Masks for THIS frame reflect presses just assigned and releases
			// not yet freed.
			for (std::size_t h = 0; h < kHands; ++h) {
				uint32_t stripped = 0;
				for (std::size_t b = 0; b < kBits; ++b) {
					if (m_route[h][b] == Route::HelperDrag || m_route[h][b] == Route::Modal)
						stripped |= 1u << b;
				}
				m_strip[h] = stripped;
			}
			for (std::size_t h = 0; h < kHands; ++h) {
				for (std::size_t b = 0; b < kBits; ++b) {
					if (released[h] & (1u << b))
						m_route[h][b] = Route::None;
				}
			}
		}

		/// Bits to strip from the focused client's Frame for `hand` this frame
		/// (held, pressed, AND released views — pair integrity).
		[[nodiscard]] uint32_t StrippedBits(std::size_t hand) const { return m_strip[hand]; }

		/// Drop all leases (handedness flip, overlay hidden): callers should
		/// zero their delivered masks the same frame so clients observe clean
		/// releases via their own edge diff.
		void Reset()
		{
			for (auto& hand : m_route)
				hand.fill(Route::None);
			m_strip = {};
		}

	private:
		std::array<std::array<Route, kBits>, kHands> m_route{};
		std::array<uint32_t, kHands> m_strip{};
	};
}
