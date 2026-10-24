#pragma once

// Minimal FD2D test harness (no external framework).
//
//   FD2D_TEST(Suite_Name) { FD2D_CHECK(x == 1); FD2D_REQUIRE(ptr != nullptr); }
//
// FD2D_CHECK records a failure and continues; FD2D_REQUIRE records a failure and
// aborts the current test. TestMain.cpp runs every test whose name starts with
// the optional command-line prefix and returns non-zero on any failure.

#include <functional>
#include <vector>

namespace FD2DTest
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* name { nullptr };
        TestFn fn { nullptr };
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(const char* name, TestFn fn)
        {
            Registry().push_back({ name, fn });
        }
    };

    struct RequireFailed
    {
    };

    void ReportFailure(const char* file, int line, const char* expression);

    // Runs once after all selected tests.
    void RegisterTeardown(std::function<void()> teardown);
}

#define FD2D_TEST(name) \
    static void name(); \
    static const FD2DTest::Registrar name##_registrar(#name, &name); \
    static void name()

#define FD2D_CHECK(expr) \
    do { \
        if (!(expr)) \
        { \
            FD2DTest::ReportFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define FD2D_REQUIRE(expr) \
    do { \
        if (!(expr)) \
        { \
            FD2DTest::ReportFailure(__FILE__, __LINE__, #expr); \
            throw FD2DTest::RequireFailed {}; \
        } \
    } while (0)
