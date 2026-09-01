# HLSL is compiled to DXBC at build time and embedded as a byte array.
#
# D3DCompile is never called at runtime: d3dcompiler_47 is routinely absent from a Proton
# prefix, and that is the single most common Linux failure mode for tools of this shape.
# Compiling here also means a broken shader fails the build rather than the game, which
# matters when the developer cannot run the game. (docs/RESEARCH.md §1.4)

find_program(FXC_EXECUTABLE fxc
    HINTS
        "$ENV{WindowsSdkVerBinPath}x64"
        "$ENV{WindowsSdkDir}bin/x64"
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin"
    PATH_SUFFIXES
        x64
        10.0.26100.0/x64
        10.0.22621.0/x64
        10.0.19041.0/x64
    DOC "Legacy HLSL compiler, for shader model 5.0")

if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR
        "fxc not found. It ships with the Windows SDK; set FXC_EXECUTABLE to its path.")
endif()

message(STATUS "Using fxc: ${FXC_EXECUTABLE}")

# compile_shader(<target> <hlsl-path> <entry> <profile> <c-symbol> [<extra-dependency>...])
#
# Emits a header defining `<c-symbol>` as a byte array, adds it to the target's sources so the
# build orders correctly, and puts its directory on the include path.
#
# Any trailing arguments are added to the custom command's DEPENDS. fxc resolves #include
# relative to the including file, but CMake cannot see through that, so a shader that includes a
# shared .hlsli must name it here or an edit to the .hlsli will not rebuild anything — the kind
# of staleness that ships a wrong image from a correct source tree.
function(compile_shader TARGET SOURCE ENTRY PROFILE SYMBOL)
    get_filename_component(name ${SOURCE} NAME_WE)
    set(outdir "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    set(out "${outdir}/${name}.h")

    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${outdir}"
        # /WX so a shader warning fails the build. A silently miscompiled shader produces a
        # wrong image rather than an error, which is the failure mode this project can least
        # afford.
        COMMAND "${FXC_EXECUTABLE}" /nologo /T ${PROFILE} /E ${ENTRY}
                /Fh "${out}" /Vn ${SYMBOL} /O3 /WX "${SOURCE}"
        DEPENDS "${SOURCE}" ${ARGN}
        COMMENT "fxc ${name}.hlsl -> ${name}.h (${PROFILE})"
        VERBATIM)

    target_sources(${TARGET} PRIVATE "${out}")
    target_include_directories(${TARGET} PRIVATE "${outdir}")
endfunction()
