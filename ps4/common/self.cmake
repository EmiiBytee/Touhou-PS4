# th_add_self(<target> GAME|MINIAPP)
#
# Like PacBrew's add_self(), but with the identity matching the kind of app:
#  MINIAPP: PacBrew's defaults (PAID 0x3800000000000035 + its system-style authinfo). This is
#           what lets Piglet, the shell's OpenGL ES, initialize; the pkg must be category "gde".
#  GAME:    the OpenOrbis samples' identity (PAID 0x3800000000000011, default authinfo), as
#           used by homebrew that runs as a real game (category "gd") and draws through
#           SceVideoOut.
function(th_add_self project kind)
    if(kind STREQUAL "MINIAPP")
        set(args --paid 0x3800000000000035 --authinfo
            000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000)
    else()
        set(args --paid 0x3800000000000011)
    endif()
    add_custom_command(
        OUTPUT "${project}.self"
        COMMAND ${CMAKE_COMMAND} -E env "OO_PS4_TOOLCHAIN=$ENV{OPENORBIS}" "$ENV{OPENORBIS}/bin/create-fself"
                "-in=${project}" "-out=${project}.oelf" --eboot eboot.bin ${args}
        COMMAND ${CMAKE_COMMAND} -E touch "${project}.self"
        VERBATIM
        DEPENDS "${project}")
    add_custom_target("${project}_self" ALL DEPENDS "${project}.self")
endfunction()
