# Shared compiler options for our own targets (not for third-party sources).
function(acp_set_warnings target)
    target_compile_options(${target} PRIVATE
        /W4 /WX          # all warnings, as errors
        /permissive-     # standard-conforming C++
        /utf-8           # UTF-8 source and execution charset
        /Zc:__cplusplus
        /EHsc
    )
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endfunction()
