#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
enum stream_type { STREAM_VIDEO, STREAM_AUDIO, STREAM_SUB };
#include "dvd-logical-contracts.h"
static int checks,failed;
#define CHECK(x) do { checks++;if (!(x)) { failed++;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
struct mp_codec_params {const char *codec;void *first_packet,*decoder,*decoder_desc;};
struct sh_stream {enum stream_type type;int index,demuxer_id;struct mp_codec_params *codec;
    bool dvd_nav_stream,default_track,selected,absent;int dvd_nav_logical,dvd_nav_id;
    _Atomic uint64_t dvd_nav_generation;char *lang;void *ds;};
struct demux_packet {int stream,len;double pts,dts,duration;bool keyframe;int64_t pos;
    unsigned char buffer[20],side_data[4];};
struct priv {struct sh_stream **outer_streams;int num_outer_streams;
    struct stream_dvd_streams dvd_streams;struct sh_stream *dvd_audio[8],*dvd_subs[32];
    struct pending_sub {struct demux_packet *pkt;struct sh_stream *sh;uint64_t seq;} *pending_subs;
    int num_pending_subs;uint64_t av_map_seq;};
struct demuxer;
struct demux_internal { struct demuxer *d_thread;int lock,events;void (*wakeup_cb)(void *);void *wakeup_cb_ctx;};
struct demuxer {struct demux_internal *in;struct priv *priv;void *packet_pool;int streams;};
#define talloc_free(p) ((void)(p))
#define talloc_steal(p,x) ((void)(p),(void)(x))
static char *copy_string(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);memcpy(p,s,n);return p;}
#define talloc_strdup(p,s) ((void)(p),copy_string(s))
#define MP_TARRAY_APPEND(p,a,n,...) do {(void)(p);(a)=realloc((a),(size_t)((n)+1)*sizeof(*(a)));(a)[(n)++]=(__VA_ARGS__);}while(0)
static struct sh_stream *demux_alloc_sh_stream(enum stream_type t){struct sh_stream *s=calloc(1,sizeof(*s));s->type=t;s->codec=calloc(1,sizeof(*s->codec));return s;}
static void demux_add_sh_stream(struct demuxer *d,struct sh_stream *s){s->index=d->streams++;}
static bool demux_stream_is_selected(struct sh_stream *s){return s->selected;}
static void set_dvd_sub_palette(struct demuxer *d,struct sh_stream *s){(void)d;(void)s;}
static struct demux_packet *demux_copy_packet(void *pool,struct demux_packet *src){(void)pool;struct demux_packet *d=malloc(sizeof(*d));*d=*src;return d;}
#include "native.h"
typedef struct demuxer demuxer_t;
#define mp_assert assert
#define mp_mutex_lock(p) (++*(p))
#define mp_mutex_unlock(p) (--*(p))
#define MP_VERBOSE(...) ((void)0)
#define DEMUX_EVENT_STREAMS 4
static void update_stream_eager_state(struct demux_internal *in){(void)in;}
static void wakeup(void *p){++*(int *)p;}
#include "availability.h"
int main(void){
    struct priv p={.dvd_streams={.generation=5,.active_audio=2}};struct demuxer d={.priv=&p};
    for(int i=0;i<8;i++)p.dvd_streams.audio[i].id=-1;
    for(int i=0;i<32;i++)p.dvd_streams.sub[i].id=-1;
    p.dvd_streams.audio[2]=(struct stream_dvd_stream){.id=0x80,.lang="en"};
    p.dvd_streams.audio[6]=(struct stream_dvd_stream){.id=0x80,.lang="ja"};
    struct mp_codec_params codec={.codec="ac3"};struct sh_stream src={.type=STREAM_AUDIO,.demuxer_id=0x80,.codec=&codec};
    CHECK(sync_dvd_stream(&d,&src)==p.dvd_audio[2]);
    CHECK(d.streams==2 && p.dvd_audio[2]!=p.dvd_audio[6]);
    CHECK(!strcmp(p.dvd_audio[2]->lang,"en") && !strcmp(p.dvd_audio[6]->lang,"ja"));
    CHECK(p.dvd_audio[6]->dvd_nav_logical==6 && p.dvd_audio[6]->dvd_nav_id==0x80);
    p.dvd_audio[6]->selected=true;CHECK(selected_dvd_audio(&p,&src)==p.dvd_audio[6]);
    CHECK(p.dvd_audio[2]->default_track && !p.dvd_audio[6]->default_track);
    // New catalog removes one alias while preserving another. Routing uses
    // only current mappings; old sh_stream identity remains valid for teardown.
    p.dvd_streams.generation++;p.dvd_streams.audio[6].id=0x88;
    struct sh_stream *old6=p.dvd_audio[6];memset(p.dvd_audio,0,sizeof(p.dvd_audio));
    sync_dvd_stream(&d,&src);CHECK(p.dvd_audio[6]==NULL && selected_dvd_audio(&p,&src)==NULL);
    struct mp_codec_params dts={.codec="dts"};struct sh_stream src_dts={.type=STREAM_AUDIO,.demuxer_id=0x88,.codec=&dts};
    sync_dvd_stream(&d,&src_dts);CHECK(p.dvd_audio[6]!=old6 && p.dvd_audio[6]->dvd_nav_id==0x88);
    CHECK(!strcmp(p.dvd_audio[6]->codec->codec,"dts"));
    int count=d.streams;sync_dvd_stream(&d,&src_dts);CHECK(d.streams==count);
    p.dvd_streams.sub[2]=(struct stream_dvd_stream){.id=0x25,.lang="en"};
    p.dvd_streams.sub[6]=(struct stream_dvd_stream){.id=0x25,.lang="ja"};
    struct mp_codec_params spu={.codec="dvd_subtitle"};struct sh_stream sub={.type=STREAM_SUB,.demuxer_id=0x25,.codec=&spu};
    sync_dvd_stream(&d,&sub);CHECK(p.dvd_subs[2] && p.dvd_subs[6] && p.dvd_subs[3]==NULL);
    struct demux_packet packet={.len=7,.pts=2.5,.dts=2.4,.duration=1,.pos=123,.keyframe=true,
        .buffer={1,2,3,4,5,6,7},.side_data={9,8,7,6}};
    queue_dvd_sub_aliases(&d,&sub,&packet);CHECK(p.num_pending_subs==2);
    for(int i=0;i<2;i++){
        struct demux_packet *q=p.pending_subs[i].pkt;
        CHECK(q!=&packet && q->len==packet.len && !memcmp(q->buffer,packet.buffer,7));
        CHECK(q->pts==2.5 && q->dts==2.4 && q->duration==1 && q->pos==123 && q->keyframe);
        CHECK(!memcmp(q->side_data,packet.side_data,4) && q->stream==p.pending_subs[i].sh->index);
    }
    CHECK(p.pending_subs[0].pkt!=p.pending_subs[1].pkt);
    int wakeups=0;struct demux_internal in={.d_thread=&d,.wakeup_cb=wakeup,.wakeup_cb_ctx=&wakeups};d.in=&in;
    demux_set_stream_absent(&d,old6,true);
    CHECK(wakeups==1 && in.events==DEMUX_EVENT_STREAMS && old6->absent && in.lock==0);
    demux_set_stream_absent(&d,old6,true);CHECK(wakeups==1);
    demux_set_stream_absent(&d,old6,false);CHECK(wakeups==2 && !old6->absent);
    printf("DVD logical demux: %d checks, %d failures\n",checks,failed);return failed!=0;
}
