/* Exercise production DVD read/control serialization with a paused STOP event.
 * The VM is scripted; disc parsing and Android playback remain separate gates. */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL _Thread_local
#endif

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK mp_mutex;
typedef HANDLE event_t;
typedef HANDLE thread_t;
typedef DWORD (WINAPI *thread_fn)(void *);
#define THREAD_ENTRY DWORD WINAPI
static void mutex_init(mp_mutex *m) { InitializeSRWLock(m); }
static void mutex_destroy(mp_mutex *m) { (void)m; }
static void mutex_lock(mp_mutex *m) { AcquireSRWLockExclusive(m); }
static void mutex_unlock(mp_mutex *m) { ReleaseSRWLockExclusive(m); }
static bool mutex_trylock(mp_mutex *m) { return TryAcquireSRWLockExclusive(m) != 0; }
static void event_init(event_t *e) { *e = CreateEvent(NULL, TRUE, FALSE, NULL); assert(*e); }
static void event_destroy(event_t *e) { assert(CloseHandle(*e)); }
static void event_set(event_t *e) { assert(SetEvent(*e)); }
static void event_wait(event_t *e) { assert(WaitForSingleObject(*e, 5000) == WAIT_OBJECT_0); }
static thread_t start_thread(thread_fn fn, void *arg)
{
    HANDLE thread = CreateThread(NULL, 0, fn, arg, 0, NULL);
    assert(thread);
    return thread;
}
static void join_thread(thread_t thread)
{
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
    assert(CloseHandle(thread));
}
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t mp_mutex;
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; bool signaled; } event_t;
typedef pthread_t thread_t;
typedef void *(*thread_fn)(void *);
#define THREAD_ENTRY void *
static void mutex_init(mp_mutex *m) { assert(pthread_mutex_init(m, NULL) == 0); }
static void mutex_destroy(mp_mutex *m) { assert(pthread_mutex_destroy(m) == 0); }
static void mutex_lock(mp_mutex *m) { assert(pthread_mutex_lock(m) == 0); }
static void mutex_unlock(mp_mutex *m) { assert(pthread_mutex_unlock(m) == 0); }
static bool mutex_trylock(mp_mutex *m) { return pthread_mutex_trylock(m) == 0; }
static void event_init(event_t *e)
{
    mutex_init(&e->mutex);
    assert(pthread_cond_init(&e->cond, NULL) == 0);
    e->signaled = false;
}
static void event_destroy(event_t *e)
{
    assert(pthread_cond_destroy(&e->cond) == 0);
    mutex_destroy(&e->mutex);
}
static void event_set(event_t *e)
{
    mutex_lock(&e->mutex);
    e->signaled = true;
    assert(pthread_cond_broadcast(&e->cond) == 0);
    mutex_unlock(&e->mutex);
}
static void event_wait(event_t *e)
{
    struct timespec deadline;
    assert(timespec_get(&deadline, TIME_UTC) == TIME_UTC);
    deadline.tv_sec += 5;
    mutex_lock(&e->mutex);
    while (!e->signaled)
        assert(pthread_cond_timedwait(&e->cond, &e->mutex, &deadline) == 0);
    mutex_unlock(&e->mutex);
}
static thread_t start_thread(thread_fn fn, void *arg)
{
    pthread_t thread;
    assert(pthread_create(&thread, NULL, fn, arg) == 0);
    return thread;
}
static void join_thread(thread_t thread) { assert(pthread_join(thread, NULL) == 0); }
#endif

#define STREAM_OK 1
#define STREAM_ERROR -1
#define STREAM_CTRL_NAV_STILL_SKIP 6
#define STREAM_CTRL_NAV_DRAIN_ACK 7
#define DVD_MENU_Root 1
#define DVD_MENU_Title 2
#define STREAM_UNSUPPORTED -2
#define STREAM_CTRL_NAV_CMD 1
#define STREAM_CTRL_GET_NAV_STATE 2
#define STREAM_CTRL_GET_NUM_TITLES 3
#define STREAM_CTRL_GET_CURRENT_TITLE 4
#define STREAM_CTRL_GET_CURRENT_TIME 5
#define DVDNAV_STATUS_OK 1
#define DVDNAV_BLOCK_OK 1
#define DVDNAV_NOP 2
#define DVDNAV_STOP 3
#define DVDNAV_STILL_FRAME 4
#define DVDNAV_WAIT 5
#define DVD_TIME_TO_S(t) ((t) / 90000.0)
#define MP_VERBOSE(...) ((void)0)
#define MP_FATAL(...) ((void)0)
#define MP_ERR(...) ((void)0)
#define MP_DBG(...) ((void)0)
#define MP_TRACE(s, format, name, event) ((void)(s), (void)(format), (void)(name), (void)(event))
#define LOOKUP_NAME(table, event) ((void)(event), "scripted event")

struct highlight { int unused; };
struct stream_nav_state {
    uint64_t dvd_generation;
    int active_audio_logical, active_sub_logical;
    uint32_t change_id, discontinuity_id, still_id;
    bool nav_active, menu_supported, no_audio, menu_active, still_active, failed;
    bool drain_pending, drain_immediate, drain_user_activation, sub_visible;
    int still_duration, src_w, src_h, active_audio_id, active_sub_id, angle, num_angles;
    struct highlight hl;
};
struct stream_nav_still_skip { uint32_t id; };
struct stream_nav_cmd { int action; };
typedef struct { int reads; bool fail; } dvdnav_t;
typedef struct { int length; } dvdnav_still_event_t;
struct stream_dvd_streams {
    uint64_t generation;
    uint32_t discontinuity_id;
    struct {int id;} audio[8];
    int active_audio, active_sub;
    bool sub_visible;
};
#define STREAM_CTRL_GET_DVD_STREAMS 1234
#define STREAM_CTRL_SET_CURRENT_TITLE 1235
#define STREAM_CTRL_SEEK_TO_TIME 1236
#define STREAM_CTRL_SET_ANGLE 1237
struct priv {
    struct stream_dvd_streams dvd_streams;
    bool dvd_catalog_dirty, dvd_selection_dirty;
    mp_mutex vm_lock, lock;
    dvdnav_t *dvdnav;
    bool terminal_stop, pending_drain, wait_pending, still_active, nav_pts_valid;
    bool at_boundary, drain_enabled, drain_immediate, drain_user_activation;
    bool activation_pending_jump, failed, menu_support_known, menu_supported, sub_visible;
    int src_w, src_h, audio_physical, audio_logical, sub_physical;
    uint32_t still_id, nav_change_id;
    struct highlight hl;
    int still_duration, num_titles, current_title;
    int64_t current_time;
    uint32_t discontinuity_id;
    struct stream_nav_state nav_state;
};
typedef struct { struct priv *priv; bool *cancel; } stream_t;
static bool mp_cancel_test(bool *cancel) { return cancel && *cancel; }
static int skip_result = DVDNAV_STATUS_OK, skip_calls;
static int wait_result = DVDNAV_STATUS_OK, wait_calls;
static int dvdnav_still_skip(dvdnav_t *dvd) { (void)dvd; skip_calls++; return skip_result; }
static int dvdnav_wait_skip(dvdnav_t *dvd) { (void)dvd; wait_calls++; return wait_result; }
static event_t event_fetched, allow_event, control_attempted;
static THREAD_LOCAL bool control_thread;
static mp_mutex *observed_vm;
static bool control_was_blocked, pause_stop;

static void mp_mutex_lock(mp_mutex *m)
{
    if (control_thread && m == observed_vm) {
        bool acquired = mutex_trylock(m);
        if (acquired)
            mutex_unlock(m);
        control_was_blocked = !acquired;
        event_set(&control_attempted);
    }
    mutex_lock(m);
}
static void mp_mutex_unlock(mp_mutex *m) { mutex_unlock(m); }
static void pause_event_handler(void)
{
    if (pause_stop) {
        event_set(&event_fetched);
        event_wait(&allow_event);
    }
}
static int dvdnav_get_next_block(dvdnav_t *dvd, void *buf, int *event, int *len)
{
    (void)buf;
    dvd->reads++;
    *event = DVDNAV_STOP;
    *len = 0;
    return dvd->fail ? 0 : DVDNAV_STATUS_OK;
}
typedef int dvdnav_status_t;
static int dvdnav_get_next_block_with_wait(dvdnav_t *dvd, void *buf, int *event, int *len)
{ return dvdnav_get_next_block(dvd, buf, event, len); }
static int dvdnav_get_position(dvdnav_t *dvd, uint32_t *position, uint32_t *length)
{
    (void)dvd;
    *position = 0;
    *length = 1;
    return DVDNAV_STATUS_OK;
}
static bool do_nav_cmd(stream_t *s, struct stream_nav_cmd *cmd, bool *menu_jump)
{
    (void)s;
    *menu_jump = cmd->action == 1;
    return *menu_jump;
}
static int dvdnav_current_title_info(dvdnav_t *dvd, int32_t *title, int32_t *part)
{ (void)dvd; *title = 1; *part = 1; return DVDNAV_STATUS_OK; }
static int64_t dvdnav_get_current_time(dvdnav_t *dvd) { (void)dvd; return 0; }
static void dvdnav_get_angle_info(dvdnav_t *dvd, int32_t *angle, int32_t *count)
{ (void)dvd; *angle = 1; *count = 1; }
#define DVD_AUDIO_STREAM 0
static int dvdnav_get_number_of_streams(dvdnav_t *dvd, int type)
{ (void)dvd; assert(type == DVD_AUDIO_STREAM); return 0; }
static int dvdnav_menu_available(dvdnav_t *dvd, int menu)
{ (void)dvd; (void)menu; return DVDNAV_STATUS_OK; }
static void refresh_video_resolution(struct priv *p) { p->src_w = 720; p->src_h = 480; }
static bool in_menu_domain(dvdnav_t *dvd) { (void)dvd; return false; }
static int dvd_audio_to_substream(struct priv *p, int logical, int stream)
{ (void)p; (void)logical; return stream; }

static event_t catalog_entered, allow_catalog;
static bool pause_catalog;
static struct stream_dvd_streams read_dvd_streams(struct priv *p)
{
    if (pause_catalog) {
        event_set(&catalog_entered);
        event_wait(&allow_catalog);
    }
    return p->dvd_streams;
}
static void read_dvd_selection(struct priv *p, struct stream_dvd_streams *streams)
{ (void)p; (void)streams; }
static bool same_dvd_streams(const struct stream_dvd_streams *a,
                             const struct stream_dvd_streams *b)
{ (void)a; (void)b; return true; }
#include "disc-streams-under-test.h"

static THREAD_ENTRY read_stop(void *arg)
{
    char buffer[2048];
    assert(fill_buffer(arg, buffer, sizeof(buffer)) == 0);
    return 0;
}
static THREAD_ENTRY open_menu(void *arg)
{
    struct stream_nav_cmd cmd = {.action = 1};
    control_thread = true;
    assert(control(arg, STREAM_CTRL_NAV_CMD, &cmd) == STREAM_OK);
    return 0;
}

static THREAD_ENTRY publish_catalog(void *arg)
{
    struct priv *p = arg;
    mp_mutex_lock(&p->vm_lock);
    publish_state(p);
    mp_mutex_unlock(&p->vm_lock);
    return 0;
}

int main(void)
{
    dvdnav_t dvd = {0};
    struct priv p = {.dvdnav = &dvd, .discontinuity_id = 3,
                     .nav_state = {.change_id = 3}, .drain_enabled = true};
    stream_t s = {.priv = &p};
    mutex_init(&p.vm_lock);
    mutex_init(&p.lock);
    event_init(&event_fetched);
    event_init(&allow_event);
    event_init(&control_attempted);
    observed_vm = &p.vm_lock;
    pause_stop = true;

    thread_t reader = start_thread(read_stop, &s);
    event_wait(&event_fetched);
    // Player queries read the last published snapshot while VM I/O is held.
    struct stream_nav_state snapshot = {0};
    assert(control(&s, STREAM_CTRL_GET_NAV_STATE, &snapshot) == STREAM_OK);
    assert(snapshot.discontinuity_id == 0 && !snapshot.failed);
    thread_t command = start_thread(open_menu, &s);
    event_wait(&control_attempted);
    assert(control_was_blocked);
    event_set(&allow_event);
    join_thread(reader);
    join_thread(command);
    // STOP is applied first. The successful jump must clear terminal EOF.
    assert(!p.terminal_stop && p.pending_drain && p.drain_immediate);
    assert(p.discontinuity_id == 4 && dvd.reads == 1);
    assert(control(&s, STREAM_CTRL_GET_NAV_STATE, &snapshot) == STREAM_OK);
    assert(snapshot.discontinuity_id == 4 && !snapshot.failed);

    pause_stop = false;
    char buffer[2048];
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    assert(dvd.reads == 1); // No read crosses an unacknowledged drain.
    p.pending_drain = false;
    p.terminal_stop = true;
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    assert(dvd.reads == 1); // Terminal EOF does not restart First Play.
    p.terminal_stop = false;
    dvd.fail = true;
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    assert(dvd.reads == 2);
    assert(control(&s, STREAM_CTRL_GET_NAV_STATE, &snapshot) == STREAM_OK);
    assert(snapshot.failed && p.failed && !snapshot.still_active && !snapshot.drain_pending);
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0 && dvd.reads == 2);
    assert(mutex_trylock(&p.vm_lock)); // Failed read releases VM ownership.
    mutex_unlock(&p.vm_lock);

    // A matched still skip failure is fatal, while a stale timer is not.
    p.failed = false;
    p.still_active = true;
    p.still_duration = 1;
    p.still_id = 7;
    skip_result = 0;
    struct stream_nav_still_skip skip = {.id = 6};
    assert(control(&s, STREAM_CTRL_NAV_STILL_SKIP, &skip) == STREAM_ERROR);
    assert(!p.failed && skip_calls == 0);
    skip.id = 7;
    assert(control(&s, STREAM_CTRL_NAV_STILL_SKIP, &skip) == STREAM_ERROR);
    assert(control(&s, STREAM_CTRL_GET_NAV_STATE, &snapshot) == STREAM_OK);
    assert(p.failed && snapshot.failed && !snapshot.still_active && skip_calls == 1);
    assert(control(&s, STREAM_CTRL_NAV_STILL_SKIP, &skip) == STREAM_ERROR && skip_calls == 1);

    // Cancellation is an explicit stop, not an I/O failure.
    bool cancelled = true;
    s.cancel = &cancelled;
    p.failed = false;
    p.still_active = true;
    p.still_duration = 1;
    assert(control(&s, STREAM_CTRL_NAV_STILL_SKIP, &skip) == STREAM_ERROR);
    assert(!p.failed && !p.nav_state.failed);
    p.still_active = true;
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    assert(!p.failed && !p.nav_state.failed && !p.still_active);
    s.cancel = NULL;

    // Ordinary STOP remains terminal EOF rather than an error.
    dvd.fail = false;
    assert(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    assert(p.terminal_stop && !p.failed && !p.nav_state.failed);

    // Both forms of WAIT continuation must publish failure and clear the hold.
    p.terminal_stop = false;
    p.wait_pending = p.pending_drain = true;
    wait_result = 0;
    assert(control(&s, STREAM_CTRL_NAV_DRAIN_ACK, NULL) == STREAM_ERROR);
    assert(p.failed && p.nav_state.failed && !p.pending_drain && !p.wait_pending);
    assert(wait_calls == 1);
    assert(control(&s, STREAM_CTRL_NAV_DRAIN_ACK, NULL) == STREAM_OK && wait_calls == 1);
    p.failed = false;
    p.drain_enabled = false;
    assert(process_event(&s, DVDNAV_WAIT, NULL, 0) == 0);
    assert(p.failed && wait_calls == 2);

    p.failed = false;
    dvdnav_still_event_t zero_still = {.length = 0};
    int previous_skips = skip_calls;
    assert(process_event(&s, DVDNAV_STILL_FRAME, &zero_still, sizeof(zero_still)) == 0);
    assert(p.failed && skip_calls == previous_skips + 1 && !p.still_active);

    // Successfully continuing these events remains nonfatal.
    p.failed = false;
    wait_result = skip_result = DVDNAV_STATUS_OK;
    assert(process_event(&s, DVDNAV_WAIT, NULL, 0) == -1 && !p.failed);
    assert(process_event(&s, DVDNAV_STILL_FRAME, &zero_still, sizeof(zero_still)) == -1);
    assert(!p.failed && !p.still_active);

    // A slow native catalog getter must not hold the query publication lock.
    // Compile this same fixture against the earlier publisher to see RED at
    // mutex_trylock; the fixed publisher also services the actual GET control
    // before the native getter is released.
    event_init(&catalog_entered);
    event_init(&allow_catalog);
    p.terminal_stop = p.pending_drain = false;
    p.dvd_catalog_dirty = true;
    pause_catalog = true;
    thread_t publisher = start_thread(publish_catalog, &p);
    event_wait(&catalog_entered);
    assert(mutex_trylock(&p.lock));
    mutex_unlock(&p.lock);
    assert(control(&s, STREAM_CTRL_GET_NAV_STATE, &snapshot) == STREAM_OK);
    event_set(&allow_catalog);
    join_thread(publisher);
    pause_catalog = false;
    event_destroy(&allow_catalog);
    event_destroy(&catalog_entered);

    event_destroy(&control_attempted);
    event_destroy(&allow_event);
    event_destroy(&event_fetched);
    mutex_destroy(&p.lock);
    mutex_destroy(&p.vm_lock);
    puts("mpv DVD serialization: passed (STOP/jump, nonblocking state, drain, errors)");
    return 0;
}
