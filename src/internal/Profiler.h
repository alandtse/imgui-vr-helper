// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Tracy profiler wrapper. Instrumentation is opt-in: the shipped build leaves
// Tracy off, so every macro below compiles to nothing and the helper pays zero
// runtime cost. Build with `xmake f --tracy=y && xmake` to capture the VR
// overlay's per-frame cost (DispatchFrame, the eye-composite passes), then
// connect the Tracy profiler.
//
// No FrameMark here on purpose: the helper runs inside the game's render loop
// (a Present/Submit detour), not its own frame loop — the host owns the frame
// boundary. The helper only emits scoped zones, which nest under whatever frame
// is active.

#pragma once

#if defined(IMGUI_VR_HELPER_TRACY)
#	include <tracy/Tracy.hpp>
#else
#	define ZoneScoped
#	define ZoneScopedN(name)
#endif
