qt_add_executable(ollamadev main.cpp)
target_link_libraries(ollamadev PRIVATE odv-core Qt6::Core Qt6::Network)
set_target_properties(ollamadev PROPERTIES OUTPUT_NAME ollamadev)
