#pragma once

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>

#include <mpv/client.h>

namespace audio_pcm {

constexpr size_t kMaxFrameBytes = 64 * 1024;
constexpr size_t kFrameCount = 32;
constexpr size_t kFrameInfoCount = 8;

class Queue {
public:
    void reset() {
        generation_.fetch_add(1, std::memory_order_release);
    }

    uint32_t take_dropped_frames() {
        return dropped_frames_.exchange(0, std::memory_order_relaxed);
    }

    void push(const mpv_audio_pcm_frame *frame) {
        if (!frame) {
            reset();
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            record_drop();
            return;
        }

        auto generation = generation_.load(std::memory_order_acquire);
        if (generation != written_generation_) {
            begin_ = count_ = 0;
            written_generation_ = generation;
        }
        const size_t frame_bytes = get_frame_bytes(*frame);
        if (!frame_bytes) {
            record_drop();
            return;
        }
        const size_t samples = frame->sample_count;
        const size_t slot_samples = kMaxFrameBytes / frame_bytes;
        const size_t chunks = (samples - 1) / slot_samples + 1;
        const size_t skipped = chunks > slots_.size() ? chunks - slots_.size() : 0;
        if (skipped)
            record_drop(static_cast<uint32_t>(skipped));
        for (size_t offset = skipped * slot_samples; offset < samples;) {
            const size_t count = std::min(slot_samples, samples - offset);
            Slot &slot = slots_[(begin_ + count_) % slots_.size()];
            copy_chunk(slot, *frame, frame_bytes, offset, count, generation);
            if (count_ == slots_.size()) {
                begin_ = (begin_ + 1) % slots_.size();
                record_drop();
            } else {
                ++count_;
            }
            offset += count;
        }
    }

    int read(void *buffer, size_t capacity,
              std::array<int64_t, kFrameInfoCount> &info) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto generation = generation_.load(std::memory_order_acquire);
        if (!count_ || generation != written_generation_)
            return 0;
        const Slot &slot = slots_[begin_];
        if (capacity < slot.size)
            return MPV_ERROR_INVALID_PARAMETER;
        memcpy(buffer, slot.bytes.data(), slot.size);
        info = slot.info;
        begin_ = (begin_ + 1) % slots_.size();
        --count_;
        return static_cast<int>(slot.size);
    }

private:
    struct Slot {
        std::array<uint8_t, kMaxFrameBytes> bytes;
        std::array<int64_t, kFrameInfoCount> info;
        size_t size;
    };
    void record_drop(uint32_t count = 1) {
        dropped_frames_.fetch_add(count, std::memory_order_relaxed);
    }

    static size_t sample_size(mpv_audio_pcm_format format) {
        switch (format) {
        case MPV_AUDIO_PCM_U8: return 1;
        case MPV_AUDIO_PCM_S16: return 2;
        case MPV_AUDIO_PCM_S32:
        case MPV_AUDIO_PCM_F32: return 4;
        case MPV_AUDIO_PCM_S64:
        case MPV_AUDIO_PCM_F64: return 8;
        }
        return 0;
    }

    static size_t get_frame_bytes(const mpv_audio_pcm_frame &frame) {
        const size_t sample_bytes = sample_size(frame.format);
        if (!sample_bytes || frame.channel_count <= 0 ||
            frame.channel_count > 64 || frame.sample_count <= 0 ||
            frame.sample_rate <= 0 || frame.end_us < frame.start_us ||
            !frame.planes ||
            (frame.plane_count != 1 && frame.plane_count != frame.channel_count))
            return 0;
        const size_t frame_bytes = frame.channel_count * sample_bytes;
        if (static_cast<size_t>(frame.sample_count) > SIZE_MAX / frame_bytes)
            return 0;
        for (int plane = 0; plane < frame.plane_count; ++plane) {
            if (!frame.planes[plane])
                return 0;
        }
        return frame_bytes;
    }

    static int64_t timestamp_at(const mpv_audio_pcm_frame &frame, size_t offset) {
        const uint64_t duration = static_cast<uint64_t>(frame.end_us) - frame.start_us;
        const uint64_t samples = frame.sample_count;
        // Dividing before multiplying keeps the interpolation exact without a wide product.
        const uint64_t elapsed = duration / samples * offset + duration % samples * offset / samples;
        return static_cast<int64_t>(static_cast<uint64_t>(frame.start_us) + elapsed);
    }

    static void copy_chunk(Slot &slot, const mpv_audio_pcm_frame &frame,
                           size_t frame_bytes, size_t offset, size_t count, int64_t generation) {
        const size_t plane_stride = frame_bytes / frame.plane_count;
        const size_t plane_bytes = count * plane_stride;
        for (int plane = 0; plane < frame.plane_count; ++plane) {
            memcpy(slot.bytes.data() + plane * plane_bytes,
                   frame.planes[plane] + offset * plane_stride, plane_bytes);
        }
        slot.size = count * frame_bytes;
        slot.info = {timestamp_at(frame, offset), timestamp_at(frame, offset + count),
                     frame.sample_rate, frame.channel_count, frame.format, frame.plane_count,
                     static_cast<int64_t>(count), generation};
    }

    std::array<Slot, kFrameCount> slots_{};
    std::mutex mutex_;
    // Keep the static queue zero-initialized so its PCM slots occupy BSS.
    std::atomic<int64_t> generation_{0};
    std::atomic<uint32_t> dropped_frames_{0};
    int64_t written_generation_ = 0;
    size_t begin_ = 0;
    size_t count_ = 0;
};

} // namespace audio_pcm
