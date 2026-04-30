// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Overlay.h"

#include <toml++/toml.hpp>

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

		void FromToml(const toml::table& t, Settings& s)
		{
			// Top-level scalars (the file is intentionally flat for v1 —
			// section grouping is a polish item if/when settings grow).
			s.menuScale = tomlGet<float>(t, "menuScale", s.menuScale);
			s.positioningMethod = static_cast<PositioningMethod>(
				tomlGet<int64_t>(t, "positioningMethod",
					static_cast<int64_t>(s.positioningMethod)));
			s.attachMode = static_cast<AttachMode>(
				tomlGet<int64_t>(t, "attachMode",
					static_cast<int64_t>(s.attachMode)));
			s.attachController = static_cast<ImGuiVRHelperPluginAPI::InputDeviceType>(
				tomlGet<int64_t>(t, "attachController",
					static_cast<int64_t>(s.attachController)));
			s.hmdOffsetX = tomlGet<double>(t, "hmdOffsetX", s.hmdOffsetX);
			s.hmdOffsetY = tomlGet<double>(t, "hmdOffsetY", s.hmdOffsetY);
			s.hmdOffsetZ = tomlGet<double>(t, "hmdOffsetZ", s.hmdOffsetZ);
			s.controllerOffsetX = tomlGet<double>(t, "controllerOffsetX", s.controllerOffsetX);
			s.controllerOffsetY = tomlGet<double>(t, "controllerOffsetY", s.controllerOffsetY);
			s.controllerOffsetZ = tomlGet<double>(t, "controllerOffsetZ", s.controllerOffsetZ);
			s.enableWandPointing = tomlGet<bool>(t, "enableWandPointing", s.enableWandPointing);
			s.enableDragToReposition = tomlGet<bool>(t, "enableDragToReposition", s.enableDragToReposition);
			s.autoResetDistance = tomlGet<double>(t, "autoResetDistance", s.autoResetDistance);
			s.mouseDeadzone = tomlGet<double>(t, "mouseDeadzone", s.mouseDeadzone);
			s.logLevel = tomlGet<std::string>(t, "logLevel", s.logLevel);
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
				<< "enableDragToReposition = " << (s.enableDragToReposition ? "true" : "false") << "\n"
				<< "mouseDeadzone = " << s.mouseDeadzone << "\n"
				<< "autoResetDistance = " << s.autoResetDistance << "  # game units\n";
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
