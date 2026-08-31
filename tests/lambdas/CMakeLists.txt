target_sources(sun_tests PRIVATE
    test_bound_methods.cpp
    test_capture_discovery.cpp
    test_params.cpp
    test_ref_captures.cpp
    test_ref_lambda_types.cpp
)

add_subdirectory(generic)
