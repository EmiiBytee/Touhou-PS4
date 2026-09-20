# Writes Embeds.hpp with the shader sources as null-terminated char arrays.
function(embed file var out)
    file(READ ${file} hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " bytes "${hex}")
    file(APPEND ${out} "\nstatic char ${var}[] = {${bytes}0};\n")
endfunction()

get_filename_component(dir ${OUT} DIRECTORY)
file(MAKE_DIRECTORY ${dir})
file(WRITE ${OUT} "#pragma once\n")
embed(${VERT} vertShaderBytes ${OUT})
embed(${FRAG} fragShaderBytes ${OUT})
