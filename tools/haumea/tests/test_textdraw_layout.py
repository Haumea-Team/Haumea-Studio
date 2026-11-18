# SPDX-License-Identifier: GPL-2.0-or-later
"""Narrow docks and saved TextDraw layouts, using real UI input."""
import unittest

import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_i18n as harness


@unittest.skipUnless(harness.BINARY and harness.XVFB, 'needs the editor and Xvfb')
class TextDrawLayoutTests(unittest.TestCase):
    editor = harness.EditorTests.editor

    def test_workspace_buttons_fit_the_command_bar(self):
        # The editors are listed under the editor tile; in each editor the tile,
        # its own last group and Test in game stay whole on the bar.
        script = '\n'.join((
            "click_item '*' '##editor'", 'wait 2',
            "assert item_fully_visible '*' 'Map'",
            "assert item_fully_visible '*' 'TextDraws'",
            "assert item_fully_visible '*' 'Code'",
            "click_item '*' 'TextDraws'", 'wait 5',
            "assert item_fully_visible '*' 'editor'",
            "assert item_fully_visible '*' 'Canvas'",
            "assert item_fully_visible '*' 'Test in game'",
            "click_item '*' '##editor'", 'wait 2',
            "click_item '*' 'Code'", 'wait 5',
            "assert item_fully_visible '*' 'editor'",
            "assert item_fully_visible '*' 'Test in game'",
            "click_item '*' '##editor'", 'wait 2',
            "click_item '*' 'Map'", 'wait 5',
            "assert item_fully_visible '*' 'editor'",
            "assert item_fully_visible '*' 'Test in game'", 'exit',
        ))
        for scale in (1.0, 1.25, 1.5, 2.0):
            with self.subTest(scale=scale):
                self.editor(script, savesettings=f'''settings_version 8
language en
ui_scale {scale}
show_browser_window 0
show_instance_window 0
show_code_hint 0
show_textdraws_hint 0
''')

    def test_narrow_dock_in_bundled_languages(self):
        # Dear ImGui 1.91.6+ uses CRC32C for its saved node IDs.
        root = 0xffffffff
        for byte in b'HaumeaTextDrawDockSpace':
            root ^= byte
            for _ in range(8):
                root = (root >> 1) ^ (0x82f63b78 if root & 1 else 0)
        root ^= 0xffffffff
        # The layers over the properties, in a 280 px column.
        ini = f'''[Window][###textdraws]
Pos=1000,52
Size=280,300
Collapsed=0
DockId=0x00000003,0

[Window][###td_properties]
Pos=1000,354
Size=280,418
Collapsed=0
DockId=0x00000004,0

[Docking][Data]
DockSpace ID=0x{root:08X} Pos=0,52 Size=1280,724 Split=X
  DockNode ID=0x00000001 Parent=0x{root:08X} SizeRef=998,724 CentralNode=1
  DockNode ID=0x00000002 Parent=0x{root:08X} SizeRef=280,724 Split=Y
    DockNode ID=0x00000003 Parent=0x00000002 SizeRef=280,300
    DockNode ID=0x00000004 Parent=0x00000002 SizeRef=280,418
'''
        script = '''open "$UI_FIXTURES/ui_map.pwn"
click_item '*' '##editor'
wait 2
click_item '*' 'TextDraws'
wait 8
assert window_docked 'Layers' == 1
assert window_width 'Layers' < 300
assert window_width 'Properties' < 300
assert textdraw_canvas_right < 1000
assert clipped_items == 0
click_item 'TextDraws' 'Text'
wait 5
set_item 'Properties' '###Text' 'Narrow inspector' tab
wait 5
assert textdraws == 1
assert clipped_items == 0
click_item 'Properties' 'Layout'
wait 3
assert clipped_items == 0
click_item 'Properties' 'Content'
wait 3
assert clipped_items == 0
scroll_until 'Properties' 'Code settings'
click_item 'Properties' 'Code settings'
wait 3
assert clipped_items == 0
click_item '*' '##editor'
wait 2
click_item '*' 'Map'
wait 3
click_item '*' '##editor'
wait 2
click_item '*' 'TextDraws'
wait 3
assert window_docked 'Layers' == 1
assert window_width 'Layers' < 300
exit
'''
        for language in ('en', 'pt-BR', 'zh-CN'):
            with self.subTest(language=language):
                out, _, _ = self.editor(script, ini=ini, savesettings=f'''settings_version 8
language {language}
show_browser_window 0
show_instance_window 0
show_code_hint 0
show_textdraws_hint 0
''')
                # A second process must restore the user's dock, rather than rebuild the default.
                self.editor('''click_item '*' '##editor'
wait 2
click_item '*' 'TextDraws'
wait 5
assert window_docked 'Layers' == 1
assert window_width 'Layers' < 300
exit
''', ini=out, savesettings=f'settings_version 8\nlanguage {language}\nshow_textdraws_hint 0\nshow_code_hint 0\n')
