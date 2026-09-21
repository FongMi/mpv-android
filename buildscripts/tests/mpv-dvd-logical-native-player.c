#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
enum stream_type { STREAM_VIDEO, STREAM_AUDIO, STREAM_SUB, STREAM_TYPE_COUNT };
enum { STREAM_OK=1, STREAM_ERROR=-1, STREAM_CTRL_GET_NAV_STATE=10,
    STREAM_CTRL_SET_DVD_STREAM, DVDNAV_STATUS_OK=1, DVD_AUDIO_STREAM, DVD_SUBTITLE_STREAM,
    DVD_AUDIO_FORMAT_AC3=0, DVD_AUDIO_FORMAT_MPEG=2, DVD_AUDIO_FORMAT_MPEG2_EXT=3,
    DVD_AUDIO_FORMAT_LPCM=4, DVD_AUDIO_FORMAT_DTS=6, FLAG_MARK_SELECTION=1 };
#include "dvd-logical-contracts.h"
static int checks,failed,attribute_reads;
#define CHECK(x) do { checks++;if (!(x)) { failed++;fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while(0)
#define mp_assert assert
#define MP_WARN(...) ((void)0)
#define MP_ERR(...) ((void)0)
#define MP_VERBOSE(...) ((void)0)
struct codec {const char *codec;};
struct sh_stream { struct codec *codec; enum stream_type type;int demuxer_id,index; bool dvd_nav_stream;
    int dvd_nav_logical,dvd_nav_id; _Atomic uint64_t dvd_nav_generation; };
struct track { enum stream_type type; struct sh_stream *stream; int user_tid;
    bool selected,image,dependent_track; void *sink,*d_sub; };
struct stream_nav_state { uint64_t dvd_generation;int active_audio_logical,active_sub_logical;
    bool no_audio,sub_visible,menu_active; };
typedef struct { int audio_count,sub_count,domain,audio[8],sub[32],format[8],ast,spst,set_calls;
    uint16_t alang[8],slang[32];bool reject; } dvdnav_t;
struct priv { dvdnav_t *dvdnav; struct stream_dvd_streams dvd_streams;uint32_t discontinuity_id;
    bool sub_visible,failed,terminal_stop,pending_drain,wait_pending,dvd_catalog_dirty,dvd_selection_dirty; };
typedef struct stream { struct priv *priv; } stream_t;
static int dvdnav_is_domain_vts(dvdnav_t *d) { return d->domain==4; }
static int dvdnav_is_domain_vtsm(dvdnav_t *d) { return d->domain==3; }
static int dvdnav_is_domain_vmgm(dvdnav_t *d) { return d->domain==2; }
static int dvdnav_get_number_of_stream_attributes(dvdnav_t *d,int t) {attribute_reads++;return t==DVD_AUDIO_STREAM?d->audio_count:d->sub_count;}
static int dvdnav_get_active_logical_stream(dvdnav_t *d,int t) {return t==DVD_AUDIO_STREAM?d->ast:d->spst&31;}
static int dvdnav_get_audio_logical_stream(dvdnav_t *d,int i) {return d->audio[i];}
static int dvdnav_get_spu_logical_stream(dvdnav_t *d,int i) {return d->sub[i];}
static uint16_t dvdnav_audio_stream_format(dvdnav_t *d,int i) {return (uint16_t)d->format[i];}
static uint16_t dvdnav_audio_stream_to_lang(dvdnav_t *d,int i) {return d->alang[i];}
static uint16_t dvdnav_spu_stream_to_lang(dvdnav_t *d,int i) {return d->slang[i];}
static int dvdnav_get_active_spu_stream(dvdnav_t *d) {return d->sub[d->spst&31] | ((d->spst&0x40)?0:0x80);}
static int dvdnav_toggle_spu_stream(dvdnav_t *d,int show) {if (show)d->spst|=0x40;else d->spst&=~0x40;return 1;}
static int dvdnav_set_active_stream(dvdnav_t *d,int i,int type) {
    d->set_calls++;if(d->reject)return -1;
    if(type==DVD_AUDIO_STREAM)d->ast=i;else d->spst=i|(d->spst&0x40);return 1;
}
#ifndef BASELINE
#include "native.h"
#endif
enum {MPSEEK_ABSOLUTE=1,MPSEEK_EXACT=2};
struct MPOpts { int stream_id[3][STREAM_TYPE_COUNT]; bool show_dependent_tracks; };
struct disc_nav_state {struct track *menu_selected_track,*menu_saved_track;};
struct chain { struct track *track; };
struct MPContext {struct MPOpts *opts;struct track *current_track[3][STREAM_TYPE_COUNT];
    int accepted_track_selection[3][STREAM_TYPE_COUNT];struct disc_nav_state nav;
    struct track **tracks;int num_tracks;void *demuxer,*mconfig,*track_layout_hash;
    struct chain *ao_chain,*vo_chain;stream_t *stream;struct {int type;} seek;int changes,refreshes;bool playback_initialized;};
static double get_current_time(struct MPContext *p) {(void)p;return 2.0;}
static void queue_seek(struct MPContext *p,int type,double time,int precision,int flags) {
    CHECK(type==MPSEEK_ABSOLUTE && time==2.0 && precision==MPSEEK_EXACT && !flags);
    p->refreshes++;p->seek.type=type;
}
static const int num_ptracks[]={1,1,3};
static struct disc_nav_state *get_state(struct MPContext *p){return &p->nav;}
static struct stream *disc_nav_get_stream(struct MPContext *p){return p->stream;}
static int stream_control(stream_t *s,int cmd,void *arg) {
    if(cmd==STREAM_CTRL_GET_NAV_STATE){
        struct stream_nav_state *n=arg;
        n->dvd_generation=s->priv->dvd_streams.generation;return 1;
    }
#ifndef BASELINE
    return select_dvd_stream(s->priv,arg);
#else
    (void)arg;return -1;
#endif
}
static void mark_track_selection(struct MPContext *p,int o,enum stream_type t,int id){p->opts->stream_id[o][t]=id;p->accepted_track_selection[o][t]=id;}
#define uninit_video_chain(p) ((void)(p))
#define handle_force_window(p,b) ((void)(p),(void)(b))
#define clear_audio_output_buffers(p) ((void)(p))
#define uninit_audio_chain(p) ((void)(p))
#define uninit_audio_out(p) ((void)(p))
#define uninit_sub(p,t) ((void)(p),(void)(t))
#define reselect_demux_stream(p,t,b) ((void)(p),(void)(t),(void)(b))
#define sub_control(a,b,c) ((void)(a),(void)(c))
#define sub_reset(a) ((void)(a))
#define reinit_video_chain(p) ((void)(p))
#define reinit_audio_chain(p) ((void)(p))
#define reinit_sub(p,t) ((void)(p),(void)(t))
#define mp_notify(p,e,a) ((p)->changes++)
#define mp_wakeup_core(p) ((void)(p))
#define talloc_free(p) ((void)(p))
#define talloc_steal(p,x) ((void)(p),(x))
#define track_layout_hash(p) ((void)(p),(void *)0)
static bool mp_switch_track_n(struct MPContext*,int,enum stream_type,struct track*,int);
#ifndef BASELINE
#include "player.h"
#endif
#include "switch.h"
#ifndef BASELINE
enum {M_PROPERTY_GET,M_PROPERTY_PRINT,M_PROPERTY_SWITCH,M_PROPERTY_SET,M_PROPERTY_OK=10,M_PROPERTY_ERROR=-1};
struct m_property {const int *priv;};struct m_property_switch_arg {double inc;};
static struct track *mp_track_by_tid(struct MPContext *p,enum stream_type type,int tid){
    for (int i = 0; i < p->num_tracks; i++) {
        if (p->tracks[i]->type == type && p->tracks[i]->user_tid == tid) {
            return p->tracks[i];
        }
    }
    return NULL;
}
static bool track_in_current_edition(struct MPContext *p,struct track *t){(void)p;(void)t;return true;}
static struct track *select_default_track(struct MPContext *p,int order,int type){(void)order;return mp_track_by_tid(p,type,1);}
static int mp_property_generic_option(struct MPContext *p,struct m_property *prop,int action,void *arg){(void)p;(void)prop;(void)action;(void)arg;return 0;}
typedef struct MPContext MPContext;
#define print_track_list(p,s) ((void)(p),(void)(s))
#define talloc_new(p) (p)
#define talloc_asprintf(p,...) ((void)(p),(char *)NULL)
#define talloc_strdup(p,s) ((void)(p),(void)(s),(char *)NULL)
#include "command.h"

#endif
int main(void){
    dvdnav_t d={.audio_count=7,.sub_count=7,.domain=4,.ast=2,.spst=2|0x40};
    for(int i=0;i<8;i++) d.audio[i]=-1;
    for(int i=0;i<32;i++) d.sub[i]=-1;
    d.audio[2]=d.audio[6]=0;d.alang[2]=0x656e;d.alang[6]=0x6a61;
    d.sub[2]=d.sub[6]=5;
    struct priv v={.dvdnav=&d,.discontinuity_id=7};stream_t stream={.priv=&v};
#ifndef BASELINE
    v.dvd_streams=read_dvd_streams(&v);
    CHECK(v.dvd_streams.audio[2].id==0x80 && v.dvd_streams.audio[6].id==0x80);
    CHECK(!strcmp(v.dvd_streams.audio[2].lang,"en") && !strcmp(v.dvd_streams.audio[6].lang,"ja"));
    CHECK(v.dvd_streams.audio[0].id==-1 && v.dvd_streams.sub[6].id==0x25);
#else
    v.dvd_streams.audio[2].id=v.dvd_streams.audio[6].id=0x80;
#endif
    v.dvd_streams.generation=9;
    struct sh_stream sh[2]={{.dvd_nav_stream=true,.dvd_nav_logical=2,.dvd_nav_generation=9},
                           {.dvd_nav_stream=true,.dvd_nav_logical=6,.dvd_nav_generation=9}};
    struct track ts[2]={{.type=STREAM_AUDIO,.stream=&sh[0],.user_tid=1,.selected=true},
                       {.type=STREAM_AUDIO,.stream=&sh[1],.user_tid=2}};
    struct track *tracks[]={&ts[0],&ts[1]};struct MPOpts opts={0};opts.stream_id[0][STREAM_AUDIO]=-1;
    struct MPContext p={.opts=&opts,.tracks=tracks,.num_tracks=2,.demuxer=&v,.stream=&stream};
    p.playback_initialized=true;
    p.current_track[0][STREAM_AUDIO]=&ts[0];p.accepted_track_selection[0][STREAM_AUDIO]=-1;
    CHECK(mp_switch_track_n(&p,0,STREAM_AUDIO,&ts[1],FLAG_MARK_SELECTION));
    CHECK(d.ast==6 && d.set_calls==1);
    CHECK(p.refreshes==1);
    CHECK(p.current_track[0][STREAM_AUDIO]==&ts[1] && opts.stream_id[0][STREAM_AUDIO]==2);
    d.reject=true;
    CHECK(!mp_switch_track_n(&p,0,STREAM_AUDIO,&ts[0],FLAG_MARK_SELECTION));
    CHECK(d.ast==6 && p.current_track[0][STREAM_AUDIO]==&ts[1] && opts.stream_id[0][STREAM_AUDIO]==2);
    d.reject=false;
    sh[0].dvd_nav_generation=8;
    CHECK(!mp_switch_track_n(&p,0,STREAM_AUDIO,&ts[0],FLAG_MARK_SELECTION));
    CHECK(d.ast==6 && p.current_track[0][STREAM_AUDIO]==&ts[1] && opts.stream_id[0][STREAM_AUDIO]==2);
#ifndef BASELINE
    // Config callback receives the option after m_config copied it. It must
    // restore the last accepted preference without switching player or VM.
    p.playback_initialized=true;sh[0].dvd_nav_generation=8;
    opts.stream_id[0][STREAM_AUDIO]=1;
    update_track_switch(&p,0,STREAM_AUDIO);
    CHECK(opts.stream_id[0][STREAM_AUDIO]==2 && p.current_track[0][STREAM_AUDIO]==&ts[1] && d.ast==6);
    static const int def[]={0,STREAM_AUDIO};struct m_property prop={.priv=def};int id=1;
    CHECK(mp_property_switch_track(&p,&prop,M_PROPERTY_SET,&id)==M_PROPERTY_ERROR);
    CHECK(opts.stream_id[0][STREAM_AUDIO]==2 && d.ast==6);
    // sid cycle has no valid track while the native epoch is held. It must end.
    static const int subdef[]={0,STREAM_SUB};struct m_property subprop={.priv=subdef};
    struct m_property_switch_arg inc={.inc=1};v.pending_drain=true;
    CHECK(mp_property_switch_track(&p,&subprop,M_PROPERTY_SWITCH,&inc)==M_PROPERTY_ERROR);
    v.pending_drain=false;
    v.dvd_streams.generation=0;
    CHECK(!mp_switch_track_n(&p,0,STREAM_AUDIO,&ts[0],FLAG_MARK_SELECTION));
    CHECK(opts.stream_id[0][STREAM_AUDIO]==2 && p.current_track[0][STREAM_AUDIO]==&ts[1]);
    v.dvd_streams.generation=9;
    sh[0].dvd_nav_generation=9;
    struct stream_nav_state nav={.dvd_generation=9,.active_audio_logical=2,.active_sub_logical=2,.sub_visible=true};
    int calls=d.set_calls;d.ast=2;sync_dvd_track_selection(&p,&nav);
    CHECK(p.current_track[0][STREAM_AUDIO]==&ts[0] && d.set_calls==calls);
    d.ast=6;nav.active_audio_logical=6;sync_dvd_track_selection(&p,&nav);
    CHECK(p.current_track[0][STREAM_AUDIO]==&ts[1] && d.set_calls==calls);
    struct stream_dvd_select select={.generation=9,.type=STREAM_SUB,.logical=-1};
    CHECK(select_dvd_stream(&v,&select)==1 && d.spst==2);
    select.logical=6;CHECK(select_dvd_stream(&v,&select)==1 && d.spst==(6|0x40));
    v.pending_drain=true;select.logical=2;CHECK(select_dvd_stream(&v,&select)==-1 && d.spst==(6|0x40));
    // Pre-command WAIT keeps the presented VM. Its drain id must not retire
    // the logical tracks, and selection must publish while the VM waits.
    v.wait_pending=true;v.discontinuity_id++;v.dvd_selection_dirty=true;
    struct stream_nav_state waiting={0};
    v.dvd_streams=update_dvd_streams(&v,&waiting);
    CHECK(v.dvd_streams.generation==9 && v.dvd_streams.discontinuity_id==v.discontinuity_id);
    CHECK(select_dvd_stream(&v,&select)==1 && d.spst==(2|0x40));
    v.dvd_selection_dirty=true;v.dvd_streams=update_dvd_streams(&v,&waiting);
    CHECK(waiting.active_sub_logical==2 && waiting.sub_visible);
    select.logical=-1;CHECK(select_dvd_stream(&v,&select)==1 && d.spst==2);
    v.dvd_selection_dirty=true;v.dvd_streams=update_dvd_streams(&v,&waiting);
    CHECK(waiting.active_sub_logical==2 && !waiting.sub_visible);
    v.pending_drain=false;v.wait_pending=false;
    v.discontinuity_id++;CHECK(select_dvd_stream(&v,&select)==-1);
    v.discontinuity_id--;d.audio_count=0;d.domain=2;d.sub_count=1;d.sub[0]=0;
    struct stream_dvd_streams menu=read_dvd_streams(&v);
    CHECK(menu.audio[0].id==-1 && menu.audio[6].id==-1 && menu.sub[0].id==0x20 && menu.sub[6].id==-1);
    // Retired menu aliases must not be retained or selected again.
    struct codec spu_codec={.codec="dvd_subtitle"};
    struct sh_stream old_sub={.codec=&spu_codec,.dvd_nav_stream=true,.dvd_nav_generation=0};
    struct sh_stream new_sub={.codec=&spu_codec,.dvd_nav_stream=true,.dvd_nav_generation=9};
    struct track subtracks[2]={{.type=STREAM_SUB,.stream=&old_sub,.user_tid=3,.selected=true},
                              {.type=STREAM_SUB,.stream=&new_sub,.user_tid=4}};
    struct track *menu_tracks[]={&subtracks[0],&subtracks[1]};p.tracks=menu_tracks;p.num_tracks=2;
    p.current_track[0][STREAM_SUB]=&subtracks[0];p.nav.menu_selected_track=&subtracks[0];
    p.nav.menu_saved_track=&subtracks[0];opts.stream_id[0][STREAM_SUB]=-1;
    CHECK(!track_is_visible(&p,&subtracks[0]) && track_is_visible(&p,&subtracks[1]));
    ensure_menu_sub_selection(&p,true);
    CHECK(p.current_track[0][STREAM_SUB]==&subtracks[1]);
    ensure_menu_sub_selection(&p,false);
    CHECK(p.current_track[0][STREAM_SUB]==NULL);
    // A future VM catalog and STOP must not retire the presented aliases.
    struct stream_nav_state held={0};v.pending_drain=true;v.dvd_catalog_dirty=true;
    v.dvd_streams=update_dvd_streams(&v,&held);CHECK(held.dvd_generation==9 && !held.no_audio);
    v.pending_drain=false;v.terminal_stop=true;
    v.dvd_streams=update_dvd_streams(&v,&held);CHECK(held.dvd_generation==9 && !held.no_audio);
    v.terminal_stop=false;v.dvd_streams=update_dvd_streams(&v,&held);
    CHECK(held.dvd_generation==10 && held.no_audio);
    int reads=attribute_reads;
    for(int i=0;i<100;i++) v.dvd_streams=update_dvd_streams(&v,&held);
    CHECK(attribute_reads==reads);
    v.dvd_selection_dirty=true;v.dvd_streams=update_dvd_streams(&v,&held);
    CHECK(attribute_reads==reads);
    d.audio_count=4;d.domain=4;d.audio[0]=d.audio[1]=d.audio[2]=d.audio[3]=0;
    d.format[0]=0;d.format[1]=6;d.format[2]=4;d.format[3]=2;
    menu=read_dvd_streams(&v);CHECK(menu.audio[0].id==0x80 && menu.audio[1].id==0x88 && menu.audio[2].id==0xa0 && menu.audio[3].id==0x1c0);
#endif
    printf("DVD logical native/player: %d checks, %d failures\n",checks,failed);return failed!=0;
}
