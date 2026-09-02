function(add_si_test NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE
        SegmentedInterprocess::segmented_interprocess
    )
    add_test(NAME ${NAME} COMMAND ${NAME})
    set_tests_properties(${NAME} PROPERTIES TIMEOUT 60)
endfunction()

add_si_test(test_ncrit_trie)
add_si_test(test_segmented_offset_ptr)
add_si_test(test_segmented_managed_memory)
add_si_test(test_multithreaded)
add_si_test(test_shm_multithreaded)

add_si_test(test_persistence)
