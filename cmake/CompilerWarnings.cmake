function(manny_apply_compiler_warnings target_name)
    if(MSVC)
        set(
            project_warnings
            /W4
            /permissive-
            /Zc:__cplusplus
            /Zc:preprocessor
            /utf-8
        )

        if(MANNY_WARNINGS_AS_ERRORS)
            list(APPEND project_warnings /WX)
        endif()
    else()
        set(
            project_warnings
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wdouble-promotion
            -Wformat=2
            -Wnon-virtual-dtor
            -Wnull-dereference
            -Wold-style-cast
            -Woverloaded-virtual
            -Wshadow
            -Wsign-conversion
        )

        if(MANNY_WARNINGS_AS_ERRORS)
            list(APPEND project_warnings -Werror)
        endif()
    endif()

    target_compile_options(${target_name} PRIVATE ${project_warnings})
endfunction()

