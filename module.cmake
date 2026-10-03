# wxl-loot-beam: per-extension build glue, included by the core's extension loop after the target is
# created. The module is header-only besides the core SDK and the two game .cpp files the core compiles
# into every extension, so the only extra step is deploying the INI beside the DLL (the core copies the
# DLL itself).

# The companion AzerothCore module under server/ is its own CMake project for the server side and must
# not be linked into the client DLL, but the core discovers extension sources with a recursive glob.
# Mark its sources header-only so the glob finds them while the client target ignores them.
file(GLOB_RECURSE WXL_LOOT_BEAM_SERVER_SRC "${wxl_ext_dir}/server/*.cpp")
if(WXL_LOOT_BEAM_SERVER_SRC)
    set_source_files_properties(${WXL_LOOT_BEAM_SERVER_SRC} PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

if(CLIENT_PATH)
    add_custom_command(TARGET ${wxl_ext_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CLIENT_PATH}/Extensions/${wxl_ext_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${wxl_ext_dir}/wxl-loot-beam.ini"
                "${CLIENT_PATH}/Extensions/${wxl_ext_name}/wxl-loot-beam.ini"
        COMMENT "Deploy wxl-loot-beam config -> ${CLIENT_PATH}/Extensions/${wxl_ext_name}")
endif()
