// Jediný soubor v testovacím binárce, který definuje doctest main().
// Zbytek testovacích souborů jen #include "doctest/doctest.h" a přidává
// TEST_CASE bloky, které se sem slinkují.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
