add_executable(odv-tests smoke.cpp)
target_link_libraries(odv-tests PRIVATE odv-core Qt6::Core Qt6::Network)
add_test(NAME smoke COMMAND odv-tests)
