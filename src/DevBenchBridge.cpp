// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// devbench tool registrations. Two surfaces:
//
//   inspect kind=imguivrhelper  — live input/focus dump (wand hit + override
//       flag, drag state, per-hand held wire masks, lease strips, clients with
//       panel dims, relevant settings). One call replaces the two-log
//       correlation exercise the pointer-divergence investigation needed.
//   imguivrhelper.input         — synthetic input: force the wand pointer to a
//       UV, press/release buttons, or clear overrides. Injected on the input
//       thread through the same state real events land in, so leases, combos,
//       and edge detection behave identically — deterministic clicks at exact
//       UVs with no headset and no aim error.
//
// Handlers run on devbench's listener thread: they only touch atomics, the
// injection queue, and mutex-guarded/diagnostic reads — never IVR* calls.

#include "pch.h"

#include "DevBenchBridge.h"

#include "HelperImpl.h"
#include "Input.h"
#include "Overlay.h"

#include <RE/B/BSOpenVRControllerDevice.h>

#include <DevBenchAPI.h>
#include <nlohmann/json.hpp>

namespace ImGuiVRHelper::DevBenchBridge
{
	namespace
	{
		using json = nlohmann::json;

		// Shared handler wrapper: no exception may cross the DLL boundary, and
		// a_write is called exactly once (same contract as sibling bridges).
		void RunHandler(json (*a_build)(const json&), const char* a_argsJson,
			void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			json out;
			try {
				json args = json::object();
				if (a_argsJson && *a_argsJson)
					args = json::parse(a_argsJson);
				if (!args.is_object())
					throw std::runtime_error("arguments must be a JSON object");
				out = a_build(args);
			} catch (const std::exception& e) {
				out = json{ { "error", "invalid request" }, { "detail", e.what() } };
			} catch (...) {
				out = json{ { "error", "unknown handler error" } };
			}
			const std::string dumped = out.dump();
			a_write(a_sink, dumped.c_str());
		}

		json BuildInspect(const json&)
		{
			return json::parse(HelperImpl::GetSingleton().DiagnosticsJson());
		}

		void InspectHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			RunHandler(&BuildInspect, a_argsJson, a_sink, a_write);
		}

		uint32_t ButtonKeyFromName(const std::string& name)
		{
			using Keys = RE::BSOpenVRControllerDevice::Keys;
			if (name == "trigger")
				return static_cast<uint32_t>(Keys::kTrigger);
			if (name == "grip")
				return static_cast<uint32_t>(Keys::kGrip);
			if (name == "ax")
				return static_cast<uint32_t>(Keys::kXA);
			if (name == "by")
				return static_cast<uint32_t>(Keys::kBY);
			if (name == "stick")
				return static_cast<uint32_t>(Keys::kJoystickTrigger);
			if (name == "pad")
				return static_cast<uint32_t>(Keys::kTouchpadClick);
			return 0;
		}

		json BuildInput(const json& args)
		{
			auto& state = Overlay::State::GetSingleton();
			const std::string action = args.value("action", "");

			if (action == "pointer") {
				const float u = args.value("u", 0.5f);
				const float v = args.value("v", 0.5f);
				state.debugPointer.u.store(u, std::memory_order_relaxed);
				state.debugPointer.v.store(v, std::memory_order_relaxed);
				state.debugPointer.active.store(true, std::memory_order_relaxed);
				return json{ { "ok", true }, { "pointer", { { "u", u }, { "v", v } } } };
			}
			if (action == "button") {
				const std::string name = args.value("button", "");
				const uint32_t key = ButtonKeyFromName(name);
				if (!key)
					return json{ { "error", "unknown button" },
						{ "expected", "trigger|grip|ax|by|stick|pad" } };
				const bool pressed = args.value("pressed", true);
				const bool primary = args.value("hand", std::string("primary")) != "secondary";
				Input::InjectButton(primary, key, pressed);
				return json{ { "ok", true }, { "button", name }, { "pressed", pressed } };
			}
			if (action == "clear") {
				state.debugPointer.active.store(false, std::memory_order_relaxed);
				return json{ { "ok", true } };
			}
			return json{ { "error", "unknown action" }, { "expected", "pointer|button|clear" } };
		}

		void InputHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			RunHandler(&BuildInput, a_argsJson, a_sink, a_write);
		}
	}

	void Install()
	{
		auto* dvb = DevBenchAPI::GetDevBenchInterface001();
		if (!dvb) {
			logs::info("devbench not installed; helper tools not registered");
			return;
		}

		static constexpr auto kInspectDesc = R"({
			"description": "ImGuiVRHelper live input/focus state: wand hit + override flag, drag state, per-hand held wire masks, input-lease strips, registered clients with panel dims, input settings.",
			"inputSchema": { "type": "object", "properties": {} },
			"readOnly": true
		})";
		static constexpr auto kInputDesc = R"({
			"description": "Synthetic VR input for ImGuiVRHelper. action=pointer {u,v}: force the wand hit to a panel UV (persists until clear). action=button {button: trigger|grip|ax|by|stick|pad, pressed, hand: primary|secondary}: inject a press/release on the input thread (same path as real events). action=clear: drop the pointer override.",
			"inputSchema": { "type": "object", "properties": {
				"action": { "type": "string", "enum": ["pointer", "button", "clear"] },
				"u": { "type": "number" }, "v": { "type": "number" },
				"button": { "type": "string" }, "pressed": { "type": "boolean" },
				"hand": { "type": "string", "enum": ["primary", "secondary"] } },
				"required": ["action"] },
			"readOnly": false
		})";

		// inspect extension keeps the agent-facing tool list small; hosts older
		// than 1.5.0 lack that vtable slot, so fall back to a top-level tool.
		if (dvb->GetBuildNumber() >= 10500) {
			dvb->RegisterToolExtension("inspect", "imguivrhelper", kInspectDesc, &InspectHandler, nullptr);
		} else {
			dvb->RegisterTool("imguivrhelper.inspect", kInspectDesc, &InspectHandler, nullptr);
		}
		dvb->RegisterTool("imguivrhelper.input", kInputDesc, &InputHandler, nullptr);
		logs::info("devbench bridge registered (host build {})", dvb->GetBuildNumber());
	}
}
