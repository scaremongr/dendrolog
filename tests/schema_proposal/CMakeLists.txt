dendrolog_add_test(schema_proposal LIBRARIES dendrolog_core TIMEOUT 120)
target_compile_definitions(schema_proposal PRIVATE
    DENDROLOG_PROPOSAL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/request-v1.json")
