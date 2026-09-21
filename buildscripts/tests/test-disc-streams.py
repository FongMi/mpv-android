#!/usr/bin/env python3
"""Run production disc stream and playlist functions with mocked I/O.

Examples:
  python3 tests/test-disc-streams.py --libbluray deps/libbluray
  python3 tests/test-disc-streams.py --libdvdnav deps/libdvdnav
  python3 tests/test-disc-streams.py --mpv deps/mpv

These host regressions cover menu discovery/entry, failed seek state,
demux reopen and terminal failure propagation, Blu-ray still/transition EOF,
transport metadata, DVD read/control serialization and domain audio counts.
They do not exercise actual disc I/O or Android playback.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def braced(source, start):
    opening = source.index("{", start)
    end, depth = opening + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return opening, end


def function(source, name):
    # The selected functions contain no braces in strings or comments.
    match = re.search(r"^[\w *]+\b" + name + r"\([^;{]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Missing function: {name}")
    return source[match.start():braced(source, match.start())[1]]


def bluray_code(path):
    navigation = (path / "src/libbluray/bdnav/navigation.c").read_text(encoding="utf-8")
    api = (path / "src/libbluray/bluray.c").read_text(encoding="utf-8")
    return "\n\n".join((function(navigation, "nav_get_title_list"),
                         function(api, "bd_get_titles_with_menu_info"),
                         function(api, "bd_get_titles")))


def mpv_code(path):
    bluray = (path / "stream/stream_bluray.c").read_text(encoding="utf-8")
    dvd = (path / "stream/stream_dvdnav.c").read_text(encoding="utf-8")
    disc = (path / "demux/demux_disc.c").read_text(encoding="utf-8")
    start = bluray.index("    b->menu_supported =")
    end = bluray.index('    MP_VERBOSE(s, "bdnav: cfg_title=', start)
    capability = ("static void configure_menu(stream_t *s, int has_interactive_graphics)\n"
                  "{\n    struct bluray_priv_s *b = s->priv;\n"
                  "    const BLURAY_DISC_INFO *info = bd_get_disc_info(b->bd);\n" +
                  bluray[start:end] + "}\n")
    opening, end = braced(bluray, bluray.index("    case STREAM_CTRL_SEEK_TO_TIME:"))
    seek = ("static int seek_bluray(stream_t *s, void *arg)\n"
            "{\n    struct bluray_priv_s *b = s->priv;\n" +
            bluray[opening + 1:end - 1] + "}\n")
    return "\n\n".join((
        function(bluray, "clear_still"), function(bluray, "play_menu"),
        function(bluray, "start_hdmv_navigation"), capability, seek,
        function(bluray, "open_bluray_from_stream"),
        function(dvd, "open_dvdnav_iso_stream"),
        function(disc, "remember_reset_time"), function(disc, "d_seek"),
        function(disc, "process_discontinuity"), function(disc, "reopen_slave"),
        function(disc, "d_read_packet")))


def dvd_serialization_code(path):
    dvd = (path / "stream/stream_dvdnav.c").read_text(encoding="utf-8")
    events = function(dvd, "process_event")
    start = events.index("    case DVDNAV_BLOCK_OK:")
    end = events.index("    case DVDNAV_NAV_PACKET:", start)
    still_start = events.index("    case DVDNAV_STILL_FRAME:")
    still_end = events.index("    case DVDNAV_HOP_CHANNEL:", still_start)
    process = ("static int process_event(stream_t *s, int event, void *buf, int len)\n"
               "{\n    struct priv *priv = s->priv;\n    dvdnav_t *dvdnav = priv->dvdnav;\n"
               "    pause_event_handler();\n    switch (event) {\n" +
               events[start:end] + events[still_start:still_end] +
               "    }\n    return -1;\n}\n")
    controls = function(dvd, "control_locked")
    selected = []
    for name in ("STREAM_CTRL_NAV_CMD", "STREAM_CTRL_NAV_STILL_SKIP", "STREAM_CTRL_NAV_DRAIN_ACK"):
        start = controls.index("    case " + name + ":")
        selected.append(controls[start:braced(controls, start)[1]])
    control = ("static int control_locked(stream_t *stream, int cmd, void *arg)\n"
               "{\n    struct priv *priv = stream->priv;\n"
               "    dvdnav_t *dvdnav = priv->dvdnav;\n    switch (cmd) {\n" +
               "\n".join(selected) + "\n    }\n    return STREAM_UNSUPPORTED;\n}\n")
    helpers = [function(dvd, "clear_still"), function(dvd, "bump_discontinuity")]
    if "static void mark_failed(" in dvd:
        helpers.append(function(dvd, "mark_failed"))
    return "\n\n".join(helpers + [
        function(dvd, "update_dvd_streams"), function(dvd, "publish_state"), function(dvd, "handle_nav_cmd"), process,
        function(dvd, "fill_buffer"), control, function(dvd, "control")])


def demux_failure_code(path):
    header = (path / "demux/demux.h").read_text(encoding="utf-8")
    core = (path / "demux/demux.c").read_text(encoding="utf-8")
    player = (path / "player/loadfile.c").read_text(encoding="utf-8")
    nav = (path / "player/discnav.c").read_text(encoding="utf-8")
    nav_failure = function(nav, "handle_nav_failure") if "static bool handle_nav_failure(" in nav else ""
    nav_update = function(nav, "disc_nav_update")
    nav_update = nav_update[:nav_update.index("    bool still = have && nav.still_active;")]
    nav_update += "    nav_fallthrough++;\n}\n"
    start = header.index("enum demux_event {")
    events = header[start:braced(header, start)[1]] + ";"
    update = function(player, "update_demuxer_properties")
    opening = update.index("{")
    sync = ("static void sync_demuxer_properties(struct MPContext *mpctx)\n" +
            update[opening:update.index("    int events = demuxer->events;")] + "}\n")
    start = player.index("terminate_playback:") + len("terminate_playback:")
    finish = ("static void finish_demuxer(struct MPContext *mpctx)\n{\n" +
              player[start:player.index("    if (!mpctx->stop_play)", start)] + "}\n")
    return "\n\n".join((events, function(core, "demux_set_failed"),
                         function(core, "demux_update"),
                         function(core, "demux_drive_nav"),
                         function(core, "queue_seek"), function(core, "read_packet"),
                         function(player, "handle_demuxer_failure"), sync, finish,
                         nav_failure, nav_update))


def dvd_audio_code(path):
    dvd = (path / "stream/stream_dvdnav.c").read_text(encoding="utf-8")
    player = (path / "player/discnav.c").read_text(encoding="utf-8")
    events = function(dvd, "process_event")
    start = events.index("    case DVDNAV_AUDIO_STREAM_CHANGE:")
    event = events[start:braced(events, start)[1]]
    dispatch = ("static void audio_event(stream_t *s, void *buf)\n{\n"
                "    struct priv *priv = s->priv;\n"
                "    switch (DVDNAV_AUDIO_STREAM_CHANGE) {\n" + event + "\n    }\n}\n")
    state = function(dvd, "publish_state")
    availability = state[state.index("    bool no_audio ="):
                         state.index("    if (!priv->menu_support_known)")]
    published = re.search(r"\.active_audio_id = (.*?),\s*\.active_sub_id",
                          state, re.S).group(1)
    return "\n\n".join((function(dvd, "dvd_audio_to_substream"),
                         function(dvd, "dvd_audio_logical"),
                         function(dvd, "mp_dvdnav_lang_from_aid"), dispatch,
                         "static struct stream_nav_state audio_state(struct priv *priv)\n{\n"
                         "    dvdnav_t *dvdnav = priv->dvdnav;\n" + availability +
                         "    return (struct stream_nav_state) { .no_audio = no_audio,\n"
                         "        .active_audio_id = " + published + " };\n}\n",
                         "static int published_audio(struct priv *priv)\n{\n"
                         "    return audio_state(priv).active_audio_id;\n}\n",
                         function(player, "find_dvd_track"),
                         function(player, "sync_dvd_track_selection"),
                         function(player, "find_track_by_demuxer_id"),
                         function(player, "sync_disc_track_selection")))


def dvd_audio_count_code(path):
    api = (path / "src/dvdnav.c").read_text(encoding="utf-8")
    vm = (path / "src/vm/vmget.c").read_text(encoding="utf-8")
    return "\n\n".join((function(vm, "vm_get_audio_stream"),
                         function(api, "dvdnav_get_audio_logical_stream"),
                         function(api, "dvdnav_get_number_of_streams")))


DVD_AUDIO_FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
enum { DVD_AUDIO_FORMAT_AC3 = 0, DVD_AUDIO_FORMAT_MPEG = 2,
       DVD_AUDIO_FORMAT_MPEG2_EXT = 3, DVD_AUDIO_FORMAT_LPCM = 4,
       DVD_AUDIO_FORMAT_DTS = 6, DVDNAV_AUDIO_STREAM_CHANGE = 1,
       DVD_AUDIO_STREAM = 0 };
typedef struct {
    int physical[8], title_domain, menu_audio_count;
    uint16_t format[8], language[8];
} dvdnav_t;
struct priv { dvdnav_t *dvdnav; int audio_physical, audio_logical, nav_change_id; };
typedef struct stream { struct priv *priv; } stream_t;
typedef struct { int physical, logical; } dvdnav_audio_stream_change_event_t;
#define MP_VERBOSE(s, ...) ((void)(s))
#define MP_TRACE(s, ...) ((void)(s))
struct stream_nav_state {
    bool no_audio, sub_visible, menu_active;
    int active_audio_id, active_sub_id, angle, num_angles;
    uint64_t dvd_generation;
    int active_audio_logical, active_sub_logical;
    unsigned discontinuity_id;
};
enum stream_type { STREAM_AUDIO, STREAM_SUB, STREAM_TYPE_COUNT };
struct sh_stream { int demuxer_id; bool dvd_nav_stream;
    int dvd_nav_logical; uint64_t dvd_nav_generation; };
#define atomic_load(p) (*(p))
struct track { enum stream_type type; struct sh_stream *stream; };
struct disc_nav_state {
    bool track_sync_seen, last_sub_visible;
    unsigned last_track_disc_id;
    int last_audio_id, last_missing_audio_id, last_sub_id, last_angle;
    struct track *menu_selected_track;
};
struct MPOpts { int stream_id[1][2]; };
struct MPContext {
    struct disc_nav_state nav;
    struct MPOpts *opts;
    int num_tracks, switches;
    struct track **tracks, *current_track[1][2];
};
static struct disc_nav_state *get_state(struct MPContext *ctx) { return &ctx->nav; }
static void mp_switch_track_n(struct MPContext *ctx, int order,
                              enum stream_type type, struct track *track, int flags)
{
    (void)flags;
    ctx->current_track[order][type] = track;
    ctx->switches++;
}
static void mp_notify_property(struct MPContext *ctx, const char *name)
{ (void)ctx; (void)name; }
static uint16_t dvdnav_audio_stream_format(dvdnav_t *dvd, int logical)
{ assert(logical >= 0 && logical < 8); return dvd->format[logical]; }
static int dvdnav_get_audio_logical_stream(dvdnav_t *dvd, int logical)
{
    assert(logical >= 0 && logical < 8);
    int physical = dvd->physical[dvd->title_domain ? logical : 0];
    return !dvd->title_domain && physical < 0 ? 0 : physical;
}
static uint16_t dvdnav_audio_stream_to_lang(dvdnav_t *dvd, int logical)
{ return dvd->language[logical]; }
static int dvdnav_is_domain_vts(dvdnav_t *dvd) { return dvd->title_domain; }
static int dvdnav_get_number_of_streams(dvdnav_t *dvd, int type)
{
    assert(type == DVD_AUDIO_STREAM);
    if (!dvd->title_domain)
        return dvd->menu_audio_count;
    int count = 0;
    for (int i = 0; i < 8; i++)
        count += dvd->physical[i] >= 0;
    return count;
}
#include "disc-streams-under-test.h"
int main(void)
{
    dvdnav_t dvd = {.title_domain = 1, .menu_audio_count = 1};
    for (int i = 0; i < 8; i++) {
        dvd.physical[i] = -1;
        dvd.format[i] = 0xffff;
        dvd.language[i] = 0xffff;
    }
    struct priv p = {.dvdnav = &dvd, .audio_physical = -1, .audio_logical = -1};
    stream_t s = {.priv = &p};
    dvd.physical[2] = 5;
    dvd.format[2] = DVD_AUDIO_FORMAT_DTS;
    dvd.language[2] = 0x6a61;
    dvdnav_audio_stream_change_event_t event = {.logical = 2, .physical = 5};
    audio_event(&s, &event);
    assert(published_audio(&p) == 0x8d && p.nav_change_id == 1);
    assert(mp_dvdnav_lang_from_aid(&s, 0x8d) == 0x6a61);
    assert(mp_dvdnav_lang_from_aid(&s, 0x85) == 0);
    audio_event(&s, &event);
    assert(p.nav_change_id == 1);
    /* AST_REG may remain 15 or name an inactive PGC slot. The VM uses
     * the first active logical stream without rewriting that register. */
    event.logical = 15;
    audio_event(&s, &event);
    assert(p.audio_logical == 2 && published_audio(&p) == 0x8d);
    assert(p.nav_change_id == 1);
    dvd.format[0] = DVD_AUDIO_FORMAT_AC3;
    event.logical = 0;
    audio_event(&s, &event);
    assert(p.audio_logical == 2 && published_audio(&p) == 0x8d);
    assert(p.nav_change_id == 1);
    /* Four codecs share an ordinal without sharing a logical stream. */
    const uint16_t formats[] = {0, 6, 4, 3};
    const int ids[] = {0x80, 0x88, 0xa0, 0x1c0};
    dvd.physical[2] = -1;
    for (int i = 0; i < 4; i++) {
        dvd.physical[i] = 0;
        dvd.format[i] = formats[i];
        dvd.language[i] = (uint16_t)(0x6560 + i);
    }
    for (int i = 0; i < 4; i++) {
        event.logical = i;
        event.physical = 0;
        audio_event(&s, &event);
        assert(published_audio(&p) == ids[i]);
        assert(mp_dvdnav_lang_from_aid(&s, ids[i]) == 0x6560 + i);
    }
    assert(p.nav_change_id == 5);
    /* Default selection follows VM order even when codecs share an ordinal. */
    event.logical = 15;
    audio_event(&s, &event);
    assert(p.audio_logical == 0 && published_audio(&p) == 0x80);
    assert(p.nav_change_id == 6);
    audio_event(&s, &event);
    assert(p.nav_change_id == 6);
    /* Do not pair another physical stream with the first active codec. */
    dvd.physical[1] = 1;
    event.physical = 1;
    audio_event(&s, &event);
    assert(p.audio_logical == -1 && published_audio(&p) == -1);
    event.logical = 1;
    event.physical = 0;
    audio_event(&s, &event);
    assert(p.audio_logical == -1 && published_audio(&p) == -1);
    dvd.physical[1] = 0;
    event.logical = 3;
    event.physical = 0;
    audio_event(&s, &event);
    dvd.format[3] = DVD_AUDIO_FORMAT_MPEG;
    assert(published_audio(&p) == 0x1c0);
    /* Ambiguous aliases must not silently choose the first language. */
    dvd.physical[7] = 0;
    dvd.format[7] = DVD_AUDIO_FORMAT_AC3;
    dvd.language[7] = 0x6672;
    assert(mp_dvdnav_lang_from_aid(&s, 0x80) == 0);
    dvd.title_domain = 0;
    event.logical = 15;
    audio_event(&s, &event);
    assert(p.audio_logical == 0 && published_audio(&p) == 0x80);
    event.logical = 3;
    audio_event(&s, &event);
    assert(p.audio_logical == 0 && published_audio(&p) == 0x80);
    assert(mp_dvdnav_lang_from_aid(&s, 0x80) == dvd.language[0]);
    dvd.language[0] = 0xffff;
    assert(mp_dvdnav_lang_from_aid(&s, 0x80) == 0);
    assert(mp_dvdnav_lang_from_aid(&s, -1) == 0);
    assert(mp_dvdnav_lang_from_aid(&s, 0x180) == 0);
    assert(dvd_audio_to_substream(&p, -1, 0) == -1);
    assert(dvd_audio_to_substream(&p, 8, 0) == -1);
    assert(dvd_audio_to_substream(&p, 0, -1) == -1);
    assert(dvd_audio_to_substream(&p, 0, 8) == -1);
    dvd.format[0] = 7;
    assert(dvd_audio_to_substream(&p, 0, 0) == -1);
    /* Menu physical 0 does not alone establish an available audio codec. */
    dvd.physical[0] = -1;
    dvd.format[0] = 0xffff;
    event.logical = 15;
    audio_event(&s, &event);
    assert(p.audio_logical == 0 && published_audio(&p) == -1);
    dvd.title_domain = 1;
    for (int i = 0; i < 8; i++)
        dvd.physical[i] = -1;
    event.logical = 15;
    audio_event(&s, &event);
    assert(p.audio_logical == -1 && published_audio(&p) == -1);
    event.physical = -1;
    audio_event(&s, &event);
    assert(published_audio(&p) == -1);

    /* An absent menu stream can have zero-filled attributes (AC3). Its route
     * still defaults to physical 0; neither establishes that audio exists. */
    dvd.title_domain = 0;
    dvd.menu_audio_count = 0;
    assert(dvdnav_get_number_of_streams(&dvd, DVD_AUDIO_STREAM) == 0);
    dvd.format[0] = DVD_AUDIO_FORMAT_AC3;
    event.physical = 0;
    audio_event(&s, &event);
    struct stream_nav_state silent = audio_state(&p);
    assert(silent.no_audio && silent.active_audio_id == -1);
    struct sh_stream audio = {.demuxer_id = 0x80};
    struct track old_audio = {.type = STREAM_AUDIO, .stream = &audio};
    struct track *tracks[] = {&old_audio};
    struct MPOpts opts = {.stream_id = {{-1, -2}}};
    struct MPContext ctx = {.opts = &opts, .num_tracks = 1, .tracks = tracks};
    struct stream_nav_state title = {.active_audio_id = 0x80, .discontinuity_id = 1};
    sync_disc_track_selection(&ctx, &s, &title);
    assert(ctx.current_track[0][STREAM_AUDIO] == &old_audio && ctx.switches == 1);
    silent.discontinuity_id = 2;
    sync_disc_track_selection(&ctx, &s, &silent);
    assert(ctx.current_track[0][STREAM_AUDIO] == NULL && ctx.switches == 2);
    sync_disc_track_selection(&ctx, &s, &silent);
    assert(ctx.current_track[0][STREAM_AUDIO] == NULL && ctx.switches == 2);
    /* Conversely, a declared menu audio stream uses the same physical-0
     * fallback with an inactive PGC control, and must remain selectable. */
    dvd.menu_audio_count = 1;
    struct stream_nav_state audible = audio_state(&p);
    assert(!audible.no_audio && audible.active_audio_id == 0x80);
    audible.discontinuity_id = 3;
    sync_disc_track_selection(&ctx, &s, &audible);
    sync_disc_track_selection(&ctx, &s, &audible);
    assert(ctx.current_track[0][STREAM_AUDIO] == &old_audio && ctx.switches == 3);
    dvd.menu_audio_count = -1;
    assert(!audio_state(&p).no_audio); // Unavailable is not proof of no audio.
    puts("DVD logical audio routing: OK");
    return 0;
}
'''


def bluray_navigation_code(path):
    bluray = (path / "stream/stream_bluray.c").read_text(encoding="utf-8")
    nav = (path / "player/discnav.c").read_text(encoding="utf-8")
    player = (path / "player/playloop.c").read_text(encoding="utf-8")
    controls = function(bluray, "bluray_stream_control_locked")
    selected = []
    for name in ("STREAM_CTRL_NAV_CMD", "STREAM_CTRL_SET_CURRENT_TITLE",
                 "STREAM_CTRL_GET_NAV_STATE"):
        start = controls.index("    case " + name + ":")
        selected.append(controls[start:braced(controls, start)[1]])
    control = ("static int bluray_stream_control_locked(stream_t *s, int cmd, void *arg)\n"
               "{\n    struct bluray_priv_s *b = s->priv;\n    switch (cmd) {\n" +
               "\n".join(selected) + "\n    }\n    return STREAM_UNSUPPORTED;\n}\n")
    # Also run the original source to demonstrate the regression before repair.
    eof_guard = ("disc_nav_prevents_eof" if "disc_nav_prevents_eof" in nav
                 else "disc_nav_is_drain_pending")
    return "\n\n".join((function(bluray, "hold_still"),
                         function(bluray, "clear_still"),
                         function(bluray, "bluray_stream_fill_buffer"), control,
                         function(nav, eof_guard), function(player, "handle_eof")))


def run(code, fixture, compiler):
    with tempfile.TemporaryDirectory(prefix="disc-streams-") as directory:
        work = Path(directory)
        (work / "disc-streams-under-test.h").write_text(code, encoding="utf-8")
        exe = work / ("test.exe" if os.name == "nt" else "test")
        if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
            flags = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{work}",
                     str(fixture), f"/Fe:{exe}", f"/Fo:{work / 'test.obj'}"]
        else:
            flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(work),
                     str(fixture), "-o", str(exe), "-lm"]
        subprocess.run(compiler + flags, cwd=work, check=True)
        subprocess.run([str(exe)], cwd=work, check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sources = parser.add_mutually_exclusive_group(required=True)
    sources.add_argument("--libbluray", type=Path)
    sources.add_argument("--libdvdnav", type=Path)
    sources.add_argument("--mpv", type=Path)
    parser.add_argument("--cc", nargs="+", default=["cc"])
    args = parser.parse_args()
    directory = Path(__file__).resolve().parent
    if args.libbluray:
        run(bluray_code(args.libbluray), directory / "bluray-menu-info.c", args.cc)
    elif args.libdvdnav:
        run(dvd_audio_count_code(args.libdvdnav), directory / "dvdnav-audio-count.c", args.cc)
    else:
        run(mpv_code(args.mpv), directory / "mpv-disc-streams.c", args.cc)
        run(demux_failure_code(args.mpv), directory / "mpv-demux-failure.c", args.cc)
        run(bluray_navigation_code(args.mpv),
            directory / "mpv-bluray-navigation.c", args.cc)
        run(dvd_serialization_code(args.mpv),
            directory / "mpv-dvdnav-serialization.c", args.cc)
        with tempfile.TemporaryDirectory(prefix="dvd-audio-fixture-") as temporary:
            fixture = Path(temporary) / "dvd-audio.c"
            fixture.write_text(DVD_AUDIO_FIXTURE, encoding="utf-8")
            run(dvd_audio_code(args.mpv), fixture, args.cc)


if __name__ == "__main__":
    main()
