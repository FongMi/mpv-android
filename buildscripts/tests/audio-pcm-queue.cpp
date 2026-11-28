#undef NDEBUG
#include <cassert>
#include <vector>

#include "app/src/main/jni/audio_pcm_queue.h"

static audio_pcm::Queue queue;

static void push_sample(uint8_t value)
{
    const uint8_t *planes[] = {&value};
    mpv_audio_pcm_frame frame{};
    frame.planes = planes;
    frame.plane_count = frame.sample_count = frame.channel_count = 1;
    frame.sample_rate = 16000;
    frame.format = MPV_AUDIO_PCM_U8;
    queue.push(&frame);
}

static int read_sample(std::array<int64_t, audio_pcm::kFrameInfoCount> &info)
{
    uint8_t value = 0;
    return queue.read(&value, sizeof(value), info) == 1 ? value : -1;
}

static void test_large_frame(bool planar)
{
    constexpr int channels = 6;
    constexpr int samples = 8192;
    constexpr size_t sample_bytes = sizeof(int16_t);
    constexpr size_t first_samples = audio_pcm::kMaxFrameBytes / (channels * sample_bytes);
    constexpr int64_t start_us = 2000000;
    constexpr int64_t end_us = 2085333;
    constexpr int64_t boundary_us = start_us + (end_us - start_us) * first_samples / samples;
    const int plane_count = planar ? channels : 1;
    const size_t plane_stride = channels * sample_bytes / plane_count;

    std::array<std::vector<uint8_t>, channels> data;
    std::array<const uint8_t *, channels> planes{};
    for (int plane = 0; plane < plane_count; ++plane) {
        data[plane].resize(samples * plane_stride);
        for (size_t i = 0; i < data[plane].size(); ++i)
            data[plane][i] = static_cast<uint8_t>((i + plane * 19) % 251);
        planes[plane] = data[plane].data();
    }

    mpv_audio_pcm_frame frame{};
    frame.planes = planes.data();
    frame.plane_count = plane_count;
    frame.sample_count = samples;
    frame.channel_count = channels;
    frame.sample_rate = 96000;
    frame.format = MPV_AUDIO_PCM_S16;
    frame.start_us = start_us;
    frame.end_us = end_us;

    queue.reset();
    queue.push(&frame);
    std::array<uint8_t, audio_pcm::kMaxFrameBytes> buffer{};
    std::array<int64_t, audio_pcm::kFrameInfoCount> info{};
    int64_t generation = -1;
    size_t offset = 0;
    for (size_t count : {first_samples, samples - first_samples}) {
        const size_t bytes = count * channels * sample_bytes;
        const size_t plane_bytes = count * plane_stride;
        assert(queue.read(buffer.data(), buffer.size(), info) == static_cast<int>(bytes));
        assert(info[0] == (offset ? boundary_us : start_us));
        assert(info[1] == (offset ? end_us : boundary_us));
        assert(info[2] == 96000 && info[3] == channels);
        assert(info[4] == MPV_AUDIO_PCM_S16 && info[5] == plane_count);
        assert(info[6] == static_cast<int64_t>(count));
        if (offset)
            assert(info[7] == generation);
        generation = info[7];
        for (int plane = 0; plane < plane_count; ++plane)
            assert(memcmp(buffer.data() + plane * plane_bytes,
                          planes[plane] + offset * plane_stride, plane_bytes) == 0);
        offset += count;
    }
    assert(queue.read(buffer.data(), buffer.size(), info) == 0);
    assert(queue.take_dropped_frames() == 0);
}

int main()
{
    std::array<int64_t, audio_pcm::kFrameInfoCount> info{};
    info.fill(-1);
    assert(read_sample(info) == -1);
    assert(info.back() == -1);
    queue.reset();
    push_sample(7);
    assert(read_sample(info) == 7);
    const auto generation = info.back();
    assert(generation == 1);

    for (size_t i = 0; i <= audio_pcm::kFrameCount; ++i)
        push_sample(static_cast<uint8_t>(i));
    mpv_audio_pcm_frame invalid{};
    queue.push(&invalid);
    assert(queue.take_dropped_frames() == 2);

    for (size_t i = 1; i <= audio_pcm::kFrameCount; ++i) {
        assert(read_sample(info) == static_cast<int>(i));
        assert(info.back() == generation);
    }
    assert(read_sample(info) == -1);

    push_sample(44);
    queue.push(nullptr);
    info.fill(-1);
    assert(read_sample(info) == -1);
    assert(info.back() == -1);
    push_sample(45);
    assert(read_sample(info) == 45);
    assert(info.back() != generation);

    test_large_frame(false);
    test_large_frame(true);
}
