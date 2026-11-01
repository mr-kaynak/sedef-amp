# Shared compiler settings for Sedef targets.
add_library(sedef_compiler_settings INTERFACE)
add_library(sedef::compiler_settings ALIAS sedef_compiler_settings)

if(MSVC)
    target_compile_options(sedef_compiler_settings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /fp:fast
        /wd4324) # C4324: padding added for alignas (cache-line separation is the point)
    if(SEDEF_WARNINGS_AS_ERRORS)
        target_compile_options(sedef_compiler_settings INTERFACE /WX)
    endif()
else()
    target_compile_options(sedef_compiler_settings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
        -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
        $<$<CONFIG:Release,RelWithDebInfo>:-O3>
        -fno-math-errno)
    if(SEDEF_WARNINGS_AS_ERRORS)
        target_compile_options(sedef_compiler_settings INTERFACE -Werror)
    endif()
    if(SEDEF_SANITIZE)
        target_compile_options(sedef_compiler_settings INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(sedef_compiler_settings INTERFACE -fsanitize=address,undefined)
    endif()
    if(SEDEF_SANITIZE_THREAD)
        target_compile_options(sedef_compiler_settings INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(sedef_compiler_settings INTERFACE -fsanitize=thread)
    endif()
endif()
