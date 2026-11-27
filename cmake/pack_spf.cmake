# pack_spf.cmake: packs a sample app wallpaper into a Screen+ file (.spf), as the app's own Export would.
#   cmake -DNAME=Aurora [-DTITLE="Koi Pond" | -DTITLE_FILE=<a file holding it>] -DEXE=<aurora.exe> -DPROPS=<LivelyProperties.json> -DPREVIEW=<preview.jpg>
#         [-DASSETS=<dir>] [-DLICENSE=<LICENSE.txt>] -DOUT=<Aurora.spf> -P pack_spf.cmake
# Layout (file_format.c): manifest.json, media/wallpaper/<the exe, its properties (if any), its preview, its assets'
# files and folders, its LICENSE.txt>, thumbnail<ext>. Programs look for their assets beside themselves.
# type 4 = WP_APP, format 1 = lively (its Customise panel is LivelyProperties.json).
if(DEFINED TITLE_FILE AND EXISTS "${TITLE_FILE}")
    file(READ "${TITLE_FILE}" TITLE)
endif()
if(NOT DEFINED TITLE)
    set(TITLE "${NAME}")   # the library's title; NAME is the file's
endif()
get_filename_component(_out_dir "${OUT}" DIRECTORY)
set(_stage "${_out_dir}/spf-stage-${NAME}")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}/media/wallpaper")
file(COPY "${EXE}" DESTINATION "${_stage}/media/wallpaper")
get_filename_component(_exe "${EXE}" NAME)
file(COPY "${PREVIEW}" DESTINATION "${_stage}/media/wallpaper")
if(EXISTS "${PROPS}")
    file(COPY "${PROPS}" DESTINATION "${_stage}/media/wallpaper")
endif()
if(DEFINED ASSETS AND IS_DIRECTORY "${ASSETS}")
    file(COPY "${ASSETS}/" DESTINATION "${_stage}/media/wallpaper")
endif()
if(DEFINED LICENSE AND EXISTS "${LICENSE}")
    file(COPY "${LICENSE}" DESTINATION "${_stage}/media/wallpaper")
endif()
get_filename_component(_ext "${PREVIEW}" LAST_EXT)
string(TOLOWER "${_ext}" _ext)
set(_thumb "thumbnail${_ext}")
configure_file("${PREVIEW}" "${_stage}/${_thumb}" COPYONLY)
file(WRITE "${_stage}/manifest.json"
    "{\"screenplus\":\"spf\",\"version\":1,\"name\":\"${TITLE}\",\"type\":4,\"format\":1,\"flags\":0,\"volCut\":0,"
    "\"slideSec\":0,\"path\":\"media/wallpaper/${_exe}\",\"thumb\":\"${_thumb}\"}")
file(REMOVE "${OUT}")
file(GLOB _items RELATIVE "${_stage}" "${_stage}/*")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip ${_items}
                WORKING_DIRECTORY "${_stage}" RESULT_VARIABLE _rc)
file(REMOVE_RECURSE "${_stage}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "pack_spf: could not write ${OUT}")
endif()
