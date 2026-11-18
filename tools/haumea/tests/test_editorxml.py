# SPDX-License-Identifier: GPL-2.0-or-later
"""Structured XML documents, migration, and paired-save failure recovery."""
from pathlib import Path
import glob
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
from haumea_test_env import editor_env, run_editor

LIBRW = os.environ.get('LIBRW')
BINARY = os.environ.get('HAUMEA_BINARY') or next(iter(sorted(glob.glob(str(ROOT / 'bin/*/Release/haumea')))), None)
GAME = os.environ.get('HAUMEA_GAME_DIR')
DRIVER = r'''
#include "editorxml.h"
#include <cassert>
#include <filesystem>
#include <fstream>
using namespace EditorXML;
namespace fs=std::filesystem;
std::string bytes(const std::string &p){std::string b,e;assert(ReadFile(p,&b,&e));return b;}
void put(const std::string &p,const std::string &b){std::ofstream f(p,std::ios::binary);f<<b;assert(f.good());}
int main(int argc,char **argv){
 std::string error,mode=argv[1],p=std::string(argv[2])+"/map.pwn";
 Node root("haumea"),copy;root.set("kind","map");root.add("scene");
 if(mode=="codec"){
  std::string value="é & < > \" '\n\t\r😀";root.set("note",value);
  assert(Parse(Serialize(root),&copy,&error));assert(copy.get("note")==value);
  assert(Parse("<a note='&#x1F600;&#233;&#10;' />",&copy,&error));assert(copy.get("note")=="😀é\n");
  for(const char *s:{"<a><b></a>","<a x='1' x='2'/>","<!DOCTYPE a><a/>","<a x='&unknown;'/>","<a x='&#0;'/>","<.a/>","<a/>oops","<a>pawn</a>"})assert(!Parse(s,&copy,&error));
  std::string deep;for(int i=0;i<40;i++)deep+="<a>";for(int i=0;i<40;i++)deep+="</a>";
  assert(!Parse(deep,&copy,&error));
 }else if(mode=="save"){
  std::string old="// exact legacy\r\n\xE9\r\n";put(p,old);
  assert(WriteDocument(p,"clean1",root,&error));assert(bytes(p+".pre-xml.bak")==old);
  assert(ReadDocument(p,"clean1","map",&copy,&error)==1);
  assert(WriteDocument(p,"clean2",root,&error));assert(bytes(p+".pre-xml.bak")==old);
  assert(!fs::exists(p+".pre-xml-2.bak"));assert(!fs::exists(Sidecar(p)+".previous"));
  assert(ReadDocument(p,"external change","map",&copy,&error)==2);
  assert(ReadDocument(p,"clean2","textdraws",&copy,&error)==-1);
 }else if(mode=="recovery"){
  assert(WriteDocument(p,"first",root,&error));std::string first=bytes(Sidecar(p));
  assert(WriteDocument(p,"second",root,&error));put(Sidecar(p)+".previous",first);put(p,"first");
  assert(ReadDocument(p,"first","map",&copy,&error)==1);
  assert(WriteDocument(p,"third",root,&error));assert(bytes(p)=="third");
  assert(ReadDocument(p,"third","map",&copy,&error)==1);
 }else if(mode=="failure"){
  put(p,"keep me");fs::create_directory(Sidecar(p));
  assert(!WriteDocument(p,"new",root,&error));assert(bytes(p)=="keep me");fs::remove(Sidecar(p));
  put(Sidecar(p),"<haumea version='99' kind='map'/>");
  assert(!WriteDocument(p,"new",root,&error));assert(bytes(p)=="keep me");
  assert(bytes(Sidecar(p))=="<haumea version='99' kind='map'/>");
 }
}
'''


@unittest.skipUnless(shutil.which('g++') and LIBRW and (Path(LIBRW) / 'src/lodepng/lodepng.h').exists(),
                     'needs g++ and LIBRW (same dependency as the editor)')
class XMLCodecTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='haumea-xml-codec-')
        driver = Path(cls.tmp.name, 'driver.cpp')
        driver.write_text(DRIVER)
        cls.exe = str(Path(cls.tmp.name, 'driver'))
        subprocess.run(['g++', '-std=c++17', '-ffunction-sections', '-fdata-sections',
                        '-I', str(ROOT / 'tools/haumea'), '-I', LIBRW, str(driver),
                        str(ROOT / 'tools/haumea/editorxml.cpp'), str(ROOT / 'tools/haumea/updatecore.cpp'),
                        '-Wl,--gc-sections', '-o', cls.exe], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check(self, mode):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([self.exe, mode, tmp], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_codec(self): self.check('codec')
    def test_backup_and_revision(self): self.check('save')
    def test_interrupted_pair(self): self.check('recovery')
    def test_failed_save_retains_original(self): self.check('failure')


@unittest.skipUnless(GAME and BINARY and (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')),
                     'needs the built editor, a game fixture and private display')
class XMLMigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='haumea-xml-map-')
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)

    def run_pairs(self, *pairs, textdraw=False):
        args = [BINARY, '--game', GAME, '--textdraw-roundtrip' if textdraw else '--pawn-roundtrip']
        args += [str(p) for pair in pairs for p in pair]
        result = run_editor(args, cwd=self.folder, env=editor_env(self.folder), capture_output=True, text=True)
        self.assertNotIn(': -1 ', result.stdout, result.stdout[-4000:])
        for _, target in pairs:
            self.assertTrue(target.is_file(), result.stdout[-4000:])
        return result.stdout

    def xml(self, path):
        document = ET.parse(str(path) + '.xml')
        self.assertEqual(document.getroot().get('pawn-sha256'), hashlib.sha256(path.read_bytes()).hexdigest())
        return document

    def test_legacy_scene_and_stable_pair(self):
        target = self.folder / 'map.pwn'
        original = (HERE / 'fixtures/pawn_roundtrip.pwn').read_bytes()
        target.write_bytes(original)
        self.run_pairs((target, target))
        first = target.read_bytes()
        self.assertEqual(Path(str(target)+'.pre-xml.bak').read_bytes(), original)
        self.assertNotIn(b'@group(', first)
        self.assertNotIn(b'@note(', first)
        self.assertNotIn(b'@race(', first)
        self.assertNotIn(b'// Haumea:', first)
        xml = self.xml(target)
        sign = next(n for n in xml.findall('./scene/object') if n.find('properties') is not None and n.find('properties').get('name') == 'sign')
        self.assertEqual(sign.find('properties').get('group'), 'Shop')
        self.assertEqual(sign.find('properties').get('comment'), 'front sign')
        self.assertTrue(sign.findall('material'))
        props=xml.findall('./scene/object/properties')
        self.assertTrue(any(p.get('comment')=='crowd' for p in props))
        self.assertEqual(sum(p.get('cpRace')=='race1' for p in props),2)
        first_xml = ET.tostring(xml.getroot().find('scene'))
        self.run_pairs((target, target))
        self.assertEqual(target.read_bytes(), first)
        self.assertEqual(ET.tostring(self.xml(target).getroot().find('scene')), first_xml)

    def test_xml_values_and_external_pawn_edits(self):
        source, target, copy = [self.folder / name for name in ('old.pwn', 'map.pwn', 'copy.pwn')]
        source.write_text('stock Init(){ new sign = CreateObject(1271, 10.0, 20.0, 30.0, 0.0, 0.0, 0.0); // @group("Doors") @note("A & B")\n}\n')
        self.run_pairs((source, target))
        xml = self.xml(target)
        obj = xml.getroot().find('./scene/object')
        obj.set('position.x', '17.25')
        xml.write(str(target)+'.xml', encoding='utf-8', xml_declaration=True)
        self.run_pairs((target, copy))
        self.assertIn('17.25, 20.0, 30.0', copy.read_text())
        # Changed Pawn keeps its runtime values; the named object's notes survive.
        target.write_text(target.read_text().replace('10.0, 20.0', '42.0, 20.0'))
        self.run_pairs((target, copy))
        self.assertIn('42.0, 20.0', copy.read_text())
        props = self.xml(copy).getroot().find('./scene/object/properties')
        self.assertEqual(props.get('group'), 'Doors')
        self.assertEqual(props.get('comment'), 'A & B')

    def test_native_keeps_editor_only_entities_and_pins(self):
        source, target, copy = [self.folder / name for name in ('old.pwn', 'map.pwn', 'copy.pwn')]
        source.write_text('stock Init(){ CreateDynamicCP(10.0, 20.0, 30.0, 3.0); }\n')
        self.run_pairs((source, target))
        xml = self.xml(target)
        xml.getroot().set('format', '0')
        pins = xml.getroot().find('alignment')
        ET.SubElement(pins, 'pin', {'id':'1', 'position.x':'100', 'position.y':'200', 'position.z':'3', 'measured':'true', 'end.x':'110', 'end.y':'200', 'end.z':'3', 'end-pin':'2'})
        ET.SubElement(pins, 'pin', {'id':'2', 'position.x':'110', 'position.y':'200', 'position.z':'3'})
        xml.write(str(target)+'.xml', encoding='utf-8', xml_declaration=True)
        self.run_pairs((target, copy), (copy, target))
        self.assertNotIn('CreateDynamicCP(', target.read_text())
        saved = self.xml(target)
        self.assertEqual(saved.getroot().find('./scene/object').get('kind'), '10')
        self.assertEqual(len(saved.findall('./alignment/pin')), 2)
        self.assertEqual(saved.getroot().find('./alignment/pin').get('end-pin'), '2')

    def test_deleted_world_does_not_consume_same_model_replacement(self):
        source,target,copy=[self.folder/name for name in ('old.pwn','map.pwn','copy.pwn')]
        source.write_text('stock Init(playerid){\n'
            'RemoveBuildingForPlayer(playerid, 3555, 2063.1328, -1731.7812, 15.273438, 0.25);\n'
            'CreateObject(3555, 2063.1328, -1731.7812, 15.273438, 0.0, 0.0, 0.0);\n'
            'CreateObject(1271, 10.0, 20.0, 30.0, 0.0, 0.0, 0.0);\n}\n')
        self.run_pairs((source,target),(target,copy))
        self.assertEqual(target.read_text(),copy.read_text().replace('Haumea_copy','Haumea_map'))
        self.assertEqual(copy.read_text().count('CreateObject(3555,'),1)
        self.assertEqual(ET.tostring(self.xml(target).getroot().find('scene')),
                         ET.tostring(self.xml(copy).getroot().find('scene')))

    def test_xml_removal_spheres_supply_runtime_settings(self):
        source, target, copy = [self.folder / name for name in ('old.pwn','map.pwn','copy.pwn')]
        source.write_text('stock Init(playerid){ RemoveBuildingForPlayer(playerid, -1, 100.0, 200.0, 3.0, 1.0); }\n')
        self.run_pairs((source,target))
        xml=self.xml(target)
        xml.getroot().find('./removals/sphere').set('radius','2.5')
        xml.write(str(target)+'.xml',encoding='utf-8',xml_declaration=True)
        self.run_pairs((target,copy))
        self.assertIn('RemoveBuildingForPlayer(playerid, -1, 100.0, 200.0, 3.0, 2.5)',copy.read_text())
        self.assertEqual(self.xml(copy).getroot().find('./removals/sphere').get('radius'),'2.5')

    def test_invalid_xml_is_not_overwritten(self):
        target = self.folder / 'map.pwn'
        original = 'stock Init(){ CreateObject(1271, 1.0, 2.0, 3.0, 0.0, 0.0, 0.0); }\n'
        target.write_text(original)
        sidecar = Path(str(target)+'.xml')
        sidecar.write_text('<haumea version="1" kind="map"><scene><object id="1" model="1271" position.x="nan"/></scene></haumea>')
        saved_xml = sidecar.read_bytes()
        result = run_editor([BINARY,'--game',GAME,'--pawn-roundtrip',str(target),str(target)], cwd=self.folder,
                            env=editor_env(self.folder), capture_output=True, text=True)
        self.assertIn(': -1 operation(s)', result.stdout)
        self.assertEqual(target.read_text(), original)
        self.assertEqual(sidecar.read_bytes(), saved_xml)

    def test_textdraw_layers_guides_and_clean_runtime(self):
        source, target, copy = [self.folder / name for name in ('old.pwn','hud.pwn','hud2.pwn')]
        source.write_text('''stock Init(){
// Haumea: guide x 25.000 board "Login"
// Haumea: reference "reference.png" 1.0 2.0 100.0 50.0 0xFFFFFFFF locked board "Login"
new Text:title = TextDrawCreate(30.0, 40.0, "Hello"); // Haumea: locked hidden group "Header" board "Login" shape "rounded 1 2"
}\n''')
        self.run_pairs((source,target),textdraw=True)
        self.assertNotIn('// Haumea:',target.read_text())
        self.assertNotIn('reference.png',target.read_text())
        saved = self.xml(target)
        layers=saved.findall('./textdraws/textdraw')
        self.assertEqual(len(layers),2)
        self.assertEqual(layers[0].get('font'),'6')
        self.assertEqual(layers[1].get('group'),'Header')
        self.assertEqual(layers[1].get('locked'),'true')
        self.assertEqual(layers[1].get('shape'),'rounded 1 2')
        self.assertEqual(saved.getroot().find('./textdraws/guide').get('at'),'25')
        self.run_pairs((target,copy),textdraw=True)
        self.assertEqual(ET.tostring(saved.getroot().find('textdraws')), ET.tostring(self.xml(copy).getroot().find('textdraws')))
        self.assertEqual(target.read_text(),copy.read_text().replace('hud2','hud'))
