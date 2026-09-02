target_sources(sun_tests PRIVATE
    test_bigint.cpp
    test_byte_order.cpp
    test_convert.cpp
    test_http.cpp
    test_json.cpp
    test_math.cpp
    test_matrix.cpp
    test_networking.cpp
    test_option.cpp
    test_tls.cpp
)

add_subdirectory(collections)
add_subdirectory(concurrency)
add_subdirectory(io)
add_subdirectory(sys)
add_subdirectory(text)
