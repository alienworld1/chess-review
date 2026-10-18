chess_review_add_qt_test(persistence_tests persistence INTEGRATION
    SOURCES DatabaseTest.cpp
    LIBRARIES chess_review_persistence Qt6::Sql)
