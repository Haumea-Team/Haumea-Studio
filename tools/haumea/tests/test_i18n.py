# SPDX-License-Identifier: GPL-2.0-or-later
"""Translation: the extractor, the pt-BR and zh-CN catalogs, and the editor loading a
catalog (a language is a lang/<code>.po file), keeping the saved layout of
windows whose labels became "translated###English" IDs.

The editor parts need the editor binary and Xvfb, and skip without them.
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'i18n'))
sys.path.insert(0, str(HERE))
import extract  # noqa: E402
import merge  # noqa: E402


class ExtractTests(unittest.TestCase):
    def test_conversions(self):
        self.assertEqual(extract.conversions('%d of %s (%.1f%%)'), ['d', 's', 'f'])
        self.assertEqual(extract.conversions('%lld bytes %5.2f'), ['d', 'f'])
        self.assertEqual(extract.conversions('100%% sure'), [])

    def test_marks(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, 'a.cpp')
            src.write_text(
                'x = _("Size##id"); y = L("Object Info"); z = LI(ICON_FA_GEAR, "Tools");\n'
                't = TI(ICON_FA_COPY, "Copy" " as Pawn"); static const char *n = N_("Sky\\n");\n'
                'foo_("not a mark"); // _("in a comment still counts")\n')
            found = {}
            extract.scan(str(src), found)
        self.assertEqual(sorted(found), ['Copy as Pawn', 'Object Info', 'Size', 'Sky\n', 'Tools',
                                         'in a comment still counts'])

    def test_c_escapes(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp, 'a.cpp')
            src.write_text(r'a = _("\xC2\xB1 5 mm"); b = _("tab\there \"q\" \303\251"); c = _("100%%");')
            found = {}
            extract.scan(str(src), found)
        self.assertEqual(sorted(found), ['100%%', 'tab\there "q" \u00e9', '\u00b1 5 mm'])

    def test_po_round_trip(self):
        text = 'a\nb "quoted"\t%d'
        entries = extract.parse_po('msgid ""\nmsgstr ""\n\nmsgid ' + extract.po_quote(text) +
                                   '\nmsgstr ' + extract.po_quote('x\n' + text) + '\n')
        self.assertEqual(entries[1][:2], [text, 'x\n' + text])


class CatalogTests(unittest.TestCase):
    def test_zh_cn_matches_current_ui(self):
        """Every current UI key is translated, with no stale or duplicate keys."""
        entries = [(key, value, fuzzy) for key, value, fuzzy in extract.parse_po(
            (ROOT / 'tools/haumea/lang/zh-CN.po').read_text(encoding='utf-8')) if key]
        keys = [key for key, _, _ in entries]
        self.assertEqual(len(keys), len(set(keys)), 'duplicate Chinese translation keys')
        self.assertEqual(sorted(keys), sorted(extract.collect()), 'Chinese catalog needs refreshing')
        self.assertEqual([key for key, value, fuzzy in entries if not value or fuzzy], [])

    def test_placeholders(self):
        self.assertEqual(extract.check_catalogs(), 0)

    def test_no_conflicts_between_staging_files(self):
        self.assertEqual(merge.build()[1], [])

    def test_no_unused_msgids(self):
        self.assertEqual(merge.build()[2], [], 'staging msgids no source file marks; remove them')

    def test_pt_br_is_complete(self):
        """Every marked string has a pt-BR translation (lang/pt-BR.po or a staging file)."""
        done = extract.translated(extract.pt_br_paths())
        found = extract.collect()
        missing = sorted(t for t in found if t not in done)
        by_file = {}
        for t in missing:
            by_file.setdefault(found[t][0].rsplit(':', 1)[0], []).append(t)
        report = '\n'.join('%s: %d, e.g. %r' % (f, len(v), v[0]) for f, v in sorted(by_file.items()))
        self.assertEqual(missing, [], 'untranslated into pt-BR:\n' + report)


# ---- the editor ----

try:
    from test_ui_scripts import BINARY, XVFB, shared, start_xvfb
    from haumea_test_env import editor_env, run_editor
except ImportError:
    BINARY = XVFB = None

ICONS = (ROOT / 'tools' / 'haumea' / 'icons.h').read_text()


def icon_text(name):
    """The UTF-8 of ICON_FA_<name> from icons.h."""
    m = re.search(r'#define\s+%s\s+"((?:\\x[0-9a-fA-F]{2})+)"' % re.escape(name), ICONS)
    return bytes(int(x, 16) for x in re.findall(r'\\x([0-9a-fA-F]{2})', m.group(1))).decode('utf-8') if m else None


def old_titles():
    """{'###Tools': '<icon> Tools'}: the title each LI(ICON, "Text") window had
    before its label became an ID, read from the source so no window is missed."""
    out = {}
    pattern = re.compile(r'\bLI\(\s*(ICON_FA_\w+)\s*,\s*"((?:[^"\\]|\\.)*)"')
    for path in (ROOT / 'tools' / 'haumea').glob('*.cpp'):
        for icon, text in pattern.findall(path.read_text(encoding='utf-8', errors='replace')):
            glyph = icon_text(icon)
            if glyph:
                out['###' + text] = glyph + ' ' + text
    return out


XX_PO = '''msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\\n"
"Language-Name: Xx Test\\n"

msgid "Language"
msgstr "Idioma"

msgid "Auto"
msgstr "Automatico"

msgid "Auto follows the system language when Haumea has it, else English.\\nA language is a lang/<code>.po file beside the editor."
msgstr "Auto %d"
'''


def run_editor_script(work, env, script_text, seconds_of_frames=600):
    """Play a UI script in a private Xvfb; the editor's output."""
    script = Path(work, 'script.uis')
    script.write_text(script_text)
    tmp, game = shared()
    xvfb, env['DISPLAY'] = start_xvfb()
    env.pop('XAUTHORITY', None)
    try:
        return run_editor([BINARY, '--ui-script', str(script), '--game', game, '--workspace', str(work)],
                          cwd=work, env=env, timeout=300)
    finally:
        xvfb.terminate()
        xvfb.wait(10)


def ini_sections(text):
    out = {}
    for m in re.finditer(r'^\[Window\]\[(.*)\]\n((?:[^\[\n].*\n)*)', text, re.M):
        out[m.group(1)] = dict(line.split('=', 1) for line in m.group(2).splitlines() if '=' in line)
    return out


@unittest.skipUnless(BINARY and XVFB, 'needs the editor and Xvfb')
class EditorTests(unittest.TestCase):
    def test_zh_cn_glyphs_and_redesigned_controls(self):
        """The bundled font draws Chinese, including the new feature and settings labels."""
        self.editor('''open "$UI_FIXTURES/ui_floor.pwn"
wait 50
assert missing_catalog_glyphs == 0
assert window 'Object Browser'
assert window '物体浏览器'
click_item 'Object Browser' '##browser_source'
wait 3
assert item '*' '角色'
assert item '*' '路径'
assert item '*' '标记'
assert item '*' '文本与音频'
assert item '*' '模型文件'
click_item '*' '角色'
wait 5
assert item 'Object Browser' '##place_skin'
shortcut window_preferences
wait 5
assert window '首选项'
assert item 'Preferences' '主题'
click_item 'Preferences' '主题'
wait 3
assert item '*' '深色'
assert item '*' '浅色'
click_item '*' '浅色'
wait 3
assert missing_catalog_glyphs == 0
click_item 'Preferences' 'Performance'
wait 3
assert item 'Preferences' '距离雾'
assert item 'Preferences' '垂直同步'
assert item 'Preferences' '限制帧率'
assert item 'Preferences' '##stream_distance'
assert clipped_items == 0
exit
''', savesettings='language zh-CN\nsettings_version 8\nshow_browser_window 1\nshow_instance_window 1\n')

    def editor(self, script, ini=None, savesettings=None, lang=None):
        tmp, game = shared()
        work = tempfile.mkdtemp(dir=tmp)
        env = editor_env(work, virtual_display=True)
        config = Path(env['XDG_CONFIG_HOME'], 'haumea')
        config.mkdir(parents=True, exist_ok=True)
        if ini is not None:
            (config / 'imgui.ini').write_text(ini, encoding='utf-8')
        (config / 'savesettings.txt').write_text(savesettings if savesettings is not None else
            'settings_version 8\nshow_browser_window 0\nshow_instance_window 0\n')
        if lang is not None:
            Path(work, 'lang').mkdir()
            Path(work, 'lang', 'xx.po').write_text(lang, encoding='utf-8')
            env['HAUMEA_LANG_DIR'] = str(Path(work, 'lang'))
        shutil.copytree(HERE / 'ui' / 'fixtures', Path(work, 'fixtures'))
        env['UI_FIXTURES'] = str(Path(work, 'fixtures'))
        result = run_editor_script(work, env, script)
        log = '\n'.join(l for l in result.stdout.splitlines() if l.startswith('[ui-script]'))
        if os.environ.get('I18N_DEBUG'):
            print(log)
        self.assertEqual(result.returncode, 0, log or result.stdout[-3000:])
        ini_out = config / 'imgui.ini'
        return (ini_out.read_text(encoding='utf-8') if ini_out.exists() else ''), \
               ((config / 'savesettings.txt').read_text() if (config / 'savesettings.txt').exists() else ''), \
               result.stdout + result.stderr

    def test_mapping_tool_controls_fit(self):
        """New curve controls remain usable in each bundled language at panel width."""
        script = '''open "$UI_FIXTURES/ui_map.pwn"
shortcut window_tools
wait 5
click_item 'Tools' 'Paths & curves'
click_item 'Tools' 'New path'
wait 5
assert clipped_items == 0
click_item 'Tools' 'Point settings'
wait 3
assert clipped_items == 0
scroll_until 'Tools' 'Model ID'
assert clipped_items == 0
scroll_until 'Tools' 'Advanced'
click_item 'Tools' 'Advanced'
wait 3
assert clipped_items == 0
exit
'''
        for language in ('en', 'pt-BR', 'zh-CN'):
            with self.subTest(language=language):
                self.editor(script, savesettings='settings_version 8\nlanguage ' + language +
                            '\nshow_browser_window 0\nshow_instance_window 0\n')

    def test_old_layout_survives(self):
        """An imgui.ini saved before a window's label became ###English keeps its place and dock."""
        # Panels show once a map is open; the saved flags carry them to the second run.
        ini_a, saved_a, _ = self.editor('open "$UI_FIXTURES/ui_floor.pwn"\nwait 100\n' +
                                        'shortcut window_object_info\nshortcut window_browser\n' +
                                        'shortcut window_map_objects\nshortcut window_tools\nwait 600\nexit\n')
        windows = ini_sections(ini_a)
        marked = [n for n in windows if n.startswith('###')]
        if not marked:
            self.skipTest('no window uses L()/LI() yet')
        # Write it as the old editor did: each window under its old title.
        icon_of = old_titles()
        old = ini_a
        for name in marked:
            title = icon_of.get(name, name[3:])
            old = old.replace('[Window][%s]' % name, '[Window][%s]' % title)
            old = re.sub(r'Selected=0x%08X' % zlib.crc32(name.encode()),
                         'Selected=0x%08X' % zlib.crc32(title.encode()), old)
        self.assertNotEqual(old, ini_a)
        ini_b, _, _ = self.editor('open "$UI_FIXTURES/ui_floor.pwn"\nwait 600\nexit\n', ini=old,
                                  savesettings=saved_a)
        after = ini_sections(ini_b)
        for name in marked:
            self.assertIn(name, after, name + ' lost its saved settings')
            self.assertEqual(after[name].get('DockId'), windows[name].get('DockId'), name + ' undocked')
        self.assertEqual(re.findall(r'Selected=0x\w+', ini_b), re.findall(r'Selected=0x\w+', ini_a))

    def panels(self, titles):
        return self.PANELS.replace('exit\n', ''.join('assert window "%s"\n' % t for t in titles) + 'exit\n')

    PANELS = ('open "$UI_FIXTURES/ui_floor.pwn"\nwait 50\n' +
              ''.join('shortcut window_%s\n' % w for w in (
                  'camera', 'changes', 'textures', 'nodes')) +
              'wait 60\nassert clipped_items >= 0\n'
              'shortcut window_shortcuts\nwait 20\nassert clipped_items >= 0\n'
              'shortcut window_preferences\nwait 20\nassert clipped_items >= 0\n' +
              ''.join("click_item 'Preferences' '%s'\nwait 10\nassert clipped_items >= 0\n" % tab
                      for tab in ('Shortcuts', 'Camera', 'Test server', 'Privacy', 'Updates')) +
              'exit\n')

    PANEL_FLAGS = 'settings_version 8\n' + ''.join('show_%s_window 1\n' % w for w in (
        'instance', 'browser', 'map_objects', 'view', 'time_weather'))

    @staticmethod
    def clipped(output):
        """The labels the script logged as cut off. Not by window: its title is translated too."""
        return set(re.findall(r"clipped '(.*?)' in '", output))

    def test_pt_br_labels_and_overflow(self):
        """The same panels in pt-BR: the labels are Portuguese, and none runs past its
        window unless it already does in English."""
        _, _, english = self.editor(self.panels(['Object Browser', 'Preferences']), savesettings='language en\n' + self.PANEL_FLAGS)
        _, _, portuguese = self.editor(self.panels(['Navegador de objetos', 'Informações do objeto', 'Objetos do mapa', 'Preferências']),
                                     savesettings='language pt-BR\n' + self.PANEL_FLAGS)
        grew = sorted(self.clipped(portuguese) - self.clipped(english))
        self.assertEqual(grew, [], 'labels cut off in pt-BR only (shorten the translation):\n' +
                         '\n'.join(grew))

    def test_code_workspace_is_translated(self):
        # Code is a workspace now: it hides the map's panels while selected.
        self.editor('''open "$UI_FIXTURES/ui_floor.pwn"
wait 10
click_item '*' 'Code'
wait 10
assert window 'PawnIDE'
assert item '*' 'Code'
assert item 'PawnIDE' 'Salvar script'
assert item 'PawnIDE' 'Localizar / Substituir'
assert no_window 'Navegador de objetos'
click_item '*' 'Map'
wait 5
assert window 'Navegador de objetos'
exit
''', savesettings='language pt-BR\n' + self.PANEL_FLAGS)

    def test_catalog_and_picker(self):
        """A catalog in HAUMEA_LANG_DIR is a language: the setting loads it, a
        translation with other printf conversions is ignored, and the picker saves its choice."""
        script = '''shortcut window_preferences
wait 5
assert window "Preferences"
click_item 'Preferences' '##language'
wait 3
assert item '*' 'Automatico'
assert item '*' 'English'
assert item '*' 'Xx Test'
click_item '*' 'English'
wait 5
exit
'''
        _, saved, output = self.editor(script, savesettings='language xx\n', lang=XX_PO)
        self.assertIn('language en', saved.splitlines())
        self.assertIn('Language xx: 1 translations ignored', output)


if __name__ == '__main__':
    unittest.main()
