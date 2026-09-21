/* Production mpv stream operations against the complete libdvdnav VM/IFO parser. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dvdread/ifo_read.h>
#include <dvdnav/title_layout.h>
#include "vm/decoder.h"
#include "vm/vm.h"
#include "dvdnav_internal.h"

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define DVD_TIMEBASE 90000
#define DVD_TIME_TO_S(x) ((x) / (double)DVD_TIMEBASE)
#define DVD_TIME_FROM_S(x) ((int64_t)((x) * DVD_TIMEBASE))
#define DVD_SRC_W_DEFAULT 720
#define DVD_SRC_H_DEFAULT 576
#define STREAM_OK 1
#define STREAM_ERROR -1
#define STREAM_UNSUPPORTED -2
#define SEEK_HR 2
#define MP_ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define MPMIN(a,b) ((a)<(b)?(a):(b))
#define LOOKUP_NAME(a,i) "event"
enum stream_type {STREAM_VIDEO, STREAM_AUDIO, STREAM_SUB};
typedef pthread_mutex_t mp_mutex;
#define mp_mutex_lock(x) pthread_mutex_lock(x)
#define mp_mutex_unlock(x) pthread_mutex_unlock(x)
typedef struct stream {void *priv;void *cancel;} stream_t;
struct mp_rect {int x0,y0,x1,y1;};
static int mp_cancel_test(void *cancel) {(void)cancel;return 0;}
static void stream_drop_buffers(stream_t *stream) {(void)stream;}
static void quiet_log(void *object, const char *format, ...) {(void)object;(void)format;}
#define MP_DBG quiet_log
#define MP_TRACE quiet_log
#define MP_VERBOSE quiet_log
#define MP_ERR quiet_log
#define MP_FATAL quiet_log
#define MP_WARN quiet_log
#include "mpv-presented-under-test.h"

static int seek_file(void *context, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(context, (int64_t)offset, SEEK_SET);
#else
    return fseeko(context, (off_t)offset, SEEK_SET);
#endif
}
static int read_file(void *context, void *buffer, int bytes) {
    size_t n = fread(buffer, 1, (size_t)bytes, context);
    return ferror(context) ? -1 : (int)n;
}

static bool read_wait(stream_t *s) {
    unsigned char buffer[2048];
    struct priv *p = s->priv;
    for (unsigned i = 0; i < 40000; i++) {
        int result = fill_buffer(s, buffer, sizeof(buffer));
        if (result == 0) {
            CHECK(!p->failed);
            return p->pending_drain && p->wait_pending;
        }
        CHECK(result == 2048);
    }
    return false;
}

static void test_presented_commands(const char *directory, int branch) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/stream-command-branch.iso", directory);
    FILE *file = fopen(path, "rb");
    CHECK(file != NULL);
    if (!file) return;
    dvdnav_t *dvd = NULL;
    dvdnav_stream_cb cb = {seek_file, read_file, NULL};
    CHECK(dvdnav_open_stream(&dvd, file, &cb) == DVDNAV_STATUS_OK);
    CHECK(dvdnav_set_PGC_positioning_flag(dvd, 1) == DVDNAV_STATUS_OK);
    CHECK(dvdnav_program_play(dvd, 1, 2, 2) == DVDNAV_STATUS_OK);
    struct priv p = {.dvdnav=dvd, .audio_physical=-1, .audio_logical=-1,
                     .sub_physical=-1};
    pthread_mutex_init(&p.vm_lock, NULL);
    pthread_mutex_init(&p.lock, NULL);
    stream_t s = {.priv=&p};
    unsigned char buffer[2048];
    CHECK(fill_buffer(&s, buffer, sizeof(buffer)) == 2048);
    CHECK(control(&s, STREAM_CTRL_NAV_DRAIN_ENABLE, NULL) == STREAM_OK);
    uint64_t generation = p.dvd_streams.generation;
    bool waiting = read_wait(&s);
    CHECK(waiting && !p.terminal_stop && dvd->vm->state.pgcN == 2);
    CHECK(dvd->vm->state.registers.GPRM[15] == 0);
    if (!waiting) goto end;
    CHECK(p.dvd_streams.generation == generation);
    CHECK(p.dvd_streams.audio[0].id == 0x80 && p.dvd_streams.audio[6].id == 0x80);
    CHECK(!strcmp(p.dvd_streams.audio[0].lang,"en") && !strcmp(p.dvd_streams.audio[6].lang,"ja"));
    struct stream_dvd_select selection = {.generation=generation, .type=STREAM_AUDIO, .logical=6};
    CHECK(control(&s, STREAM_CTRL_SET_DVD_STREAM, &selection) == STREAM_OK);
    CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 6);
    CHECK(p.nav_state.active_audio_logical == 6 && p.nav_state.drain_pending);
    selection.type=STREAM_SUB;selection.logical=1;
    CHECK(control(&s, STREAM_CTRL_SET_DVD_STREAM, &selection) == STREAM_OK);
    CHECK(p.nav_state.active_sub_logical == 1 && p.nav_state.sub_visible);
    selection.logical=-1;
    CHECK(control(&s, STREAM_CTRL_SET_DVD_STREAM, &selection) == STREAM_OK);
    CHECK(p.nav_state.active_sub_logical == 1 && !p.nav_state.sub_visible);
    CHECK(dvd->vm->state.SPST_REG == 1);
    CHECK(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    CHECK(dvd->vm->state.registers.GPRM[15] == 0);
    double seek[] = {6, SEEK_HR};
    CHECK(control(&s, STREAM_CTRL_SEEK_TO_TIME, seek) == STREAM_OK);
    CHECK(!p.pending_drain && !p.wait_pending);
    CHECK(fill_buffer(&s, buffer, sizeof(buffer)) == 2048);
    CHECK(dvd->vm->state.pgcN == 2 && dvd->vm->state.registers.GPRM[15] == 0);
    CHECK(dvdnav_get_active_logical_stream(dvd, DVD_AUDIO_STREAM) == 6);
    CHECK(read_wait(&s));
    generation=p.dvd_streams.generation;
    selection=(struct stream_dvd_select){.generation=generation, .type=STREAM_AUDIO,
                                        .logical=branch==0 ? 6 : 0};
    CHECK(control(&s, STREAM_CTRL_SET_DVD_STREAM, &selection) == STREAM_OK);
    if (branch == 1) {
        selection.type=STREAM_SUB;selection.logical=1;
        CHECK(control(&s, STREAM_CTRL_SET_DVD_STREAM, &selection) == STREAM_OK);
    }
    CHECK(dvd->vm->state.registers.GPRM[15] == 0);
    CHECK(control(&s, STREAM_CTRL_NAV_DRAIN_ACK, NULL) == STREAM_OK);
    CHECK(control(&s, STREAM_CTRL_NAV_DRAIN_ACK, NULL) == STREAM_OK);
    int result=fill_buffer(&s, buffer, sizeof(buffer));
    CHECK(dvd->vm->state.registers.GPRM[15] == 1);
    if (branch == 2) {
        CHECK(result == 0 && p.terminal_stop && !p.pending_drain);
        CHECK(fill_buffer(&s, buffer, sizeof(buffer)) == 0);
    } else {
        CHECK(result == 2048 && dvd->vm->state.pgcN == 1);
        CHECK(fill_buffer(&s, buffer, sizeof(buffer)) == 2048);
    }
    CHECK(dvd->vm->state.registers.GPRM[15] == 1);
end:
    pthread_mutex_destroy(&p.lock);
    pthread_mutex_destroy(&p.vm_lock);
    dvdnav_close(dvd);
    fclose(file);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    test_presented_commands(argv[1],0);
    test_presented_commands(argv[1],1);
    test_presented_commands(argv[1],2);
    printf("mpv presented VM: %u checks, %u failures\n",checks,failures);
    return failures != 0;
}
