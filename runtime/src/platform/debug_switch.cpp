// The game's debug server commands (debug_switch.h). The client is tools/switch/wwhd_debug.py; docs/debug-server.md.
#include "debug_switch.h"

#include <switch.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../build_info.h"
#include "../gfx/switch_renderer.h"
#include "../mods/mods.h"
#include "../mods/warps.h"
#include "../runtime.h"
#include "../overlay/hostui.h"
#include "debug_server.h"
#include "host.h"
#include "settings_switch.h"

namespace render { uint64_t frame_count(); }

namespace gfxdk { std::string seen_programs(); }  // gfx/deko/shaders_dk.cpp

namespace debug_switch {
namespace {
std::atomic<bool> g_quit{false};
int g_port = 0;
bool g_running = false;
constexpr int kPort = 6543;

std::string ip_text() {
    in_addr a;
    a.s_addr = uint32_t(gethostid());  // libnx: the console's address on the network (nifm), network order
    return inet_ntoa(a);
}

bool game_running() { return render::frame_count() > 0; }  // guest memory and the game's data are there

debugsrv::Reply info(const debugsrv::Args&) {
    char b[512];
    AppletType type = appletGetAppletType();
    snprintf(b, sizeof b,
             "version %s (%s)\nframe %llu\nstage %s\nheap never used %zu MiB\nrunning as %s\naddress %s:%d\n",
             build::version(), build::commit(), (unsigned long long)render::frame_count(),
             game_running() ? mods::current_stage().c_str() : "-", heap_never_used_mib(),
             type == AppletType_Application ? "application (the HOME menu forwarder: reload works)"
                                            : "applet (hbmenu: reload does not work, use quit and nxlink)",
             ip_text().c_str(), g_port);
    return debugsrv::ok(b);
}

debugsrv::Reply warps(const debugsrv::Args&) {
    std::string out;
    int i = 0;
    char b[160];
    auto add = [&](const mods::Warp& w) {
        snprintf(b, sizeof b, "%3d  %-8s room %2d point %3d  %s\n", i++, w.stage, w.room, w.point, w.label ? w.label : "");
        out += b;
    };
    for (const mods::Warp& w : mods::kMainWarps) add(w);
    for (const mods::Warp& w : mods::kAllWarps) add(w);
    return debugsrv::ok(out);
}

debugsrv::Reply warp(const debugsrv::Args& a) {
    if (a.empty()) return debugsrv::err("usage: warp <number from warps> | warp <stage> [room] [point]");
    if (!game_running()) return debugsrv::err("the game has not started yet");
    const int mainCount = int(sizeof mods::kMainWarps / sizeof mods::kMainWarps[0]);
    const int allCount = int(sizeof mods::kAllWarps / sizeof mods::kAllWarps[0]);
    char* end;
    const long n = strtol(a[0].c_str(), &end, 10);
    std::string stage;
    int room = 0, point = 0;
    if (*end == 0 && a.size() == 1) {
        if (n < 0 || n >= mainCount + allCount) return debugsrv::err("no warp number " + a[0] + " (warps lists them)");
        const mods::Warp& w = n < mainCount ? mods::kMainWarps[n] : mods::kAllWarps[n - mainCount];
        stage = w.stage, room = w.room, point = w.point;
    } else {
        stage = a[0];
        if (stage.size() > 7) return debugsrv::err("a stage name has 7 characters at most");
        if (a.size() > 1) room = atoi(a[1].c_str());
        if (a.size() > 2) point = atoi(a[2].c_str());
    }
    LOG("[debug] warp to %s room %d point %d requested", stage.c_str(), room, point);
    mods::request_warp(stage.c_str(), room, point);
    return debugsrv::ok("warp to " + stage + " room " + std::to_string(room) + " point " + std::to_string(point) +
                        " requested (it happens once a file is loaded and no other scene change is pending)");
}

void remove_dir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) remove((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    rmdir(dir.c_str());
}

// "shot [game]": the next frame's pictures through the renderer's capture (gfx/deko/dk_capture.h), sent and deleted
debugsrv::Reply shot(const debugsrv::Args& a) {
    if (!game_running()) return debugsrv::err("the renderer has not presented a frame yet");
    const bool game = !a.empty() && a[0] == "game";
    const uint64_t before = gfxsw::pictures_done_frame();
    const uint64_t first = gfxsw::request_pictures();
    uint64_t done = 0;
    for (int i = 0; i < 400; i++) {  // 20 s: the PNG writer is a background thread
        done = gfxsw::pictures_done_frame();
        if (done != before && done >= first) break;
        done = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!done) return debugsrv::err("no capture within 20 s (is the game presenting frames?)");
    const std::string n = std::to_string(done);
    const std::string dir = host::config_dir() + "/captures/" + n;
    debugsrv::Reply r;
    r.file = dir + "/frame_" + n + (game ? ".png" : "_window.png");
    r.after = [dir] { remove_dir(dir); };
    return r;
}

debugsrv::Reply reload(const debugsrv::Args&) {
    if (appletGetAppletType() != AppletType_Application)
        return debugsrv::err("reload needs the HOME menu forwarder (running from hbmenu: quit, then nxlink or hbmenu)");
    debugsrv::Reply r = debugsrv::ok("restarting: the forwarder loads the NRO from the SD card again");
    r.after = [] {
        LOG("[debug] reload: the program restarts (appletRestartProgram)");
        log_flush();
        debugsrv::stop();
        static const char arg[] = "wwhd debug reload";
        const Result rc = appletRestartProgram(arg, sizeof arg);
        // (on success the system ends this process)
        LOG("[debug] reload failed: appletRestartProgram rc 0x%x", rc);
    };
    return r;
}

// game memory as hex, 16 bytes a line (big-endian words as the game sees them): MEM2 (data, heap) only
debugsrv::Reply peek(const debugsrv::Args& a) {
    if (a.empty()) return debugsrv::err("peek <address> [bytes]");
    const uint32_t addr = uint32_t(strtoul(a[0].c_str(), nullptr, 16)) & ~3u;
    const uint32_t len = std::min<uint32_t>(a.size() > 1 ? uint32_t(strtoul(a[1].c_str(), nullptr, 0)) : 64u, 4096u);
    if (addr < mem::kMem2Start || addr + len > mem::kMem2End || addr + len < addr)
        return debugsrv::err("peek: outside MEM2 (10000000-4FFFFFFF)");
    std::string out;
    char b[16];
    for (uint32_t o = 0; o < len; o += 4) {
        if (o % 16 == 0) {
            snprintf(b, sizeof b, "%s%08X:", o ? "\n" : "", addr + o);
            out += b;
        }
        snprintf(b, sizeof b, " %08X", ld32(addr + o));
        out += b;
    }
    return debugsrv::ok(out + "\n");
}

// a big-endian word into game memory (MEM2), for experiments on the game's data
debugsrv::Reply poke(const debugsrv::Args& a) {
    if (a.size() < 2) return debugsrv::err("poke <address> <word>");
    const uint32_t addr = uint32_t(strtoul(a[0].c_str(), nullptr, 16)) & ~3u;
    const uint32_t v = uint32_t(strtoul(a[1].c_str(), nullptr, 0));
    if (addr < mem::kMem2Start || addr + 4 > mem::kMem2End) return debugsrv::err("poke: outside MEM2 (10000000-4FFFFFFF)");
    const uint32_t old = ld32(addr);
    st32(addr, v);
    LOG("[debug] poke %08X: %08X -> %08X", addr, old, v);
    char b[64];
    snprintf(b, sizeof b, "%08X: %08X -> %08X\n", addr, old, v);
    return debugsrv::ok(b);
}

// the shader programs the game has drawn with (gfx/deko/shaders_dk.cpp): for precompiling from the game's files
debugsrv::Reply programs(const debugsrv::Args&) { return debugsrv::ok(gfxdk::seen_programs()); }

debugsrv::Reply quit(const debugsrv::Args&) {
    debugsrv::Reply r = debugsrv::ok("quitting");
    r.after = [] {
        LOG("[debug] quit requested");
        debugsrv::stop();
        g_quit = true;
    };
    return r;
}

}  // namespace

void start() {
    std::string on;
    if (!hostui::get(switch_settings::kKeyDebugServer, on) || on != "1") return;
    g_port = kPort;
    debugsrv::add_command("info", "info                   build, frame, stage, heap, address", info);
    debugsrv::add_command("warps", "warps                  the warp destinations, numbered", warps);
    debugsrv::add_command("warp", "warp <n> | warp <stage> [room] [point]", warp);
    debugsrv::add_command("shot", "shot [game]            PNG of the next frame (game: the game's picture alone)", shot);
    debugsrv::add_command("reload", "reload                 restart: the forwarder loads the NRO again (put it first)", reload);
    debugsrv::add_command("quit", "quit                   end the program", quit);
    debugsrv::add_command("peek", "peek <addr> [bytes]    game memory (MEM2) as hex words", peek);
    debugsrv::add_command("programs", "programs               shader programs drawn: kind hash size first bytes", programs);
    debugsrv::add_command("poke", "poke <addr> <word>     write a word of game memory (MEM2)", poke);
    debugsrv::Config c;
    c.port = g_port;
    c.root = host::config_dir();
    c.log = [](const char* line) { LOG("%s", line); };
    c.threadStart = [] { host::raise_thread_priority(); };
    g_running = debugsrv::start(c);
    if (g_running)
        LOG("[debug] server listening on %s:%d (Switch tab > Debug, settings.ini %s=1; client: "
            "tools/switch/wwhd_debug.py; local network only, no password)", ip_text().c_str(), g_port,
            switch_settings::kKeyDebugServer);
}

bool quit_requested() { return g_quit.load(std::memory_order_relaxed); }
bool running() { return g_running; }
int port() { return g_port; }

}  // namespace debug_switch
