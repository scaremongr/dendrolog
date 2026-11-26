dendrolog_add_test(line_template LIBRARIES dendrolog_core TIMEOUT 120)
target_compile_definitions(line_template PRIVATE
    DENDROLOG_PATTERNS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/patterns"
    DENDROLOG_TEMPLATES_SOURCE_DIR="${PROJECT_SOURCE_DIR}/resources/templates")
