#pragma once

#if defined(_WIN32) && !defined(RBSCAN_STATIC)
#if defined(RBSCAN_BUILDING)
#define RBSCAN_API __declspec(dllexport)
#else
#define RBSCAN_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define RBSCAN_API __attribute__((visibility("default")))
#else
#define RBSCAN_API
#endif

// Exported classes hold their state behind a std::shared_ptr<Impl>. MSVC warns
// that the member has no DLL interface (C4251); that is harmless here because
// callers never touch it, and the C++ API requires the same toolset anyway.
#if defined(_MSC_VER)
#define RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN \
    __pragma(warning(push)) __pragma(warning(disable : 4251 4275))
#define RBSCAN_SUPPRESS_DLL_WARNINGS_END __pragma(warning(pop))
#else
#define RBSCAN_SUPPRESS_DLL_WARNINGS_BEGIN
#define RBSCAN_SUPPRESS_DLL_WARNINGS_END
#endif
