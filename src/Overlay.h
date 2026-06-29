// SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
// Copyright (c) 2025 ImGuiVRHelper contributors. See COPYING and EXCEPTIONS.md.
//
// Overlay subsystem state: positioning settings, attach modes, wand-laser
// intersection state, and the fixed-world transform fallback.
//
// This is the helper-side replacement for the overlay-related portions of
// Community Shaders' VR class (settings.VRMenu*, wandState,
// fixedWorldOverlayPosition, Config::*). Overlay submission, drag, and the
// in-scene fallback all consume State::GetSingleton().

#pragma once

#include "internal/HUDGeometry.h"

#include <SimpleMath.h>
#include <imgui.h>
#include <openvr.h>

#include "ImGuiVRHelperInput.h"  // InputDeviceType

namespace ImGuiVRHelper::Overlay
{
	using DirectX::SimpleMath::Matrix;
	using DirectX::SimpleMath::Vector3;
	using ImGuiVRHelperPluginAPI::InputDeviceType;

	// ---- Constants ------------------------------------------------------

	namespace Config
	{
		inline constexpr int kOverlayWidth = 1920;
		inline constexpr int kOverlayHeight = 1080;
		inline constexpr float kOverlayAspect = static_cast<float>(kOverlayHeight) / static_cast<float>(kOverlayWidth);

		// Default supersample for view-filling panels (see Settings::hudSupersample).
		// A HUD layer fills most of the FOV (see hudCoverage), so a 1x texture
		// stretched that wide reads as pixelated; 2x keeps text sharp. Clients learn
		// the real panel size from GetPanel and scale their framebuffer to match
		// (see StatusHUD's DisplayFramebufferScale).
		inline constexpr int kHUDSupersample = 2;
		inline constexpr int kMaxHUDSupersample = 3;  // VRAM ceiling; clamps the setting

		inline constexpr float kDefaultMenuScale = 1.0f;
		inline constexpr float kMinMenuScale = 0.1f;
		inline constexpr float kMaxMenuScale = 5.0f;

		// Fraction of each eye's view the HUD quad covers (see hudCoverage). Min/max
		// are shared by the settings slider and the render-time clamp so the two
		// can't disagree.
		inline constexpr float kDefaultHUDCoverage = 0.5f;
		inline constexpr float kMinHUDCoverage = 0.2f;
		inline constexpr float kMaxHUDCoverage = 1.0f;

		// HMD-relative defaults (meters).
		inline constexpr float kDefaultHMDOffsetX = 0.195f;
		inline constexpr float kDefaultHMDOffsetY = -0.375f;
		inline constexpr float kDefaultHMDOffsetZ = -1.355f;

		// Controller-relative defaults (meters).
		inline constexpr float kDefaultControllerOffsetX = 0.295f;
		inline constexpr float kDefaultControllerOffsetY = 0.211f;
		inline constexpr float kDefaultControllerOffsetZ = 0.063f;

		/// Scale matrix matching the overlay's aspect ratio. Y is scaled
		/// proportionally so the overlay stays visually square.
		inline Matrix CreateScaleMatrix(float scale)
		{
			return Matrix::CreateScale(scale, scale * kOverlayAspect, scale);
		}
	}

	// ---- Enums ----------------------------------------------------------

	enum class AttachMode
	{
		HMDOnly = 0,
		ControllerOnly = 1,
		Both = 2,
		None = 3,
	};

	enum class PositioningMethod
	{
		HMDRelative = 0,
		FixedWorld = 1,
	};

	enum class OverlayType
	{
		HMD,
		Controller,
	};

	// ---- Settings -------------------------------------------------------

	struct Settings
	{
		float menuScale = Config::kDefaultMenuScale;
		PositioningMethod positioningMethod = PositioningMethod::FixedWorld;
		AttachMode attachMode = AttachMode::HMDOnly;
		InputDeviceType attachController = InputDeviceType::Secondary;

		// HMD-relative offsets.
		float hmdOffsetX = Config::kDefaultHMDOffsetX;
		float hmdOffsetY = Config::kDefaultHMDOffsetY;
		float hmdOffsetZ = Config::kDefaultHMDOffsetZ;

		// Controller-relative offsets.
		float controllerOffsetX = Config::kDefaultControllerOffsetX;
		float controllerOffsetY = Config::kDefaultControllerOffsetY;
		float controllerOffsetZ = Config::kDefaultControllerOffsetZ;

		bool enableWandPointing = true;

		// Drag-to-reposition. On by default so the documented grip-to-move gesture
		// works out of the box; gated to off-panel grip so it can't hijack on-panel
		// menu input.
		bool enableDragToReposition = true;
		float autoResetDistance = 1000.0f;  ///< game units; 0 disables auto-reset

		/// When true, an overlay may only be OPENED while the game is paused
		/// (RE::UI::GameIsPaused()), so menus never affect live gameplay —
		/// matching Community Shaders' original pause-only menu. Switching
		/// between already-open overlays and closing are always allowed.
		bool onlyOpenWhilePaused = true;

		/// Show a one-time welcome banner on the HUD at startup (how to open
		/// overlays). Auto-dismisses after ~30s or when you enter the game.
		bool showWelcome = true;

		/// Controller combos the helper owns for opening / closing the active
		/// overlay. Open focuses the last-opened overlay (or the first mod
		/// overlay on a cold start); close releases focus. Defaults match
		/// Community Shaders' original VR menu binds so muscle memory carries
		/// over. Rebindable from the controller map; persisted as packed values.
		/// OpenVR key codes: X/A = 7, B/Y = 1, grip = 2.
		std::vector<ImGuiVRHelperPluginAPI::InputCombo> openMenuKeys = {
			ImGuiVRHelperPluginAPI::InputCombo::Secondary(7),
			ImGuiVRHelperPluginAPI::InputCombo::Secondary(1),
		};
		std::vector<ImGuiVRHelperPluginAPI::InputCombo> closeMenuKeys = {
			ImGuiVRHelperPluginAPI::InputCombo::Both(2),
		};

		/// Preferred order of mod overlays (by client name) for the overlay cycle
		/// and the open combo's "first mod" pick (top = opened first). Clients not
		/// listed fall to the end in registration order. Empty = registration
		/// order. Set from the helper's "Overlay order" UI.
		std::vector<std::string> overlayOrder;

		// Thumbstick deadzone (also used for drag-depth control). Bumped
		// from 0.1 to 0.2 — most VR controllers have hardware drift in
		// the 0.05-0.15 range, and 0.1 left the cursor noticeably
		// drifting after stick release. SteamVR's own input deadzone
		// defaults are also in the 0.2 ballpark.
		float mouseDeadzone = 0.2f;

		/// RGBA color blended into the overlay while it's being dragged.
		/// Default: yellow at 30% alpha, matching SCS's visual feedback.
		float dragHighlightColor[4] = { 1.0f, 1.0f, 0.0f, 0.3f };

		/// Spdlog level name: "trace", "debug", "info", "warn", "err",
		/// "critical", "off". Applied at plugin load. Default "info".
		std::string logLevel = "info";

		/// Smoke test for kClientFlag_HUDMode. When on, the helper
		/// registers a synthetic HUD-mode client and renders a
		/// calibration grid into its panel RTV every frame. Toggle
		/// off to clear. Lives in the Diagnostics section of the
		/// settings UI; default off.
		bool showHUDDemo = false;

		/// Distance (meters) from the HMD to the HUD-mode plane. All
		/// kClientFlag_HUDMode clients render onto a flat quad at this
		/// depth in front of the user. Closer values feel like a
		/// "thin glass layer" / monitor stuck to the headset; farther
		/// values feel like a billboard floating in space. 0.5 is
		/// roughly the minimum comfortable focal distance on most VR
		/// lenses; pushing closer than that strains the eyes.
		float hudDepth = 1.0f;

		/// Fraction of each eye's view frustum the HUD plane fills, at
		/// hudDepth. The helper derives the quad's extents (and off-center
		/// position, since HMD frustums are asymmetric) directly from the
		/// per-eye projection tangents, so the HUD covers the actual view
		/// like a screen rather than a small fixed-FOV window. 1.0 = edge
		/// to edge (may clip at the lens mask on some HMDs); smaller values
		/// shrink it toward a comfortable window. Clamped to
		/// [kMinHUDCoverage, kMaxHUDCoverage].
		float hudCoverage = Config::kDefaultHUDCoverage;

		/// Shape of the HUD panel. Flat = original 2-triangle quad (default).
		/// Cylinder = tessellated arc that wraps horizontally around the
		/// player, matching the natural curvature of VR lenses so the panel
		/// spans a wider FOV without corner clipping or perspective distortion.
		InSceneOverlay::HUDShape hudShape = InSceneOverlay::HUDShape::Flat;

		/// Number of horizontal segments for the cylinder mesh (HUDShape::Cylinder
		/// only). Higher = smoother curve, more triangles. 16 is imperceptible
		/// from 32 at typical VR resolutions; default 24 is a safe middle ground.
		int hudCylinderSegments = 24;

		/// Base resolution of every overlay panel texture, before HUD
		/// supersampling. Higher = sharper but more VRAM; pick to suit the
		/// headset/monitor. Common presets are offered in the settings UI
		/// (1920x1080 / 2560x1440 / 3840x2160). Applied when panels allocate,
		/// so a change takes effect on restart. Defaults to kOverlayWidth/Height.
		int baseWidth = Config::kOverlayWidth;
		int baseHeight = Config::kOverlayHeight;

		/// Supersample multiplier for panels that fill the view: HUD layers, the
		/// settings panel and the toasts/welcome. Their texture is
		/// baseWidth*hudSupersample wide so wide-FOV content stays crisp instead
		/// of pixelated. 1 = off. Clamped to [1, 3]. Applied at allocation
		/// (restart). Defaults to kHUDSupersample.
		int hudSupersample = Config::kHUDSupersample;

		/// Vertical placement of the toasts/welcome banner, as a fraction of the
		/// panel height (0 = top edge, 1 = bottom). The banner is pivoted by its
		/// top-center. Clamped to [0, 1]. Default 0.18 (near the top).
		float toastTopFraction = 0.18f;
	};

	// ---- Runtime state --------------------------------------------------

	struct WandState
	{
		bool isIntersecting = false;
		ImVec2 uvCoordinates = ImVec2(0.0f, 0.0f);
		vr::TrackedDeviceIndex_t controllerIndex = vr::k_unTrackedDeviceIndexInvalid;
		Vector3 rayOrigin = Vector3::Zero;
		Vector3 rayDirection = Vector3::Zero;
	};

	struct FixedWorldPosition
	{
		Matrix m = Matrix::Identity;
		bool initialized = false;
	};

	struct DragState
	{
		bool dragging = false;
		vr::TrackedDeviceIndex_t controllerIndex = vr::k_unTrackedDeviceIndexInvalid;
		bool isPrimary = false;
		bool isSecondary = false;
		Matrix initialControllerMatrix = Matrix::Identity;
		Matrix initialOverlayMatrix = Matrix::Identity;
		Matrix startControllerMatrix = Matrix::Identity;

		enum class Mode
		{
			None,
			FixedWorld,
			HMD,
			Controller,
		} mode = Mode::None;

		Vector3 initialHMDOffset = Vector3::Zero;
		Vector3 initialControllerOffset = Vector3::Zero;
		float initialHMDScale = 1.0f;
	};

	// ---- Singleton ------------------------------------------------------

	/// Persist Settings to Data/SKSE/Plugins/ImGuiVRHelper.toml. Safe to
	/// call any time after Globals::IsReady().
	void SaveSettings();

	/// Load Settings from Data/SKSE/Plugins/ImGuiVRHelper.toml if it
	/// exists. Missing fields fall back to defaults; corrupt files log a
	/// warning and leave defaults intact. If a legacy .json from a prior
	/// helper version is present, its values are migrated and the .json
	/// is removed on the way through.
	void LoadSettings();

	/// If the TOML config file doesn't exist on disk, write the current
	/// (default) Settings to it so the user has a discoverable file to
	/// edit. Idempotent — silent if the file is already present.
	void WriteDefaultsIfMissing();

	/// Apply Settings::logLevel to spdlog's default logger. Safe to call
	/// after SKSE::Init has set up logging.
	void ApplyLogLevel();

	class State
	{
	public:
		static State& GetSingleton();

		Settings settings;
		WandState wandState;
		FixedWorldPosition fixedWorld;
		DragState dragState;

		bool lastKnownLeftHandedMode = false;
		bool overlayVisible = false;  ///< populated by HelperImpl::IsOverlayVisible

		/// World position the player was at when fixed-world overlay last
		/// snapped. Used by auto-reset distance check.
		RE::NiPoint3 savedPlayerWorldPos{};

		/// Per-controller polled state, populated by Input.cpp once that
		/// port lands. Until then, all isPressed/thumbsticks read 0.
		RE::VRControllerState primaryControllerState{};
		RE::VRControllerState secondaryControllerState{};

	private:
		State() = default;
	};
}
