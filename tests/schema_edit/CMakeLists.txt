find_package(Qt6 REQUIRED COMPONENTS Test)
dendrolog_add_test(schema_edit LIBRARIES dendrolog_app Qt6::Test TIMEOUT 120)
