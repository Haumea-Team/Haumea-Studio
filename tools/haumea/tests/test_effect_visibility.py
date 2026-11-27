# SPDX-License-Identifier: GPL-2.0-or-later
"""Light coronas and effect markers must follow their owning instance."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_samp_rotation import extract

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which(os.environ.get('CXX', 'c++')), 'needs a C++ compiler')
class EffectVisibilityTests(unittest.TestCase):
    def test_deleted_hidden_and_filtered_instances_emit_no_effects(self):
        source = (ROOT / 'tools/haumea/effects.cpp').read_text()
        render = '\n'.join(extract(source, signature) for signature in (
            'void\nRenderLights(void)', 'void\nRender(void)'))
        fixture = r'''
#include <cassert>
#include <vector>
#define nil nullptr
namespace rw {
 enum {FOGENABLE,VERTEXALPHA,ZTESTENABLE,ZWRITEENABLE,SRCBLEND,DESTBLEND,
       TEXTURERASTER,BLENDONE,BLENDINVSRCALPHA};
 void SetRenderState(int,int){}
 void SetRenderStatePtr(int,void*){}
}
void DefinedState(){}
struct ObjectInst {
 int m_objectId=1;
 bool m_isDeleted=false,m_editorHidden=false,playerHidden=false,attachmentVisible=true;
};
struct CPtrNode {void*item; CPtrNode*next;};
struct {CPtrNode*first;} instances;
// What BuildRenderList's scan saw: here, every instance (the filters are RenderLights').
std::vector<ObjectInst*> scanned;
const std::vector<ObjectInst*>&GetScannedInsts(){
 scanned.clear();
 for(CPtrNode*n=instances.first;n;n=n->next) scanned.push_back((ObjectInst*)n->item);
 return scanned;
}
struct ObjectDef {int m_effectIndex=0,m_numEffects=2;} model;
ObjectDef*GetObjectDef(int id){return id==1?&model:nil;}
bool PlayerViewHides(ObjectInst*i){return i->playerHidden;}
bool AttachmentWorkspaceShowsInstance(ObjectInst*i){return i->attachmentVisible;}
enum {FX_LIGHT,FX_PARTICLE};
struct Effect {int type;};
Effect effects[]={{FX_LIGHT},{FX_PARTICLE}};
Effect*GetEffect(int i){return &effects[i];}
std::vector<ObjectInst*> lights,markers;
void RenderLightEffect(Effect*,ObjectInst*i){lights.push_back(i);}
void RenderEffect(Effect*,ObjectInst*i){markers.push_back(i);}
''' + render + r'''
void check(int expected){
 lights.clear();markers.clear();RenderLights();Render();
 assert(lights.size()==expected);
 assert(markers.size()==expected*2);
}
int main(){
 ObjectInst first,second;
 CPtrNode b{&second,nil},a{&first,&b};instances.first=&a;
 check(2);
 // Deletion and redo hide only this instance, keeping another of the same model.
 first.m_isDeleted=true;check(1);assert(lights.front()==&second);
 // Undo restores the instance's light without mutating the shared model effects.
 first.m_isDeleted=false;check(2);
 first.m_isDeleted=true;check(1);
 second.m_editorHidden=true;check(0);
 second.m_editorHidden=false;check(1);
 // Player-view filters and isolated attachment previews hide the effects too.
 second.playerHidden=true;check(0);
 second.playerHidden=false;check(1);
 second.attachmentVisible=false;check(0);
 second.attachmentVisible=true;check(1);
 // A model with no known definition contributes no orphan lights or markers.
 second.m_objectId=99;check(0);
}
'''
        with tempfile.TemporaryDirectory(prefix='haumea-effect-visibility-') as tmp:
            cpp, exe = Path(tmp, 'effects.cpp'), Path(tmp, 'effects')
            cpp.write_text(fixture)
            build = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11', str(cpp), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
