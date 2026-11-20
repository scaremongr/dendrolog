find_package(Qt6 REQUIRED COMPONENTS Test)
dendrolog_add_test(row_chrome LIBRARIES dendrolog_widgets Qt6::Test TIMEOUT 300 COST 15)
