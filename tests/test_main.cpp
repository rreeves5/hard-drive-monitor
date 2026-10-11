#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "util.hpp"

int main(int argc, char** argv) {
    hdmon::set_env("NO_COLOR", "1");
    hdmon::console_init(false);
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
