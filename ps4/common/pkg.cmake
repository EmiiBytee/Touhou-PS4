# Builds the staging directory add_pkg() packs into the .pkg:
#   sce_sys/icon0.png          from the local package-art template, or OpenOrbis fallback
#   sce_sys/pic1.png           local 1920x1080 background (optional)
#   sce_sys/about/right.sprx   from the OpenOrbis samples
#   sce_module/*.prx           from the OpenOrbis samples (libc, Fios2)
#
# With TH_BUNDLE_ASSETS=ON it also copies the user's own game files into assets/ for a
# PERSONAL pkg that must never be shared. The final OpenGNM ports do not use Piglet modules.
option(TH_BUNDLE_ASSETS "Bundle your own game data into a personal pkg" OFF)

function(th_prepare_pkg_dir out template)
    set(samples $ENV{OPENORBIS}/samples/piglet)
    file(REMOVE_RECURSE ${out})
    file(MAKE_DIRECTORY ${out}/sce_sys/about ${out}/sce_module)
    # Package artwork is deliberately not stored in the public repository unless its
    # redistribution rights are known. A local icon wins; otherwise use OpenOrbis' sample
    # icon so a source checkout remains buildable.
    if(EXISTS ${template}/sce_sys/icon0.png)
        file(COPY ${template}/sce_sys/icon0.png DESTINATION ${out}/sce_sys)
    else()
        file(COPY ${samples}/sce_sys/icon0.png DESTINATION ${out}/sce_sys)
        message(WARNING "No local icon0.png; using the OpenOrbis sample icon")
    endif()
    if(EXISTS ${template}/sce_sys/pic1.png)
        file(COPY ${template}/sce_sys/pic1.png DESTINATION ${out}/sce_sys)
    endif()
    file(COPY ${samples}/sce_sys/about/right.sprx DESTINATION ${out}/sce_sys/about)
    file(GLOB modules ${samples}/sce_module/*.prx)
    file(COPY ${modules} DESTINATION ${out}/sce_module)

    if(NOT TH_BUNDLE_ASSETS)
        return()
    endif()

    # ARGN: files/dirs relative to TH_ASSETS_DIR that the game reads.
    if(NOT IS_DIRECTORY "${TH_ASSETS_DIR}")
        message(FATAL_ERROR "TH_BUNDLE_ASSETS needs -DTH_ASSETS_DIR=<your game folder>")
    endif()
    foreach(item ${ARGN})
        if(NOT EXISTS "${TH_ASSETS_DIR}/${item}")
            message(FATAL_ERROR "missing ${TH_ASSETS_DIR}/${item}")
        endif()
        file(COPY "${TH_ASSETS_DIR}/${item}" DESTINATION ${out}/assets)
    endforeach()
    message(STATUS "Bundling game data from ${TH_ASSETS_DIR} (personal pkg, do not share)")

    # The games render text with MS Gothic, which is not part of the game files.
    set(TH_FONT_FILE "/mnt/c/Windows/Fonts/msgothic.ttc" CACHE FILEPATH "msgothic.ttc to bundle")
    if(EXISTS "${TH_FONT_FILE}")
        file(COPY "${TH_FONT_FILE}" DESTINATION ${out}/assets)
    else()
        message(WARNING "${TH_FONT_FILE} not found: put msgothic.ttc in the game's /data folder")
    endif()

    # English (or other) translation: the thcrap patch stack next to the game, if any.
    if(IS_DIRECTORY "${TH_ASSETS_DIR}/thcrap" AND DEFINED TH_GAME_ID)
        execute_process(
            COMMAND python3 ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/stage_thcrap.py
                    "${TH_ASSETS_DIR}/thcrap" ${TH_GAME_ID} ${out}/assets/patch
                    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../private/overlay/${TH_GAME_ID}
            RESULT_VARIABLE staged)
        if(staged EQUAL 0)
            message(STATUS "Bundling thcrap translation patches from ${TH_ASSETS_DIR}/thcrap")
        endif()
    endif()

endfunction()
