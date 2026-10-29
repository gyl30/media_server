set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third/spdlog EXCLUDE_FROM_ALL)
target_include_directories(spdlog_header_only SYSTEM INTERFACE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third/spdlog/include>)
