/* Execute the production probe/drain owners with a boundary reached in probe. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks, failures;
#define CHECK(x) do {checks++;if (!(x)) {failures++;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}} while(0)
#define MP_NOPTS_VALUE (-1e20)
#define STREAM_OK 1
enum {STREAM_CTRL_GET_DISC_NAME,STREAM_CTRL_NAV_DRAIN_ENABLE,STREAM_CTRL_NAV_DRAIN_ACK,
      STREAM_CTRL_GET_NAV_STATE,STREAM_CTRL_GET_TIME_LENGTH,STREAM_CTRL_GET_CURRENT_TIME};
enum demux_check {DEMUX_CHECK_FORCE};
enum {STATUS_PLAYING,STATUS_EOF,MPSEEK_ABSOLUTE,MPSEEK_KEYFRAME,MPSEEK_FLAG_NAV=16};
struct stream_nav_state {bool drain_pending,drain_immediate,drain_user_activation;uint32_t discontinuity_id;};
struct stream_info {const char *name;};
typedef struct stream {struct stream_info *info;struct stream_nav_state nav;
    bool first_payload,drain_enabled,protected_probe;int acks,commands;} stream_t;
struct priv {bool is_cdda,is_dvd,is_dvda,is_bd;double skip_audio_until,reset_base_time;
    uint32_t last_discontinuity_id;void *slave;};
typedef struct demuxer {struct priv *priv;stream_t *stream;int stream_origin,depth;
    void *cancel,*global,*metadata;bool seekable,partially_seekable,no_cache_seeking;double duration;} demuxer_t;
struct demuxer_params {const char *force_format;stream_t *external_stream;int stream_flags,depth;};
#define talloc_zero(c,t) ((void)(c),calloc(1,sizeof(t)))
#define talloc_free free
static void mp_tags_set_str(void *t,const char *k,const char *v) {(void)t;(void)k;(void)v;}
static int stream_control(stream_t *s,int cmd,void *arg) {
    switch(cmd) {
    case STREAM_CTRL_NAV_DRAIN_ENABLE:
        CHECK(s->first_payload);s->drain_enabled=true;s->nav.drain_pending=false;return 1;
    case STREAM_CTRL_NAV_DRAIN_ACK:s->acks++;s->nav.drain_pending=false;return 1;
    case STREAM_CTRL_GET_NAV_STATE:*(struct stream_nav_state *)arg=s->nav;return 1;
    case STREAM_CTRL_GET_TIME_LENGTH:*(double *)arg=4;return 1;
    case STREAM_CTRL_GET_CURRENT_TIME:*(double *)arg=3;return 1;
    default:return -1;
    }
}
static int stream_read_peek(stream_t *s,void *buffer,int length) {
    (void)buffer;CHECK(length==1 && !s->drain_enabled);s->first_payload=true;return 1;
}
static void reset_pts(demuxer_t *d) {(void)d;}
static void *demux_open_url(const char *name,struct demuxer_params *p,void *cancel,void *global) {
    (void)cancel;(void)global;CHECK(!strcmp(name,"-") && !strcmp(p->force_format,"+lavf"));
    stream_t *s=p->external_stream;
    s->protected_probe=s->drain_enabled;
    if (s->drain_enabled) {s->nav.drain_pending=true;s->nav.discontinuity_id++;}
    else s->commands++;
    return s;
}
static void sync_streams(demuxer_t *d) {(void)d;}
static void add_still_stream(demuxer_t *d) {(void)d;}
static void add_stream_chapters(demuxer_t *d) {(void)d;}
static void add_stream_editions(demuxer_t *d) {(void)d;}
static void sync_initial_edition(demuxer_t *d) {(void)d;}
#include "native.h"

struct disc_nav_state {bool drain_was_pending;uint32_t last_drain_disc_id;};
struct MPOpts {bool pause;};
struct MPContext {struct disc_nav_state nav;struct MPOpts *opts;void *demuxer,*ao;
    struct {int type,flags;double amount;} seek;int audio_status,video_status,queues,timeouts;};
static struct disc_nav_state *get_state(struct MPContext *p) {return &p->nav;}
static bool ao_is_playing(void *ao) {return ao!=NULL;}
static void mp_set_timeout(struct MPContext *p,double t) {(void)t;p->timeouts++;}
static void queue_seek(struct MPContext *p,int type,double amount,int exact,int flags) {
    CHECK(exact==MPSEEK_KEYFRAME);p->queues++;p->seek.type=type;p->seek.amount=amount;p->seek.flags=flags;
}
#define MP_VERBOSE(...) ((void)0)
#include "player.h"

int main(void) {
    struct stream_info info={.name="dvdnav"};stream_t stream={.info=&info};
    demuxer_t demux={.stream=&stream};
    CHECK(d_open(&demux,DEMUX_CHECK_FORCE)==0);
    CHECK(stream.protected_probe && stream.nav.drain_pending && stream.commands==0);
    CHECK(stream.acks==0 && demux.priv->last_discontinuity_id==stream.nav.discontinuity_id);
    struct MPOpts opts={.pause=true};struct MPContext p={.opts=&opts,.demuxer=&demux};
    CHECK(check_async_discontinuity(&p,&stream,&stream.nav,true));
    CHECK(p.queues==0 && stream.acks==0);
    p.audio_status=p.video_status=STATUS_EOF;
    p.seek.type=MPSEEK_ABSOLUTE;p.seek.amount=1.25;
    CHECK(check_async_discontinuity(&p,&stream,&stream.nav,true));
    CHECK(p.queues==0 && p.seek.amount==1.25 && p.seek.flags==0);
    p.seek.type=0;
    CHECK(!check_async_discontinuity(&p,&stream,&stream.nav,true));
    CHECK(p.queues==1 && p.seek.flags==MPSEEK_FLAG_NAV && stream.acks==0);
    CHECK(!check_async_discontinuity(&p,&stream,&stream.nav,true));
    CHECK(p.queues==1);
    free(demux.priv);
    printf("DVD probe/drain ownership: %d checks, %d failures\n",checks,failures);
    return failures!=0;
}
