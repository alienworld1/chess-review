chess_review_add_qt_test(persistence_tests persistence INTEGRATION
    SOURCES DatabaseTest.cpp
    LIBRARIES chess_review_persistence Qt6::Sql)

chess_review_add_qt_test(game_repository_tests game_repository INTEGRATION
    SOURCES GameRepositoryTest.cpp
    LIBRARIES chess_review_startup Qt6::Sql)
