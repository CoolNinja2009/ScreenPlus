# Screen+ for Linux: `screenplus` (CLI + background service, plain libc) and `screenplus-engine` (the Hyprland
# renderer). KDE Plasma needs no engine: the wallpaper runs in plasmashell, in the plugin under kde/.
option(SCREENPLUS_ENGINE "Build screenplus-engine (needs GTK 4, gtk4-layer-shell, libmpv, WebKitGTK)" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_SOURCE_DIR}/src)
set(SHARED
    compat.c winobj.c run.c desktop_hypr.c apprules.c
    ${ROOT}/common/util.c ${ROOT}/common/paths.c ${ROOT}/common/log.c ${ROOT}/common/json.c
    ${ROOT}/settings/config.c ${ROOT}/renderer/html/wallpaper_api.c)

add_executable(screenplus cli.c service.c desktop_kde.c url_import.c http_linux.c preview.c feed.c webhost.c ${SHARED}
    ${ROOT}/app/cli_server.c ${ROOT}/app/library.c ${ROOT}/app/store.c ${ROOT}/app/store_sources.c
    ${ROOT}/app/update.c ${ROOT}/app/samples_update.c ${ROOT}/app/depth.c)
target_link_libraries(screenplus PRIVATE m pthread ${CMAKE_DL_LIBS})
set_target_properties(screenplus PROPERTIES ENABLE_EXPORTS ON)   # screenplus-ui.so calls back into it

add_compile_definitions(_GNU_SOURCE SP_VERSION_STR="${SCREENPLUS_VERSION}${SCREENPLUS_SUFFIX}")
target_compile_options(screenplus PRIVATE -Wall -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas)

option(SCREENPLUS_UI "Build screenplus-ui.so, the window (needs SDL3, Cairo, Pango, fontconfig)" ON)
find_package(PkgConfig REQUIRED)
if(SCREENPLUS_UI)
    pkg_check_modules(UI REQUIRED IMPORTED_TARGET sdl3 cairo pangocairo fontconfig)
    add_library(screenplus-ui MODULE ui_sdl.c ui_thumbs.c ui_stubs.c d2d_cairo.c
        ${ROOT}/ui/ui_core.c ${ROOT}/ui/ui_pages.c ${ROOT}/ui/textedit.c)
    # The Ubuntu .deb carries SDL3 and gtk4-layer-shell beside the program (scripts/build-libs-linux.sh)
    set_target_properties(screenplus-ui PROPERTIES PREFIX "" INSTALL_RPATH "\$ORIGIN")
    target_link_libraries(screenplus-ui PRIVATE PkgConfig::UI m pthread)
    target_link_options(screenplus-ui PRIVATE -Wl,-Bsymbolic)   # its ui_* must not resolve to the service's forwarders
    target_compile_options(screenplus-ui PRIVATE -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas)
endif()

# screenplus-depth.so: `screenplus --depth`. FFmpeg's libraries and, at run time, ONNX Runtime (downloaded on request);
# only its header comes in at build time.
option(SCREENPLUS_DEPTH "Build screenplus-depth.so, Make wallpaper cooler (needs FFmpeg's libraries)" ON)
if(SCREENPLUS_DEPTH)
    # Only ONNX Runtime's C API header is needed to build (the library itself is downloaded on request at run time). It
    # comes from the NuGet package the Windows build uses (CMakeLists.txt's ORT_VERSION/ORT_SHA256: keep in step), not
    # from GitHub's release downloads, which fail now and then (504s took a release pipeline down on 2026-11-08).
    set(ORT_PKG ${CMAKE_BINARY_DIR}/onnxruntime.nupkg)
    set(ORT_SHA 57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681)
    set(ORT_INC ${CMAKE_BINARY_DIR}/ort/build/native/include)
    if(NOT EXISTS ${ORT_INC}/onnxruntime_c_api.h)
        foreach(try RANGE 1 4)
            file(DOWNLOAD https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg
                 ${ORT_PKG} STATUS ORT_STATUS)
            list(GET ORT_STATUS 0 ORT_CODE)
            if(ORT_CODE EQUAL 0)
                file(SHA256 ${ORT_PKG} ORT_GOT)
                string(TOUPPER "${ORT_GOT}" ORT_GOT)
                if(ORT_GOT STREQUAL ORT_SHA)
                    break()
                endif()
                set(ORT_STATUS "SHA-256 mismatch")
            endif()
            message(STATUS "ONNX Runtime download failed (${ORT_STATUS}), try ${try} of 4")
            execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 20)
        endforeach()
        if(NOT ORT_GOT STREQUAL ORT_SHA)
            message(FATAL_ERROR "Could not download ONNX Runtime's headers: ${ORT_STATUS}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${ORT_PKG} DESTINATION ${CMAKE_BINARY_DIR}/ort PATTERNS "build/native/include/*")
        file(REMOVE ${ORT_PKG})
    endif()
    pkg_check_modules(DEPTH REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
    add_library(screenplus-depth MODULE depth_module.c wic_linux.c ${ROOT}/app/depth_infer.c)
    # The .rpm and the Ubuntu .deb carry their own FFmpeg in ffmpeg/ (scripts/build-ffmpeg-linux.sh); elsewhere the system's is used
    set_target_properties(screenplus-depth PROPERTIES PREFIX "" INSTALL_RPATH "\$ORIGIN/ffmpeg")
    target_include_directories(screenplus-depth PRIVATE ${ORT_INC})
    target_link_libraries(screenplus-depth PRIVATE PkgConfig::DEPTH m pthread ${CMAKE_DL_LIBS})
    target_link_options(screenplus-depth PRIVATE -Wl,-Bsymbolic)
    target_compile_options(screenplus-depth PRIVATE -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas -Wno-deprecated-declarations)
    install(TARGETS screenplus-depth DESTINATION lib/screenplus)
endif()

if(SCREENPLUS_ENGINE)
    # gtk4-layer-shell has to come before libwayland-client on the link line
    pkg_check_modules(ENGINE REQUIRED IMPORTED_TARGET gtk4-layer-shell-0 gtk4 webkitgtk-6.0 mpv epoxy)
    add_executable(screenplus-engine engine_gtk.c ${SHARED})
    set_target_properties(screenplus-engine PROPERTIES INSTALL_RPATH "\$ORIGIN")
    target_link_libraries(screenplus-engine PRIVATE PkgConfig::ENGINE m pthread ${CMAKE_DL_LIBS})
    target_compile_options(screenplus-engine PRIVATE -Wall -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas -Wno-deprecated-declarations)
endif()

install(TARGETS screenplus DESTINATION lib/screenplus)
if(SCREENPLUS_ENGINE)
    install(TARGETS screenplus-engine DESTINATION lib/screenplus)
endif()
if(SCREENPLUS_UI)
    install(TARGETS screenplus-ui DESTINATION lib/screenplus)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/resources/fonts DESTINATION lib/screenplus)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/resources/depth.html ${CMAKE_SOURCE_DIR}/resources/splat.html DESTINATION lib/screenplus)
install(DIRECTORY kde DESTINATION lib/screenplus)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/samples DESTINATION lib/screenplus)
