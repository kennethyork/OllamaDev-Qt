qt_add_executable(ollamadev-ade
    main.cpp
    MainWindow.cpp MainWindow.h
    Canvas.cpp Canvas.h
    Pane.cpp Pane.h
    TerminalWidget.cpp TerminalWidget.h
    VtParser.cpp VtParser.h
    BoardPane.cpp BoardPane.h
    EditorPane.cpp EditorPane.h
    FilesPane.cpp FilesPane.h
    Theme.cpp Theme.h
    PaneHost.h
    PaneRegistry.cpp PaneRegistry.h
    Panes.cpp
    BrowserPane.cpp BrowserPane.h
    GraphPane.cpp GraphPane.h
    TopologyPane.cpp TopologyPane.h
    CoderPane.cpp CoderPane.h
    ChatPane.cpp ChatPane.h
    CodeSearchPane.cpp CodeSearchPane.h
    VoicePane.cpp VoicePane.h
    AgentTeamPane.cpp AgentTeamPane.h
    TasksPane.cpp TasksPane.h
    StartPane.cpp StartPane.h
    CommandPalette.cpp CommandPalette.h
    ManageDialogs.cpp ManageDialogs.h
    ThemeDialog.cpp ThemeDialog.h
    BrainPane.cpp BrainPane.h
)
target_link_libraries(ollamadev-ade PRIVATE odv-core Qt6::Core Qt6::Network Qt6::Widgets)
set_target_properties(ollamadev-ade PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)

# A real browser pane (QWebEngineView) when QtWebEngine is available; the Browser
# pane falls back to the QTextBrowser reader otherwise, so a machine without the
# (large, Chromium-based) WebEngine module still builds and runs. On by default;
# turn off with -DODV_WEBENGINE=OFF for a lean build.
option(ODV_WEBENGINE "Use QtWebEngine for the Browser pane" ON)
# When a release build MUST have the real browser, this makes a missing WebEngine
# a hard error rather than a silent reader fallback — so a reader-only artifact can
# never ship by accident.
option(ODV_WEBENGINE_REQUIRED "Fail the build if QtWebEngine is not found" OFF)
if(ODV_WEBENGINE)
    if(ODV_WEBENGINE_REQUIRED)
        find_package(Qt6 REQUIRED COMPONENTS WebEngineWidgets)
    else()
        find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)
    endif()
    if(Qt6WebEngineWidgets_FOUND)
        target_link_libraries(ollamadev-ade PRIVATE Qt6::WebEngineWidgets)
        target_compile_definitions(ollamadev-ade PRIVATE ODV_HAS_WEBENGINE=1)
        message(STATUS "Browser pane: QtWebEngine (full browser)")
    else()
        message(STATUS "Browser pane: QTextBrowser reader (QtWebEngine not found — "
                       "install qt6-webengine-dev for the full browser)")
    endif()
endif()
