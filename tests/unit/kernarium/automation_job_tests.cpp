// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumAutomationJob.hpp"
#include "require.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using kernarium::automation::JobAtStart;
using kernarium::automation::parseJobAtStart;
using uml::testing::require;

/* The launch asks for the job with 1 and for nothing with 0 or by leaving the
 * variable out. */
void test_the_driver_asks_with_one()
{
    require(parseJobAtStart("1") == JobAtStart::on, "1 asks for the job");
    require(parseJobAtStart("0") == JobAtStart::off, "0 asks for none");
    require(parseJobAtStart(nullptr) == JobAtStart::off,
        "an unset variable asks for none");
}

/* Anything else is the driver's mistake. It is reported and acts as no
 * request, so a misspelt value cannot start a job a test did not mean. */
void test_anything_else_is_a_mistake()
{
    for (const char* value :
        {"", "2", "01", "10", " 1", "1 ", "true", "yes", "on", "-1"})
        require(parseJobAtStart(value) == JobAtStart::invalid, value);
}

/* The name a driver sets is part of the contract. */
void test_the_name_is_the_documented_one()
{
    require(std::strcmp(kernarium::automation::jobAtStartVariable,
                "KERNARIUM_BACKGROUND_JOB_AT_START") == 0,
        "the variable's name");
}

} // namespace

int main()
{
    test_the_driver_asks_with_one();
    test_anything_else_is_a_mistake();
    test_the_name_is_the_documented_one();
    std::puts("automation job tests passed");
    return EXIT_SUCCESS;
}
