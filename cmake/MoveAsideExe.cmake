# Pre-link step: Windows won't let the linker overwrite a running exe, but it does allow renaming
# one. Move the old exe aside so `build/` can always be rebuilt while Refractory is open.
# Input: EXE (full path of the exe about to be linked).
# file(REMOVE) would abort on a locked file, so deletions go through `cmake -E rm`, whose failure is ignored.
if(NOT EXISTS "${EXE}")
    return()
endif()
get_filename_component(_dir "${EXE}" DIRECTORY)
get_filename_component(_name "${EXE}" NAME_WE)

# Clean up earlier moved-aside copies; ones that are still running stay locked and are skipped.
file(GLOB _old "${_dir}/${_name}.old*.exe")
foreach(_f IN LISTS _old)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${_f}" RESULT_VARIABLE _ignored ERROR_QUIET)
endforeach()

# Try to delete outright first (works when nothing is running it).
execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${EXE}" RESULT_VARIABLE _ignored ERROR_QUIET)
if(NOT EXISTS "${EXE}")
    return()
endif()

string(TIMESTAMP _stamp "%Y%m%d%H%M%S")
file(RENAME "${EXE}" "${_dir}/${_name}.old-${_stamp}.exe" RESULT _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Cannot replace ${EXE} (${_rc}); close Refractory and rebuild.")
endif()
