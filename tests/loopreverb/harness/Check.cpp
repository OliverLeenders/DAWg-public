#include "Check.h"

#include <string_view>

namespace harness
{

namespace
{
int failures = 0;
} // namespace

std::vector<TestCase>& tests()
{
    static std::vector<TestCase> all;
    return all;
}

void fail (const char* file, int line, const std::string& message)
{
    ++failures;
    std::cerr << file << "(" << line << "): FAILED: " << message << "\n";
}

} // namespace harness

int main (int argc, char** argv)
{
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int run = 0;

    for (const auto& test : harness::tests())
    {
        if (! filter.empty() && test.name.find (filter) == std::string::npos)
            continue;

        ++run;
        const auto before = harness::failures;
        test.body();
        std::cout << (harness::failures == before ? "pass  " : "FAIL  ") << test.name << "\n";
    }

    std::cout << run << " tests, " << harness::failures << " failed checks\n";
    return harness::failures == 0 ? 0 : 1;
}
