#!/usr/bin/env python3
"""Exercise mpv's real DVD stream operations against native VM/IFO fixtures."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mpv', type=Path, required=True)
    parser.add_argument('--libdvdnav', type=Path, required=True)
    parser.add_argument('--dvdread-source', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    spec = importlib.util.spec_from_file_location('disc_streams', here/'test-disc-streams.py')
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    dvd = (args.mpv/'stream/stream_dvdnav.c').read_text(encoding='utf-8')
    stream = (args.mpv/'stream/stream.h').read_text(encoding='utf-8')

    def declaration(source, kind, name):
        start = source.index(kind+' '+name+' {')
        return source[start:helpers.braced(source, start)[1]]+';'

    declarations = [declaration(stream, 'enum', 'stream_ctrl')]
    declarations += [declaration(stream, 'struct', name) for name in (
        'mp_dvdnav_highlight', 'stream_nav_state', 'stream_dvd_stream',
        'stream_dvd_streams', 'stream_dvd_select')]
    declarations += [declaration(dvd, 'struct', 'priv')]
    declarations += ['static void update_highlight(struct priv *p) {(void)p;}',
                     'static struct stream_dvd_streams read_dvd_streams(struct priv *p);',
                     'static bool same_dvd_streams(const struct stream_dvd_streams *a, const struct stream_dvd_streams *b);']
    functions = [helpers.function(dvd, name) for name in (
        'in_menu_domain', 'pci_num_buttons', 'dvd_domain_name',
        'dvd_audio_to_substream', 'dvd_audio_logical', 'refresh_video_resolution',
        'clear_still', 'mark_failed', 'bump_discontinuity', 'process_event',
        'dvd_stream_language', 'read_dvd_selection', 'read_dvd_streams',
        'same_dvd_streams', 'select_dvd_stream', 'update_dvd_streams',
        'publish_state', 'fill_buffer', 'seek_time_ticks')]
    controls = helpers.function(dvd, 'control_locked')
    selected = ['    case STREAM_CTRL_SET_DVD_STREAM: return select_dvd_stream(priv, arg);']
    for name in ('STREAM_CTRL_SEEK_TO_TIME', 'STREAM_CTRL_NAV_DRAIN_ENABLE', 'STREAM_CTRL_NAV_DRAIN_ACK'):
        start = controls.index('    case '+name+':')
        selected.append(controls[start:helpers.braced(controls, start)[1]])
    functions += ['static int control_locked(stream_t *stream, int cmd, void *arg) {\n'
                  'struct priv *priv=stream->priv; dvdnav_t *dvdnav=priv->dvdnav; switch(cmd) {\n'+
                  '\n'.join(selected)+'\n} return STREAM_UNSUPPORTED;\n}',
                  helpers.function(dvd, 'control')]
    with tempfile.TemporaryDirectory(prefix='mpv-dvd-presented-') as directory:
        work = Path(directory)
        (work/'mpv-presented-under-test.h').write_text('\n\n'.join(declarations+functions), encoding='utf-8')
        shutil.copy2(here/'mpv-dvd-presented-vm.c', work/'dvdnav-title-scope.c')
        # Reuse the complete native VM compiler and its platform compatibility.
        shutil.copy2(here/'test-libdvdnav-title-scope.py', work/'native-runner.py')
        subprocess.run([sys.executable, str(work/'native-runner.py'), str(args.libdvdnav.resolve()),
                        '--dvdread-source', str(args.dvdread_source.resolve()),
                        '--fixtures', str(args.fixtures.resolve()), '--cc', args.cc], check=True)


if __name__ == '__main__':
    main()
