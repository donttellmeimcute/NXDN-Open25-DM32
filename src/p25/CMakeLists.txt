# This target is a portable software library, not a radio firmware image.
add_library(dm32_p25 STATIC p25.c)
target_include_directories(dm32_p25 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(dm32_p25 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
