#pragma once

#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// A minimal test runner, so the workspace needs no downloads. Write tests as
//
//     TEST ("the gain is applied") { CHECK (...); CHECK_NEAR (a, b, tolerance); }
//
// and run the tests executable (optionally with a substring of the test names to run).

namespace harness
{

/**
    A registered test.
*/
struct TestCase
{
    /**
        Its name.
    */
    std::string name;

    /**
        Its body.
    */
    std::function<void()> body;
};

/**
    Returns every registered test.
*/
std::vector<TestCase>& tests();

/**
    Records a failed check.

    @param file     Source file.
    @param line     Source line.
    @param message  What failed.
*/
void fail (const char* file, int line, const std::string& message);

/**
    Registers a test at static initialisation.
*/
struct Registrar
{
    /**
        Registers the test.

        @param name  Its name.
        @param body  Its body.
    */
    Registrar (std::string name, std::function<void()> body)
    {
        tests().push_back ({ std::move (name), std::move (body) });
    }
};

} // namespace harness

#define HARNESS_JOIN2(a, b) a##b
#define HARNESS_JOIN(a, b) HARNESS_JOIN2 (a, b)
#define HARNESS_TEST_IMPL(name, id)                                                                                    \
    static void id();                                                                                                  \
    static const harness::Registrar HARNESS_JOIN (id, _registrar) { name, &id };                                       \
    static void id()
#define TEST(name) HARNESS_TEST_IMPL (name, HARNESS_JOIN (harness_test_, __LINE__))
#define CHECK(expression)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (! (expression))                                                                                            \
            harness::fail (__FILE__, __LINE__, #expression);                                                           \
    }                                                                                                                  \
    while (false)
#define CHECK_NEAR(actual, expected, tolerance)                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        const double harness_a = (actual), harness_e = (expected);                                                     \
        if (! (std::abs (harness_a - harness_e) <= (tolerance)))                                                       \
            harness::fail (__FILE__, __LINE__,                                                                         \
                           std::string (#actual) + " = " + std::to_string (harness_a) + ", expected "                  \
                               + std::to_string (harness_e) + " +- " + std::to_string (tolerance));                    \
    }                                                                                                                  \
    while (false)
