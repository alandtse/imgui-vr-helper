-- SPDX-License-Identifier: GPL-3.0-or-later WITH LicenseRef-Modding-Exception
-- devbench cross-plugin API (MIT-licensed header + handshake stub, compiled by
-- the consumer — see src/DevBenchApiStub.cpp). Lets the helper expose its input
-- diagnostics and synthetic-input injection as devbench MCP/REST tools when
-- devbench is installed; runtime no-op otherwise.
package("devbench-api")
set_kind("library", { headeronly = true })
set_homepage("https://github.com/alandtse/devbench")
set_description("Cross-plugin API for the devbench SKSE dev harness (MCP/REST tool registration)")
set_license("MIT")

add_urls("https://github.com/alandtse/devbench.git", { submodules = false })
add_versions("1.5.0", "508ebed9ba845beab481887cf5ec948b68ffbf9c")

on_install(function(package)
    os.cp("include/DevBenchAPI.h", package:installdir("include"))
    os.cp("include/DevBenchAPI.cpp", package:installdir("include"))
    os.cp("include/DevBenchAPI.LICENSE.txt", package:installdir())
end)
