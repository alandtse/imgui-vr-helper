// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// VRIK compatibility shim: reads VRIK's per-frame camera-bob/height-smoothing offset so
// world-quad billboards (built from real, un-bobbed OpenVR tracking) can be shifted to match
// how VRIK's head-bob makes the rest of Skyrim's own scene render appear to move. Without this,
// a VRIK head-bob user sees world-quad billboards visually drift relative to bobbing NPCs/scenery
// while walking, even though the billboard's own computed position never moves (root-caused via
// RenderDoc + in-game telemetry: RoomNode's position and orientation are provably stable through
// active movement, so the divergence isn't in this project's conversion pipeline).
//
// vrikPluginApi::IVrikInterface001 (used below) is VRIK's own unmodified public SDK header —
// third_party/vrikinterface/vrikinterface001.h — included directly rather than hand-copied, so
// the ABI-critical vtable layout stays byte-identical to VRIK's own source instead of a
// maintained-by-hand duplicate. src/skse64/{PluginAPI,NiTypes}.h shim the legacy skse64 type
// names that header was written against onto this project's own types (see those files for why).

#include "pch.h"

#include "VrikCompat.h"

#include "../third_party/vrikinterface/vrikinterface001.h"

#include <cmath>

namespace ImGuiVRHelper::VrikCompat
{
	namespace
	{
		vrikPluginApi::IVrikInterface001* g_vrik = nullptr;
		bool g_fetchAttempted = false;

		// VRIK's handshake message (see vrikinterface001.cpp): dispatch kMessage_GetInterface to
		// "VRIK", it fills getApiFunction, then call that with revision 1 for the interface.
		struct VrikMessage
		{
			void* (*getApiFunction)(unsigned int revisionNumber) = nullptr;
		};
		constexpr std::uint32_t kMessage_GetInterface = 0xF2AFAEE6;  // chosen by VRIK, not us
	}

	void TryFetchInterface()
	{
		if (g_fetchAttempted)
			return;
		g_fetchAttempted = true;

		auto* messaging = SKSE::GetMessagingInterface();
		if (!messaging)
			return;

		VrikMessage msg;
		// dataLen = sizeof(VrikMessage*), not sizeof(VrikMessage): matches VRIK's own reference
		// dispatch call exactly. VRIK's receiver doesn't validate dataLen, but keep this
		// bit-identical to their SDK rather than "fix" a value we don't control the other end of.
		messaging->Dispatch(kMessage_GetInterface, &msg, sizeof(VrikMessage*), "VRIK");
		if (!msg.getApiFunction) {
			logs::info("VrikCompat: VRIK not installed (or interface unavailable)");
			return;
		}

		try {
			g_vrik = static_cast<vrikPluginApi::IVrikInterface001*>(msg.getApiFunction(1));
			if (g_vrik) {
				logs::info("VrikCompat: VRIK interface acquired (build {})", g_vrik->getBuildNumber());
			} else {
				logs::warn("VrikCompat: VRIK responded but revision 1 interface was unavailable");
			}
		} catch (...) {
			g_vrik = nullptr;
			logs::error("VrikCompat: getApiFunction/getBuildNumber threw; VRIK compat disabled");
		}
	}

	RE::NiPoint3 GetCameraOffset()
	{
		if (!g_vrik)
			return RE::NiPoint3::Zero();
		try {
			// Only catches a thrown C++ exception (e.g. VRIK's own getter failing internally) —
			// not a hardware access violation from a genuine vtable/ABI mismatch, which this
			// build isn't compiled with /EHa to catch. That's an accepted, pre-existing tradeoff
			// in this project (see hk_Present's DispatchFrame catch), not special-cased here.
			const auto offset = g_vrik->getFinalCameraOffsettingAmount();
			// Untrusted external output: a NaN/Inf here would flow straight into origin.z and
			// then per-billboard math whose own isfinite guards (InSceneOverlay.cpp) only cover
			// client-supplied positions, not this compensation term.
			if (!std::isfinite(offset.x) || !std::isfinite(offset.y) || !std::isfinite(offset.z)) {
				logs::warn("VrikCompat: getFinalCameraOffsettingAmount returned a non-finite value");
				return RE::NiPoint3::Zero();
			}
			return offset;
		} catch (...) {
			g_vrik = nullptr;
			logs::error("VrikCompat: getFinalCameraOffsettingAmount threw; disabling VRIK compat");
			return RE::NiPoint3::Zero();
		}
	}
}
