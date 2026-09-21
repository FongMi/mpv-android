/* Execute production demux failure publication and player handling with mocked
 * queues and locks. This checks call ordering, not real thread scheduling. */
#include <assert.h>
#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MP_NOPTS_VALUE (-DBL_MAX)
#define MP_ADD_PTS(a, b) ((a) + (b))
#define SEEK_CACHED 1
#define SEEK_SATAN 2
#define SEEK_BLOCK 4
#define MPV_ERROR_UNKNOWN_FORMAT -17
#define mp_assert assert
#define MP_TRACE test_log
#define MP_VERBOSE test_log
#define MP_WARN test_log
#define MP_ERR test_log

enum stop_play_reason {
    KEEP_PLAYING, AT_END_OF_FILE, PT_NEXT_ENTRY, PT_CURRENT_ENTRY,
    PT_STOP, PT_QUIT, PT_ERROR,
};

struct demux_packet { struct demux_packet *next; int stream; };
struct demux_queue { double last_ts; bool is_bof; };
struct demux_stream {
    bool eager, selected, refreshing, back_restarting, back_resuming;
    struct demux_packet *reader_head;
    struct demux_queue *queue;
    double base_ts, back_seek_pos;
    int type;
    unsigned eof_marks;
};
struct sh_stream { struct demux_stream *ds; };
struct demux_cached_range { struct demux_queue **streams; };
struct timed_metadata { int unused; };
struct demux_internal;
typedef struct demuxer demuxer_t;
struct demuxer_desc {
    bool (*read_packet)(demuxer_t *, struct demux_packet **);
};
struct demuxer {
    struct demux_internal *in;
    const struct demuxer_desc *desc;
    bool failed, seekable, cancelled, interrupted;
    int events, live_state, num_chapters, num_editions, edition;
    int64_t filesize;
    double duration;
    void *chapters, *editions;
};
struct demux_internal {
    demuxer_t *d_thread, *d_user;
    int lock;
    unsigned wakeup;
    void (*wakeup_cb)(void *);
    void *wakeup_cb_ctx;
    int events, num_streams, seek_flags;
    struct sh_stream **streams;
    struct demux_cached_range *current_range;
    bool threading, reading, blocked, nav_pump, nav_active, eof, seeking;
    bool back_demuxing, hyst_active, warned_queue_overflow;
    bool after_seek, after_seek_to_start, force_metadata_update, lists_changed;
    double min_secs, hyst_secs, ts_offset, last_playback_pts, duration, seek_pts;
    uint64_t hyst_bytes, max_bytes;
    int64_t stream_size;
    void *staged_chapters, *staged_editions;
    int num_staged_chapters, num_staged_editions, staged_edition;
};
#define STREAM_CTRL_GET_NAV_STATE 1
#define STREAM_CTRL_NAV_STILL_SKIP 2
#define STATUS_EOF 4
struct stream_nav_state { bool failed, still_active; int still_duration; uint32_t still_id; };
struct stream_nav_still_skip { uint32_t id; };
struct stream_info { const char *name; };
struct stream { const struct stream_info *info; struct stream_nav_state nav; bool skip_failed; };
struct disc_nav_state {
    bool timed_still_seen;
    uint32_t timed_still_id;
    double timed_still_last_tick, timed_still_remaining;
};
struct MPContext {
    demuxer_t *demuxer;
    enum stop_play_reason stop_play;
    int error_playing;
    struct disc_nav_state *disc_nav;
    struct stream *stream;
    bool disc_nav_still_frame, paused;
    int video_status, audio_status;
    void *ao;
    struct { int type; } seek;
};

static unsigned wakeups, reads, cache_updates, cache_searches, reader_clears;
static unsigned fresh_ranges, seeks, nav_fallthrough;
static struct demux_internal *active;
static bool fail_on_read;
static struct disc_nav_state nav_state;

void demux_set_failed(demuxer_t *demuxer);
static struct stream *disc_nav_get_stream(struct MPContext *mpctx)
{
    return mpctx->stream;
}
static struct disc_nav_state *get_state(struct MPContext *mpctx)
{
    (void)mpctx;
    return &nav_state;
}
static int stream_control(struct stream *s, int command, void *arg)
{
    if (command == STREAM_CTRL_NAV_STILL_SKIP) {
        assert(((struct stream_nav_still_skip *)arg)->id == s->nav.still_id);
        s->nav.failed = s->skip_failed;
        s->nav.still_active = false;
        return s->skip_failed ? -1 : 1;
    }
    assert(command == STREAM_CTRL_GET_NAV_STATE);
    *(struct stream_nav_state *)arg = s->nav;
    return 1;
}
static bool check_async_discontinuity(struct MPContext *mpctx, struct stream *s,
                                     struct stream_nav_state *nav, bool is_dvd)
{ (void)mpctx; (void)s; (void)nav; (void)is_dvd; return false; }
static bool ao_is_playing(void *ao) { (void)ao; return false; }
static double mp_time_sec(void) { return 10; }
static void mp_set_timeout(struct MPContext *mpctx, double seconds)
{ (void)mpctx; (void)seconds; }

static void test_log(void *ctx, const char *format, ...)
{
    (void)ctx;
    (void)format;
}
static void mp_mutex_lock(int *lock)
{
    assert(*lock == 0);
    *lock = 1;
}
static void mp_mutex_unlock(int *lock)
{
    assert(*lock == 1);
    *lock = 0;
}
static void mp_cond_signal(unsigned *condition)
{
    assert(active->lock == 1);
    (*condition)++;
}
static void wakeup(void *ctx)
{
    assert(ctx == active && active->lock == 1);
    wakeups++;
}
static void update_cache(struct demux_internal *in)
{
    assert(in->lock == 1);
    cache_updates++;
}
static struct timed_metadata *lookup_timed_metadata(struct demux_internal *in,
                                                   double pts)
{
    assert(in->lock == 1);
    (void)pts;
    return NULL;
}
static void update_final_metadata(demuxer_t *demuxer, struct timed_metadata *data)
{
    (void)demuxer;
    (void)data;
}
static void demux_update_replaygain(demuxer_t *demuxer) { (void)demuxer; }
static double get_current_time(struct MPContext *mpctx)
{
    (void)mpctx;
    return 0;
}
static bool demux_cancel_test(demuxer_t *demuxer) { return demuxer->cancelled; }
static bool demux_read_interrupted(demuxer_t *demuxer)
{
    assert(demuxer->in->lock == 0);
    return demuxer->interrupted;
}
static bool lazy_stream_needs_wait(struct demux_stream *ds)
{
    (void)ds;
    return false;
}
static void mark_stream_eof(struct demux_stream *ds) { ds->eof_marks++; }
static uint64_t get_forward_buffered_bytes(struct demux_stream *ds)
{
    (void)ds;
    return 0;
}
static const char *stream_type_name(int type) { (void)type; return "video"; }
static void add_packet_locked(struct sh_stream *sh, struct demux_packet *pkt)
{
    (void)sh;
    (void)pkt;
    assert(false); // The scripted reader emits no packets.
}
static struct demux_cached_range *find_cache_seek_range(struct demux_internal *in,
                                                       double pts, int flags)
{
    assert(in->lock == 1);
    (void)pts;
    (void)flags;
    cache_searches++;
    return NULL;
}
static void clear_reader_state(struct demux_internal *in, bool clear_back_state)
{
    assert(in->lock == 1);
    (void)clear_back_state;
    reader_clears++;
}
static void execute_cache_seek(struct demux_internal *in,
                               struct demux_cached_range *range,
                               double pts, int flags)
{
    (void)in;
    (void)range;
    (void)pts;
    (void)flags;
    assert(false); // No cached range is returned by this fixture.
}
static void switch_to_fresh_cache_range(struct demux_internal *in)
{
    assert(in->lock == 1);
    fresh_ranges++;
}
static void back_demux_see_packets(struct demux_stream *ds) { (void)ds; }
static void wakeup_ds(struct demux_stream *ds) { (void)ds; }
static void execute_seek(struct demux_internal *in)
{
    assert(in->lock == 1);
    seeks++;
}
static bool read_success(demuxer_t *demuxer, struct demux_packet **pkt)
{
    assert(demuxer->in->lock == 0 && !*pkt);
    reads++;
    if (fail_on_read) {
        demux_set_failed(demuxer);
        return false;
    }
    return true;
}

#include "disc-streams-under-test.h"

struct fixture {
    demuxer_t producer, consumer;
    struct demux_internal in;
    struct MPContext player;
    struct demux_stream ds;
    struct sh_stream sh;
    struct sh_stream *streams[1];
    struct demux_queue queue;
};

static void init(struct fixture *f)
{
    static const struct demuxer_desc desc = { .read_packet = read_success };
    memset(f, 0, sizeof(*f));
    memset(&nav_state, 0, sizeof(nav_state));
    wakeups = reads = cache_updates = cache_searches = reader_clears = 0;
    fresh_ranges = seeks = nav_fallthrough = 0;
    fail_on_read = false;
    f->producer.in = f->consumer.in = &f->in;
    f->producer.desc = &desc;
    f->producer.seekable = true;
    f->in.d_thread = &f->producer;
    f->in.d_user = &f->consumer;
    f->in.threading = true;
    f->in.max_bytes = UINT64_MAX;
    f->in.wakeup_cb = wakeup;
    f->in.wakeup_cb_ctx = &f->in;
    f->ds.eager = f->ds.selected = true;
    f->ds.queue = &f->queue;
    f->queue.last_ts = f->ds.base_ts = MP_NOPTS_VALUE;
    f->sh.ds = &f->ds;
    f->streams[0] = &f->sh;
    f->in.streams = f->streams;
    f->in.num_streams = 1;
    f->player.demuxer = &f->consumer;
    active = &f->in;
}

static void test_failed_navigation_wakes_eof_demuxer(void)
{
    struct fixture f;
    init(&f);
    const struct stream_info info = {.name = "dvdnav"};
    struct stream stream = {.info = &info, .nav = {.failed = true}};
    f.player.stream = &stream;
    f.player.paused = true;
    f.player.video_status = f.player.audio_status = STATUS_EOF;
    f.player.disc_nav_still_frame = true;
    f.in.eof = true;
    disc_nav_update(&f.player);
    assert(f.in.nav_pump && f.in.reading && f.in.wakeup == 1);
    assert(!nav_fallthrough && !f.player.disc_nav_still_frame);
    assert(!f.producer.failed && !f.consumer.failed);

    // The demux thread owns failure publication, even when initially at EOF.
    fail_on_read = true;
    mp_mutex_lock(&f.in.lock);
    assert(read_packet(&f.in));
    mp_mutex_unlock(&f.in.lock);
    assert(reads == 1 && f.producer.failed && !f.consumer.failed);
    sync_demuxer_properties(&f.player);
    assert(f.player.stop_play == PT_ERROR);

    init(&f);
    stream.nav.failed = false;
    f.player.stream = &stream;
    f.in.eof = true;
    disc_nav_update(&f.player);
    assert(nav_fallthrough == 1 && !f.in.nav_pump && !f.in.reading);
    assert(!f.in.wakeup && !f.producer.failed && !f.consumer.failed);

    // A timed still can fail in this same update, after its initial snapshot.
    init(&f);
    stream.nav = (struct stream_nav_state){.still_active = true, .still_duration = 1,
                                            .still_id = 7};
    stream.skip_failed = true;
    f.player.stream = &stream;
    f.player.video_status = f.player.audio_status = STATUS_EOF;
    f.player.disc_nav_still_frame = true;
    nav_state.timed_still_seen = true;
    nav_state.timed_still_id = 7;
    nav_state.timed_still_last_tick = 8;
    nav_state.timed_still_remaining = 1;
    f.in.eof = true;
    disc_nav_update(&f.player);
    assert(stream.nav.failed && f.in.nav_pump && f.in.reading && f.in.wakeup == 1);
    assert(!nav_fallthrough && !f.player.disc_nav_still_frame);
}

static void test_failure_publication(void)
{
    struct fixture f;
    init(&f);
    demux_set_failed(&f.producer);
    assert(f.producer.failed && !f.consumer.failed);
    assert(f.in.events == DEMUX_EVENT_FAILED && !f.consumer.events);
    assert(wakeups == 1 && !f.in.lock);
    demux_set_failed(&f.producer);
    assert(wakeups == 1);

    sync_demuxer_properties(&f.player);
    assert(f.consumer.failed && (f.consumer.events & DEMUX_EVENT_FAILED));
    assert(!f.in.events && !f.in.lock);
    assert(f.player.stop_play == PT_ERROR);
    assert(f.player.error_playing == MPV_ERROR_UNKNOWN_FORMAT);

    f.consumer.events = 0;
    demux_set_failed(&f.producer);
    sync_demuxer_properties(&f.player);
    assert(wakeups == 1 && !f.consumer.events && f.consumer.failed);
}

static void test_initialization_is_not_failure(void)
{
    struct fixture f;
    init(&f);
    f.in.events = DEMUX_EVENT_ALL;
    sync_demuxer_properties(&f.player);
    assert(!f.consumer.failed && !f.producer.failed);
    assert(f.consumer.events & DEMUX_EVENT_FAILED);
    assert(f.player.stop_play == KEEP_PLAYING && !f.player.error_playing);
    assert(!wakeups);
}

static void test_failed_nav_and_seek_do_not_rearm(void)
{
    struct fixture f;
    init(&f);
    demux_drive_nav(&f.consumer);
    assert(f.in.nav_pump && f.in.reading && f.in.wakeup == 1);
    f.in.nav_pump = f.in.reading = false;
    f.in.eof = true;
    demux_set_failed(&f.producer);
    assert(!f.consumer.failed); // Refuse even before the consumer update.
    demux_drive_nav(&f.consumer);
    assert(!f.in.nav_pump && !f.in.reading && f.in.wakeup == 1);
    mp_mutex_lock(&f.in.lock);
    assert(!queue_seek(&f.in, 10, 0, true));
    mp_mutex_unlock(&f.in.lock);
    assert(f.in.eof && !f.in.seeking && !f.in.reading);
    assert(!cache_searches && !reader_clears && !fresh_ranges && !seeks);

    init(&f);
    mp_mutex_lock(&f.in.lock);
    assert(queue_seek(&f.in, 10, 0, true));
    mp_mutex_unlock(&f.in.lock);
    assert(f.in.seeking && f.in.seek_pts == 10);
    assert(cache_searches == 1 && reader_clears == 1 && fresh_ranges == 1);
}

static void test_failed_reader_stops_at_eof(void)
{
    struct fixture f;
    init(&f);
    f.in.reading = true;
    mp_mutex_lock(&f.in.lock);
    assert(read_packet(&f.in));
    mp_mutex_unlock(&f.in.lock);
    assert(reads == 1 && f.in.reading && !f.in.eof);

    demux_set_failed(&f.producer);
    mp_mutex_lock(&f.in.lock);
    assert(read_packet(&f.in));
    assert(!read_packet(&f.in));
    mp_mutex_unlock(&f.in.lock);
    assert(reads == 1 && !f.in.reading && f.in.eof);
    assert(f.ds.eof_marks == 1 && cache_updates == 2);
    sync_demuxer_properties(&f.player);
    assert(f.player.stop_play == PT_ERROR);
}

static void test_cancelled_read_is_not_failure(void)
{
    struct fixture f;
    init(&f);
    f.in.reading = true;
    f.producer.cancelled = true;
    mp_mutex_lock(&f.in.lock);
    assert(!read_packet(&f.in));
    mp_mutex_unlock(&f.in.lock);
    sync_demuxer_properties(&f.player);
    assert(!reads && !wakeups && !f.producer.failed && !f.consumer.failed);
    assert(f.player.stop_play == KEEP_PLAYING && !f.player.error_playing);
}

static void test_failure_during_read(void)
{
    struct fixture f;
    init(&f);
    fail_on_read = true;
    f.in.reading = true;
    mp_mutex_lock(&f.in.lock);
    assert(read_packet(&f.in));
    mp_mutex_unlock(&f.in.lock);
    assert(reads == 1 && f.producer.failed && !f.consumer.failed);
    assert(f.in.eof && !f.in.reading && f.ds.eof_marks == 1);
    assert(wakeups == 2); // Failure publication and the first EOF each wake up.
    sync_demuxer_properties(&f.player);
    assert(f.player.stop_play == PT_ERROR);
    assert(f.player.error_playing == MPV_ERROR_UNKNOWN_FORMAT);
}

static void test_failure_after_last_property_update(void)
{
    struct fixture f;
    init(&f);
    sync_demuxer_properties(&f.player);
    demux_set_failed(&f.producer);
    f.player.stop_play = AT_END_OF_FILE;
    assert(!f.consumer.failed);
    finish_demuxer(&f.player);
    assert(f.consumer.failed && !f.in.events);
    assert(f.player.stop_play == PT_ERROR);
    assert(f.player.error_playing == MPV_ERROR_UNKNOWN_FORMAT);

    init(&f);
    f.player.stop_play = AT_END_OF_FILE;
    finish_demuxer(&f.player);
    assert(f.player.stop_play == AT_END_OF_FILE && !f.player.error_playing);
}

static void test_user_stop_and_existing_errors_are_preserved(void)
{
    const enum stop_play_reason reasons[] = {
        PT_STOP, PT_QUIT, PT_NEXT_ENTRY, PT_CURRENT_ENTRY, PT_ERROR,
    };
    for (unsigned n = 0; n < sizeof(reasons) / sizeof(reasons[0]); n++) {
        struct fixture f;
        init(&f);
        demux_set_failed(&f.producer);
        f.player.stop_play = reasons[n];
        f.player.error_playing = -91;
        sync_demuxer_properties(&f.player);
        finish_demuxer(&f.player);
        assert(f.player.stop_play == reasons[n] && f.player.error_playing == -91);
    }
}

int main(void)
{
    test_failed_navigation_wakes_eof_demuxer();
    test_failure_publication();
    test_initialization_is_not_failure();
    test_failed_nav_and_seek_do_not_rearm();
    test_failed_reader_stops_at_eof();
    test_cancelled_read_is_not_failure();
    test_failure_during_read();
    test_failure_after_last_property_update();
    test_user_stop_and_existing_errors_are_preserved();
    puts("mpv demux failure tests passed");
    return 0;
}
