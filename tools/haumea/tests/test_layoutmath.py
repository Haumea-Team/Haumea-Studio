# SPDX-License-Identifier: GPL-2.0-or-later
"""Curve subdivision, arc-length placement, and frames through a vertical loop."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

class LayoutMathTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('c++'), 'needs a C++ compiler')
    def test_curves_and_frames(self):
        with tempfile.TemporaryDirectory(prefix='haumea-layout-math-') as tmp:
            source=Path(tmp,'check.cpp')
            source.write_text(r'''
#include "layoutmath.h"
#include <cassert>
using namespace LayoutMath;
bool near(Point a,Point b,float eps=.002f){return length(sub(a,b))<eps;}
int main(){
 std::vector<Knot> line(2);line[0].p={0,0,0};line[1].p={10,0,0};automaticHandles(line,false);
 auto s=sample(line,false,false,2);assert(s.size()==6);for(int i=0;i<6;i++){assert(near(s[i].p,{i*2.f,0,0}));assert(near(s[i].tangent,{1,0,0}));}
 assert(sample(line,true,false,0).empty());assert(sample(line,true,false,1,3).size()==3);
 Knot a=line[0],b=line[1];a.out={2,3,4};b.in={-2,3,-2};Knot originalA=a,originalB=b;
 Knot mid=split(a,b);
 for(int i=0;i<=100;i++){float t=i/100.f;Point divided=t<=.5f?bezier(a,mid,t*2):bezier(mid,b,t*2-1);assert(near(divided,bezier(originalA,originalB,t)));}
 mid.p[2]+=10;assert(bezier(a,mid,1)[2]>10);
 const float radius=10,k=.5522847498f;std::vector<Knot> loop;
 for(int i=0;i<4;i++){float angle=i*1.570796327f;Knot n;n.p={0,radius*std::cos(angle),radius*std::sin(angle)};n.out={0,-radius*std::sin(angle)*k,radius*std::cos(angle)*k};n.in=mul(n.out,-1);n.handles=HANDLES_ALIGNED;loop.push_back(n);}
 auto ring=sample(loop,true,true,.25f);assert(ring.size()>250&&ring.size()<254);
 for(size_t i=0;i<ring.size();i++){
  const auto &p=ring[i];assert(std::isfinite(p.p[2]));assert(std::abs(length(p.normal)-1)<.001f);assert(std::abs(dot(p.normal,p.tangent))<.001f);
  assert(std::abs(std::hypot(p.p[1],p.p[2])-radius)<.01f);
  if(i){assert(dot(p.normal,ring[i-1].normal)>.99f);assert(std::abs(length(sub(p.p,ring[i-1].p))-.25f)<.003f);}
 }
 assert(!near(ring.front().p,ring.back().p,.01f));
 std::vector<Knot> degenerate(3);for(auto &p:degenerate)p.p={1,1,1};assert(sample(degenerate,true,false,1).empty());
 std::vector<Knot> corner(3);corner[0].p={0,0,0};corner[1].p={10,0,0};corner[2].p={10,10,0};auto c=sample(corner,false,false,2);assert(near(c[6].tangent,{0,1,0}));
}''')
            exe=Path(tmp,'check')
            subprocess.run(['c++','-std=c++14','-I',str(Path(__file__).resolve().parents[1]),source,'-o',exe],check=True)
            subprocess.run([exe],check=True)

    @unittest.skipUnless(shutil.which('c++'), 'needs a C++ compiler')
    def test_handles_tilt_and_range(self):
        with tempfile.TemporaryDirectory(prefix='haumea-layout-math-') as tmp:
            source=Path(tmp,'check.cpp')
            source.write_text(r'''
#include "layoutmath.h"
#include <cassert>
using namespace LayoutMath;
bool near(Point a,Point b,float eps=.002f){return length(sub(a,b))<eps;}
int main(){
 // Corner handles point a third of the way at the neighbours: the curve is the polyline.
 std::vector<Knot> corner(3);corner[0].p={0,0,0};corner[1].p={10,0,0};corner[2].p={10,10,0};
 for(auto &k:corner)k.handles=HANDLES_VECTOR;automaticHandles(corner,false);
 assert(near(corner[1].in,{-10/3.f,0,0})&&near(corner[1].out,{0,10/3.f,0})&&near(corner[0].in,{0,0,0}));
 for(int i=0;i<=20;i++){Point p=bezier(corner[0],corner[1],i/20.f);assert(std::abs(p[1])<.001f);}
 auto c=sample(corner,true,false,1);assert(std::abs(c.back().distance-20)<.05f);
 // Tilt eases from point to point and splits keep it.
 std::vector<Knot> line(2);line[0].p={0,0,0};line[1].p={10,0,0};line[1].tilt=90;automaticHandles(line,false);
 auto t=sample(line,true,false,1);assert(t.size()==11);assert(std::abs(t.front().tilt)<.01f&&std::abs(t.back().tilt-90)<.01f);
 assert(std::abs(t[5].tilt-45)<1);assert(t[2].tilt<t[2].distance*9);
 Knot a=line[0],b=line[1];Knot mid=split(a,b);assert(std::abs(mid.tilt-45)<.01f);assert(mid.handles==HANDLES_ALIGNED);
 // A range keeps its ends; an even count over it lands on both.
 auto r=sample(line,true,false,1,2000,.2f,.8f);assert(near(r.front().p,{2,0,0})&&near(r.back().p,{8,0,0})&&r.size()==7);
 float len=curveLength(line,true,false);assert(std::abs(len-10)<.01f);
 auto n=sample(line,true,false,len*.6f/6,7,.2f,.8f);assert(n.size()==7&&near(n.back().p,{8,0,0}));
 auto odd=sample(line,true,false,10/3.f,4);assert(odd.size()==4&&near(odd.back().p,{10,0,0}));
 // A partial range of a closed curve keeps its last object; the whole curve doesn't repeat the first.
 const float radius=10,k=.5522847498f;std::vector<Knot> loop;
 for(int i=0;i<4;i++){float angle=i*1.570796327f;Knot q;q.p={radius*std::cos(angle),radius*std::sin(angle),0};q.out={-radius*std::sin(angle)*k,radius*std::cos(angle)*k,0};q.in=mul(q.out,-1);q.handles=HANDLES_ALIGNED;loop.push_back(q);}
 float ring=curveLength(loop,true,true);auto whole=sample(loop,true,true,ring/8);assert(whole.size()==8);
 auto half=sample(loop,true,true,ring/8,2000,0,.5f);assert(half.size()==5);
}''')
            exe=Path(tmp,'check')
            subprocess.run(['c++','-std=c++14','-I',str(Path(__file__).resolve().parents[1]),source,'-o',exe],check=True)
            subprocess.run([exe],check=True)
