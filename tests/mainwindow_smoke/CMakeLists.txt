find_package(Qt6 REQUIRED COMPONENTS Test)
dendrolog_add_test(mainwindow_smoke LIBRARIES dendrolog_app Qt6::Test TIMEOUT 300 COST 55)
# Иконки тулбаров и справка — из ресурсов, как у приложения.
target_sources(mainwindow_smoke PRIVATE ${PROJECT_SOURCE_DIR}/resources/resources.qrc)
# Встроенные блочные схемы 0.6.0 — каталог настроек сценария обновления.
target_compile_definitions(mainwindow_smoke PRIVATE
    DENDROLOG_V3_PATTERNS_DIR="${PROJECT_SOURCE_DIR}/tests/builtin_schemas/v3")
