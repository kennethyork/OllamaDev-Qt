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
    Session.cpp Session.h
    Skills.cpp Skills.h
    Memory.cpp Memory.h
    Mcp.cpp Mcp.h
    CodeIndex.cpp CodeIndex.h
    WebSearch.cpp WebSearch.h
    Stt.cpp Stt.h
    Tui.cpp Tui.h
    Lsp.cpp Lsp.h
    Eval.cpp Eval.h
    Watch.cpp Watch.h
    Terminals.cpp Terminals.h
    Hooks.cpp Hooks.h
    GitFlow.cpp GitFlow.h
    Verify.cpp Verify.h
)

target_include_directories(odv-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(odv-core PUBLIC Qt6::Core Qt6::Network Qt6::Concurrent)
if(UNIX AND NOT APPLE)
    target_link_libraries(odv-core PUBLIC util)  # forkpty
endif()
