-- Local override of the xmake-repo tracy package, pinned to the exact upstream
-- commit Community Shaders uses (vcpkg 0.13.3-1706ac57), so the helper's Tracy
-- client speaks the same wire protocol as CS's profiler tooling. xmake-repo only
-- packages released tags (<= v0.12.2); this fetches the commit via git.
-- Body mirrors xmake-repo/packages/t/tracy/xmake.lua, changing only the URL/version.
package("tracy")
set_homepage("https://github.com/wolfpld/tracy")
set_description("C++ frame profiler")

add_urls("https://github.com/wolfpld/tracy.git")
add_versions("0.13.3", "1706ac57acc8d067134e2f59aa67421d48bfb31a")

add_configs("cmake", { description = "Use cmake buildsystem", default = true, type = "boolean" })

add_configs("tracy_enable", { type = "boolean", default = true, description = "Enable profiling" })
add_configs("on_demand", { type = "boolean", default = false, description = "On-demand profiling" })
add_configs(
    "enforce_callstack",
    { type = "boolean", default = false, description = "Enfore callstack collection for tracy regions" }
)
add_configs(
    "callstack",
    { type = "boolean", default = false, description = "Enable all callstack related functionality" }
)
add_configs(
    "callstack_inlines",
    { type = "boolean", default = false, description = "Enable the inline functions in callstacks" }
)
add_configs(
    "only_localhost",
    { type = "boolean", default = false, description = "Only listen on the localhost interface" }
)
add_configs(
    "broadcast",
    { type = "boolean", default = false, description = "Enable client discovery by broadcast to local network" }
)
add_configs("only_ipv4", {
    type = "boolean",
    default = false,
    description = "Tracy will only accept connections on IPv4 addresses (disable IPv6)",
})
add_configs("code_transfer", { type = "boolean", default = false, description = "Enable collection of source code" })
add_configs("context_switch", { type = "boolean", default = false, description = "Enable capture of context switches" })
add_configs("exit", {
    type = "boolean",
    default = false,
    description = "Client executable does not exit until all profile data is sent to server",
})
add_configs("sampling", { type = "boolean", default = false, description = "Enable call stack sampling" })
add_configs("verify", { type = "boolean", default = false, description = "Enable zone validation for C API" })
add_configs(
    "vsync_capture",
    { type = "boolean", default = false, description = "Enable capture of hardware Vsync events" }
)
add_configs(
    "frame_image",
    { type = "boolean", default = false, description = "Enable the frame image support and its thread" }
)
add_configs("system_tracing", { type = "boolean", default = false, description = "Enable systrace sampling" })
add_configs("patchable_nopsleds", {
    type = "boolean",
    default = false,
    description = "Enable nopsleds for efficient patching by system-level tools (e.g. rr)",
})
add_configs("timer_fallback", { type = "boolean", default = false, description = "Use lower resolution timers" })
add_configs(
    "libunwind_backtrace",
    { type = "boolean", default = false, description = "Use libunwind backtracing where supported" }
)
add_configs("symbol_offline_resolve", {
    type = "boolean",
    default = false,
    description = "Instead of full runtime symbol resolution, only resolve the image path and offset to enable offline symbol resolution",
})
add_configs("libbacktrace_elf_dynload_support", {
    type = "boolean",
    default = false,
    description = "Enable libbacktrace to support dynamically loaded elfs in symbol resolution resolution after the first symbol resolve operation",
})
add_configs("delayed_init", {
    type = "boolean",
    default = false,
    description = "Enable delayed initialization of the library (init on first call)",
})
add_configs(
    "manual_lifetime",
    { type = "boolean", default = false, description = "Enable the manual lifetime management of the profile" }
)
add_configs("fibers", { type = "boolean", default = true, description = "Enable fibers support" })
add_configs("crash_handler", { type = "boolean", default = false, description = "Enable crash handling" })
add_configs("verb", { type = "boolean", default = false, description = "Enable verbose logging" })

-- This commit's headers use relative "../common" / "../client" includes, so
-- tracy/, common/, client/ must be siblings under the include root.
add_includedirs("include")

if is_plat("windows", "mingw") then
    add_syslinks("ws2_32", "dbghelp")
elseif is_plat("linux") then
    add_syslinks("pthread")
elseif is_plat("bsd") then
    add_syslinks("pthread", "execinfo")
end

on_load(function(package)
    if package:config("cmake") then
        package:add("deps", "cmake")
    end
end)

on_install(function(package)
    io.replace(
        "public/client/TracyProfiler.cpp",
        [[#ifdef TRACY_ENABLE]],
        [[#ifdef TRACY_ENABLE
#ifdef __MINGW32__
#define __try try
#define __except(filter) catch(...)
#endif]],
        { plain = true }
    )
    io.replace(
        "public/client/TracyProfiler.cpp",
        [[RelationProcessorDie]],
        [[static_cast<LOGICAL_PROCESSOR_RELATIONSHIP>(5)]],
        { plain = true }
    )
    local configs = {}
    table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
    table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))

    -- This commit defaults TRACY_ENABLE OFF, which builds an empty client lib
    -- (unresolved tracy symbols at link). Pass it explicitly.
    table.insert(configs, "-DTRACY_ENABLE=" .. (package:config("tracy_enable") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_ON_DEMAND=" .. (package:config("on_demand") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_CALLSTACK=" .. (package:config("enforce_callstack") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_NO_CALLSTACK=" .. (package:config("callstack") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_CALLSTACK_INLINES=" .. (package:config("callstack_inlines") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_ONLY_LOCALHOST=" .. (package:config("only_localhost") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_NO_BROADCAST=" .. (package:config("broadcast") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_ONLY_IPV4=" .. (package:config("only_ipv4") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_NO_CODE_TRANSFER=" .. (package:config("code_transfer") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_CONTEXT_SWITCH=" .. (package:config("context_switch") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_EXIT=" .. (package:config("exit") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_SAMPLING=" .. (package:config("sampling") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_VERIFY=" .. (package:config("verify") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_VSYNC_CAPTURE=" .. (package:config("vsync_capture") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_FRAME_IMAGE=" .. (package:config("frame_image") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_NO_SYSTEM_TRACING=" .. (package:config("system_tracing") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_PATCHABLE_NOPSLEDS=" .. (package:config("patchable_nopsleds") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_DELAYED_INIT=" .. (package:config("delayed_init") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_MANUAL_LIFETIME=" .. (package:config("manual_lifetime") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_FIBERS=" .. (package:config("fibers") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_NO_CRASH_HANDLER=" .. (package:config("crash_handler") and "OFF" or "ON"))
    table.insert(configs, "-DTRACY_TIMER_FALLBACK=" .. (package:config("timer_fallback") and "ON" or "OFF"))
    table.insert(configs, "-DTRACY_LIBUNWIND_BACKTRACE=" .. (package:config("libunwind_backtrace") and "ON" or "OFF"))
    table.insert(
        configs,
        "-DTRACY_SYMBOL_OFFLINE_RESOLVE=" .. (package:config("symbol_offline_resolve") and "ON" or "OFF")
    )
    table.insert(
        configs,
        "-DTRACY_LIBBACKTRACE_ELF_DYNLOAD_SUPPORT="
            .. (package:config("libbacktrace_elf_dynload_support") and "ON" or "OFF")
    )

    -- collect tracy defines from cmake configs
    for _, config in ipairs(configs) do
        local define, value = config:match("-D(TRACY_%S+)=(.*)")
        if define and value and value == "ON" then
            package:add("defines", define)
        end
    end

    import("package.tools.cmake").install(package, configs)

    -- The CMake install at this commit is incomplete and uses a layout that
    -- breaks the headers' relative "../common" / "../client" includes. Lay
    -- the public tree out verbatim (tracy/, common/, client/ as siblings)
    -- so <tracy/Tracy.hpp> and its relative includes all resolve.
    os.cp("public/tracy/*", package:installdir("include/tracy/"))
    os.cp("public/common/*", package:installdir("include/common/"))
    os.cp("public/client/*", package:installdir("include/client/"))
end)

on_test(function(package)
    if package:config("tracy_enable") then
        assert(package:check_cxxsnippets({
            test = [[
                #include <tracy/Tracy.hpp>
                void test() {
                    TracyPlotConfig("PlotConfig", tracy::PlotFormatType::Number, true, true, 0);
                }
            ]],
        }, { configs = { languages = "c++14" } }))
    end
end)
