"""Test extracted mpv DVD stream, player, demux, and probe functions.

The fixtures compile production functions copied from the supplied mpv source
tree, with host stubs for surrounding APIs. They cover logical track selection
and publication, packet aliases, and ownership of probe/drain transitions.
Baseline mode adapts the old player switch signature to report its outcome.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


TEST_DIRECTORY = Path(__file__).resolve().parent


def extract_function(source, name):
    match = re.search(
        r'^[\w *]+\b' + name + r'\([^;{]*\)\s*\{', source, re.M
    )
    if not match:
        raise ValueError(name)
    start = source.index('{', match.start())
    end = start + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def extract_structure(source, name):
    start = source.index('struct ' + name + ' {')
    end = source.index('\n};', start) + 3
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--mpv', type=Path, required=True)
    parser.add_argument('--cc', nargs='+', default=['cc'])
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--contract-mpv', type=Path)
    args = parser.parse_args()

    paths = [
        'stream/stream_dvdnav.c',
        'player/discnav.c',
        'player/loadfile.c',
        'demux/demux_disc.c',
        'stream/stream.h',
    ]
    sources = [(args.mpv / path).read_text(encoding='utf-8') for path in paths]
    dvd_source, player_source, loadfile_source, disc_source, stream_header = sources
    command_source = (args.mpv / 'player/command.c').read_text(encoding='utf-8')

    # Baseline compiles the actual old player switch, not an emulated rejection path.
    switch_function = extract_function(loadfile_source, 'mp_switch_track_n')
    if args.baseline:
        switch_function = switch_function.replace(
            'void mp_switch_track_n(', 'static void original_switch('
        )
        switch_function += (
            '\nstatic bool mp_switch_track_n(struct MPContext *p,int o,'
            'enum stream_type t,struct track *x,int f) { '
            'original_switch(p,o,t,x,f);return p->current_track[o][t]==x; }'
        )

    with tempfile.TemporaryDirectory(prefix='mpv-dvd-logical-') as directory:
        work = Path(directory)
        if args.contract_mpv:
            stream_header = (args.contract_mpv / 'stream/stream.h').read_text(
                encoding='utf-8'
            )
        contracts = '\n'.join(
            extract_structure(stream_header, name)
            for name in (
                'stream_dvd_stream',
                'stream_dvd_streams',
                'stream_dvd_select',
            )
        )
        (work / 'dvd-logical-contracts.h').write_text(contracts, encoding='utf-8')

        native_functions = ''
        player_functions = ''
        if not args.baseline:
            native_functions = '\n\n'.join(
                extract_function(dvd_source, name)
                for name in (
                    'dvd_audio_to_substream',
                    'dvd_stream_language',
                    'read_dvd_selection',
                    'read_dvd_streams',
                    'same_dvd_streams',
                    'select_dvd_stream',
                    'update_dvd_streams',
                )
            )
            player_functions = '\n\n'.join(
                extract_function(player_source, name)
                for name in (
                    'disc_nav_select_track',
                    'find_dvd_track',
                    'sync_dvd_track_selection',
                    'is_dvd_sub_track',
                    'ensure_menu_sub_selection',
                )
            )
        cases = [('native-player', native_functions, player_functions, switch_function)]
        if not args.baseline:
            demux_functions = '\n\n'.join(
                extract_function(disc_source, name)
                for name in (
                    'dvd_alias_matches',
                    'adopt_codec_params',
                    'sync_dvd_alias',
                    'sync_dvd_stream',
                    'selected_dvd_audio',
                    'queue_dvd_sub_aliases',
                )
            )
            cases.append(('demux', demux_functions, '', ''))
            cases.append((
                'probe',
                extract_function(disc_source, 'd_open'),
                extract_function(player_source, 'check_async_discontinuity'),
                '',
            ))

        for name, native_functions, player_functions, switch_function in cases:
            (work / 'native.h').write_text(native_functions, encoding='utf-8')
            (work / 'player.h').write_text(player_functions, encoding='utf-8')
            (work / 'switch.h').write_text(switch_function, encoding='utf-8')

            command_functions = ''
            if not args.baseline:
                command_functions = (
                    extract_function(loadfile_source, 'track_is_visible')
                    + '\n\n'
                    + '\n\n'.join(
                        extract_function(command_source, function_name)
                        for function_name in (
                            'track_next',
                            'mp_property_switch_track',
                            'update_track_switch',
                        )
                    )
                )
            (work / 'command.h').write_text(command_functions, encoding='utf-8')
            demux_source = (args.mpv / 'demux/demux.c').read_text(encoding='utf-8')
            availability_function = extract_function(
                demux_source, 'demux_set_stream_absent'
            )
            (work / 'availability.h').write_text(
                availability_function, encoding='utf-8'
            )

            fixture = TEST_DIRECTORY / ('mpv-dvd-logical-' + name + '.c')
            executable = work / (name + ('.exe' if os.name == 'nt' else ''))
            compiler = args.cc
            if Path(compiler[0]).stem.lower() in ['cl', 'clang-cl']:
                flags = [
                    '/nologo',
                    '/std:c11',
                    '/experimental:c11atomics',
                    '/W4',
                    '/WX',
                    '/wd4505',
                    f'/I{work}',
                    f'/I{TEST_DIRECTORY}',
                    str(fixture),
                    f'/Fe:{executable}',
                    f'/Fo:{work / name}.obj',
                ]
                if args.baseline:
                    flags.append('/DBASELINE')
            else:
                flags = [
                    '-std=c11',
                    '-Wall',
                    '-Wextra',
                    '-Werror',
                    '-Wno-unused-function',
                    '-I', str(work),
                    '-I', str(TEST_DIRECTORY),
                    str(fixture),
                    '-o', str(executable),
                ]
                if args.baseline:
                    flags.append('-DBASELINE')
            subprocess.run(compiler + flags, cwd=work, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
