dendrolog_add_test(field_schema LIBRARIES dendrolog_widgets TIMEOUT 240 COST 10)
# Встроенные схемы (:/patterns) — ресурсы exe, тесту их нужно добавить самому.
target_sources(field_schema PRIVATE ${PROJECT_SOURCE_DIR}/resources/resources.qrc)
