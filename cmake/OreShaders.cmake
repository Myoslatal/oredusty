# Ore framework - GLSL/HLSL -> SPIR-V build rules.
#
#   ore_add_shaders(<target>
#                   [SHADERS <file.vert> <file.frag> ...]
#                   [OUTPUT_DIR <dir>]        # default: <current binary dir>/shaders
#                   [COMPILE_OPTIONS <flags>] # extra glslc flags
#                   [EMBED]                   # also emit a C++ header with the SPIR-V bytes
#                   [EMBED_OUTPUT <header>]   # default: <OUTPUT_DIR>/ore_embedded_shaders.h
#                   [DESTINATION <dir>])      # install destination, default: shaders
#
# The compiled .spv files land in OUTPUT_DIR and are added as a build dependency of <target>.
# A compile definition ORE_SHADER_DIR="<OUTPUT_DIR>" is set so the runtime can find them.

find_program(ORE_GLSLC_EXECUTABLE NAMES glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
find_program(ORE_SPIRV_VAL_EXECUTABLE NAMES spirv-val)

function(_ore_shader_stage out file)
    get_filename_component(_ext "${file}" EXT)
    string(TOLOWER "${_ext}" _ext)
    if(_ext STREQUAL ".vert")
        set(_stage vertex)
    elseif(_ext STREQUAL ".frag")
        set(_stage fragment)
    elseif(_ext STREQUAL ".comp")
        set(_stage compute)
    elseif(_ext STREQUAL ".geom")
        set(_stage geometry)
    elseif(_ext STREQUAL ".tesc")
        set(_stage tesscontrol)
    elseif(_ext STREQUAL ".tese")
        set(_stage tesseval)
    elseif(_ext STREQUAL ".mesh")
        set(_stage mesh)
    elseif(_ext STREQUAL ".task")
        set(_stage task)
    elseif(_ext STREQUAL ".rgen")
        set(_stage raygen)
    elseif(_ext STREQUAL ".rmiss")
        set(_stage miss)
    elseif(_ext STREQUAL ".rchit")
        set(_stage closesthit)
    elseif(_ext STREQUAL ".rahit")
        set(_stage anyhit)
    elseif(_ext STREQUAL ".rint")
        set(_stage intersection)
    elseif(_ext STREQUAL ".rcall")
        set(_stage callable)
    else()
        message(FATAL_ERROR "ore_add_shaders: unsupported shader extension '${_ext}' (${file})")
    endif()
    set(${out} ${_stage} PARENT_SCOPE)
endfunction()

function(ore_add_shaders TARGET)
    cmake_parse_arguments(ARG "EMBED" "OUTPUT_DIR;EMBED_OUTPUT;DESTINATION" "SHADERS;COMPILE_OPTIONS" ${ARGN})

    if(NOT ARG_SHADERS)
        message(FATAL_ERROR "ore_add_shaders(${TARGET}): no SHADERS given")
    endif()
    if(NOT ORE_GLSLC_EXECUTABLE)
        message(FATAL_ERROR "ore_add_shaders(${TARGET}): glslc was not found. Install the Vulkan SDK / shaderc package.")
    endif()

    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    endif()
    if(NOT ARG_DESTINATION)
        set(ARG_DESTINATION "shaders")
    endif()
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")

    set(_outputs "")
    set(_stage_flag "")
    foreach(_src IN LISTS ARG_SHADERS)
        get_filename_component(_abs "${_src}" ABSOLUTE)
        if(NOT EXISTS "${_abs}")
            message(FATAL_ERROR "ore_add_shaders(${TARGET}): shader '${_src}' does not exist")
        endif()
        get_filename_component(_name "${_abs}" NAME)
        _ore_shader_stage(_stage "${_abs}")
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
        set(_dep "${ARG_OUTPUT_DIR}/${_name}.d")
        get_filename_component(_dir "${_abs}" DIRECTORY)

        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_opt "-O0")
            set(_dbg "-g")
        else()
            set(_opt "-O")
            set(_dbg "")
        endif()

        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${ORE_GLSLC_EXECUTABLE}" -fshader-stage=${_stage} --target-env=vulkan1.3
                    ${_opt} ${_dbg} -MD -MF "${_dep}" -I "${_dir}" -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    ${ARG_COMPILE_OPTIONS} "${_abs}" -o "${_out}"
            DEPENDS "${_abs}"
            DEPFILE "${_dep}"
            COMMENT "GLSL -> SPIR-V ${_name}"
            VERBATIM)
        list(APPEND _outputs "${_out}")
    endforeach()

    add_custom_target(${TARGET}_shaders DEPENDS ${_outputs})
    add_dependencies(${TARGET} ${TARGET}_shaders)
    set_target_properties(${TARGET} PROPERTIES ORE_SHADER_OUTPUTS "${_outputs}")
    target_compile_definitions(${TARGET} PRIVATE ORE_SHADER_DIR="${ARG_OUTPUT_DIR}")

    if(NOT ARG_EMBED AND ORE_EMBED_SHADERS)
        set(ARG_EMBED TRUE)
    endif()
    if(ARG_EMBED)
        if(NOT ARG_EMBED_OUTPUT)
            set(ARG_EMBED_OUTPUT "${ARG_OUTPUT_DIR}/ore_embedded_shaders.h")
        endif()
        add_custom_command(
            OUTPUT "${ARG_EMBED_OUTPUT}"
            COMMAND ${CMAKE_COMMAND}
                    -DINPUT=${_outputs}
                    -DOUTPUT=${ARG_EMBED_OUTPUT}
                    -DNAME_SPACE=ore::embedded_${TARGET}
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedSpirv.cmake"
            DEPENDS ${_outputs} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedSpirv.cmake"
            COMMENT "Embedding SPIR-V into ${ARG_EMBED_OUTPUT}"
            VERBATIM)
        add_custom_target(${TARGET}_embed DEPENDS "${ARG_EMBED_OUTPUT}")
        add_dependencies(${TARGET} ${TARGET}_embed)
        target_include_directories(${TARGET} PRIVATE "${ARG_OUTPUT_DIR}")
    endif()

    if(NOT ARG_EMBED)
        install(FILES ${_outputs} DESTINATION "${ARG_DESTINATION}" OPTIONAL)
    endif()
endfunction()
