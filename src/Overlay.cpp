// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Overlay.h"

namespace ImGuiVRHelper::Overlay
{
	State& State::GetSingleton()
	{
		static State instance;
		return instance;
	}
}
