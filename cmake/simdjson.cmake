set(SIMDJSON_EXCEPTIONS OFF CACHE BOOL "" FORCE)
add_subdirectory(third/simdjson EXCLUDE_FROM_ALL)
get_target_property(simdjson_include_directories simdjson INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(simdjson PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${simdjson_include_directories}")
