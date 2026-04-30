-- ImGuiVRHelper xmake build script
-- SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception

set_xmakever("2.8.2")

set_config("rex_ini", true)

includes("lib/commonlibsse-ng")

set_project("ImGuiVRHelper")
set_license("GPL-3.0")

local version = "0.1.0"
local ver = version:split("%.")
set_version(version)

set_languages("c++23")
set_warnings("allextra")
set_policy("package.requires_lock", true)
add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Runtime dependencies. Pinned versions resolved into xmake-requires.lock.
add_requires("openvr")
add_requires("imgui", { configs = { dx11 = true, win32 = true } })
add_requires("nlohmann_json")
add_requires("directxtk") -- SimpleMath, used for matrix helpers in VRUtils

target("ImGuiVRHelper")
add_deps("commonlibsse-ng")
add_packages("openvr", "imgui", "nlohmann_json", "directxtk")

set_basename("imgui-vr-helper")

add_shflags("/DEBUG", { force = true })

set_configvar("VERSION_MAJOR", tonumber(ver[1]))
set_configvar("VERSION_MINOR", tonumber(ver[2]))
set_configvar("VERSION_PATCH", tonumber(ver[3]))
set_configvar("VERSION_STRING", version)

-- Numeric version defines available at compile time. The C side
-- stringifies these into IMGUI_VR_HELPER_VERSION_STRING to avoid the
-- shell-quoting headache of passing a quoted string through /D.
add_defines(
    "IMGUI_VR_HELPER_VERSION_MAJOR=" .. ver[1],
    "IMGUI_VR_HELPER_VERSION_MINOR=" .. ver[2],
    "IMGUI_VR_HELPER_VERSION_PATCH=" .. ver[3],
    "IMGUI_VR_HELPER_BUILD_NUMBER=((" .. ver[1] .. "<<16)|(" .. ver[2] .. "<<8)|" .. ver[3] .. ")"
)

add_rules("commonlibsse-ng.plugin", {
    name = "ImGuiVRHelper",
    author = "ImGuiVRHelper contributors",
    description = "OpenVR overlay/input glue for ImGui-based SKSE mods",
})

add_files("src/**.cpp")
add_headerfiles("src/**.h")

-- Build the client-side handshake stub into the helper too, so the
-- helper itself can resolve the same interface for self-test paths.
add_files("api/ImGuiVRHelperAPI.cpp")
add_headerfiles("api/**.h")

add_includedirs("src", "api")
set_pcxxheader("src/pch.h")

-- Auto-deploy on build: set SkyrimPluginTargets to one or more paths
-- separated by ';' to copy the DLL + PDB into each Skyrim Data dir.
after_build(function(target)
    local deploy_dirs = os.getenv("SkyrimPluginTargets")
    if not deploy_dirs then
        return
    end
    local dll = target:targetfile()
    local pdb = target:symbolfile()
    for _, dir in ipairs(deploy_dirs:split(";")) do
        dir = dir:trim()
        if dir ~= "" then
            local dest = path.join(dir, "SKSE", "Plugins")
            os.mkdir(dest)
            os.cp(dll, dest)
            if os.isfile(pdb) then
                os.cp(pdb, dest)
            end
            print("Deployed to " .. dest)
        end
    end
end)
