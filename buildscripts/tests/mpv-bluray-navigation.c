/* Run production Blu-ray reads/controls and player EOF handling. The scripted
 * libbluray event stream permits a player poll at every state publication. */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STREAM_OK 1
#define STREAM_UNSUPPORTED -2
#define STREAM_CTRL_NAV_CMD 1
#define STREAM_CTRL_GET_NAV_STATE 2
#define STREAM_CTRL_SET_CURRENT_TITLE 3
#define STREAM_NAV_UP 1
#define STREAM_NAV_DOWN 2
#define STREAM_NAV_LEFT 3
#define STREAM_NAV_RIGHT 4
#define STREAM_NAV_SELECT 5
#define STREAM_NAV_MENU_ROOT 6
#define STREAM_NAV_MENU_TITLE 7
#define STREAM_NAV_MENU_POPUP 8
#define STREAM_NAV_PREV_MENU 9
#define STREAM_NAV_MOUSE_MOVE 10
#define STREAM_NAV_MOUSE_CLICK 11
#define BD_VK_NONE 0
#define BD_VK_UP 1
#define BD_VK_DOWN 2
#define BD_VK_LEFT 3
#define BD_VK_RIGHT 4
#define BD_VK_ENTER 5
#define BD_VK_POPUP 6
#define BD_VK_MOUSE_ACTIVATE 7
#define BD_EVENT_NONE 0
#define BD_EVENT_ERROR 1
#define BD_EVENT_READ_ERROR 2
#define BD_EVENT_END_OF_TITLE 3
#define BD_EVENT_IDLE 4
#define BD_EVENT_STILL 5
#define BD_EVENT_STILL_TIME 6
#define BD_EVENT_PLAYLIST 7
#define BD_EVENT_TITLE 8
#define BD_EVENT_ANGLE 9
#define BD_EVENT_AUDIO_STREAM 10
#define BD_EVENT_PG_TEXTST_STREAM 11
#define BD_EVENT_PG_TEXTST 12
#define BD_EVENT_SEEK 13
#define BD_EVENT_DISCONTINUITY 14
#define BLURAY_TITLE_FIRST_PLAY 0xffff
#define BLURAY_TITLE_TOP_MENU 0
#define BLURAY_POLL_TIME_S 0.010
#define BLURAY_NAV_EOF_POLLS 100
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))
#define MP_VERBOSE(...) ((void)0)
#define MP_WARN(...) ((void)0)
#define MP_ERR(...) ((void)0)
#define MP_DBG(...) ((void)0)
#define MP_TRACE(...) ((void)0)
#define atomic_load(p) (*(p))
#define STATUS_EOF 2
#define AT_END_OF_FILE 1

typedef struct { int pid; } BLURAY_STREAM_INFO;
typedef struct {
    int audio_stream_count, pg_stream_count;
    BLURAY_STREAM_INFO *audio_streams, *pg_streams;
} BLURAY_CLIP_INFO;
typedef struct {
    int clip_count, angle_count;
    BLURAY_CLIP_INFO *clips;
} BLURAY_TITLE_INFO;
typedef struct { uint32_t event, param; } BD_EVENT;
typedef struct { bool bdj; } BLURAY_TITLE;
typedef struct {
    unsigned num_titles;
    const BLURAY_TITLE *const *titles;
    const BLURAY_TITLE *first_play, *top_menu;
} BLURAY_DISC_INFO;
struct scripted_read { BD_EVENT event; int bytes; unsigned repeats; };
typedef struct {
    uint64_t position;
    struct scripted_read reads[12];
    unsigned count, next, payload_reads, input_calls;
    int input_result;
} BLURAY;
struct bd_overlay_plane { bool visible; int disp_w, disp_h; };
struct bluray_priv_s {
    BLURAY *bd;
    int vm_lock, overlay_lock;
    bool hdmv_mode, menu_supported, data_delivered, still_active;
    bool read_pos_known, resync_owed, transition_pending, failed, bdj_title;
    int still_duration, num_titles, current_angle, current_title, current_playlist;
    int audio_stream_num, sub_stream_num;
    bool sub_visible;
    uint32_t still_id, discontinuity_id, nav_change_id, *title_to_playlist;
    uint64_t next_read_pos;
    BLURAY_TITLE_INFO *title_info;
    struct bd_overlay_plane ig, pg;
};
struct stream_nav_state {
    bool nav_active, menu_supported, menu_active, overlay_visible, still_active;
    bool drain_pending, transition_pending, no_audio, sub_visible, failed;
    int still_duration, src_w, src_h, active_audio_id, active_sub_id, angle, num_angles;
    uint32_t still_id, change_id, discontinuity_id;
};
struct stream_nav_cmd { int action, x, y; };
typedef struct stream { struct bluray_priv_s *priv; bool *cancel; } stream_t;
struct MPContext {
    stream_t *stream;
    struct { int type; } seek;
    bool paused, disc_nav_still_frame;
    void *video_out, *ao_chain;
    struct { bool is_coverart; } *vo_chain;
    int audio_status, video_status, stop_play;
};

static struct MPContext *active_player;
static bool observe, unexpected_eof;
static bool cancel_on_transition;
static bool cancel_on_read;
static bool cancel_on_wait;
static unsigned polls, reads_during_drain;
static void poll_player(void);
static int bluray_stream_control_locked(stream_t *s, int cmd, void *arg);

static void mp_mutex_lock(int *lock) { assert(*lock == 0); *lock = 1; }
static void mp_mutex_unlock(int *lock)
{
    assert(*lock == 1);
    *lock = 0;
    if (cancel_on_transition && active_player->stream->priv->transition_pending)
        *active_player->stream->cancel = true;
    if (observe && lock == &active_player->stream->priv->overlay_lock) {
        observe = false;
        poll_player();
        observe = true;
    }
}
static bool mp_cancel_test(bool *cancel) { return *cancel; }
static void mp_cancel_wait(bool *cancel, double timeout)
{
    assert(timeout == BLURAY_POLL_TIME_S);
    if (cancel_on_wait)
        *cancel = true;
}
static uint64_t bd_tell(BLURAY *bd) { return bd->position; }
const BLURAY_DISC_INFO *bd_get_disc_info(BLURAY *bd)
{
    (void)bd;
    static const BLURAY_TITLE bdj = {.bdj = true}, hdmv = {0};
    static const BLURAY_TITLE *const titles[] = {&bdj, &bdj, &hdmv};
    static const BLURAY_DISC_INFO info = {
        .num_titles = 2, .titles = titles, .first_play = &bdj, .top_menu = &bdj,
    };
    return &info;
}
static int bd_get_current_title(BLURAY *bd) { (void)bd; return 1; }
static BLURAY_TITLE_INFO *bd_get_playlist_info(BLURAY *bd, int playlist, int angle)
{
    (void)bd; (void)playlist; (void)angle;
    return NULL;
}
static void bd_free_title_info(BLURAY_TITLE_INFO *info) { (void)info; }
static int bd_read_ext(BLURAY *bd, void *buf, int len, BD_EVENT *event)
{
    if (active_player->stream->priv->resync_owed)
        reads_during_drain++;
    if (bd->next == bd->count) {
        *event = (BD_EVENT){0};
        return 0;
    }
    struct scripted_read read = bd->reads[bd->next];
    if (read.bytes > 0 && !len) {
        *event = (BD_EVENT){0};
        return 0; // A zero-length libbluray read polls without consuming payload.
    }
    if (read.repeats > 1)
        bd->reads[bd->next].repeats--;
    else
        bd->next++;
    *event = read.event;
    if (cancel_on_read)
        *active_player->stream->cancel = true;
    if (read.bytes > 0) {
        assert(read.bytes <= len);
        memset(buf, 0xa5, (size_t)read.bytes);
        bd->position += read.bytes;
        bd->payload_reads++;
    }
    return read.bytes;
}
static int bd_user_input(BLURAY *bd, int64_t pts, uint32_t key)
{
    assert(pts == -1 && key != BD_VK_NONE);
    bd->input_calls++;
    return bd->input_result;
}
static int bd_menu_call(BLURAY *bd, int64_t pts) { (void)bd; (void)pts; return 1; }
static int bd_mouse_select(BLURAY *bd, int64_t pts, int x, int y)
{
    (void)bd; (void)pts; (void)x; (void)y;
    return 0;
}
static bool start_hdmv_navigation(stream_t *s) { (void)s; return true; }
static bool play_title(struct bluray_priv_s *b, uint32_t title)
{
    (void)b; (void)title;
    return true;
}
#define stream_nav_action_activates(action) ((action) >= STREAM_NAV_SELECT)
static struct stream *disc_nav_get_stream(struct MPContext *mpctx) { return mpctx->stream; }
static int stream_control(stream_t *s, int cmd, void *arg)
{
    return bluray_stream_control_locked(s, cmd, arg);
}
static bool vo_has_frame(void *vo) { (void)vo; return false; }

#include "disc-streams-under-test.h"

static void poll_player(void)
{
    struct stream_nav_state nav = {0};
    stream_control(active_player->stream, STREAM_CTRL_GET_NAV_STATE, &nav);
    active_player->disc_nav_still_frame = nav.still_active;
    handle_eof(active_player);
    polls++;
    unexpected_eof |= active_player->stop_play == AT_END_OF_FILE;
}
struct fixture {
    BLURAY bd;
    struct bluray_priv_s priv;
    stream_t stream;
    bool cancelled;
    struct MPContext player;
};
static void init(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->bd.position = 192;
    f->priv = (struct bluray_priv_s){
        .bd = &f->bd, .hdmv_mode = true, .data_delivered = true,
        .still_active = true, .still_duration = 10, .still_id = 3,
        .discontinuity_id = 4, .read_pos_known = true, .next_read_pos = 192,
    };
    f->stream = (stream_t){.priv = &f->priv, .cancel = &f->cancelled};
    f->player = (struct MPContext){
        .stream = &f->stream, .audio_status = STATUS_EOF,
        .video_status = STATUS_EOF, .ao_chain = &f->bd,
    };
    active_player = &f->player;
    observe = unexpected_eof = false;
    cancel_on_transition = false;
    cancel_on_read = false;
    cancel_on_wait = false;
    polls = reads_during_drain = 0;
}
static void add(struct fixture *f, int event, int param, int bytes)
{
    assert(f->bd.count < 12);
    f->bd.reads[f->bd.count++] = (struct scripted_read){
        .event = {(uint32_t)event, (uint32_t)param}, .bytes = bytes,
    };
}
static void select_button(struct fixture *f)
{
    struct stream_nav_cmd cmd = {.action = STREAM_NAV_SELECT};
    assert(stream_control(&f->stream, STREAM_CTRL_NAV_CMD, &cmd) == STREAM_OK);
}
static void test_accepted_button_without_jump(void)
{
    struct fixture f;
    init(&f);
    select_button(&f);
    poll_player();
    assert(!unexpected_eof);
    assert(f.priv.still_active && f.priv.still_id == 3);
    assert(f.priv.discontinuity_id == 4 && !f.priv.resync_owed);
    add(&f, BD_EVENT_STILL_TIME, 10, 0);
    char buf[192];
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
    assert(f.priv.still_id == 3 && f.priv.still_duration == 10);
    assert(!f.priv.transition_pending && !f.priv.resync_owed);
    unsigned edition = 0; // The synthetic Disc Menu edition.
    assert(stream_control(&f.stream, STREAM_CTRL_SET_CURRENT_TITLE, &edition) == STREAM_OK);
    assert(f.priv.still_active && f.priv.still_id == 3);
    assert(f.priv.discontinuity_id == 4 && !f.priv.resync_owed);
}
static void test_jump(bool command, int first_event)
{
    struct fixture f;
    init(&f);
    if (command)
        select_button(&f);
    add(&f, first_event, 0, 0);
    add(&f, BD_EVENT_TITLE, 1, 0);
    add(&f, BD_EVENT_PLAYLIST, 77, 0);
    add(&f, BD_EVENT_NONE, 0, 192);
    char buf[192];
    observe = true;
    int n = bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf));
    observe = false;
    poll_player();
    assert(n == 0 && !unexpected_eof && polls > 2);
    assert(f.priv.resync_owed && !f.priv.transition_pending);
    assert(!f.priv.still_active && f.bd.payload_reads == 0);
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
    assert(reads_during_drain == 0 && f.bd.payload_reads == 0);
    // Model the existing seek ACK after the player reset its drained chains.
    f.player.audio_status = f.player.video_status = 0;
    f.priv.resync_owed = false;
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 192);
    assert(f.bd.payload_reads == 1 && (unsigned char)buf[0] == 0xa5);
}
static void test_still_release(void)
{
    struct fixture f;
    init(&f);
    add(&f, BD_EVENT_STILL, 0, 0);
    add(&f, BD_EVENT_NONE, 0, 192);
    char buf[192];
    observe = true;
    int n = bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf));
    observe = false;
    poll_player();
    assert(n == 0 && !unexpected_eof && f.priv.resync_owed);
    assert(f.priv.discontinuity_id == 5 && !f.priv.still_active);
    assert(f.bd.payload_reads == 0);
}
static void test_plain_title_and_startup(void)
{
    for (int interactive = 0; interactive < 2; interactive++) {
        struct fixture f;
        init(&f);
        f.priv.hdmv_mode = interactive != 0;
        f.priv.data_delivered = !interactive;
        f.priv.read_pos_known = false;
        add(&f, BD_EVENT_TITLE, 1, 0);
        add(&f, BD_EVENT_PLAYLIST, 77, 0);
        add(&f, BD_EVENT_NONE, 0, 192);
        char buf[192];
        assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 192);
        assert(!f.priv.transition_pending && !f.priv.resync_owed);
    }
}
static void test_title_selection_from_still(void)
{
    struct fixture f;
    init(&f);
    f.priv.num_titles = 2;
    unsigned title = 1;
    assert(stream_control(&f.stream, STREAM_CTRL_SET_CURRENT_TITLE, &title) == STREAM_OK);
    poll_player();
    assert(!unexpected_eof && !f.priv.still_active && f.priv.resync_owed);
    assert(f.priv.discontinuity_id == 5 && !f.priv.read_pos_known);
}
static void test_end_of_title_seek(void)
{
    struct fixture f;
    init(&f);
    add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
    add(&f, BD_EVENT_SEEK, 0, 0);
    add(&f, BD_EVENT_NONE, 0, 192);
    char buf[192];
    observe = true;
    int n = bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf));
    observe = false;
    assert(n == 0 && !unexpected_eof && f.priv.resync_owed);
    assert(!f.priv.transition_pending && f.bd.payload_reads == 0);
}
static void test_cancelled_transition(void)
{
    struct fixture f;
    init(&f);
    add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
    cancel_on_transition = true;
    char buf[192];
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
    assert(f.cancelled && !f.priv.transition_pending);
    init(&f);
    add(&f, BD_EVENT_NONE, 0, -1);
    cancel_on_read = true;
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == -1);
    assert(f.cancelled && !f.priv.failed && !f.priv.transition_pending);
}
static void test_delayed_bdj_playlist(void)
{
    struct fixture f;
    init(&f);
    f.priv.still_active = false;
    f.priv.data_delivered = false;
    f.priv.read_pos_known = false;
    add(&f, BD_EVENT_TITLE, 1, 0);
    add(&f, BD_EVENT_NONE, 0, 192);
    char buf[192];
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 192);
    f.bd.count = f.bd.next = 0;
    add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
    f.bd.reads[0].repeats = BLURAY_NAV_EOF_POLLS * 3;
    add(&f, BD_EVENT_PLAYLIST, 77, 0);
    add(&f, BD_EVENT_NONE, 0, 192);
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
    poll_player();
    assert(!unexpected_eof && f.priv.resync_owed);
    assert(f.bd.next == 2 && f.bd.payload_reads == 1);
    f.player.audio_status = f.player.video_status = 0;
    f.priv.resync_owed = false;
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 192);
    assert(f.bd.payload_reads == 2);
}
static void test_bdj_title_classification(void)
{
    const struct { int title; bool bdj; } cases[] = {
        {BLURAY_TITLE_FIRST_PLAY, true}, {BLURAY_TITLE_TOP_MENU, true},
        {1, true}, {2, false}, {3, false},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fixture f;
        init(&f);
        f.priv.still_active = false;
        f.priv.data_delivered = false;
        f.priv.read_pos_known = false;
        f.priv.bdj_title = !cases[i].bdj;
        add(&f, BD_EVENT_TITLE, cases[i].title, 0);
        add(&f, BD_EVENT_NONE, 0, 192);
        char buf[192];
        assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 192);
        assert(f.priv.bdj_title == cases[i].bdj);
        if (f.priv.bdj_title) {
            f.priv.num_titles = 2;
            unsigned title = 1;
            assert(stream_control(&f.stream, STREAM_CTRL_SET_CURRENT_TITLE, &title) == STREAM_OK);
            assert(!f.priv.bdj_title);
        } else {
            f.bd.count = f.bd.next = 0;
            add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
            assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
            assert(!f.priv.transition_pending && !f.priv.resync_owed);
        }
    }
}
static void test_cancelled_bdj_wait(void)
{
    struct fixture f;
    init(&f);
    f.priv.still_active = false;
    f.priv.bdj_title = true;
    add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
    cancel_on_wait = true;
    char buf[192];
    assert(bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf)) == 0);
    assert(f.cancelled && !f.priv.transition_pending && !f.priv.failed);
}
static void test_terminal_eof_and_error(void)
{
    for (int error = 0; error < 4; error++) {
        struct fixture f;
        init(&f);
        if (error < 2)
            add(&f, BD_EVENT_END_OF_TITLE, 0, 0);
        if (error)
            add(&f, error == 3 ? BD_EVENT_NONE : BD_EVENT_ERROR, 1,
                error == 3 ? -1 : 0);
        char buf[192];
        observe = error != 0;
        int n = bluray_stream_fill_buffer(&f.stream, buf, sizeof(buf));
        observe = false;
        assert(n == (error ? -1 : 0));
        assert(!f.priv.transition_pending && !f.priv.resync_owed);
        assert(f.priv.failed == (error != 0));
        struct stream_nav_state nav = {0};
        stream_control(&f.stream, STREAM_CTRL_GET_NAV_STATE, &nav);
        assert(nav.failed == (error != 0));
        poll_player();
        // The demuxer publishes the fatal error after the stream callback
        // returns. The player must not terminate as ordinary EOF in between.
        assert(f.player.stop_play == (error ? 0 : AT_END_OF_FILE));
        assert(unexpected_eof == (error == 0));
    }
}
int main(void)
{
    test_accepted_button_without_jump();
    test_jump(true, BD_EVENT_TITLE);
    test_jump(false, BD_EVENT_END_OF_TITLE);
    test_jump(false, BD_EVENT_STILL);
    test_still_release();
    test_plain_title_and_startup();
    test_title_selection_from_still();
    test_end_of_title_seek();
    test_cancelled_transition();
    test_delayed_bdj_playlist();
    test_bdj_title_classification();
    test_cancelled_bdj_wait();
    test_terminal_eof_and_error();
    puts("mpv Blu-ray navigation: passed (no-op, event publication, resync, payload, terminal EOF)");
    return 0;
}
