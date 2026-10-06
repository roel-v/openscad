# Feature-only build identity. The application/settings identity remains OpenSCAD.
option(SCADMAXX_FEATURE_TITLE "Show the source branch in GUI window titles" ON)
set(SCADMAXX_TITLE_LABEL "")
if(SCADMAXX_FEATURE_TITLE)
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${CMAKE_SOURCE_DIR}"
    -C "${CMAKE_SOURCE_DIR}" rev-parse --absolute-git-dir
    OUTPUT_VARIABLE title_git_dir OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE title_git_result)
  if(NOT title_git_result EQUAL 0)
    message(FATAL_ERROR "Feature titles require a Git checkout; disable SCADMAXX_FEATURE_TITLE for archive builds.")
  endif()
  # Branch switches, including linked worktrees, must regenerate the embedded identity.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${title_git_dir}/HEAD")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${CMAKE_SOURCE_DIR}"
    -C "${CMAKE_SOURCE_DIR}" symbolic-ref --quiet --short HEAD
    OUTPUT_VARIABLE SCADMAXX_TITLE_LABEL OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE title_branch_result)
  if(title_branch_result EQUAL 1)
    get_filename_component(title_worktree "${CMAKE_SOURCE_DIR}" NAME)
    set(SCADMAXX_TITLE_LABEL "${title_worktree} (detached)")
  elseif(NOT title_branch_result EQUAL 0)
    message(FATAL_ERROR "Cannot determine feature branch.")
  endif()
endif()
# Escape a UTF-8 C++ string, including unusual but legal branch-name characters.
string(REPLACE "\\" "\\\\" SCADMAXX_TITLE_LABEL "${SCADMAXX_TITLE_LABEL}")
string(REPLACE "\"" "\\\"" SCADMAXX_TITLE_LABEL "${SCADMAXX_TITLE_LABEL}")
string(REPLACE "\n" "\\n" SCADMAXX_TITLE_LABEL "${SCADMAXX_TITLE_LABEL}")
string(REPLACE "\r" "\\r" SCADMAXX_TITLE_LABEL "${SCADMAXX_TITLE_LABEL}")
configure_file("${CMAKE_SOURCE_DIR}/src/feature-build-title.h.in"
  "${CMAKE_BINARY_DIR}/feature-build-title.h" @ONLY)
