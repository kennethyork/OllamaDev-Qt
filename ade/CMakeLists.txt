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
    CodeSearchPane.cpp CodeSearchPane.h
    VoicePane.cpp VoicePane.h
    AgentTeamPane.cpp AgentTeamPane.h
    TasksPane.cpp TasksPane.h
    StartPane.cpp StartPane.h
    CommandPalette.cpp CommandPalette.h
    ManageDialogs.cpp ManageDialogs.h
    ThemeDialog.cpp ThemeDialog.h
)
target_link_libraries(ollamadev-ade PRIVATE odv-core Qt6::Core Qt6::Network Qt6::Widgets)
set_target_properties(ollamadev-ade PROPERTIES WIN32_EXECUTABLE OFF MACOSX_BUNDLE OFF)
