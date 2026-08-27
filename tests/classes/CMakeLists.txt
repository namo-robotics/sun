target_sources(sun_tests PRIVATE
    test_classes.cpp
    test_const_methods.cpp
    test_packed.cpp
    test_struct_literals.cpp
    test_two_phase_init.cpp
)

add_subdirectory(generic)
