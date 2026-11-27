# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production uniform distance and LOD pass with controlled scene data."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class RenderDistanceTests(unittest.TestCase):
    def test_distance_and_lod_coverage(self):
        source = (ROOT / 'tools/haumea/renderer.cpp').read_text()
        queue = source[source.index('static void\nAddToLodRenderList'):
                       source.index('// One hard range')]
        distance = source[source.index('static bool\nbeyondDrawDistance'):
                          source.index('// Don\'t assume there are LODs')]
        hd = source[source.index('static int\nSetupHdVisibilitySA'):
                    source.index('static void\nProcessLodList')]
        lods = source[source.index('static void\nProcessLodList'):
                      source.index('static rw::RGBA highlightColor')]
        fixture = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>
using std::min; using std::max;
#define nil nullptr
float clamp(float v,float lo,float hi){return max(lo,min(v,hi));}
struct Vec {float x=0,y=0,z=0;};
struct Col {struct {float radius=10;} boundingSphere;};
struct ObjectDef {float draw=200; Col*m_colModel=nullptr; float GetLargestDrawDist(){return draw;}} model;
struct ObjectInst {
 ObjectInst*m_lod=nullptr; int m_objectId=0,m_numChildren=0,m_numChildrenRendered=0;
 bool m_isBigBuilding=false,user=false,onScreen=true,canCreate=true;
 bool m_selected=false; int m_highlight=0;
 Vec m_translation; void*m_rwObject=(void*)1;
 void*CreateRwObject(){return m_rwObject=canCreate?(void*)1:nullptr;}
 bool IsOnScreen(){return onScreen;}
};
struct Camera {
 Vec m_position; float m_LODmult=1;
 struct RwCamera {float farPlane=700; Vec viewWindow;} rwcam,*m_rwcam=&rwcam;
 float distanceTo(Vec p){return sqrtf(powf(p.x-m_position.x,2)+powf(p.y-m_position.y,2)+powf(p.z-m_position.z,2));}
} TheCamera;
namespace Timecycle {float currentLodDistMult=1;}
namespace sk {struct {float height=1000;} globals;}
int currentArea=0;
float gStreamDistance=1000;
ObjectDef*GetObjectDef(int){return &model;}
struct InstDist {ObjectInst*inst; float dist;};
std::vector<InstDist> lodList;
std::vector<ObjectInst*> drawn;
void AddToRenderList(ObjectInst*i,float){drawn.push_back(i);}
enum {HIGHLIGHT_NONE,HIGHLIGHT_MATCH,HIGHLIGHT_SELECTION};
void MarkInstTouched(ObjectInst*){}
'''
        visibility = r'''
static int SetupVisibilitySA(ObjectInst*i,float distance){return !beyondDrawDistance(distance);}
'''
        cases = r'''
int main(){
 ObjectInst prop,lod,parent,grandparent;
 Col col; model.m_colModel=&col;
 TheCamera.rwcam.viewWindow.y=1;
 // Every model uses the same range, regardless of IDE radius, altitude or weather.
 for(float modelDistance : {40.0f, 80.0f, 1500.0f}){
  model.draw=modelDistance;
  for(float weatherFar : {300.0f, 800.0f, 2000.0f}){
   TheCamera.rwcam.farPlane=weatherFar;
   assert(!beyondDrawDistance(999));
   assert(beyondDrawDistance(1000));
  }
 }
 // A short-distance HD building stays detailed beyond the game's 80 m switch.
 model.draw=80; parent.m_translation.x=800; prop.m_lod=&parent;
 prop.m_translation.x=400;
 float d=0;
 assert(SetupHdVisibilitySA(&prop,&d)==1 && d==400);
 // Reducing the common range takes effect immediately.
 gStreamDistance=350;
 assert(SetupHdVisibilitySA(&prop,&d)==0);
 gStreamDistance=1000;
 // One visible child suppresses the coarse fallback even with other children absent.
 lod.m_lod=nullptr; lod.m_numChildrenRendered=1;
 lodList={{&lod,100}}; ProcessLodList(); assert(drawn.empty());
 // Highlighting a selected building's LOD must not bypass coverage resolution.
 lodList.clear(); lod.m_highlight=HIGHLIGHT_SELECTION;
 AddToLodRenderList(&lod,100); ProcessLodList(); assert(drawn.empty());
 // With no visible children, the fallback renders; unavailable and offscreen ones don't.
 lod.m_numChildrenRendered=0; ProcessLodList(); assert(drawn.size()==1);
 drawn.clear(); lod.m_rwObject=nullptr; lod.canCreate=false;
 ProcessLodList(); assert(drawn.empty());
 lod.canCreate=true; lod.onScreen=false; ProcessLodList(); assert(drawn.empty());
 // A descendant suppresses super-LODs even when their sector was visited first.
 lod.onScreen=true; lod.m_lod=&parent; parent.m_lod=&grandparent;
 lod.m_numChildrenRendered=1;
 lodList={{&grandparent,500},{&parent,300},{&lod,100}};
 ProcessLodList(); assert(drawn.empty());
}
'''
        with tempfile.TemporaryDirectory(prefix='haumea-sa-render-') as directory:
            cpp = Path(directory) / 'render.cpp'
            exe = Path(directory) / 'render'
            cpp.write_text(fixture + queue + distance + visibility + hd + lods + cases)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++11',
                            str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
