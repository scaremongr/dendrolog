# dendrolog_app — окно «Find a Schema» (F-17, шаг 5) живёт в оболочке.
dendrolog_add_test(theme_mode LIBRARIES dendrolog_app TIMEOUT 120)
# Иконки панелей перекрашиваются под тему — тесту нужны настоящие ресурсы.
target_sources(theme_mode PRIVATE ${PROJECT_SOURCE_DIR}/resources/resources.qrc)
