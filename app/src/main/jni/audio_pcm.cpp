#include <jni.h>
#include <dlfcn.h>
#include <mutex>

#include <mpv/client.h>

#include "audio_pcm_queue.h"
#include "globals.h"
#include "jni_utils.h"
#include "log.h"

namespace {

audio_pcm::Queue pcm_queue;

void on_audio_pcm(void *, const mpv_audio_pcm_frame *frame) {
    pcm_queue.push(frame);
}

using SetAudioPcm = decltype(&mpv_set_audio_pcm_callback);

SetAudioPcm set_audio_pcm() {
    static auto method = reinterpret_cast<SetAudioPcm>(
        dlsym(RTLD_DEFAULT, "mpv_set_audio_pcm_callback"));
    return method;
}

} // namespace

extern "C" {
    jni_func(jboolean, supportsAudioPcm);
    jni_func(jint, setAudioPcmCapture, jboolean enabled);
    jni_func(jint, readAudioPcm, jobject buffer, jlongArray frame);
}

jni_func(jboolean, supportsAudioPcm) {
    return set_audio_pcm() != nullptr;
}

jni_func(jint, setAudioPcmCapture, jboolean enabled) {
    std::lock_guard<std::mutex> lock(g_mpv_mutex);
    mpv_handle *context = g_mpv;
    if (!context || g_shutdown_requested)
        return MPV_ERROR_UNINITIALIZED;
    SetAudioPcm method = set_audio_pcm();
    if (!method)
        return MPV_ERROR_NOT_IMPLEMENTED;

    if (enabled) {
        pcm_queue.reset();
        pcm_queue.take_dropped_frames();
    }
    int result = method(context, enabled ? on_audio_pcm : nullptr, nullptr);
    if (!enabled) {
        pcm_queue.reset();
        uint32_t dropped = pcm_queue.take_dropped_frames();
        if (dropped != 0)
            ALOGV("mode=audio stage=pcm_capture droppedFrames=%u", dropped);
    }
    return result;
}

jni_func(jint, readAudioPcm, jobject buffer, jlongArray frame) {
    void *bytes = env->GetDirectBufferAddress(buffer);
    jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (!bytes || capacity <= 0 || !frame ||
        env->GetArrayLength(frame) != audio_pcm::kFrameInfoCount)
        return MPV_ERROR_INVALID_PARAMETER;
    std::array<jlong, audio_pcm::kFrameInfoCount> info{};
    jint count = pcm_queue.read(bytes, static_cast<size_t>(capacity), info);
    if (count > 0)
        env->SetLongArrayRegion(frame, 0, info.size(), info.data());
    return count;
}
