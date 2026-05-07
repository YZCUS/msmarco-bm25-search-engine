#pragma once

// Lightweight logging that compiles to no-op when IDX_DEBUG is not defined.
// Use IDX_LOG for progress traces; never use it for user-visible output.

#include <iostream>

#ifdef IDX_DEBUG
#define IDX_LOG(x)                       \
    do {                                 \
        std::cerr << "[idx] " << x << '\n'; \
    } while (0)
#else
#define IDX_LOG(x) \
    do {           \
    } while (0)
#endif
