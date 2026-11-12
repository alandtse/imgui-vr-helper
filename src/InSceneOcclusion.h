// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#pragma once

#include <cstdint>

namespace ImGuiVRHelper::HUDOcclusion
{
	enum class BackendStatus;  // internal/HUDOcclusion.h

	/// Install the VR-only world-start hook after render-path initialization; idempotent.
	void Install();

	/// Why the last frame did or did not stamp depth. Diagnostics only.
	BackendStatus LastStatus();

	/// One-line status for the settings UI and logs.
	const char* StatusText();
}
