// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "RuntimeOverlay.h"

#include "Overlay.h"

namespace ImGuiVRHelper::RuntimeOverlay
{
	void RenderTick()
	{
		static bool warned = false;
		if (Overlay::State::GetSingleton().settings.useRuntimeOverlay && !warned) {
			logs::warn(
				"RuntimeOverlay: useRuntimeOverlay ignored for safety; native menu overlays "
				"are temporarily unavailable after reported GPU crashes. Using in-scene rendering.");
			warned = true;
		}
	}

	void InputTick() {}
}
