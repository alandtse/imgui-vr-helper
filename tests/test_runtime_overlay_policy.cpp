// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "RuntimeOverlay.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native menu overlays remain quarantined", "[runtime-overlay]")
{
	// Independent of persisted preferences, module names, runtime detection,
	// focused client, and submission thread: in-scene rendering must stay on.
	static_assert(!ImGuiVRHelper::RuntimeOverlay::kAvailable);
	static_assert(!ImGuiVRHelper::RuntimeOverlay::IsHostingPanel());
	CHECK_FALSE(ImGuiVRHelper::RuntimeOverlay::kAvailable);
	CHECK_FALSE(ImGuiVRHelper::RuntimeOverlay::IsHostingPanel());
}
