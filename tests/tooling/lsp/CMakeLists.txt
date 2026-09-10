target_sources(sun_tests PRIVATE
    test_definition.cpp
    test_hover.cpp
    test_references.cpp
    test_rename.cpp
    test_tests.cpp
    test_text_positions.cpp
)

# Exercise discovery through the server, including config files created later.
if(NOT SKIP_STDLIB)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME lsp_discovery_protocol
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_discovery_protocol.py
                $<TARGET_FILE:sun-lsp>
        )
        set_tests_properties(lsp_discovery_protocol PROPERTIES
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            TIMEOUT 180
        )
    endif()
endif()
