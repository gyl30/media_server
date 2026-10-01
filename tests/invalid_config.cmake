execute_process(
    COMMAND "${PROGRAM}" ${ARGUMENTS}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result STREQUAL "1" OR NOT output MATCHES "^usage: media_server" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Invalid config did not fail cleanly: ${result}\n${output}\n${error}")
endif()
