#include "concurrent_value_cache.h"
#include <iostream>

using namespace tnt::caching::gemini2; // If you are using a namespace

size_t get_size(int x) { return sizeof(int); }

int main() {
    concurrent_value_cache<int, int> cache([](int k){ return k * 2; }, 1024);
    cache.get(5);
    return 0;
}