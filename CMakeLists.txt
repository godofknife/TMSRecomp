cmake_minimum_required(VERSION 3.25)
project(TMSRecomp C CXX)

# Generated code from tools/recomp/lift.py (private, never committed) and the player's own data folder.
set(NX_GEN "${CMAKE_SOURCE_DIR}/private/tms/gen" CACHE PATH "lift.py output")
set(NX_SDL3 "${CMAKE_SOURCE_DIR}/private/third_party/SDL3-3.4.16" CACHE PATH "SDL3 VC development package")
set(NX_VULKAN_INCLUDE "${CMAKE_SOURCE_DIR}/private/third_party/Vulkan-Headers-1.4.328/include" CACHE PATH "Vulkan-Headers include dir")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

# no lifted code (CI, a fresh checkout): runtime only (tms + launcher), no game.dll / game.so target
set(NX_HAVE_GEN OFF)
if(EXISTS "${NX_GEN}/files.txt")
  set(NX_HAVE_GEN ON)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NX_GEN}/files.txt")
  file(STRINGS "${NX_GEN}/files.txt" GEN_FILES)
  list(TRANSFORM GEN_FILES PREPEND "${NX_GEN}/")
else()
  message(STATUS "${NX_GEN}/files.txt not found: building the runtime only (tms + launcher)")
endif()

if(WIN32)
# libvpx (VP9 movies): source tree + a build dir configured by its ./configure, listed by tools/vpx_sources.py
# AMD FidelityFX / FSR SDK 2.4.0 (MIT parts only: api + upscaler headers, the two Vulkan DLLs; NOTES s62)
set(NX_FFX "${CMAKE_SOURCE_DIR}/private/third_party/FidelityFX-SDK-2.4.0/Kits/FidelityFX" CACHE PATH "FidelityFX SDK Kits/FidelityFX dir")
set(NX_LIBVPX "${CMAKE_SOURCE_DIR}/private/third_party/libvpx" CACHE PATH "libvpx source")
set(NX_VPX_GEN "${CMAKE_SOURCE_DIR}/private/third_party/vpx-gen" CACHE PATH "configured libvpx build dir")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NX_VPX_GEN}/sources.txt")
file(STRINGS "${NX_VPX_GEN}/sources.txt" VPX_FILES)
add_library(vpx STATIC ${VPX_FILES})
target_include_directories(vpx PUBLIC "${NX_LIBVPX}" "${NX_VPX_GEN}")
target_compile_options(vpx PRIVATE -O2 -w)

file(GLOB RT_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp)
add_executable(tms ${RT_SOURCES})
# tms.exe exports the runtime entry points (guest.h NX_API) that game.dll imports through tms.lib
set_target_properties(tms PROPERTIES ENABLE_EXPORTS ON)
# optional exe/window icon: tools/make_icon.ps1 writes private/tms/icon.ico from your own copy (never committed)
#   it is the game's own art: release builds (tools/package/package.ps1) configure with -DNX_ICON= and ship no icon
set(NX_ICON "${CMAKE_SOURCE_DIR}/private/tms/icon.ico" CACHE FILEPATH "exe icon (empty = none)")
#   without it: runtime/launcher/app.ico, our own (tools/make_app_icon.py), so the release exes are not the blank default
set(NX_EXE_ICON "${CMAKE_SOURCE_DIR}/runtime/launcher/app.ico")
if(NX_ICON AND EXISTS "${NX_ICON}")
  set(NX_EXE_ICON "${NX_ICON}")
endif()
enable_language(RC)
file(WRITE "${CMAKE_BINARY_DIR}/icon.rc" "1 ICON \"${NX_EXE_ICON}\"
")
# runtime/app.manifest (UTF-8 code page: non-ASCII install / data folders), merged into both exes' manifests
target_sources(tms PRIVATE "${CMAKE_BINARY_DIR}/icon.rc" runtime/app.manifest)
# tmslauncher.exe next to tms.exe (launcher.exe up to 0.1.5): settings, mods, updates + PLAY (Direct2D / DirectWrite)
add_executable(launcher WIN32 runtime/launcher/launcher.cpp "${CMAKE_BINARY_DIR}/icon.rc" runtime/app.manifest)
set_target_properties(launcher PROPERTIES OUTPUT_NAME tmslauncher)
target_compile_definitions(launcher PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE)
target_link_libraries(launcher PRIVATE user32 gdi32 shell32 ole32 d2d1 dwrite windowscodecs winhttp dwmapi)
target_include_directories(tms PRIVATE runtime/include runtime/src "${NX_SDL3}/include" "${NX_VULKAN_INCLUDE}" "${NX_FFX}/api/include" "${NX_FFX}/upscalers/include")
target_compile_options(tms PRIVATE -ffp-contract=off -fno-strict-aliasing -Wno-unused-parameter)
target_compile_definitions(tms PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(tms PRIVATE "${NX_SDL3}/lib/x64/SDL3.lib" winmm vpx dbghelp advapi32 shell32 ole32 mfplat mfuuid d3d11 dxgi user32 gdi32 d2d1 dwrite windowscodecs)
target_link_options(tms PRIVATE -Wl,/STACK:67108864)
add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NX_SDL3}/lib/x64/SDL3.dll" $<TARGET_FILE_DIR:tms>)
# FSR 3 (vk_fsr3.cpp loads them at run time)
foreach(dll amd_fidelityfx_loader_vk.dll amd_fidelityfx_upscaler_vk.dll)
  add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NX_FFX}/signedbin/${dll}" $<TARGET_FILE_DIR:tms>)
endforeach()
# battle voice subtitles (our own translations, tools/subs)
add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/subtitles_en.tsv" $<TARGET_FILE_DIR:tms>)
add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/resist_icons" $<TARGET_FILE_DIR:tms>/resist_icons)

# game.dll: the recompiled code, built by the same script the player's setup runs (tools/setup/build_game.py) with
# clang + lld-link only. The CRT import libs come from our own .def files (no Windows SDK libraries are shipped).
if(NX_HAVE_GEN)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(NX_LLVM_LIB llvm-lib REQUIRED)
find_program(NX_LLD_LINK lld-link REQUIRED)
set(NX_SETUP "${CMAKE_SOURCE_DIR}/tools/setup")
foreach(lib nx_crt_math nx_vcruntime)
  add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/${lib}.lib"
    COMMAND "${NX_LLVM_LIB}" "/def:${NX_SETUP}/${lib}.def" /machine:x64 "/out:${CMAKE_BINARY_DIR}/${lib}.lib"
    DEPENDS "${NX_SETUP}/${lib}.def")
endforeach()
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/game.dll"
  COMMAND "${Python3_EXECUTABLE}" "${NX_SETUP}/build_game.py" --gen "${NX_GEN}" --obj "${CMAKE_BINARY_DIR}/game_obj"
    --out "${CMAKE_BINARY_DIR}/game.dll" --include "${CMAKE_SOURCE_DIR}/runtime/include"
    --lib "${CMAKE_BINARY_DIR}/tms.lib" --lib "${CMAKE_BINARY_DIR}/nx_crt_math.lib" --lib "${CMAKE_BINARY_DIR}/nx_vcruntime.lib"
    --def "${NX_SETUP}/game.def" --clang "${CMAKE_C_COMPILER}" --lld "${NX_LLD_LINK}"
  DEPENDS ${GEN_FILES} "${NX_GEN}/funcs.h" runtime/include/guest.h "${NX_SETUP}/build_game.py" "${NX_SETUP}/game.def"
    "${CMAKE_BINARY_DIR}/nx_crt_math.lib" "${CMAKE_BINARY_DIR}/nx_vcruntime.lib" tms
  USES_TERMINAL)
add_custom_target(game_dll ALL DEPENDS "${CMAKE_BINARY_DIR}/game.dll")
endif()

else()
# ------------------------------------------------------------------------------------------------ Linux
# tms (the runtime) + game.so (the lifted code, tools/setup/build_game.py, as the player's setup builds it). System
# libraries through pkg-config: SDL3, Vulkan headers, libvpx (VP9 movies), cairo + pango (menu, subtitles, button icons).
# DLSS: the driver's libnvidia-ngx.so.1 at run time (vk_dlss.cpp). No FSR 3 on Linux yet (vk_fsr3.cpp reports it unavailable).
find_package(PkgConfig REQUIRED)
pkg_check_modules(NX_LINUX_DEPS REQUIRED IMPORTED_TARGET sdl3 vulkan vpx cairo pangocairo)
find_package(Threads REQUIRED)

file(GLOB RT_SOURCES CONFIGURE_DEPENDS runtime/src/*.cpp)
add_executable(tms ${RT_SOURCES})
# tms exports the runtime entry points (guest.h NX_API) that game.so calls: -rdynamic
set_target_properties(tms PROPERTIES ENABLE_EXPORTS ON)
target_include_directories(tms PRIVATE runtime/include runtime/src)
target_compile_options(tms PRIVATE -ffp-contract=off -fno-strict-aliasing -Wno-unused-parameter)
target_link_libraries(tms PRIVATE PkgConfig::NX_LINUX_DEPS Threads::Threads ${CMAKE_DL_LIBS})
add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/data/subtitles_en.tsv" $<TARGET_FILE_DIR:tms>)
add_custom_command(TARGET tms POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/data/resist_icons" $<TARGET_FILE_DIR:tms>/resist_icons)
# launcher next to tms: the same pages as on Windows, through runtime/launcher/linux_shim.h (SDL3 + cairo / pango)
add_executable(launcher runtime/launcher/launcher.cpp)
target_include_directories(launcher PRIVATE runtime/include runtime/src)
target_link_libraries(launcher PRIVATE PkgConfig::NX_LINUX_DEPS Threads::Threads)

if(NX_HAVE_GEN)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(NX_LD_LLD ld.lld REQUIRED)
set(NX_SETUP "${CMAKE_SOURCE_DIR}/tools/setup")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/game.so"
  COMMAND "${Python3_EXECUTABLE}" "${NX_SETUP}/build_game.py" --gen "${NX_GEN}" --obj "${CMAKE_BINARY_DIR}/game_obj"
    --out "${CMAKE_BINARY_DIR}/game.so" --include "${CMAKE_SOURCE_DIR}/runtime/include" --clang "${CMAKE_C_COMPILER}"
    --lld "${NX_LD_LLD}"
  DEPENDS ${GEN_FILES} "${NX_GEN}/funcs.h" runtime/include/guest.h "${NX_SETUP}/build_game.py"
  USES_TERMINAL)
add_custom_target(game_so ALL DEPENDS "${CMAKE_BINARY_DIR}/game.so")
endif()
endif()
