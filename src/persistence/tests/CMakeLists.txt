chess_review_add_qt_test(persistence_tests persistence INTEGRATION
    SOURCES DatabaseTest.cpp
    LIBRARIES chess_review_persistence Qt6::Sql)

chess_review_add_qt_test(game_repository_tests game_repository INTEGRATION
    SOURCES GameRepositoryTest.cpp
    LIBRARIES chess_review_startup Qt6::Sql)

chess_review_add_qt_test(engine_repository_tests engine_repository INTEGRATION
    SOURCES EngineRepositoryTest.cpp
    LIBRARIES chess_review_persistence Qt6::Sql)

chess_review_add_qt_test(analysis_repository_tests analysis_repository INTEGRATION
    SOURCES AnalysisRepositoryTest.cpp
    LIBRARIES chess_review_persistence Qt6::Sql)
if(TARGET fake_uci_engine)
    add_dependencies(analysis_repository_tests fake_uci_engine)
    target_compile_definitions(analysis_repository_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")
endif()
