dendrolog_add_test(schema_inference LIBRARIES dendrolog_core TIMEOUT 120)
target_compile_definitions(schema_inference PRIVATE
    DENDROLOG_TEMPLATES_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/patterns")
