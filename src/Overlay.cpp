// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Overlay.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace ImGuiVRHelper::Overlay
{
	State& State::GetSingleton()
	{
		static State instance;
		return instance;
	}

	namespace
	{
		const std::filesystem::path kSettingsPath = "Data/SKSE/Plugins/ImGuiVRHelper.toml";

		// Helper accessor: tbl[key].value_or(fallback). TOML-style "missing
		// keys keep defaults" — same semantics as the JSON version did via
		// j.value().
		template <class T>
		T tomlGet(const toml::table& t, std::string_view key, T fallback)
		{
			if (auto v = t[key].value<T>())
				return *v;
			return fallback;
		}

		// Read an enum stored as an int64, rejecting out-of-range values from a
		// hand-edited config (an invalid cast would be undefined behavior).
		// Valid values are [0, maxValid]; anything else keeps the fallback.
		template <class E>
		E tomlEnum(const toml::table& t, std::string_view key, E fallback, int64_t maxValid)
		{
			const int64_t raw = tomlGet<int64_t>(t, key, static_cast<int64_t>(fallback));
			if (raw < 0 || raw > maxValid)
				return fallback;
			return static_cast<E>(raw);
		}

		void FromToml(const toml::table& t, Settings& s)
		{
			// Top-level scalars (the file is intentionally flat for v1 —
			// section grouping is a polish item if/when settings grow).
			s.menuScale = tomlGet<float>(t, "menuScale", s.menuScale);
			// Enums: 0 = HMD-relative / 1 = Fixed (positioningMethod); 0..3 =
			// HMD/Controller/Both/None (attachMode); 0 = Primary / 1 = Secondary
			// (attachController).
			s.positioningMethod = tomlEnum(t, "positioningMethod", s.positioningMethod, 1);
			s.attachMode = tomlEnum(t, "attachMode", s.attachMode, 3);
			s.attachController = tomlEnum(t, "attachController", s.attachController, 1);
			s.hmdOffsetX = tomlGet<float>(t, "hmdOffsetX", s.hmdOffsetX);
			s.hmdOffsetY = tomlGet<float>(t, "hmdOffsetY", s.hmdOffsetY);
			s.hmdOffsetZ = tomlGet<float>(t, "hmdOffsetZ", s.hmdOffsetZ);
			s.controllerOffsetX = tomlGet<float>(t, "controllerOffsetX", s.controllerOffsetX);
			s.controllerOffsetY = tomlGet<float>(t, "controllerOffsetY", s.controllerOffsetY);
			s.controllerOffsetZ = tomlGet<float>(t, "controllerOffsetZ", s.controllerOffsetZ);
			s.enableWandPointing = tomlGet<bool>(t, "enableWandPointing", s.enableWandPointing);
			s.useInputLeases = tomlGet<bool>(t, "useInputLeases", s.useInputLeases);
			s.enableDragToReposition = tomlGet<bool>(t, "enableDragToReposition", s.enableDragToReposition);
			s.onlyOpenWhilePaused = tomlGet<bool>(t, "onlyOpenWhilePaused", s.onlyOpenWhilePaused);
			s.showWelcome = tomlGet<bool>(t, "showWelcome", s.showWelcome);
			s.autoResetDistance = tomlGet<float>(t, "autoResetDistance", s.autoResetDistance);
			s.mouseDeadzone = tomlGet<float>(t, "mouseDeadzone", s.mouseDeadzone);
			s.logLevel = tomlGet<std::string>(t, "logLevel", s.logLevel);
			s.showHUDDemo = tomlGet<bool>(t, "showHUDDemo", s.showHUDDemo);
			s.hudDepth = tomlGet<float>(t, "hudDepth", s.hudDepth);
			s.hudCoverage = tomlGet<float>(t, "hudCoverage", s.hudCoverage);
			s.baseWidth = static_cast<int>(tomlGet<int64_t>(t, "baseWidth", s.baseWidth));
			s.baseHeight = static_cast<int>(tomlGet<int64_t>(t, "baseHeight", s.baseHeight));
			s.hudSupersample = static_cast<int>(tomlGet<int64_t>(t, "hudSupersample", s.hudSupersample));
			s.toastTopFraction = tomlGet<float>(t, "toastTopFraction", s.toastTopFraction);
			// Guard against a hand-edited TOML allocating an enormous panel (each is
			// baseW*baseH*supersample^2*4 bytes) and OOMing the GPU at startup.
			s.baseWidth = std::clamp(s.baseWidth, 640, 7680);
			s.baseHeight = std::clamp(s.baseHeight, 480, 4320);
			s.hudSupersample = std::clamp(s.hudSupersample, 1, Config::kMaxHUDSupersample);

			// Open/close combos: arrays of packed (device << 16 | key) ints. A
			// missing key keeps the default; an empty array means unbound.
			auto readCombos = [&t](std::string_view key,
								  std::vector<ImGuiVRHelperPluginAPI::InputCombo>& out) {
				if (auto arr = t[key].as_array()) {
					out.clear();
					for (const auto& el : *arr) {
						if (auto v = el.value<int64_t>()) {
							const uint32_t packed = static_cast<uint32_t>(*v);
							out.emplace_back(
								static_cast<ImGuiVRHelperPluginAPI::InputDeviceType>(packed >> 16),
								packed & 0xFFFFu);
						}
					}
				}
			};
			readCombos("openMenuKeys", s.openMenuKeys);
			readCombos("closeMenuKeys", s.closeMenuKeys);

			// Overlay display order (client names). Missing key keeps the default
			// (empty = registration order).
			if (auto arr = t["overlayOrder"].as_array()) {
				s.overlayOrder.clear();
				for (const auto& el : *arr) {
					if (auto v = el.value<std::string>())
						s.overlayOrder.push_back(*v);
				}
			}
		}

		// Build TOML by hand so we can include section headers and
		// per-field comments — that's the whole reason for switching
		// formats. Order of fields drives reading order in the file.
		std::string ToTomlString(const Settings& s)
		{
			std::ostringstream out;
			out << "# ImGuiVRHelper settings — see\n"
				<< "# https://github.com/.../README.md for details.\n"
				<< "# Edit any value and restart the game (or close the\n"
				<< "# settings UI in-headset to save).\n\n"

				<< "# Diagnostics\n"
				<< "# Spdlog verbosity: trace, debug, info, warn, err, critical, off\n"
				<< "logLevel = \"" << s.logLevel << "\"\n\n"

				<< "# Layout\n"
				<< "menuScale = " << s.menuScale << "  # meters wide\n"
				<< "# 0 = HMD only, 1 = Controller only, 2 = Both, 3 = None\n"
				<< "attachMode = " << static_cast<int>(s.attachMode) << "\n"
				<< "# 0 = HMD-relative, 1 = Fixed in world\n"
				<< "positioningMethod = " << static_cast<int>(s.positioningMethod) << "\n"
				<< "# 0 = Primary controller, 1 = Secondary controller\n"
				<< "attachController = " << static_cast<int>(s.attachController) << "\n\n"

				<< "# HMD-relative offsets (meters)\n"
				<< "hmdOffsetX = " << s.hmdOffsetX << "\n"
				<< "hmdOffsetY = " << s.hmdOffsetY << "\n"
				<< "hmdOffsetZ = " << s.hmdOffsetZ << "\n\n"

				<< "# Controller-relative offsets (meters)\n"
				<< "controllerOffsetX = " << s.controllerOffsetX << "\n"
				<< "controllerOffsetY = " << s.controllerOffsetY << "\n"
				<< "controllerOffsetZ = " << s.controllerOffsetZ << "\n\n"

				<< "# Interaction\n"
				<< "enableWandPointing = " << (s.enableWandPointing ? "true" : "false") << "\n"
				<< "# Route-on-press input leases; false reverts to the legacy input routing\n"
				<< "# (kill-switch, remove after one release cycle)\n"
				<< "useInputLeases = " << (s.useInputLeases ? "true" : "false") << "\n"
				<< "enableDragToReposition = " << (s.enableDragToReposition ? "true" : "false") << "\n"
				<< "onlyOpenWhilePaused = " << (s.onlyOpenWhilePaused ? "true" : "false") << "\n"
				<< "showWelcome = " << (s.showWelcome ? "true" : "false") << "\n"
				<< "mouseDeadzone = " << s.mouseDeadzone << "\n"
				<< "autoResetDistance = " << s.autoResetDistance << "  # game units\n\n"

				<< "# HUD-mode smoke test. When true, the helper registers a synthetic\n"
				<< "# HUD-mode client and renders a calibration grid into its panel each\n"
				<< "# frame. Verifies kClientFlag_HUDMode end-to-end without a real client.\n"
				<< "showHUDDemo = " << (s.showHUDDemo ? "true" : "false") << "\n\n"

				<< "# HUD plane geometry. All kClientFlag_HUDMode clients render onto a\n"
				<< "# flat quad at this depth. hudCoverage is the fraction of each eye's\n"
				<< "# view the quad fills (extents derived from the per-eye frustum), so\n"
				<< "# the HUD covers the view like a screen. 1.0 = edge to edge (may clip\n"
				<< "# at the lens mask); smaller values shrink it toward a comfortable\n"
				<< "# window. Don't set hudDepth below ~0.5m — most VR lenses can't focus\n"
				<< "# closer than that.\n"
				<< "hudDepth = " << s.hudDepth << "  # meters\n"
				<< "hudCoverage = " << s.hudCoverage << "  # 0.2-1.0 fraction of the view\n"
				<< "# Overlay panel texture resolution + supersampling (applied at panel\n"
				<< "# allocation, so restart to take effect). baseWidth/Height is the logical\n"
				<< "# panel size; hudSupersample multiplies it for view-filling panels (HUD\n"
				<< "# layers, settings, toasts) to keep them sharp. toastTopFraction is the\n"
				<< "# toasts' vertical placement (0 = top, 1 = bottom).\n"
				<< "baseWidth = " << s.baseWidth << "\n"
				<< "baseHeight = " << s.baseHeight << "\n"
				<< "hudSupersample = " << s.hudSupersample << "  # 1-3\n"
				<< "toastTopFraction = " << s.toastTopFraction << "  # 0-1\n";

			auto combosToToml = [](const std::vector<ImGuiVRHelperPluginAPI::InputCombo>& v) {
				std::string r = "[";
				for (std::size_t i = 0; i < v.size(); ++i) {
					if (i)
						r += ", ";
					r += std::to_string(v[i].Packed());
				}
				return r + "]";
			};
			out << "\n# Controller combos for opening / closing the active overlay.\n"
				<< "# Packed values (device << 16 | key); rebind in-headset rather than by hand.\n"
				<< "openMenuKeys = " << combosToToml(s.openMenuKeys) << "\n"
				<< "closeMenuKeys = " << combosToToml(s.closeMenuKeys) << "\n";

			out << "\n# Overlay display order (client names; top = opened first).\n"
				<< "overlayOrder = [";
			for (std::size_t i = 0; i < s.overlayOrder.size(); ++i) {
				if (i)
					out << ", ";
				out << "\"" << s.overlayOrder[i] << "\"";
			}
			out << "]\n";
			return out.str();
		}

		// One-time migration helper: if the legacy JSON file exists from
		// an older helper version, parse it minimally and seed defaults
		// on first TOML load. We support exactly the field names the JSON
		// version wrote, since that's what users may have on disk.
		void MaybeMigrateLegacyJson(Settings& s)
		{
			const std::filesystem::path legacy = "Data/SKSE/Plugins/ImGuiVRHelper.json";
			std::error_code ec;
			if (!std::filesystem::exists(legacy, ec))
				return;

			try {
				std::ifstream in(legacy);
				std::string contents((std::istreambuf_iterator<char>(in)),
					std::istreambuf_iterator<char>());
				// Very small JSON-ish reader: find each "key": value pair.
				// We use the existing nlohmann_json dependency since it's
				// still in the build for ImportLegacySettings.
				auto j = nlohmann::json::parse(contents);
				s.menuScale = j.value("menuScale", s.menuScale);
				s.positioningMethod = static_cast<PositioningMethod>(
					j.value("positioningMethod", static_cast<int>(s.positioningMethod)));
				s.attachMode = static_cast<AttachMode>(
					j.value("attachMode", static_cast<int>(s.attachMode)));
				s.attachController = static_cast<ImGuiVRHelperPluginAPI::InputDeviceType>(
					j.value("attachController", static_cast<int>(s.attachController)));
				s.hmdOffsetX = j.value("hmdOffsetX", s.hmdOffsetX);
				s.hmdOffsetY = j.value("hmdOffsetY", s.hmdOffsetY);
				s.hmdOffsetZ = j.value("hmdOffsetZ", s.hmdOffsetZ);
				s.controllerOffsetX = j.value("controllerOffsetX", s.controllerOffsetX);
				s.controllerOffsetY = j.value("controllerOffsetY", s.controllerOffsetY);
				s.controllerOffsetZ = j.value("controllerOffsetZ", s.controllerOffsetZ);
				s.enableWandPointing = j.value("enableWandPointing", s.enableWandPointing);
				s.enableDragToReposition = j.value("enableDragToReposition", s.enableDragToReposition);
				s.onlyOpenWhilePaused = j.value("onlyOpenWhilePaused", s.onlyOpenWhilePaused);
				s.showWelcome = j.value("showWelcome", s.showWelcome);
				s.autoResetDistance = j.value("autoResetDistance", s.autoResetDistance);
				s.mouseDeadzone = j.value("mouseDeadzone", s.mouseDeadzone);
				s.logLevel = j.value("logLevel", s.logLevel);

				logs::info("Migrated legacy ImGuiVRHelper.json to in-memory settings; will save as .toml");
				// Remove the legacy file so we only hit this once. The
				// caller writes the new .toml on first save.
				std::filesystem::remove(legacy, ec);
			} catch (const std::exception& e) {
				logs::warn("Legacy JSON migration failed: {} — keeping defaults", e.what());
			}
		}
	}

	void SaveSettings()
	{
		try {
			std::error_code ec;
			std::filesystem::create_directories(kSettingsPath.parent_path(), ec);

			const auto body = ToTomlString(State::GetSingleton().settings);
			std::ofstream out(kSettingsPath);
			if (!out) {
				logs::warn("SaveSettings: couldn't open {} for writing", kSettingsPath.string());
				return;
			}
			out << body;
		} catch (const std::exception& e) {
			logs::warn("SaveSettings: {}", e.what());
		}
	}

	void LoadSettings()
	{
		// First-run path: if there's a legacy .json from a previous helper
		// version, fold it into Settings before we look for the .toml.
		MaybeMigrateLegacyJson(State::GetSingleton().settings);

		std::error_code ec;
		if (!std::filesystem::exists(kSettingsPath, ec)) {
			logs::info("LoadSettings: {} not present; using defaults", kSettingsPath.string());
			return;
		}
		try {
			auto parsed = toml::parse_file(kSettingsPath.string());
			FromToml(parsed, State::GetSingleton().settings);
			logs::info("LoadSettings: loaded {}", kSettingsPath.string());
		} catch (const toml::parse_error& e) {
			logs::warn("LoadSettings TOML parse error in {} at line {}: {}",
				kSettingsPath.string(),
				e.source().begin.line,
				e.description());
		} catch (const std::exception& e) {
			logs::warn("LoadSettings: {}", e.what());
		}
	}

	void WriteDefaultsIfMissing()
	{
		std::error_code ec;
		if (std::filesystem::exists(kSettingsPath, ec)) {
			return;  // user already has one
		}
		SaveSettings();
		logs::info("WriteDefaultsIfMissing: wrote default config to {}",
			kSettingsPath.string());
	}

	void ApplyLogLevel()
	{
		const auto& s = State::GetSingleton().settings;
		const auto lvl = spdlog::level::from_str(s.logLevel);
		if (auto logger = spdlog::default_logger()) {
			logger->set_level(lvl);
			logger->flush_on(lvl);
		}
		spdlog::set_level(lvl);
		logs::info("ApplyLogLevel: spdlog level set to '{}'", s.logLevel);
	}
}
