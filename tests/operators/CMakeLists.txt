target_sources(sun_tests PRIVATE
    test_arithmetic.cpp
    test_class_arithmetic.cpp
    test_bitwise.cpp
    test_char.cpp
    test_compound_assignment.cpp
    test_literals.cpp
    test_logical.cpp
    test_slicing.cpp
    test_unsigned.cpp
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME operators_moon_arithmetic
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_moon_arithmetic.py
            --sun $<TARGET_FILE:sun>)
set_tests_properties(operators_moon_arithmetic PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} TIMEOUT 120)
