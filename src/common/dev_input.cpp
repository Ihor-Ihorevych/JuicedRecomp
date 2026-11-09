// Developer input, with JUICED_DEV set: port 1 stays plugged in, and a file named pad_cmd beside the exe presses buttons
// on it, so the game can be driven without a controller. Each line is "buttons [seconds held]", buttons joined by +,
// held 0.15 s by default; lines run one after another. The file is read and removed, checked ten times a second.
// Buttons: UP DOWN LEFT RIGHT START BACK A B X Y BLACK WHITE LT RT LS LSD LSL LSR (left stick), RS RSD RSL RSR,
// WAIT (nothing held, for a pause between presses).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"

namespace dev {

namespace {

using Clock = std::chrono::steady_clock;

struct Press {
    std::string buttons;
    double seconds;
};

std::mutex g_lock;
std::deque<Press> g_queue;
Clock::time_point g_until{}, g_next_read{};
std::string g_held;

void Apply(const std::string& s, input::Pad& p) {
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find('+', at);
        if (e == std::string::npos) e = s.size();
        std::string b = s.substr(at, e - at);
        at = e + 1;
        if (b == "UP") p.buttons |= 1;
        else if (b == "DOWN") p.buttons |= 2;
        else if (b == "LEFT") p.buttons |= 4;
        else if (b == "RIGHT") p.buttons |= 8;
        else if (b == "START") p.buttons |= 16;
        else if (b == "BACK") p.buttons |= 32;
        else if (b == "A") p.analog[0] = 255;
        else if (b == "B") p.analog[1] = 255;
        else if (b == "X") p.analog[2] = 255;
        else if (b == "Y") p.analog[3] = 255;
        else if (b == "BLACK") p.analog[4] = 255;
        else if (b == "WHITE") p.analog[5] = 255;
        else if (b == "LT") p.analog[6] = 255;
        else if (b == "RT") p.analog[7] = 255;
        else if (b == "LS") p.ly = 32767;
        else if (b == "LSD") p.ly = -32767;
        else if (b == "LSL") p.lx = -32767;
        else if (b == "LSR") p.lx = 32767;
        else if (b == "RS") p.ry = 32767;
        else if (b == "RSD") p.ry = -32767;
        else if (b == "RSL") p.rx = -32767;
        else if (b == "RSR") p.rx = 32767;
    }
}

void ReadCommands() {
    static const std::string path = (app::ExeDir() / "pad_cmd").string();
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return;
    char line[160];
    while (std::fgets(line, sizeof line, f)) {
        char b[96] = {};
        double seconds = 0.15;
        if (line[0] != '#' && std::sscanf(line, "%95s %lf", b, &seconds) >= 1) g_queue.push_back({b, seconds});
    }
    std::fclose(f);
    std::remove(path.c_str());
}

}  // namespace

void Pad(int port, input::Pad& p) {
    static const bool on = std::getenv("JUICED_DEV") != nullptr;
    if (!on || port != 0) return;
    p.connected = true;
    std::lock_guard lock(g_lock);
    auto now = Clock::now();
    if (now >= g_next_read) {
        g_next_read = now + std::chrono::milliseconds(100);
        ReadCommands();
    }
    if (now >= g_until) {
        // a press is let go for a tenth of a second before the next, so two of the same button count twice
        if (!g_held.empty()) {
            g_held.clear();
            g_until = now + std::chrono::milliseconds(100);
        } else if (!g_queue.empty()) {
            Press next = g_queue.front();
            g_queue.pop_front();
            g_held = next.buttons;
            g_until = now + std::chrono::microseconds(int64_t(next.seconds * 1e6));
            LOGI("dev: %s for %.2f s", next.buttons.c_str(), next.seconds);
        }
    }
    if (!g_held.empty()) Apply(g_held, p);
}

}  // namespace dev
