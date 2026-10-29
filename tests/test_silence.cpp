// "Silence until next reboot" (0.2.11): boot-time marker, tooltip mark, desktop alerts skipped.
#include "helpers.hpp"
#include "alerts.hpp"
#include "gui_model.hpp"
#include "silence.hpp"

using namespace hdmon;
namespace g = hdmon::gui;

TEST_SUITE("silence") {
TEST_CASE("marker holds for this boot and clears after a reboot") {
    th::TempDir t;
    std::string m = t.file("sub/silence-until-reboot");
    const int64_t boot = 1'800'000'000;
    CHECK_FALSE(silenced_at(m, boot));
    REQUIRE(set_silenced_at(m, true, boot));
    CHECK(silenced_at(m, boot));                       // app restart, same boot
    CHECK(silenced_at(m, boot + 5));                   // uptime-derived boot time drifts a little
    CHECK(silenced_at(m, boot - kSameBootToleranceSeconds));
    CHECK_FALSE(silenced_at(m, boot + 3600));          // rebooted an hour later
    CHECK_FALSE(silenced_at(m, 0));                    // boot time unknown: never silence
    REQUIRE(set_silenced_at(m, false, boot));
    CHECK_FALSE(th::fs::exists(m));
    CHECK_FALSE(silenced_at(m, boot));
    CHECK(set_silenced_at(m, false, boot));            // already off
    CHECK_FALSE(set_silenced_at(m, true, 0));
}

TEST_CASE("garbage marker is not silence") {
    th::TempDir t;
    std::string m = t.file("silence-until-reboot");
    write_file(m, "yes please\n");
    CHECK_FALSE(silenced_at(m, 1'800'000'000));
    write_file(m, "");
    CHECK_FALSE(silenced_at(m, 1'800'000'000));
}

TEST_CASE("real boot time is stable and in the past") {
    int64_t a = boot_time_epoch(), b = boot_time_epoch();
    CHECK(a > 0);
    CHECK(a <= static_cast<int64_t>(std::time(nullptr)));
    CHECK(same_boot(a, b));
    CHECK(silence_marker_path().find("silence-until-reboot") != std::string::npos);
}

TEST_CASE("tooltip adds (silenced) to the first line and keeps the Windows limit") {
    CHECK(g::tooltip_silenced("Hard Drive Monitor Beta V 0.2.11 - all OK") == "Hard Drive Monitor Beta V 0.2.11 - all OK (silenced)");
    CHECK(g::tooltip_silenced("Hard Drive Monitor\nC: OK") == "Hard Drive Monitor (silenced)\nC: OK");
    std::string long_tip = "Hard Drive Monitor Beta V 0.2.11\n" + std::string(200, 'x');
    std::string t = g::tooltip_silenced(long_tip, 127);
    CHECK(t.size() <= 127);
    CHECK(th::has(t, "(silenced)"));
}

TEST_CASE("dispatch skips the desktop pop-up while silenced, still reports it") {
    th::TempDir t;
    std::string old = get_env("XDG_DATA_HOME"), old_la = get_env("LOCALAPPDATA"), old_home = get_env("HOME");
    set_env("HOME", t.path.string());   // macOS data_dir is under $HOME
    set_env("XDG_DATA_HOME", t.path.string());
    set_env("LOCALAPPDATA", t.path.string());
    REQUIRE(set_silenced(true));
    CHECK(silenced());
    json cfg = json::parse(R"({"alerts":{"desktop":true}})");
    auto log = dispatch(cfg, {Alert{"CRITICAL", "Hard Drive Monitor", "C: NX-2TB, Critical, 40%"}});
    bool said = false;
    for (auto& l : log) said = said || th::has(l, "silenced until next reboot");
    CHECK(said);
    REQUIRE(set_silenced(false));
    CHECK_FALSE(silenced());
    set_env("XDG_DATA_HOME", old);
    set_env("LOCALAPPDATA", old_la);
    set_env("HOME", old_home);
}
}
