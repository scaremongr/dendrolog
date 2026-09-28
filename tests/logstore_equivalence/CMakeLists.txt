# Дифференциальный прогон читает диск на каждой конфигурации фильтра —
# даём запас против дефолтных 1500 с CTest на медленной машине.
dendrolog_add_test(logstore_equivalence LIBRARIES dendrolog_core TIMEOUT 900)
