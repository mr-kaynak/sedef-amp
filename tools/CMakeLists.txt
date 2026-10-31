# Developer tools.

add_executable(sedef-params sedef_params.cpp)
target_link_libraries(sedef-params PRIVATE sedef::engine sedef::compiler_settings)
