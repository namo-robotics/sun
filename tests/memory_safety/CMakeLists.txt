target_sources(sun_tests PRIVATE
    test_allocator.cpp
    test_borrow_checker.cpp
    test_global_assignment.cpp
    test_linked_list.cpp
    test_memory_safety.cpp
    test_pointers.cpp
)

add_subdirectory(drops)
add_subdirectory(refs)
