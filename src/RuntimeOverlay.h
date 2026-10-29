// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

namespace ImGuiVRHelper::RuntimeOverlay
{
	/// Capability, not the persisted user's preference. No configuration file
	/// can opt back into the unsafe submission implementation.
	inline constexpr bool kAvailable = false;

	/// Render-thread compatibility tick. Warns once if a legacy setting asks
	/// for native overlays. Never creates an overlay or submits a texture.
	void RenderTick();

	/// Input-thread compatibility tick. No runtime calls or texture handoff.
	void InputTick();

	/// Always keep the in-scene panel visible while native hosting is disabled.
	constexpr bool IsHostingPanel() noexcept { return false; }
}
