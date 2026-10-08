# Runs at build time (not configure time) so the embedded commit hash never goes stale.
# Inputs: SRC (template), OUT (generated .cpp), GIT_DIR (source dir), VERSION.
# configure_file only rewrites OUT when the text changes, so an unchanged commit
# doesn't trigger a recompile.
set(REFRACTORY_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${GIT_DIR}" OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _hash)
        set(REFRACTORY_GIT_HASH "${_hash}")
        # Uncommitted source changes: mark the build so it can't be mistaken for the commit.
        execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${GIT_DIR}" OUTPUT_VARIABLE _dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_dirty)
            string(APPEND REFRACTORY_GIT_HASH "-dirty")
        endif()
    endif()
endif()
set(REFRACTORY_VERSION "${VERSION}")
configure_file("${SRC}" "${OUT}" @ONLY)
