#pragma once
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "cli.hpp"
#include "util.hpp"

namespace th {
namespace fs = std::filesystem;
inline std::string root() { return HDMON_SOURCE_DIR; }
inline std::string fix(const std::string& name = "") {
    std::string f = (fs::path(root()) / "tests" / "fixtures").string();
    return name.empty() ? f : (fs::path(f) / fs::u8path(name)).string();
}
inline std::string expected(const std::string& name) {
    return hdmon::read_file((fs::path(root()) / "tests" / "expected" / name).string());
}
inline hdmon::json load(const std::string& name) { return hdmon::json::parse(hdmon::read_file(fix(name))); }
inline nlohmann::json plain(const hdmon::json& j) { return nlohmann::json::parse(j.dump()); }   // order-insensitive compare

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("hdmon-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    std::string file(const std::string& n) const { return (path / n).string(); }
};

struct Run { int rc; std::string out, err; };
inline Run run(std::vector<std::string> args) {
    std::ostringstream o, e;
    int rc = hdmon::run_cli(args, o, e);
    return {rc, o.str(), e.str()};
}
inline bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
}  // namespace th
