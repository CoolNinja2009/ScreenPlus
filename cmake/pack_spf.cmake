# pack_spf.cmake: packs a sample app wallpaper into a Screen+ file (.spf), as the app's own Export would.
#   cmake -DNAME=Aurora -DEXE=<aurora.exe> -DPROPS=<LivelyProperties.json> -DPREVIEW=<preview.jpg> -DOUT=<Aurora.spf>
#         -P pack_spf.cmake
# Layout (file_format.c): manifest.json, media/wallpaper/<the exe, its properties, its preview>, thumbnail<ext>.
# type 4 = WP_APP, format 1 = lively (its Customise panel is LivelyProperties.json).
get_filename_component(_out_dir "${OUT}" DIRECTORY)
set(_stage "${_out_dir}/spf-stage-${NAME}")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}/media/wallpaper")
file(COPY "${EXE}" DESTINATION "${_stage}/media/wallpaper")
get_filename_component(_exe "${EXE}" NAME)
file(COPY "${PROPS}" "${PREVIEW}" DESTINATION "${_stage}/media/wallpaper")
get_filename_component(_ext "${PREVIEW}" LAST_EXT)
string(TOLOWER "${_ext}" _ext)
set(_thumb "thumbnail${_ext}")
configure_file("${PREVIEW}" "${_stage}/${_thumb}" COPYONLY)
file(WRITE "${_stage}/manifest.json"
    "{\"screenplus\":\"spf\",\"version\":1,\"name\":\"${NAME}\",\"type\":4,\"format\":1,\"flags\":0,\"volCut\":0,"
    "\"slideSec\":0,\"path\":\"media/wallpaper/${_exe}\",\"thumb\":\"${_thumb}\"}")
file(REMOVE "${OUT}")
file(GLOB _items RELATIVE "${_stage}" "${_stage}/*")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUT}" --format=zip ${_items}
                WORKING_DIRECTORY "${_stage}" RESULT_VARIABLE _rc)
file(REMOVE_RECURSE "${_stage}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "pack_spf: could not write ${OUT}")
endif()
