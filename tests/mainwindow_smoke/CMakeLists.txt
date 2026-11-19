find_package(Qt6 REQUIRED COMPONENTS Test)
dendrolog_add_test(mainwindow_smoke LIBRARIES dendrolog_app Qt6::Test TIMEOUT 300 COST 25)
# Иконки тулбаров и справка — из ресурсов, как у приложения.
target_sources(mainwindow_smoke PRIVATE ${PROJECT_SOURCE_DIR}/resources/resources.qrc)
