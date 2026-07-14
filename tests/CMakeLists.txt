# GitGraph is the one part of the desktop with real logic in it — lane assignment,
# which is either right or obviously wrong — so it is compiled straight into the
# test binary rather than going untested because it happens to live under ade/.
# QtGui is only needed for QColor.
add_executable(odv-tests smoke.cpp ../ade/GitGraph.cpp)
target_link_libraries(odv-tests PRIVATE odv-core Qt6::Core Qt6::Gui Qt6::Network)
# The regression tests read the sources: both bugs they guard are bugs of
# omission, which no happy-path run can catch.
target_compile_definitions(odv-tests PRIVATE ODV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME smoke COMMAND odv-tests)
