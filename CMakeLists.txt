cmake_minimum_required(VERSION 3.21)
set(SCREENPLUS_VERSION "1.8.0" CACHE STRING "Screen+ version (major.minor.patch); release builds pass the tag")
project(ScreenPlus VERSION ${SCREENPLUS_VERSION} LANGUAGES C)

if(NOT MSVC)
    message(FATAL_ERROR "Screen+ is built with MSVC (Visual Studio 2022 Build Tools or newer).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Screen+ targets 64-bit Windows only.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no VC++ redist needed

# ---------------------------------------------------------------- WebView2 SDK (BSD-3-Clause)
set(WEBVIEW2_VERSION "1.0.4258.31")
set(WEBVIEW2_SHA256 "56F7F4B8BF9AEE4B8EFEFBBDD4F67D5F74EBD1B100ED0806DA71BF76AF481AA9")
set(WEBVIEW2_SDK_DIR "${CMAKE_BINARY_DIR}/_deps/webview2" CACHE PATH "Extracted Microsoft.Web.WebView2 NuGet package")
if(NOT EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
    set(_pkg "${CMAKE_BINARY_DIR}/_deps/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg")
    message(STATUS "Downloading WebView2 SDK ${WEBVIEW2_VERSION} from nuget.org")
    file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
         "${_pkg}" EXPECTED_HASH SHA256=${WEBVIEW2_SHA256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${WEBVIEW2_SDK_DIR}")
endif()

# ---------------------------------------------------------------- ONNX Runtime C API headers (MIT)
# Only the headers are used at build time. The DirectML build of onnxruntime.dll comes from the very same
# package at run time (src/app/depth.c downloads it on the user's request, pinned by the same SHA-256).
set(ORT_VERSION "1.24.4")
set(ORT_SHA256 "57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681")
set(ORT_SDK_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime" CACHE PATH "Extracted Microsoft.ML.OnnxRuntime.DirectML NuGet package")
if(NOT EXISTS "${ORT_SDK_DIR}/build/native/include/onnxruntime_c_api.h")
    set(_pkg "${CMAKE_BINARY_DIR}/_deps/microsoft.ml.onnxruntime.directml.${ORT_VERSION}.nupkg")
    message(STATUS "Downloading ONNX Runtime ${ORT_VERSION} (DirectML) from nuget.org")
    file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/${ORT_VERSION}/microsoft.ml.onnxruntime.directml.${ORT_VERSION}.nupkg"
         "${_pkg}" EXPECTED_HASH SHA256=${ORT_SHA256} TLS_VERIFY ON)
    file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${ORT_SDK_DIR}" PATTERNS "build/native/include/*")
endif()

# ---------------------------------------------------------------- FFmpeg (LGPL-2.1+, shared)
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg install prefix (include/, lib/, bin/)")
if(NOT EXISTS "${FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg not found in ${FFMPEG_DIR}. Run scripts/build-ffmpeg.sh (MSYS2 UCRT64) first, "
                        "or pass -DFFMPEG_DIR=<prefix of an LGPL shared FFmpeg 8.x build>.")
endif()
set(FFMPEG_LIBS avformat avcodec avutil swscale swresample)
set(FFMPEG_DLLS "")
foreach(lib ${FFMPEG_LIBS})
    file(GLOB _dll "${FFMPEG_DIR}/bin/${lib}-*.dll")
    if(NOT _dll)
        message(FATAL_ERROR "Missing ${FFMPEG_DIR}/bin/${lib}-*.dll")
    endif()
    list(GET _dll 0 _dll)
    list(APPEND FFMPEG_DLLS "${_dll}")
    get_filename_component(_dllname "${_dll}" NAME)
    list(APPEND FFMPEG_DELAYLOAD "/DELAYLOAD:${_dllname}")
    # MSVC import library generated from FFmpeg's .def (more reliable than the dlltool output)
    file(GLOB _def "${FFMPEG_DIR}/lib/${lib}-*.def")
    set(_implib "${CMAKE_BINARY_DIR}/ffmpeg-implib/${lib}.lib")
    if(_def AND NOT EXISTS "${_implib}")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ffmpeg-implib")
        get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
        execute_process(COMMAND "${_linker_dir}/lib.exe" /nologo /machine:x64 "/def:${_def}" "/out:${_implib}"
                        RESULT_VARIABLE _r OUTPUT_QUIET)
    endif()
    if(EXISTS "${_implib}")
        list(APPEND FFMPEG_IMPLIBS "${_implib}")
    else()
        list(APPEND FFMPEG_IMPLIBS "${FFMPEG_DIR}/lib/${lib}.lib")
    endif()
endforeach()

# ---------------------------------------------------------------- shaders (fxc, SM 4.0)
find_program(FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64" REQUIRED)
set(SHADER_SRC "${CMAKE_SOURCE_DIR}/src/renderer/video/video.hlsl")
set(SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SHADER_HEADERS "")
foreach(entry vs_main:vs_4_0:video_vs ps_nv12:ps_4_0:video_ps_nv12 ps_i420:ps_4_0:video_ps_i420 ps_rgb:ps_4_0:video_ps_rgb)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 ep)
    list(GET parts 1 profile)
    list(GET parts 2 name)
    add_custom_command(OUTPUT "${SHADER_OUT}/${name}.h"
        COMMAND "${FXC}" /nologo /O3 /Qstrip_reflect /Qstrip_debug /T ${profile} /E ${ep} /Vn g_${ep} /Fh "${SHADER_OUT}/${name}.h" "${SHADER_SRC}"
        DEPENDS "${SHADER_SRC}" VERBATIM)
    list(APPEND SHADER_HEADERS "${SHADER_OUT}/${name}.h")
endforeach()

# ---------------------------------------------------------------- application
set(SOURCES
    src/main.c
    src/common/util.c src/common/log.c src/common/paths.c src/common/json.c src/common/webfetch.c
    src/common/ytdlp.c src/common/scrnsave.c src/common/autostart.c
    src/screensaver/screensaver.c
    src/settings/config.c
    src/monitor/monitor.c
    src/desktop/desktop_host.c
    src/diagnostics/diag.c
    src/engine/engine.c src/engine/occlusion.c src/engine/audio_visualizer.c src/engine/now_playing.c
    src/engine/sysinfo.c src/engine/stills.c
    src/renderer/image/image_renderer.c
    src/renderer/video/video_renderer.c src/renderer/video/video_audio.c src/renderer/video/ffmpeg_thumb.c
    src/renderer/video/video_stream.c
    src/renderer/html/html_renderer.c src/renderer/html/wallpaper_api.c
    src/app/app.c src/app/supervisor.c src/app/startup.c src/app/library.c src/app/preview_capture.c
    src/app/cli_server.c src/app/url_import.c src/app/depth.c src/app/depth_infer.c src/app/store.c src/app/update.c
    src/app/store_sources.c
    src/tray/tray.c
    src/ui/ui_core.c src/ui/ui_pages.c src/ui/textedit.c src/ui/thumbs.c src/ui/identify.c
    resources/screenplus.rc
)
add_executable(ScreenPlus WIN32 ${SOURCES} ${SHADER_HEADERS})
target_include_directories(ScreenPlus PRIVATE src "${SHADER_OUT}")
target_include_directories(ScreenPlus SYSTEM PRIVATE "${WEBVIEW2_SDK_DIR}/build/native/include" "${FFMPEG_DIR}/include"
                                                     "${ORT_SDK_DIR}/build/native/include")
set_source_files_properties(resources/screenplus.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/resources/depth.html")
target_compile_definitions(ScreenPlus PRIVATE UNICODE _UNICODE COBJMACROS NOMINMAX
                                              _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C
                                              _CRT_SECURE_NO_WARNINGS
                                              SP_VERSION_STR="${PROJECT_VERSION}" SP_VER_MAJOR=${PROJECT_VERSION_MAJOR}
                                              SP_VER_MINOR=${PROJECT_VERSION_MINOR} SP_VER_PATCH=${PROJECT_VERSION_PATCH})
target_compile_options(ScreenPlus PRIVATE /W4 /utf-8 /guard:cf /GS /sdl- /wd4204 /wd4221 /wd4100)
target_link_options(ScreenPlus PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf ${FFMPEG_DELAYLOAD}
                                       /MANIFEST:NO)
target_link_libraries(ScreenPlus PRIVATE
    "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    ${FFMPEG_IMPLIBS} delayimp
    user32 gdi32 shell32 shlwapi ole32 oleaut32 uuid comctl32 comdlg32 advapi32 version
    d2d1 dwrite d3d11 dxgi dxguid windowscodecs dwmapi shcore uxtheme wtsapi32 powrprof psapi bcrypt ole32 avrt mmdevapi runtimeobject iphlpapi pdh winhttp
    taskschd)

# Sample wallpapers besides Aurora: setup bundles only Aurora and, when "Sample wallpapers" is ticked, runs
# `screenplus samples install`, which downloads this release's ScreenPlus-<version>-samples.zip (CI uploads it),
# pinned by the size and SHA-256 below. Packed at configure time, which reruns when a sample file changes.
file(GLOB_RECURSE SAMPLE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/samples/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SAMPLE_FILES})
file(GLOB SAMPLE_DIRS RELATIVE "${CMAKE_SOURCE_DIR}/samples" "${CMAKE_SOURCE_DIR}/samples/*")
list(REMOVE_ITEM SAMPLE_DIRS Aurora)
list(SORT SAMPLE_DIRS)
set(SAMPLES_ZIP "${CMAKE_BINARY_DIR}/ScreenPlus-${PROJECT_VERSION}-samples.zip")
string(SHA256 SAMPLES_LIST "${SAMPLE_FILES}")
set(SAMPLES_PACK OFF)
if(NOT EXISTS "${SAMPLES_ZIP}" OR NOT SAMPLES_LIST STREQUAL "${SAMPLES_PACKED_LIST}")
    set(SAMPLES_PACK ON)
endif()
foreach(f IN LISTS SAMPLE_FILES)
    if("${f}" IS_NEWER_THAN "${SAMPLES_ZIP}")
        set(SAMPLES_PACK ON)
    endif()
endforeach()
if(SAMPLES_PACK)   # only when a sample changed: the zip isn't byte-reproducible (libarchive records access times)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${SAMPLES_ZIP}" --format=zip "--mtime=2000-01-01 00:00:00 UTC"
                            -- ${SAMPLE_DIRS}
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/samples" RESULT_VARIABLE SAMPLES_RC)
    if(SAMPLES_RC)
        message(FATAL_ERROR "Could not pack the sample wallpapers into ${SAMPLES_ZIP}")
    endif()
    set(SAMPLES_PACKED_LIST "${SAMPLES_LIST}" CACHE INTERNAL "")
endif()
file(SHA256 "${SAMPLES_ZIP}" SAMPLES_SHA256)
file(SIZE "${SAMPLES_ZIP}" SAMPLES_SIZE)

# Terminal companion, shipped as ScreenPlus.com next to ScreenPlus.exe: a terminal resolves
# `screenplus` to the .com first (PATHEXT order), shortcuts and the startup entry name the .exe.
add_executable(ScreenPlusCli src/cli/cli_main.c src/common/util.c src/common/json.c src/common/paths.c
                             src/common/webfetch.c src/common/scrnsave.c src/common/autostart.c)
set_target_properties(ScreenPlusCli PROPERTIES OUTPUT_NAME ScreenPlus SUFFIX ".com" PDB_NAME ScreenPlusCli)
target_include_directories(ScreenPlusCli PRIVATE src)
target_compile_definitions(ScreenPlusCli PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00
                                                 _CRT_SECURE_NO_WARNINGS SP_VERSION_STR="${PROJECT_VERSION}"
                                                 SP_SAMPLES_SIZE=${SAMPLES_SIZE} SP_SAMPLES_SHA256="${SAMPLES_SHA256}")
target_compile_options(ScreenPlusCli PRIVATE /W4 /utf-8 /guard:cf /GS /wd4204 /wd4221 /wd4100)
target_link_options(ScreenPlusCli PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /guard:cf /INCREMENTAL:NO)
target_link_libraries(ScreenPlusCli PRIVATE user32 shell32 shlwapi ole32 oleaut32 advapi32 bcrypt winhttp taskschd)

# Validation helpers (not shipped): check the wallpaper is attached and visible behind the
# icons, and print the livelySystemInformation payload the engine feeds web wallpapers.
add_executable(desktop_probe tests/desktop_probe.c)
target_link_libraries(desktop_probe PRIVATE user32 gdi32 dwmapi)
add_executable(sysinfo_test tests/sysinfo_test.c
    src/engine/sysinfo.c src/common/util.c src/common/json.c src/common/log.c src/common/paths.c)
target_compile_definitions(sysinfo_test PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(sysinfo_test PRIVATE /W4 /utf-8 /wd4100)
target_link_libraries(sysinfo_test PRIVATE pdh advapi32 dxgi dxguid bcrypt shlwapi)

# Ship FFmpeg DLLs, licenses and the sample wallpaper next to the executable.
add_custom_command(TARGET ScreenPlus POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} "$<TARGET_FILE_DIR:ScreenPlus>"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ScreenPlus>" "$<TARGET_FILE_DIR:ScreenPlus>/ScreenPlus.scr"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/samples" "$<TARGET_FILE_DIR:ScreenPlus>/samples"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ScreenPlus>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_SOURCE_DIR}/LICENSE"
            "$<TARGET_FILE_DIR:ScreenPlus>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_DIR}/COPYING.LGPLv2.1" "${FFMPEG_DIR}/BUILDINFO.txt"
            "$<TARGET_FILE_DIR:ScreenPlus>/licenses"
    VERBATIM)

# ---------------------------------------------------------------- installer (NSIS, optional)
# Produces build/ScreenPlus-Setup-<version>.exe from the files staged next to ScreenPlus.exe.
# Install NSIS 3 (winget install NSIS.NSIS) or pass -DMAKENSIS=<path to makensis.exe>.
find_program(MAKENSIS makensis PATHS "C:/Program Files (x86)/NSIS" "C:/Program Files/NSIS" "$ENV{LOCALAPPDATA}/Programs/NSIS")
if(MAKENSIS)
    set(SETUP_EXE "${CMAKE_BINARY_DIR}/ScreenPlus-Setup-${PROJECT_VERSION}.exe")
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _stage)
    file(TO_NATIVE_PATH "${SETUP_EXE}" _setup)
    add_custom_command(OUTPUT "${SETUP_EXE}"
        COMMAND "${MAKENSIS}" /V2 "/DVERSION=${PROJECT_VERSION}" "/DSTAGE=${_stage}" "/DOUTFILE=${_setup}"
                "${CMAKE_SOURCE_DIR}/installer/screenplus.nsi"
        DEPENDS ScreenPlus ScreenPlusCli "${CMAKE_SOURCE_DIR}/installer/screenplus.nsi" "${CMAKE_SOURCE_DIR}/installer/wizard.bmp"
                "${CMAKE_SOURCE_DIR}/installer/header.bmp" "${CMAKE_SOURCE_DIR}/LICENSE"
        COMMENT "Building ScreenPlus-Setup-${PROJECT_VERSION}.exe" VERBATIM)
    add_custom_target(ScreenPlusSetup ALL DEPENDS "${SETUP_EXE}")
else()
    message(STATUS "NSIS (makensis) not found: the installer will not be built. Install it with: winget install NSIS.NSIS")
endif()
