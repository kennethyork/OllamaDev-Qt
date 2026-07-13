add_library(odv-core STATIC
    Version.h
    Json.cpp Json.h
    Config.cpp Config.h
    Parallel.cpp Parallel.h
    Backend.cpp Backend.h
    OllamaBackend.cpp OllamaBackend.h
    CliBackend.cpp CliBackend.h
    Models.cpp Models.h
    SecScan.cpp SecScan.h
    Sandbox.cpp Sandbox.h
    Board.cpp Board.h
    Tools.cpp Tools.h
    Agent.cpp Agent.h
    Crew.cpp Crew.h
    Pty.cpp Pty.h
)

target_include_directories(odv-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odv-core PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent)
if(UNIX AND NOT APPLE)
    target_link_libraries(odv-core PUBLIC util)  # forkpty
endif()
