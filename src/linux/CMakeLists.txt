# Screen+ for Linux: `screenplus` (CLI + background service, plain libc) and `screenplus-engine` (the Hyprland
# renderer). KDE Plasma needs no engine: the wallpaper runs in plasmashell, in the plugin under kde/.
option(SCREENPLUS_ENGINE "Build screenplus-engine (needs GTK 4, gtk4-layer-shell, libmpv, WebKitGTK)" ON)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_SOURCE_DIR}/src)
set(SHARED
    compat.c run.c desktop_hypr.c
    ${ROOT}/common/util.c ${ROOT}/common/paths.c ${ROOT}/common/log.c ${ROOT}/common/json.c
    ${ROOT}/settings/config.c ${ROOT}/renderer/html/wallpaper_api.c)

add_executable(screenplus cli.c service.c desktop_kde.c url_import.c ${SHARED}
    ${ROOT}/app/cli_server.c ${ROOT}/app/library.c)
target_link_libraries(screenplus PRIVATE m pthread)

add_compile_definitions(_GNU_SOURCE SP_VERSION_STR="${SCREENPLUS_VERSION}")
target_compile_options(screenplus PRIVATE -Wall -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas)

if(SCREENPLUS_ENGINE)
    find_package(PkgConfig REQUIRED)
    # gtk4-layer-shell has to come before libwayland-client on the link line
    pkg_check_modules(ENGINE REQUIRED IMPORTED_TARGET gtk4-layer-shell-0 gtk4 webkitgtk-6.0 mpv epoxy)
    add_executable(screenplus-engine engine_gtk.c ${SHARED})
    target_link_libraries(screenplus-engine PRIVATE PkgConfig::ENGINE m pthread ${CMAKE_DL_LIBS})
    target_compile_options(screenplus-engine PRIVATE -Wall -Wno-unused -Wno-missing-braces -Wno-unknown-pragmas -Wno-deprecated-declarations)
endif()

install(TARGETS screenplus DESTINATION lib/screenplus)
if(SCREENPLUS_ENGINE)
    install(TARGETS screenplus-engine DESTINATION lib/screenplus)
endif()
install(DIRECTORY kde DESTINATION lib/screenplus)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/samples DESTINATION lib/screenplus)
