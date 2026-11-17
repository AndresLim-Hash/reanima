#pragma once

// Signalsmith Stretch 1.4.0 (MIT, external/signalsmith-stretch-1.4.0) with Signalsmith Linear 0.6.4
// (MIT, external/signalsmith-linear-0.6.4): pitch and time without one depending on the other.
// Their headers are included here only, with the compiler's warnings for third-party code off.

#if defined (_MSC_VER)
 #pragma warning (push, 0)
 #pragma warning (disable: 4244 4267 4456 4457 4458 4459 4100 4127 4189 4305 4996)
#elif defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wall"
 #pragma GCC diagnostic ignored "-Wextra"
#endif

#include "signalsmith-stretch.h"

#if defined (_MSC_VER)
 #pragma warning (pop)
#elif defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif
