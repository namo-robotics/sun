target_sources(sun_tests PRIVATE
    test_bound_methods.cpp
    test_capture_discovery.cpp
    test_frame_bound_handles.cpp
    test_params.cpp
    test_lifetime_checking.cpp
    test_lifetime_syntax.cpp
    test_ref_captures.cpp
    test_ref_lambda_types.cpp
    test_sub_frame_scopes.cpp
)

add_subdirectory(generic)
