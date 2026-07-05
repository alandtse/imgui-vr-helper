// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2026 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// devbench tool registrations. Two surfaces:
//
//   inspect kind=imguivrhelper  — live input/focus dump (wand hit + override
//       flag, drag state, per-hand held wire masks, lease strips, clients with
//       panel dims, relevant settings). One call gives a headless caller
//       ground truth for wand/client correlation instead of cross-referencing
//       two separately-logged streams by hand.
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

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>

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

		// dumppanel intentionally accepts any local path the caller wants (a
		// debugging/verification tool, not a sandboxed one) -- but two shapes are
		// worth rejecting outright rather than handing to SaveWICTextureToFile:
		// a UNC/network path (`\\host\share\...`) makes this process open an
		// outbound SMB connection and attempt NTLM auth against whatever host the
		// caller named, and an NTFS reserved device name (CON, NUL, COM1, ...) as
		// a path component can hang or misbehave regardless of directory. Returns
		// a reason string, or empty if the path is acceptable.
		std::string RejectUnsafeDumpPath(const std::string& path)
		{
			constexpr size_t kMaxPathLen = 4096;
			if (path.size() > kMaxPathLen)
				return "path exceeds maximum length";
			if (path.size() >= 2 &&
				((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/')))
				return "UNC/network paths are not allowed";
			static constexpr std::string_view kReservedNames[] = {
				"CON", "PRN", "AUX", "NUL",
				"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
				"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
			};
			const auto stem = std::filesystem::path(path).stem().string();
			for (const auto& reserved : kReservedNames) {
				if (stem.size() == reserved.size() &&
					std::equal(stem.begin(), stem.end(), reserved.begin(),
						[](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == b; }))
					return "reserved device name in path";
			}
			return {};
		}

		json BuildDumpPanel(const json& args)
		{
			// Resolve client by explicit id, else by name (as inspect reports it).
			uint32_t id = args.value("client_id", 0u);
			if (id == 0 && args.contains("name") && args["name"].is_string()) {
				const auto name = args["name"].get<std::string>();
				for (const auto& c : HelperImpl::GetSingleton().SnapshotClients()) {
					if (c.name == name) {
						id = c.client_id;
						break;
					}
				}
			}
			if (id == 0)
				return json{ { "error", "unknown client" }, { "hint", "pass client_id or name (see inspect kind=imguivrhelper)" } };

			// Path is treated as UTF-8 end-to-end (JSON strings are UTF-8; the actual
			// UTF-16 conversion for the WinAPI call happens in ServicePanelDumps).
			std::string path = args.value("path", std::string());
			if (path.empty()) {
				// Fetch USERPROFILE wide (may contain non-ASCII) and re-encode to UTF-8
				// rather than _dupenv_s's narrow/ANSI-codepage string, which would
				// mangle a non-ASCII username.
				std::string home = ".";
				wchar_t* wprofile = nullptr;
				size_t wlen = 0;
				if (_wdupenv_s(&wprofile, &wlen, L"USERPROFILE") == 0 && wprofile) {
					const int n = WideCharToMultiByte(
						CP_UTF8, 0, wprofile, -1, nullptr, 0, nullptr, nullptr);
					if (n > 0) {
						// n includes the NUL terminator; convert into a scratch buffer
						// rather than a string sized n-1, which would have
						// WideCharToMultiByte write that NUL into the string's own
						// (implicit, not meant to be written) terminator slot.
						std::vector<char> buf(static_cast<size_t>(n));
						WideCharToMultiByte(
							CP_UTF8, 0, wprofile, -1, buf.data(), n, nullptr, nullptr);
						home.assign(buf.data(), static_cast<size_t>(n) - 1);
					}
					free(wprofile);
				}
				path = std::format("{}\\My Games\\Skyrim VR\\SKSE\\imguivrhelper-panel-{}.png", home, id);
			} else if (const auto reason = RejectUnsafeDumpPath(path); !reason.empty()) {
				return json{ { "error", "unsafe path" }, { "reason", reason } };
			}

			HelperImpl::GetSingleton().RequestPanelDump(id, path);
			// The write happens on the next render frame (immediate-context bound
			// there); the file exists a frame or two later. Caller reads the PNG.
			return json{ { "ok", true }, { "client_id", id }, { "path", path },
				{ "note", "written on the next frame; read the PNG path" } };
		}

		void DumpPanelHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			RunHandler(&BuildDumpPanel, a_argsJson, a_sink, a_write);
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

		static constexpr auto kDumpDesc = R"({
			"description": "Save a client's helper panel texture (its own rendered content, no composited wand dot) to a PNG. client_id or name (see inspect kind=imguivrhelper), optional path. Written on the next frame; read the returned path. Use with action=pointer to verify a client's wand/canvas mapping headlessly.",
			"inputSchema": { "type": "object", "properties": {
				"client_id": { "type": "integer" }, "name": { "type": "string" },
				"path": { "type": "string" } } },
			"readOnly": false
		})";

		// devbench's packed build number is major*10000 + minor*100 + patch.
		constexpr uint32_t kMinInspectExtensionBuild = 10500;  // 1.5.0

		// inspect extension keeps the agent-facing tool list small; hosts older
		// than 1.5.0 lack that vtable slot, so fall back to a top-level tool.
		if (dvb->GetBuildNumber() >= kMinInspectExtensionBuild) {
			dvb->RegisterToolExtension("inspect", "imguivrhelper", kInspectDesc, &InspectHandler, nullptr);
		} else {
			dvb->RegisterTool("imguivrhelper.inspect", kInspectDesc, &InspectHandler, nullptr);
		}
		dvb->RegisterTool("imguivrhelper.input", kInputDesc, &InputHandler, nullptr);
		dvb->RegisterTool("imguivrhelper.dumppanel", kDumpDesc, &DumpPanelHandler, nullptr);
		logs::info("devbench bridge registered (host build {})", dvb->GetBuildNumber());
	}
}
