dendrolog_add_test(document_state LIBRARIES dendrolog_widgets TIMEOUT 300 COST 25)
target_compile_definitions(document_state PRIVATE
    DOCUMENT_STATE_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
