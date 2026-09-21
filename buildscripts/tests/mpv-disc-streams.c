/* Execute production stream/menu/seek functions with scripted library results.
 * Disc parsing, concurrency and device playback are outside this host test. */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STREAM_OK 1
#define STREAM_ERROR -1
#define STREAM_UNSUPPORTED -2
#define STREAM_READ 1
#define STREAM_ORIGIN_MASK 0xf0
#define STREAM_CTRL_GET_TIME_LENGTH 1
#define STREAM_CTRL_GET_NAV_STATE 2
#define STREAM_CTRL_SEEK_TO_TIME 3
#define STREAM_CTRL_NAV_DRAIN_ACK 4
#define STREAM_CTRL_GET_CURRENT_TIME 5
#define STREAM_CTRL_GET_DVD_STREAMS 6
struct stream_dvd_streams { uint64_t generation; };
#define SEEK_FACTOR 1
#define SEEK_HR 2
#define SEEK_NAV (1 << 7)
#define MP_NOPTS_VALUE (-DBL_MAX)
#define BD_TIMEBASE 90000
#define BD_TIME_FROM_S(x) ((uint64_t)((x) * BD_TIMEBASE))
#define BLURAY_MENU_TITLE -1
#define BLURAY_DEFAULT_TITLE -2
#define DVDNAV_STATUS_OK 1
#define MP_WARN(...) ((void)0)
#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)
#define MP_DBG(...) ((void)0)
#define MP_TRACE(...) ((void)0)
#define mp_assert assert
#define MAX_DVD_SPU_STREAMS 1
#define STREAM_VIDEO 0
#define STREAM_AUDIO 1
#define STREAM_SUB 2
#define MP_TARRAY_REMOVE_AT(array, count, index) \
    do { assert((index) == 0); (count)--; (void)(array); } while (0)
#define MP_TARRAY_APPEND(ctx, array, count, ...) \
    do { (void)(ctx); (array)[(count)++] = (__VA_ARGS__); } while (0)
#define mp_mutex_lock(p) ((void)(p))
#define mp_mutex_unlock(p) ((void)(p))
#define atomic_load(p) (*(p))
#define atomic_store(p, v) (*(p) = (v))
#define TA_FREEP(p) (*(p) = NULL)

typedef struct {
    bool no_menu_support, first_play_supported, top_menu_supported;
    bool bdj_detected, bdj_handled;
} BLURAY_DISC_INFO;
typedef struct {
    BLURAY_DISC_INFO info;
    bool seek_success;
    unsigned seek_calls, play_calls, menu_calls;
    int play_result, menu_result, open_result;
    int yuv_registrations, argb_registrations, closes;
    uint64_t position;
} BLURAY;
struct stream_nav_state {
    bool drain_pending, still_active, nav_active, menu_active, failed;
    uint32_t discontinuity_id;
};
typedef struct stream {
    void *priv, *global, *cancel;
    int stream_origin;
    bool seekable, is_network, streaming;
    unsigned drops, rebases, frees;
    struct stream_nav_state nav;
    bool have_nav;
    double length, clock, target;
    int seek_flags, seek_result;
    unsigned seeks, acknowledgements, peeks;
    bool script_peek;
    int peek_result;
    void (*on_peek)(struct stream *);
} stream_t;
typedef int stream_info_t;
static const stream_info_t stream_info_cb = 1, stream_info_ffmpeg = 2;
struct stream_open_args {
    void *global, *cancel;
    const char *url;
    int flags;
    const stream_info_t *sinfo;
};
struct bluray_priv_s {
    BLURAY *bd;
    stream_t *iso_stream;
    void *title_info;
    int overlay_lock, cfg_title, current_title, current_playlist, still_duration;
    bool menu_supported, hdmv_mode, still_active, read_pos_known, resync_owed;
    bool transition_pending;
    unsigned discontinuity_id, nav_change_id;
};
struct sh_stream {
    bool still_image, selected;
    int type, index, demuxer_id;
    void *codec;
};
struct demux_packet {
    int stream;
    int64_t pos;
    double pts, dts;
    bool segmented;
    void *codec;
};
struct dvd_sub_hold {
    bool pending;
    struct demux_packet *pkt;
    struct sh_stream *sh;
};
struct pending_sub {
    struct demux_packet *pkt;
    struct sh_stream *sh;
    uint64_t seq;
};
struct stream_timeline { int64_t select_pos; };
typedef struct demuxer demuxer_t;
struct demuxer_desc { void (*drop_buffers)(demuxer_t *); };
struct chapter { double pts; };
struct demuxer {
    void *priv;
    stream_t *stream;
    struct demuxer_desc *desc;
    int num_chapters;
    unsigned drops;
    struct chapter *chapters;
    void *cancel, *global, *packet_pool;
    int stream_origin, depth, num_streams;
    unsigned frees, reads;
    bool failed, interrupted;
    struct demux_packet *packet;
    void (*on_read)(struct demuxer *);
};
struct demuxer_params {
    const char *force_format;
    stream_t *external_stream;
    int stream_flags, depth;
};
struct priv {
    struct stream_dvd_streams dvd_streams;
    stream_t *iso_stream;
    void *dvdnav;
    demuxer_t *slave;
    bool is_cdda, is_dvd, is_dvda, is_bd, seek_reinit, reopen_pending;
    struct sh_stream *video_sh;
    double skip_audio_until, reset_base_time;
    int64_t last_read_pos;
    struct stream_timeline *tl_streams;
    int num_tl_streams, last_still_id;
    uint32_t last_discontinuity_id;
    void *pending_pkt;
    unsigned cleared_subs;
    struct sh_stream **slave_to_outer;
    int slave_to_outer_count, num_pending_subs;
    struct pending_sub pending_subs[1];
    struct dvd_sub_hold dvd_sub_hold[MAX_DVD_SPU_STREAMS];
    bool nav_active, eof_log_still;
    uint32_t eof_log_id;
    uint64_t av_map_seq;
    double last_video_dts;
};

static stream_t transport;
static BLURAY disc;
static int transport_result, dvd_result;
static const stream_info_t *opened_info;
static unsigned metadata_refreshes;
static unsigned open_calls, sync_calls, reset_calls, failure_calls;
static demuxer_t *open_result;
static void (*on_open)(void);
static struct sh_stream outer_stream;
static int dvdnav_stream_callbacks;

static int stream_create_with_args(struct stream_open_args *args, stream_t **result)
{
    assert(args->global && args->cancel);
    assert(args->flags == (STREAM_READ | 0x20));
    opened_info = args->sinfo;
    *result = transport_result == STREAM_OK ? &transport : NULL;
    return transport_result;
}
static BLURAY *bd_init(void) { return &disc; }
static int read_iso_blocks(void *handle, void *buf, int lba, int count)
{
    (void)handle; (void)buf; (void)lba; (void)count;
    return 0;
}
static int bd_open_stream(BLURAY *bd, void *handle,
                          int (*read)(void *, void *, int, int))
{
    assert(handle && read == read_iso_blocks);
    return bd->open_result;
}
static void bd_close(BLURAY *bd) { bd->closes++; }
static void free_stream(stream_t *s) { s->frees++; }
static int dvdnav_open_stream(void **nav, stream_t *source, int *callbacks)
{
    assert(source == &transport && callbacks == &dvdnav_stream_callbacks);
    *nav = dvd_result == DVDNAV_STATUS_OK ? &disc : NULL;
    return dvd_result;
}
static void dvdnav_set_readahead_flag(void *nav, int flag) { assert(nav && flag == 1); }
static int dvdnav_set_PGC_positioning_flag(void *nav, int flag)
{
    assert(nav && flag == 1);
    return DVDNAV_STATUS_OK;
}
static const BLURAY_DISC_INFO *bd_get_disc_info(BLURAY *bd) { return &bd->info; }
static void bd_yuv_overlay_cb(void) {}
static void bd_argb_overlay_cb(void) {}
static void bd_register_overlay_proc(BLURAY *bd, void *handle, void (*callback)(void))
{
    bd->yuv_registrations += callback ? 1 : -1;
    assert((handle != NULL) == (callback != NULL));
}
static void bd_register_argb_overlay_proc(BLURAY *bd, void *handle,
                                         void (*callback)(void), void *buffer)
{
    (void)buffer;
    if (callback)
        bd->argb_registrations++;
    else
        bd->argb_registrations = 0;
    assert((handle != NULL) == (callback != NULL));
}
static int bd_play(BLURAY *bd) { bd->play_calls++; return bd->play_result; }
static int bd_menu_call(BLURAY *bd, int64_t pts)
{
    assert(pts == -1);
    bd->menu_calls++;
    return bd->menu_result;
}
static int bd_get_current_title(BLURAY *bd) { (void)bd; return 4; }
static void bd_free_title_info(void *info) { assert(info); }
static int bd_seek_time_checked(BLURAY *bd, uint64_t time)
{
    bd->seek_calls++;
    if (bd->seek_success)
        bd->position = time;
    return bd->seek_success;
}
static void stream_drop_buffers(stream_t *s) { s->drops++; }
static void stream_rebase_position(stream_t *s) { s->rebases++; }
static void demux_seek(demuxer_t *d, double time, int flags)
{
    (void)time; (void)flags;
    d->drops++;
}
static int stream_control(stream_t *s, int command, void *arg)
{
    switch (command) {
    case STREAM_CTRL_GET_TIME_LENGTH: *(double *)arg = s->length; return STREAM_OK;
    case STREAM_CTRL_GET_CURRENT_TIME: *(double *)arg = s->clock; return STREAM_OK;
    case STREAM_CTRL_GET_DVD_STREAMS: return STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_NAV_STATE:
        *(struct stream_nav_state *)arg = s->nav;
        return s->have_nav ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_SEEK_TO_TIME:
        s->target = ((double *)arg)[0];
        s->seek_flags = (int)((double *)arg)[1];
        s->seeks++;
        return s->seek_result;
    case STREAM_CTRL_NAV_DRAIN_ACK:
        s->acknowledgements++;
        s->nav.drain_pending = false;
        return STREAM_OK;
    default: assert(false); return STREAM_ERROR;
    }
}
static int stream_read_peek(stream_t *s, void *buffer, int length)
{
    (void)buffer;
    assert(length == 1 || length == 192);
    s->peeks++;
    if (s->on_peek)
        s->on_peek(s);
    return s->script_peek ? s->peek_result : length;
}
static void clear_dvd_sub_holds(struct priv *p) { p->cleared_subs++; }
static void refresh_disc_metadata(demuxer_t *d) { (void)d; metadata_refreshes++; }
static void drop_slave(demuxer_t *d) { d->drops++; }
static void demux_free(demuxer_t *d) { if (d) d->frees++; }
static bool demux_read_interrupted(demuxer_t *d) { return d->interrupted; }
static void demux_set_failed(demuxer_t *d) { d->failed = true; failure_calls++; }
static demuxer_t *demux_open_url(const char *url, struct demuxer_params *params,
                                void *cancel, void *global)
{
    (void)cancel; (void)global;
    assert(!strcmp(url, "-") && !strcmp(params->force_format, "+lavf"));
    assert(params->external_stream && params->depth == 1);
    open_calls++;
    if (on_open)
        on_open();
    return open_result;
}
static void sync_streams(demuxer_t *d)
{
    struct priv *p = d->priv;
    sync_calls++;
    if (p->slave_to_outer_count)
        p->slave_to_outer[0] = &outer_stream;
}
static void demux_set_nav_active(demuxer_t *d, bool active) { (void)d; (void)active; }
static bool demux_stream_is_selected(struct sh_stream *sh) { return sh->selected; }
static struct demux_packet *demux_copy_packet(void *pool, struct demux_packet *pkt)
{
    (void)pool; return pkt;
}
static struct sh_stream *demux_get_stream(demuxer_t *d, int index)
{ (void)d; assert(index == 0); return &outer_stream; }
static struct sh_stream *selected_dvd_audio(struct priv *p, struct sh_stream *src)
{ (void)p; return src->selected ? src : NULL; }
static void queue_dvd_sub_aliases(demuxer_t *d, struct sh_stream *src, struct demux_packet *pkt)
{ (void)d; (void)src; (void)pkt; assert(false); /* Covered by the logical routing fixture. */ }
static bool deliver_dvd_sub(demuxer_t *d, struct sh_stream *sh,
                            struct demux_packet *pkt, struct demux_packet **out)
{
    (void)d; (void)sh; *out = pkt; return true;
}
static struct demux_packet *demux_read_any_packet(demuxer_t *d)
{
    d->reads++;
    if (d->on_read)
        d->on_read(d);
    struct demux_packet *pkt = d->packet;
    d->packet = NULL;
    return pkt;
}
static void demux_update(demuxer_t *d, double pts) { (void)d; (void)pts; }
static int demux_get_num_stream(demuxer_t *d) { return d->num_streams; }
static void reset_pts(demuxer_t *d)
{
    struct priv *p = d->priv;
    reset_calls++;
    p->seek_reinit = false;
}
static void talloc_free(void *p) { (void)p; }
static struct stream_timeline *get_stream_tl(struct priv *p, struct sh_stream *sh)
{
    (void)sh; return p->tl_streams;
}
static void map_sub_packet(demuxer_t *d, struct sh_stream *sh, struct demux_packet *pkt)
{
    (void)d; (void)sh; (void)pkt;
}
static void map_av_packet(demuxer_t *d, struct sh_stream *sh, struct demux_packet *pkt)
{
    (void)sh; pkt->pts += ((struct priv *)d->priv)->reset_base_time;
}
static void demux_set_stream_still_image(demuxer_t *d, struct sh_stream *sh, bool still)
{
    (void)d; sh->still_image = still;
}
static void inject_still(demuxer_t *d, double pts) { (void)d; (void)pts; }

#include "disc-streams-under-test.h"

static void test_menu(void)
{
    struct bluray_priv_s p = {.bd = &disc, .cfg_title = BLURAY_MENU_TITLE};
    stream_t s = {.priv = &p};
    disc = (BLURAY){.info = {.first_play_supported = true}};
    configure_menu(&s, 0);
    assert(!p.menu_supported && !p.hdmv_mode && p.cfg_title == BLURAY_DEFAULT_TITLE);
    p.cfg_title = BLURAY_MENU_TITLE;
    configure_menu(&s, 1);
    assert(p.menu_supported && p.hdmv_mode);
    disc.info.bdj_detected = true; /* Unsupported BD-J bonus must not mask HDMV. */
    configure_menu(&s, 1);
    assert(p.menu_supported && p.hdmv_mode);
    configure_menu(&s, 0);
    assert(!p.menu_supported && !p.hdmv_mode);
    disc.info.bdj_handled = true;
    p.cfg_title = BLURAY_MENU_TITLE;
    configure_menu(&s, 0);
    assert(p.menu_supported && p.hdmv_mode);
    disc.info.no_menu_support = true;
    configure_menu(&s, 1);
    assert(!p.menu_supported && !p.hdmv_mode);
    disc.info = (BLURAY_DISC_INFO){0};
    p.cfg_title = BLURAY_MENU_TITLE;
    configure_menu(&s, 1);
    assert(!p.menu_supported && !p.hdmv_mode);

    disc = (BLURAY){.info = {.top_menu_supported = true}, .play_result = 1, .menu_result = 1};
    p = (struct bluray_priv_s){.bd = &disc, .cfg_title = BLURAY_MENU_TITLE};
    configure_menu(&s, 1);
    assert(p.menu_supported && p.hdmv_mode);
    assert(play_menu(&s) && disc.play_calls == 1 && disc.menu_calls == 1);
    disc.yuv_registrations = 0;
    disc.info.first_play_supported = true;
    assert(play_menu(&s) && disc.play_calls == 2 && disc.menu_calls == 1);
    disc.yuv_registrations = 0;
    disc.play_result = 0;
    assert(!play_menu(&s) && disc.menu_calls == 1 && disc.yuv_registrations == 0);
    disc.play_result = 1;
    disc.info.first_play_supported = false;
    disc.menu_result = 0;
    assert(!play_menu(&s) && disc.yuv_registrations == 0);

    p = (struct bluray_priv_s){.bd = &disc, .menu_supported = true, .still_active = true,
                              .still_duration = 5, .title_info = &s};
    assert(!start_hdmv_navigation(&s));
    assert(!p.hdmv_mode && p.still_active && p.title_info == &s && !p.resync_owed);
    disc.menu_result = 1;
    assert(start_hdmv_navigation(&s));
    assert(p.hdmv_mode && !p.still_active && !p.title_info && p.resync_owed);
    assert(p.discontinuity_id == 1 && p.nav_change_id == 1 && p.current_title == 4);
    unsigned calls = disc.play_calls;
    assert(start_hdmv_navigation(&s) && disc.play_calls == calls);
}

static void test_bluray_seek(void)
{
    disc = (BLURAY){.position = 123456};
    struct bluray_priv_s p = {.bd = &disc, .still_active = true,
                              .still_duration = 5, .read_pos_known = true,
                              .transition_pending = true};
    stream_t s = {.priv = &p};
    double target = 30;
    assert(seek_bluray(&s, &target) == STREAM_ERROR);
    assert(disc.position == 123456 && p.still_active && p.still_duration == 5);
    assert(p.read_pos_known && s.drops == 0 && disc.seek_calls == 1);
    assert(p.transition_pending);
    double invalid[] = {-1, NAN, INFINITY, DBL_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(seek_bluray(&s, &invalid[i]) == STREAM_ERROR);
        assert(disc.seek_calls == 1 && s.drops == 0 && p.still_active);
    }
    disc.seek_success = true;
    target = 0;
    assert(seek_bluray(&s, &target) == STREAM_OK);
    assert(disc.position == 0 && !p.still_active && !p.read_pos_known && s.drops == 1);
    assert(!p.transition_pending);
}

static void test_transport(void)
{
    const char *urls[] = {"https://example.test/disc.iso", "proxy://disc.iso",
                          "media3iso://request/disc.iso", "file:///disc.iso"};
    transport_result = STREAM_OK;
    dvd_result = DVDNAV_STATUS_OK;
    for (unsigned i = 0; i < 4; i++) {
        bool network = i < 3;
        for (unsigned backend = 0; backend < 2; backend++) {
            transport = (stream_t){.seekable = true, .is_network = i < 2, .streaming = i < 2};
            disc = (BLURAY){.open_result = 1};
            struct bluray_priv_s bd_priv = {0};
            struct priv dvd_priv = {0};
            stream_t s = {.priv = backend ? (void *)&dvd_priv : (void *)&bd_priv,
                          .global = &disc, .cancel = &transport, .stream_origin = 0x20};
            int result = backend ? open_dvdnav_iso_stream(&s, urls[i])
                                 : open_bluray_from_stream(&s, urls[i]);
            assert(result == STREAM_OK && s.is_network == network && s.streaming == network);
            assert(opened_info == (i == 2 ? &stream_info_cb : &stream_info_ffmpeg));
        }
    }
    transport = (stream_t){.seekable = true, .is_network = true, .streaming = true};
    disc = (BLURAY){0};
    struct bluray_priv_s p = {0};
    stream_t s = {.priv = &p, .global = &disc, .cancel = &transport, .stream_origin = 0x20};
    assert(open_bluray_from_stream(&s, urls[0]) == STREAM_UNSUPPORTED);
    assert(!s.is_network && !s.streaming && !p.iso_stream && transport.frees == 1);
    transport_result = STREAM_ERROR;
    assert(open_bluray_from_stream(&s, urls[0]) == STREAM_ERROR && !s.is_network);
}

static void test_demux_seek(void)
{
    stream_t s = {.clock = 22, .length = 100, .have_nav = true};
    struct sh_stream video = {.still_image = true};
    struct stream_timeline tl = {.select_pos = 66};
    demuxer_t slave = {0};
    struct demuxer_desc desc = {.drop_buffers = drop_slave};
    slave.desc = &desc;
    struct priv p = {.slave = &slave, .is_bd = true, .video_sh = &video,
                     .skip_audio_until = 13, .reset_base_time = 21, .last_read_pos = 12,
                     .tl_streams = &tl, .num_tl_streams = 1, .last_still_id = 7,
                     .pending_pkt = &s};
    struct chapter chapters[] = {{0}, {10}};
    demuxer_t d = {.priv = &p, .stream = &s, .num_chapters = 2, .chapters = chapters};
    d_seek(&d, 15, SEEK_HR);
    assert(s.seeks == 1 && s.target == 10 && !(s.seek_flags & SEEK_HR));
    assert(p.skip_audio_until == 13 && p.reset_base_time == 21 && p.last_read_pos == 12);
    assert(tl.select_pos == 66 && !p.seek_reinit && p.last_still_id == 7 && p.pending_pkt == &s);
    assert(slave.drops == 0 && s.rebases == 0 && p.cleared_subs == 0);
    s.seek_result = STREAM_OK;
    d_seek(&d, 15, SEEK_HR);
    assert(p.skip_audio_until == 15 && p.reset_base_time == 22 && p.last_read_pos == 0);
    assert(tl.select_pos == -1 && p.seek_reinit && p.last_still_id == -1 && !p.pending_pkt);
    assert(slave.drops == 1 && s.rebases == 1 && p.cleared_subs == 1);
    s.nav.drain_pending = true;
    s.seek_result = STREAM_ERROR;
    d_seek(&d, 15, SEEK_NAV);
    assert(s.seeks == 2 && s.acknowledgements == 1 && metadata_refreshes == 1);
    assert(p.skip_audio_until == MP_NOPTS_VALUE && slave.drops == 2);
    s.nav.drain_pending = false;
    s.nav.discontinuity_id = 1;
    d_seek(&d, 15, SEEK_NAV);
    assert(s.seeks == 2 && slave.drops == 3);
    p.is_bd = false;
    p.is_dvd = true;
    d_seek(&d, 15, 0);
    assert(s.seeks == 3 && slave.drops == 3 && s.peeks == 0);
    s.seek_result = STREAM_OK;
    d_seek(&d, 15, 0);
    assert(slave.drops == 4 && s.peeks == 1 && p.last_discontinuity_id == 1);
    p.is_cdda = true;
    d_seek(&d, 15, 0);
    assert(s.seeks == 4 && slave.drops == 5);
}

static void test_user_seek_at_navigation_drain(void)
{
    stream_t s = {.clock = 322, .have_nav = true, .seek_result = STREAM_OK,
                  .nav = {.drain_pending = true, .discontinuity_id = 4}};
    demuxer_t slave = {0};
    struct demuxer_desc desc = {.drop_buffers = drop_slave};
    slave.desc = &desc;
    struct priv p = {.slave = &slave, .is_dvd = true,
                     .reset_base_time = MP_NOPTS_VALUE};
    demuxer_t d = {.priv = &p, .stream = &s};
    d_seek(&d, 15, SEEK_HR);
    assert(s.seeks == 1 && s.target == 15 && (s.seek_flags & SEEK_HR));
    assert(s.acknowledgements == 0);
}

struct reopen_fixture {
    stream_t stream;
    demuxer_t outer, old, replacement;
    struct priv priv;
    struct stream_timeline timeline;
    struct sh_stream *map[1];
    struct demux_packet packet;
};
static struct reopen_fixture *active_reopen;

static void init_reopen(struct reopen_fixture *f)
{
    *f = (struct reopen_fixture){0};
    f->stream = (stream_t){.have_nav = true, .clock = 25,
                           .nav = {.discontinuity_id = 2}};
    f->packet = (struct demux_packet){.pts = 3, .dts = 3, .pos = 192};
    f->replacement = (demuxer_t){.packet = &f->packet, .num_streams = 1};
    f->timeline.select_pos = 4096;
    f->priv = (struct priv){.slave = &f->old, .is_bd = true,
                           .last_discontinuity_id = 1,
                           .reset_base_time = MP_NOPTS_VALUE,
                           .skip_audio_until = MP_NOPTS_VALUE,
                           .slave_to_outer = f->map, .slave_to_outer_count = 1,
                           .tl_streams = &f->timeline, .num_tl_streams = 1};
    f->outer = (demuxer_t){.priv = &f->priv, .stream = &f->stream};
    outer_stream = (struct sh_stream){.type = STREAM_VIDEO, .selected = true,
                                     .codec = &f->stream};
    f->map[0] = &outer_stream;
    open_calls = sync_calls = reset_calls = failure_calls = metadata_refreshes = 0;
    open_result = &f->replacement;
    on_open = NULL;
    active_reopen = f;
}

static void peek_still(stream_t *s) { s->nav.still_active = true; }
static void peek_drain(stream_t *s)
{
    s->nav.drain_pending = true;
    s->nav.discontinuity_id = 3;
}
static void peek_next_title(stream_t *s)
{
    s->nav.discontinuity_id = 3;
    s->peek_result = 192;
}
static void peek_partial_drain(stream_t *s)
{
    peek_drain(s);
    s->peek_result = 64;
}
static void interrupt_peek(stream_t *s)
{
    (void)s;
    active_reopen->outer.interrupted = true;
}
static void interrupt_open(void) { active_reopen->outer.interrupted = true; }
static void drain_open(void) { peek_drain(&active_reopen->stream); }
static void still_open(void) { peek_still(&active_reopen->stream); }
static void jump_at_eof(demuxer_t *d)
{
    (void)d;
    active_reopen->stream.nav.discontinuity_id++;
}

static void assert_reopened(struct reopen_fixture *f, struct demux_packet *pkt)
{
    assert(pkt == &f->packet && pkt->segmented && pkt->codec == outer_stream.codec);
    assert(pkt->pts == 28 && pkt->stream == outer_stream.index);
    assert(f->old.frees == 1 && open_calls == 1 && sync_calls == 1);
    assert(f->priv.slave == &f->replacement && !f->priv.reopen_pending);
    assert(!f->priv.seek_reinit && reset_calls == 1 && metadata_refreshes >= 2);
    assert(f->priv.last_read_pos == 192 && f->timeline.select_pos == -1);
    assert(!f->outer.failed && failure_calls == 0);
}

static void test_reopen_waits_for_navigation(void)
{
    struct reopen_fixture f;
    init_reopen(&f);
    f.stream.script_peek = true;
    f.stream.on_peek = peek_still;
    struct demux_packet *pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt)); // A real still must drain decoders.
    assert(f.priv.reopen_pending && f.priv.last_discontinuity_id == 2);
    assert(!d_read_packet(&f.outer, &pkt));
    assert(!pkt && f.old.frees == 1 && f.stream.rebases == 1 && open_calls == 0);
    f.stream.on_peek = NULL;
    f.stream.nav.still_active = false;
    // The generation has already been adopted, but an internal resync must
    // still avoid seeking the VM while its replacement demuxer is pending.
    d_seek(&f.outer, 99, SEEK_NAV);
    assert(f.stream.seeks == 0 && f.stream.rebases == 1);
    f.stream.peek_result = 192;
    assert(d_read_packet(&f.outer, &pkt));
    assert_reopened(&f, pkt);

    init_reopen(&f);
    f.stream.script_peek = true;
    f.stream.on_peek = peek_drain;
    pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(!d_read_packet(&f.outer, &pkt)); // Pending boundary needs player ACK.
    assert(f.old.frees == 1 && f.stream.rebases == 1 && open_calls == 0);
    f.stream.on_peek = NULL;
    d_seek(&f.outer, 99, SEEK_NAV);
    assert(f.stream.seeks == 0 && f.stream.acknowledgements == 1);
    f.stream.peek_result = 192;
    assert(d_read_packet(&f.outer, &pkt));
    assert(f.priv.last_discontinuity_id == 3 && f.stream.rebases == 2);
    assert_reopened(&f, pkt);
}

static void test_reopen_failure_and_eof(void)
{
    struct reopen_fixture f;
    init_reopen(&f);
    open_result = NULL;
    struct demux_packet *pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(f.outer.failed && failure_calls == 1 && !f.priv.reopen_pending);
    assert(f.priv.last_discontinuity_id == 2 && !f.priv.slave);
    assert(!d_read_packet(&f.outer, &pkt));
    assert(open_calls == 1 && failure_calls == 1 && f.old.frees == 1);
    assert(f.stream.peeks == 1 && f.stream.rebases == 1);

    init_reopen(&f);
    f.stream.script_peek = true; // No still/drain: this is real end of data.
    assert(!d_read_packet(&f.outer, &pkt));
    assert(!f.priv.reopen_pending && !f.outer.failed && !open_calls);
    assert(!d_read_packet(&f.outer, &pkt));
    assert(f.stream.peeks == 1 && f.stream.rebases == 1);
}

static void test_user_seek_during_reopen(void)
{
    struct reopen_fixture f;
    init_reopen(&f);
    f.stream.script_peek = true;
    f.stream.on_peek = peek_still;
    struct demux_packet *pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    f.stream.seek_result = STREAM_OK;
    d_seek(&f.outer, 12, SEEK_HR);
    assert(f.stream.seeks == 1 && f.stream.target == 12);
    assert(f.stream.seek_flags & SEEK_HR);
    assert(f.priv.reopen_pending && !f.priv.slave);
}

static void test_empty_interactive_playlist(void)
{
    struct reopen_fixture f;
    init_reopen(&f);
    f.stream.nav.nav_active = true;
    f.stream.script_peek = true;
    struct demux_packet *pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(!d_read_packet(&f.outer, &pkt));
    assert(f.priv.reopen_pending && f.stream.peeks == 2 && open_calls == 0);
    assert(f.old.frees == 1 && f.stream.rebases == 1);
    f.stream.on_peek = peek_next_title;
    assert(d_read_packet(&f.outer, &pkt));
    assert(f.priv.last_discontinuity_id == 3 && f.stream.rebases == 1);
    assert_reopened(&f, pkt);
    // The newly observed generation must not trigger another probe.
    f.stream.on_peek = NULL;
    f.stream.peek_result = 0;
    pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(open_calls == 1 && f.replacement.frees == 0);
}

static void test_reopen_interruption(void)
{
    for (int stage = 0; stage < 3; stage++) {
        struct reopen_fixture f;
        init_reopen(&f);
        if (stage == 0)
            f.outer.interrupted = true;
        if (stage == 1)
            f.stream.on_peek = interrupt_peek;
        if (stage == 2) {
            open_result = NULL;
            on_open = interrupt_open;
        }
        struct demux_packet *pkt = NULL;
        assert(!d_read_packet(&f.outer, &pkt));
        assert(f.priv.reopen_pending && !f.outer.failed && failure_calls == 0);
        assert(f.old.frees == 1 && f.stream.rebases == 1);
        f.outer.interrupted = false;
        f.stream.on_peek = NULL;
        on_open = NULL;
        open_result = &f.replacement;
        assert(d_read_packet(&f.outer, &pkt));
        assert(f.priv.slave == &f.replacement && !f.priv.reopen_pending);
        assert(f.old.frees == 1 && f.stream.rebases == 1);
    }
}

static void test_boundary_during_probe_and_read(void)
{
    struct reopen_fixture f;
    init_reopen(&f);
    open_result = NULL;
    on_open = drain_open;
    struct demux_packet *pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(!f.outer.failed && f.priv.reopen_pending && failure_calls == 0);
    assert(!d_read_packet(&f.outer, &pkt));
    assert(open_calls == 1); // Held boundary does not retry a probe.
    on_open = NULL;
    open_result = &f.replacement;
    d_seek(&f.outer, 99, SEEK_NAV);
    assert(d_read_packet(&f.outer, &pkt));
    assert(open_calls == 2 && f.priv.last_discontinuity_id == 3);
    assert(f.old.frees == 1 && !f.outer.failed);

    init_reopen(&f);
    open_result = NULL;
    on_open = still_open;
    pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    // A still in the same generation does not excuse a failed format probe.
    assert(f.outer.failed && !f.priv.reopen_pending && failure_calls == 1);
    assert(!d_read_packet(&f.outer, &pkt));
    assert(open_calls == 1 && failure_calls == 1);

    init_reopen(&f);
    f.stream.script_peek = true;
    f.stream.on_peek = peek_partial_drain;
    pkt = NULL;
    assert(!d_read_packet(&f.outer, &pkt));
    assert(f.priv.reopen_pending && open_calls == 0);
    assert(f.priv.last_discontinuity_id == 2);

    init_reopen(&f);
    f.stream.nav.discontinuity_id = 1;
    f.old.on_read = jump_at_eof;
    pkt = NULL;
    assert(d_read_packet(&f.outer, &pkt));
    assert_reopened(&f, pkt);
}

static void fail_nav_read(struct demuxer *d)
{
    d->stream->nav.failed = true;
}

static void fail_nav_peek(struct stream *s)
{
    s->nav.failed = true;
}

static void test_fatal_navigation_failure(void)
{
    for (int stage = 0; stage < 4; stage++) {
        struct reopen_fixture f;
        init_reopen(&f);
        f.stream.nav.nav_active = true;
        f.priv.nav_active = true;
        f.old.stream = &f.stream;
        if (stage == 0) {
            f.stream.nav.failed = true;
        } else if (stage == 1) {
            f.stream.nav.discontinuity_id = 1;
            f.old.on_read = fail_nav_read;
        } else if (stage == 2) {
            f.stream.script_peek = true;
            f.stream.on_peek = fail_nav_peek;
        } else {
            f.stream.nav.discontinuity_id = 1;
            f.stream.script_peek = true;
            f.stream.on_peek = fail_nav_peek;
        }
        struct demux_packet *pkt = NULL;
        assert(!d_read_packet(&f.outer, &pkt));
        assert(!pkt && f.outer.failed && failure_calls == 1);
        assert(open_calls == 0);
    }
}

int main(void)
{
    test_menu();
    test_bluray_seek();
    test_transport();
    test_demux_seek();
    test_user_seek_at_navigation_drain();
    test_reopen_waits_for_navigation();
    test_reopen_failure_and_eof();
    test_user_seek_during_reopen();
    test_empty_interactive_playlist();
    test_reopen_interruption();
    test_boundary_during_probe_and_read();
    test_fatal_navigation_failure();
    puts("mpv disc streams: passed (menus, seeks, demux reopen, waits, failures, cancellation)");
    return 0;
}
