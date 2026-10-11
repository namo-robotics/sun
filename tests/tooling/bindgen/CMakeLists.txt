target_sources(sun_tests PRIVATE test_bindgen.cpp)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_bindgen.cpp"
    TARGET_DIRECTORY sun_tests PROPERTIES
    COMPILE_DEFINITIONS "SUN_BINDGEN_TEST_COMPILER=\"$<TARGET_FILE:sun>\"")
add_dependencies(sun_tests sun)
