-- OSBLanding SFSE plugin, built on CommonLibSF (libxse)
includes("lib/commonlibsf")

set_project("OSBLanding")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

target("OSBLanding")
    add_rules("commonlibsf.plugin", {
        name = "OSBLanding",
        author = "Stevenzsc",
        description = "Outpost Backup: land exactly on a stored latitude/longitude"
    })
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
