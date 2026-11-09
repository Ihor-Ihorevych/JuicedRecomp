// Hooks for default.xbe. A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

// The game's own XACT engine (section XACTENG) reads the audio processor's sample counter straight from its registers,
// 0xFE80200C, which counts 48000 samples a second (it divides by 0xBB80). There is no such hardware: map the page and
// keep the counter running from the host clock.
constexpr uint32_t kApuPage = 0xFE802000, kApuSampleCounter = 0xFE80200C;

void Loaded() {
    LOGI("game: Juiced");
    if (!mem::Commit(kApuPage, 0x1000)) {
        LOGW("game: could not map the audio processor's register page");
        return;
    }
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            kernel::W32(kApuSampleCounter, uint32_t(us * 48 / 1000));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();
}

}  // namespace game

// DirectSound's 3D and I3DL2 setters (thunks in the DSOUND section the signature scan could not name) build a 3D calculator
// on the real DirectSound object, which the host's DirectSound does not have. Positional audio is not reproduced, so they
// succeed without doing anything. Each pops its arguments (this included) as the original does.
#include "cpu/x86_ops.h"

X86_FUNC(sub_00279295) {  // this + 3 arguments, called by the XACT engine for every voice it starts
    (void)base;
    kernel::Return(ctx, 0, 4);
}

// XACT's per-frame DoWork (engine in eax, plain ret): its streaming and 3D voices need DirectSound objects the host does
// not provide yet, so it does nothing and the game runs without XACT sound.
X86_FUNC(sub_0029CF21) {
    (void)base;
    kernel::Return(ctx, 0, 0);
}
