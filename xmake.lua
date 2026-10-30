local name = "DeathStrandingDCFix"

set_project(name)
add_rules("mode.debug", "mode.release")
set_languages("cxxlatest", "clatest")
set_optimize("smallest")

target(name)
    set_kind("shared")
    -- Built as winhttp.dll so the game loads it directly (see CMakeLists.txt)
    set_basename("winhttp")
    set_prefixname("")
    set_extension(".dll")
    add_files("src/*.cpp", "src/proxy/winhttp.cpp", "src/proxy/winhttp.def", "external/safetyhook/safetyhook.cpp", "external/safetyhook/Zydis.c")
    add_files("external/imgui/imgui.cpp", "external/imgui/imgui_draw.cpp", "external/imgui/imgui_tables.cpp", "external/imgui/imgui_widgets.cpp",
              "external/imgui/backends/imgui_impl_win32.cpp", "external/imgui/backends/imgui_impl_dx12.cpp")
    -- No dxgi/d3d12 imports, see cmake/Dependencies.cmake
    add_defines("CreateDXGIFactory1=Overlay_CreateDXGIFactory1")
    add_syslinks("user32", "d3dcompiler", "dwmapi", "gdi32")
    add_includedirs("external/spdlog/include", "external/inipp", "external/safetyhook", "external/imgui", "external/imgui/backends")

    if is_plat("windows") then
        set_toolchains("msvc")
        set_runtimes("MT")
        add_cxflags("/utf-8", "/GL")
        add_ldflags("/LTCG", "/OPT:REF", "/OPT:ICF")
    end