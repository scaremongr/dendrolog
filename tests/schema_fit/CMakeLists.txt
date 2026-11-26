dendrolog_add_test(schema_fit LIBRARIES dendrolog_widgets TIMEOUT 120)
target_compile_definitions(schema_fit PRIVATE
    DENDROLOG_PATTERNS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/patterns")
