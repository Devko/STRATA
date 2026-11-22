// core/streamfifo: the frames a streamed sample has read ahead, numbered as in the file.
#include "streamfifo.hpp"

#include "check.hpp"

using namespace strata;

namespace {
StreamFifo g_fifo;   // 16 KiB: file scope, never a local

// Write `count` stereo frames numbered from next_frame(), each L = frame, R = -frame.
uint32_t write_frames(uint32_t count) {
    uint32_t written = 0;
    while (written < count) {
        uint32_t room = 0;
        int16_t *span = g_fifo.write_span(&room);
        if (room == 0) break;
        const uint32_t n = count - written < room ? count - written : room;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t frame = g_fifo.next_frame() + i;
            span[2 * i] = int16_t(frame & 0x7FFF);
            span[2 * i + 1] = int16_t(-int32_t(frame & 0x7FFF));
        }
        g_fifo.commit(n);
        written += n;
    }
    return written;
}
}  // namespace

TEST(streamfifo_holds_frames_by_their_file_number) {
    g_fifo.reset(2, 1000);
    CHECK_EQ(g_fifo.next_frame(), 1000u);
    CHECK(!g_fifo.has(1000));
    CHECK_EQ(write_frames(10), 10u);
    CHECK(!g_fifo.has(999));
    CHECK(g_fifo.has(1000));
    CHECK(g_fifo.has(1009));
    CHECK(!g_fifo.has(1010));
    CHECK_EQ(int(g_fifo.frame(1005)[0]), 1005);
    CHECK_EQ(int(g_fifo.frame(1005)[1]), -1005);
    CHECK_EQ(g_fifo.held(), 10u);
}

TEST(streamfifo_never_overwrites_a_frame_still_wanted) {
    g_fifo.reset(2, 0);
    CHECK_EQ(write_frames(StreamFifo::kFrames + 100), StreamFifo::kFrames);
    CHECK_EQ(g_fifo.held(), StreamFifo::kFrames);
    uint32_t room = 99;
    g_fifo.write_span(&room);
    CHECK_EQ(room, 0u);
    CHECK_EQ(int(g_fifo.frame(0)[0]), 0);
    // Reading frees room, and the next frames wrap into the freed slots.
    g_fifo.drop_before(300);
    CHECK_EQ(write_frames(300), 300u);
    CHECK(!g_fifo.has(299));
    CHECK(g_fifo.has(StreamFifo::kFrames + 299));
    CHECK_EQ(int(g_fifo.frame(StreamFifo::kFrames + 299)[0]), int((StreamFifo::kFrames + 299) & 0x7FFF));
    CHECK_EQ(int(g_fifo.frame(300)[0]), 300);
}

TEST(streamfifo_an_underrun_throws_away_what_arrives_late) {
    g_fifo.reset(2, 50);
    write_frames(10);                 // 50..59
    g_fifo.drop_before(80);           // the voice is already at 80
    CHECK(!g_fifo.has(59));
    CHECK_EQ(g_fifo.held(), 0u);
    write_frames(25);                 // 60..84: 60..79 are late
    CHECK(!g_fifo.has(79));
    CHECK(g_fifo.has(80));
    CHECK(g_fifo.has(84));
    CHECK_EQ(int(g_fifo.frame(82)[0]), 82);
    CHECK_EQ(g_fifo.held(), 5u);
}

TEST(streamfifo_mono_frames_are_one_sample) {
    g_fifo.reset(1, 0);
    uint32_t room = 0;
    int16_t *span = g_fifo.write_span(&room);
    CHECK_EQ(room, StreamFifo::kFrames);
    for (int i = 0; i < 4; i++) span[i] = int16_t(i * 10);
    g_fifo.commit(4);
    CHECK_EQ(int(g_fifo.frame(3)[0]), 30);
    CHECK_EQ(g_fifo.channels(), 1);
}

TEST(stream_frames_in_divides_exactly) {
    for (uint32_t fb = 1; fb <= 8; fb++) {
        for (uint32_t bytes = 0; bytes < 70000; bytes += 7) {
            CHECK_EQ(stream_frames_in(bytes, fb), bytes / fb);
        }
    }
    CHECK_EQ(stream_frames_in(100, 0), 0u);
    CHECK_EQ(stream_frames_in(100, 9), 0u);
}
