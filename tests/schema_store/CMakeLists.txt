dendrolog_add_test(schema_store LIBRARIES dendrolog_app TIMEOUT 120)
target_compile_definitions(schema_store PRIVATE
    DENDROLOG_PATTERNS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/patterns"
    DENDROLOG_V3_PATTERNS_DIR="${PROJECT_SOURCE_DIR}/tests/builtin_schemas/v3")
