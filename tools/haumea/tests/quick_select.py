#!/usr/bin/env python3
"""quick_select.py [path...]: which real-game tests run_all.sh --quick runs.

--quick runs everything on the synthetic game, then only the real-game tests
that the changed files touch. The paths are the arguments (--touch), else
`git diff --name-only main...HEAD` plus uncommitted changes. Prints shell
assignments for run_all.sh:

  UI_ONLY='a.uis b.uis'   UI scripts to play on the real game (ALL: every one)
  ROUNDTRIP=0|1           the Pawn round trip on the real game
  SHOWCASE=0|1            test_showcase on the real game

and, as comments, why each was picked. A changed source file this table
doesn't know selects every real-game test: when unsure, test more.
"""
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SRC = 'tools/haumea/'
UI_DIR = SRC + 'tests/ui/'

# Paths that need no real-game test (the synthetic run covers them, or they
# aren't code).
NONE = ('docs/', 'README', 'CHANGELOG', '.github/', 'tools/release/', 'tools/agent/',
        'patches/', SRC + 'tests/fixture/', SRC + 'tests/windows_check.py', SRC + 'tests/run_all.sh',
        SRC + 'tests/quick_select.py', SRC + 'tests/test_i18n.py', 'tools/i18n/', SRC + 'lang/', SRC + 'tests/test_namaka_install.py', SRC + 'tests/test_downloads.py', SRC + 'prefabs/')

# Source files and the real-game tests they need: UI scripts (without .uis),
# 'roundtrip', 'showcase'. Files not listed here (and not in NONE) select all.
SOURCES = {
	'tempstream.h': ['pawn_ide', 'import_export', 'roundtrip', 'textdraws', 'textdraws_boards'],
	'discordrpc.cpp': ['discord_presence'],
	'discordrpc.h': ['discord_presence'],
	'discordpresence.cpp': ['discord_presence'],
	'discordpresence.h': ['discord_presence'],
	'alignmentmath.h': ['coordinate_pin', 'pin_alignment', 'xml_pins'],
	'maptools.h': ['map_tools', 'line_feedback', 'coordinate_pin', 'pin_alignment', 'xml_pins'],
	'coordinatepin.cpp': ['coordinate_pin', 'pin_alignment', 'xml_pins'],
	'coordinatepin.h': ['coordinate_pin', 'pin_alignment', 'xml_pins'],
	'browserdiscovery.cpp': ['browser_discovery'],
	'browserdiscovery.h': ['browser_discovery'],
	'modelquery.cpp': ['browser_discovery'],
	'modelquery.h': ['browser_discovery'],
	'pawneditor.cpp': ['pawn_ide'],
	'pawneditor.h': ['pawn_ide'],
	'pawnsyntax.cpp': ['pawn_ide'],
	'pawnsyntax.h': ['pawn_ide'],
	'i18n.cpp': [],
	'i18n.h': [],
	'camera.cpp': ['fly_updown', 'ctrl_keys', 'shortcuts'],
	'texbrowser.cpp': ['texture_browser', 'texture_studio_list', 'materials'],
	'materialtext.cpp': ['materials', 'material_font', 'texture_browser', 'roundtrip'],
	'custommodels.cpp': ['custom_model', 'showcase', 'roundtrip'],
	'objectinst.cpp': ['roundtrip', 'xml_pins', 'xml_merge', 'showcase', 'import_export', 'materials', 'material_indexes', 'el_vehicle', 'el_npc', 'attachment_workspace', 'geometry_texture_clones'],
	'mapimport.cpp': ['import_export', 'roundtrip'],
	'tstudio.cpp': ['import_export', 'texture_studio_list'],
	'encoding.cpp': ['roundtrip', 'import_export'],
	'mapdocument.cpp': ['roundtrip', 'import_export'],
	'anim.cpp': ['el_actor', 'npc_preview', 'el_npc'],
	'npcpreview.cpp': ['npc_preview', 'el_npc'],
	'rec.cpp': ['npc_preview', 'el_npc'],
	'path.cpp': ['el_npc', 'npc_preview'],
	'sapaths.cpp': ['nodes', 'el_npc'],
	'nodes_ui.cpp': ['nodes'],
	'documentxml.cpp': ['roundtrip', 'xml_pins', 'xml_merge', 'radar_art', 'textdraws_guides', 'textdraws_boards'],
	'editorxml.cpp': ['roundtrip', 'xml_pins', 'xml_merge'],
	'textdraws.cpp': ['textdraws', 'textdraws_move', 'textdraws_lock', 'textdraws_boards', 'textdraws_shapes', 'textdraws_guides', 'roundtrip'],
	'textdraws_ui.cpp': ['textdraws', 'textdraws_move', 'textdraws_lock', 'textdraws_boards', 'textdraws_tools', 'textdraws_shapes', 'textdraws_guides', 'textdraws_layers', 'custom_model'],
	'testserver.cpp': ['test_in_game', 'test_server'],
	'childprocess.cpp': ['test_in_game', 'namaka'],
	'namaka.cpp': ['namaka'],
	'namaka.h': ['namaka'],
	'checkpoints.cpp': ['checkpoint_types', 'el_checkpoint'],
	'checkpoints.h': ['checkpoint_types', 'el_checkpoint'],
	'maptools.cpp': ['map_tools', 'line_feedback', 'coordinate_pin', 'pin_alignment', 'xml_pins'],
	'radarart.cpp': ['radar_art', 'roundtrip'],
	'trains.cpp': ['trains', 'roundtrip'],
	'mapcheck.cpp': ['map_check', 'material_font'],
	'playerview.cpp': ['player_view'],
	'interiors.cpp': ['interiors'],
	'startscreen.cpp': ['startscreen', 'startscreen_launch', 'first_run'],
	'workspace.cpp': ['workspace', 'startscreen', 'first_run', 'pawn_ide'],
	'shortcuts.cpp': ['shortcuts', 'preferences_rebind', 'ctrl_keys'],
	'vehiclemods.cpp': ['el_vehicle'],
	'agentbridge.cpp': [],	# the agent tests don't need the real game
	'multiplayer.cpp': [],	# test_multiplayer runs on the synthetic game
	'multiplayer.h': [],
	'mprelay.cpp': [],	# test_multiplayer, with haumea-server
	'mprelay.h': [],
	'mpprotocol.h': [],
	'telemetry.cpp': [],
	'crashreports.cpp': [],
	'updater.cpp': [],
	'versionutil.cpp': [],
}


def changed_paths():
	def git(*args):
		out = subprocess.run(['git', '-C', str(ROOT)] + list(args), capture_output=True, text=True)
		return out.stdout.split() if out.returncode == 0 else []
	return sorted(set(git('diff', '--name-only', 'main...HEAD') + git('diff', '--name-only', 'HEAD')))


def select(paths):
	scripts = {p.stem for p in (HERE / 'ui').glob('*.uis')}
	ui, roundtrip, showcase, why = set(), False, False, []
	everything = False
	for path in paths:
		if path.startswith(NONE) or (path.endswith('.md') and not path.startswith(SRC)):
			why.append(f'{path}: no real-game test')
			continue
		if path.startswith(UI_DIR):
			name = Path(path).stem
			if path.endswith('.uis') and name in scripts:
				ui.add(name)
				why.append(f'{path}: that script')
			else:
				everything = True	# a fixture or helper some scripts share
				why.append(f'{path}: every UI script (shared UI fixture)')
			continue
		if path.startswith(SRC + 'tests/pawn/'):
			roundtrip = True
			why.append(f'{path}: the round trip')
			continue
		if path.startswith(SRC + 'tests/showcase/') or path.endswith('test_showcase.py'):
			showcase = True
			why.append(f'{path}: the showcase')
			continue
		if path.startswith(SRC + 'tests/'):
			if path.endswith(('test_ui_scripts.py', 'haumea_test_env.py')):
				everything = True
				why.append(f'{path}: every UI script (the script runner)')
			else:
				why.append(f'{path}: no real-game test')
			continue
		name = Path(path).name
		if path.startswith(SRC) and name in SOURCES:
			picked = SOURCES[name]
			for test in picked:
				if test == 'roundtrip':
					roundtrip = True
				elif test == 'showcase':
					showcase = True
				elif test in scripts:
					ui.add(test)
			why.append(f'{path}: ' + (', '.join(picked) if picked else 'no real-game test'))
			continue
		if path.startswith(SRC) or path.startswith(('src/', 'vendor/', 'premake', 'build/')):
			everything = True
			why.append(f'{path}: everything (not in the table)')
			continue
		why.append(f'{path}: no real-game test')
	if everything:
		return 'ALL', True, True, why
	return ' '.join(sorted(f'{n}.uis' for n in ui)), roundtrip, showcase, why


def main():
	paths = sys.argv[1:] or changed_paths()
	ui, roundtrip, showcase, why = select(paths)
	for line in why or ['no changed files: no real-game test']:
		print(f'# {line}')
	print(f"UI_ONLY='{ui}'")
	print(f'ROUNDTRIP={int(roundtrip)}')
	print(f'SHOWCASE={int(showcase)}')


if __name__ == '__main__':
	main()
