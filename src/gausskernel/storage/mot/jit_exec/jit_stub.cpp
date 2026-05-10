/*
 * Stub to resolve missing std::__throw_bad_array_new_length()
 * when linking against binarylibs' GCC 10.3 libstdc++ with GCC 12.3 headers.
 */
#include <new>

namespace std {
void __throw_bad_array_new_length() __attribute__((weak));
}

void std::__throw_bad_array_new_length()
{
    throw std::bad_array_new_length();
}
