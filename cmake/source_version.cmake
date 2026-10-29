cmake_minimum_required(VERSION 3.21)
execute_process(COMMAND git -C ${SOURCE_DIR} describe --always --dirty --abbrev=12
    OUTPUT_VARIABLE source_version OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(CONFIGURE OUTPUT ${OUTPUT} CONTENT "#define MEDIA_SERVER_SOURCE_VERSION \"@source_version@\"\n" @ONLY)
