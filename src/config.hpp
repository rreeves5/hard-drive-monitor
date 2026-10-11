#pragma once
#include <string>

#include "util.hpp"

namespace hdmon {

// File locations
//   Linux:   history ~/.local/share/hard-drive-monitor/history.db         config ~/.config/hard-drive-monitor/config.toml
//   macOS:   history ~/Library/Application Support/hard-drive-monitor/    config ~/.config/hard-drive-monitor/config.toml
//   Windows: history %LOCALAPPDATA%\hard-drive-monitor\history.db         config %APPDATA%\hard-drive-monitor\config.toml
std::string home_dir();
std::string data_dir();
std::string default_db();
std::string config_path();

extern const char* SAMPLE_CONFIG;
json default_config();
json parse_simple_toml(const std::string& text);   // [section] + key = string/int/float/bool/array
json merge_config(const json& base, const json& over);
json load_config(const std::string& path = "");     // defaults merged with the file (if present)
// Writes SAMPLE_CONFIG; returns the path, or "" if it exists and !force.
std::string write_sample_config(const std::string& path, bool force, std::string* err);

}  // namespace hdmon
