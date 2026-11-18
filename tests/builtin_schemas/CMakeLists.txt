dendrolog_add_test(builtin_schemas LIBRARIES dendrolog_core TIMEOUT 120)
target_compile_definitions(builtin_schemas PRIVATE
    DENDROLOG_PATTERNS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/patterns")
