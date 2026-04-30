// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.

#include "pch.h"

#include "Overlay.h"

#include <nlohmann/json.hpp>

#include <filesystem>

namespace ImGuiVRHelper::Overlay
{
	State& State::GetSingleton()
	{
		static State instance;
		return instance;
	}

	namespace
	{
		const std::filesystem::path kSettingsPath = "Data/SKSE/Plugins/ImGuiVRHelper.json";

		// Each field uses j.value(key, fallback) so missing entries silently
		// keep the defaults already in the Settings struct.
		void FromJson(const nlohmann::json& j, Settings& s)
		{
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
		}

		nlohmann::json ToJson(const Settings& s)
		{
			nlohmann::json j;
			j["menuScale"] = s.menuScale;
			j["positioningMethod"] = static_cast<int>(s.positioningMethod);
			j["attachMode"] = static_cast<int>(s.attachMode);
			j["attachController"] = static_cast<int>(s.attachController);
			j["hmdOffsetX"] = s.hmdOffsetX;
			j["hmdOffsetY"] = s.hmdOffsetY;
			j["hmdOffsetZ"] = s.hmdOffsetZ;
			j["controllerOffsetX"] = s.controllerOffsetX;
			j["controllerOffsetY"] = s.controllerOffsetY;
			j["controllerOffsetZ"] = s.controllerOffsetZ;
			j["enableWandPointing"] = s.enableWandPointing;
			j["enableDragToReposition"] = s.enableDragToReposition;
			j["autoResetDistance"] = s.autoResetDistance;
			j["mouseDeadzone"] = s.mouseDeadzone;
			return j;
		}
	}

	void SaveSettings()
	{
		try {
			std::error_code ec;
			std::filesystem::create_directories(kSettingsPath.parent_path(), ec);

			const auto j = ToJson(State::GetSingleton().settings);
			std::ofstream out(kSettingsPath);
			if (!out) {
				logs::warn("SaveSettings: couldn't open {} for writing", kSettingsPath.string());
				return;
			}
			out << j.dump(2);
		} catch (const std::exception& e) {
			logs::warn("SaveSettings: {}", e.what());
		}
	}

	void LoadSettings()
	{
		std::error_code ec;
		if (!std::filesystem::exists(kSettingsPath, ec)) {
			logs::info("LoadSettings: {} not present; using defaults", kSettingsPath.string());
			return;
		}
		try {
			std::ifstream in(kSettingsPath);
			if (!in) {
				logs::warn("LoadSettings: couldn't open {} for reading", kSettingsPath.string());
				return;
			}
			nlohmann::json j;
			in >> j;
			FromJson(j, State::GetSingleton().settings);
			logs::info("LoadSettings: loaded {}", kSettingsPath.string());
		} catch (const std::exception& e) {
			logs::warn("LoadSettings parse error: {}", e.what());
		}
	}
}
