"""Compile production pilot-motion policy and camera/rig integration tests."""
import argparse
from pathlib import Path
import subprocess
import re
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_camera import function

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig',required=True)
    args=p.parse_args()
    exe=ROOT/'build/comfort-tests/pilot_motion_test.exe'
    exe.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run([args.zig,'cc','-std=c11','-O2','-UNDEBUG','-I',str(ROOT/'src'),
                    str(ROOT/'comfort/tests/pilot_motion_test.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    production=(ROOT/'src/xwa_remaster/flight.c').read_text()
    names=['fl_rows_to_quat','fl_quat_to_mat3','fl_comfort_shake_scale','fl_curmat_from_cached',
           'fl_transformaxes','fl_curmat_from_euler','fl_object_world','fl_model_matrix',
           'fl_world_to_view','fl_cockpit_model_matrix','fl_xr_env','fl_xr_eye_camera',
           'fl_pilot_motion_apply_view','fl_pilot_motion_rig_base','fl_pilot_motion_space','fl_pilot_motion_update']
    prefix='''#include "xwa_remaster/flight.h"
#include "aeron/xr.h"
#include "xwa_remaster/vr_comfort.h"
#include "xwa_remaster/vr_pilot_motion.h"
#include "xwa_remaster/vr_pilot_space.h"
#include "aeron/asset/opt_model.h"
#include <assert.h>
#include <stdio.h>
static XwaVrPilotMotion s_pilot_motion;
static struct { int hyperspace_preview; } s;
static int unfocused, reprojects;
#define Aeron_XrIsUnfocused() unfocused
#define Aeron_XrIsPresenting() 1
static XwaFlightObject test_anchor;
static const XwaFlightObject* fl_cockpit_anchor(const XwaSnapshot* snap,const XwaFlightCamera* cam) { (void)snap; (void)cam; return &test_anchor; }
static int fl_cockpit_drawn(const XwaSnapshot* snap,const XwaFlightCamera* cam) { (void)snap; return cam->cockpit_visible; }
void AeronScene_ComputeViewProj(const AeronSceneCamera* camera,float out[16]) { (void)camera; reprojects++; memset(out,0,16*sizeof(float)); }
void Aeron_LogInfo(const char* category,const char* fmt,...) { (void)category; (void)fmt; }
int Aeron_XrGetHand(int h,AeronXrHand* out) { (void)h; (void)out; return 0; }
int Aeron_XrLocateRenderModel(int h,float p[3],float q[4],AeronXrRenderModelNode* nodes,uint32_t cap) { (void)h; (void)p; (void)q; (void)nodes; (void)cap; return 0; }
'''
    prefix+=re.search(r'^#define FL_Q16_TO_RAD .+$',production,re.M)[0]+'\n'
    source=exe.with_name('pilot_camera_test.c')
    source.write_text(prefix+'\n'.join(function(production,name) for name in names)+'\n'+
                      (ROOT/'comfort/tests/pilot_camera_test.c').read_text())
    camera_exe=exe.with_name('pilot_camera_test.exe')
    subprocess.run([args.zig,'cc','-std=c11','-O2','-UNDEBUG','-I',str(ROOT/'src'),
                    '-I',str(ROOT/'aeron/include'),'-I',str(ROOT/'build/deps/SDL/include'),
                    str(source),str(ROOT/'src/xwa_remaster/vr_pilot_space.c'),'-o',str(camera_exe)],check=True)
    subprocess.run([str(camera_exe)],check=True)

if __name__=='__main__': main()
