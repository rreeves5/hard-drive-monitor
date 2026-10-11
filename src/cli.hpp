// Hard Drive Monitor command line (`hdmon`).
#pragma once
#include <ostream>
#include <string>
#include <vector>

namespace hdmon {

extern const char* VERSION;
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace hdmon
