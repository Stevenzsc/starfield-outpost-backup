-- OutpostBackup SFSE plugin, built on CommonLibSF (libxse)
includes("lib/commonlibsf")

set_project("OutpostBackup")
set_version("0.3.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

target("OutpostBackup")
    add_rules("commonlibsf.plugin", {
        name = "OutpostBackup",
        author = "Stevenzsc",
        description = "Outpost Backup: back up, land on and restore outposts"
    })
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
