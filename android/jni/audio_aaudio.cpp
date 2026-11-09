// Sound on Android through AAudio: the mixer (xdk/dsound.cpp) is pulled for interleaved stereo floats at 48 kHz.
#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

std::function<void(float*, uint32_t)> g_render;
std::atomic<AAudioStream*> g_stream{nullptr};
std::atomic<bool> g_paused{false};

aaudio_data_callback_result_t OnData(AAudioStream*, void*, void* data, int32_t frames) {
    g_render(static_cast<float*>(data), uint32_t(frames));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// the stream stopped under us (headphones unplugged, a route change): open it again on the new device
void OnError(AAudioStream*, void*, aaudio_result_t error);
bool Open();

void Reopen() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Open();
    }).detach();
}

void OnError(AAudioStream* stream, void*, aaudio_result_t error) {
    LOGE("audio: stream error %s", AAudio_convertResultToText(error));
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        g_stream = nullptr;
        AAudioStream_close(stream);
        Reopen();
    }
}

bool Open() {
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, int32_t(kRate));
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, OnData, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, OnError, nullptr);
    AAudioStream* stream = nullptr;
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOGE("audio: cannot open an output stream: %s", AAudio_convertResultToText(r));
        return false;
    }
    // a callback that asks for twice the burst keeps underruns away without much delay
    AAudioStream_setBufferSizeInFrames(stream, AAudioStream_getFramesPerBurst(stream) * 2);
    r = AAudioStream_requestStart(stream);
    if (r != AAUDIO_OK) {
        LOGE("audio: cannot start the stream: %s", AAudio_convertResultToText(r));
        AAudioStream_close(stream);
        return false;
    }
    g_stream = stream;
    if (g_paused) AAudioStream_requestPause(stream);  // opened while the app is in the background
    LOGI("audio: %d Hz, %d frames per burst", AAudioStream_getSampleRate(stream),
         AAudioStream_getFramesPerBurst(stream));
    return true;
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    g_render = std::move(render);
    if (!Open()) LOGE("audio: no sound");
}

}  // namespace audio

namespace audio {

// the app went to the background (true) or came back: the sound stops and starts with it
void SetPaused(bool paused) {
    g_paused = paused;
    LOGI("audio: %s (stream %p)", paused ? "pause" : "start", (void*)g_stream.load());
    AAudioStream* s = g_stream;
    if (!s) return;
    if (paused) AAudioStream_requestPause(s);
    else AAudioStream_requestStart(s);
}

}  // namespace audio
