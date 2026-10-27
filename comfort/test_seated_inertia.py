"""Exercise real cockpit-space pose adapters and production virtual-stick logic."""
import argparse
from pathlib import Path
import subprocess
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_camera import function

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig',required=True)
    args=p.parse_args()
    stick=(ROOT/'src/xwa_remaster/vr_flight_stick.c').read_text()
    prefix=stick[:stick.index('/* ---- small math')]
    prefix+='''
#include <assert.h>
static AeronXrHand raw_hands[2];
static int tracked[2], model_tracked;
static uint64_t test_ns;
static int virtual_active;
static float virtual_axes[3];
int test_setenv(const char* key,const char* value,int overwrite) { (void)overwrite; return _putenv_s(key,value); }
#define setenv test_setenv
Uint64 SDL_GetTicksNS(void) { return test_ns; }
int XwaVrStick_Enabled(void) { return 1; }
void Aeron_XrHapticPulse(int hand,float amplitude,int duration_ms) { (void)hand; (void)amplitude; (void)duration_ms; }
void Aeron_LogInfo(const char* category,const char* fmt,...) { (void)category; (void)fmt; }
void XwaVrConsole_CaptureGrip(int hand) { (void)hand; }
void Aeron_XrSetVirtualStick(int active,float yaw,float pitch,float roll) { virtual_active=active; virtual_axes[0]=yaw; virtual_axes[1]=pitch; virtual_axes[2]=roll; }
int Aeron_XrGetHand(int hand,AeronXrHand* out) { *out=raw_hands[hand]; return tracked[hand]; }
int Aeron_XrLocateRenderModel(int hand,float pos[3],float q[4],AeronXrRenderModelNode* nodes,uint32_t capacity) {
 (void)hand; if(!model_tracked) return 0;
 pos[0]=0.1f; pos[1]=-0.2f; pos[2]=-0.3f; q[0]=q[1]=q[2]=0; q[3]=1;
 if(nodes && capacity) { memset(nodes,0,sizeof *nodes); nodes[0].position[1]=0.04f; nodes[0].orientation[3]=1; nodes[0].visible=1; }
 return 1;
}
int XwaVrGloves_Capsules(int hand,XwaVrGloveCapsule* out,int max) {
 if(!tracked[hand] || max<1) return 0;
 memcpy(out[0].a,raw_hands[hand].position,sizeof out[0].a);
 XwaVrPilotSpace_ToCockpit(out[0].a);
 memcpy(out[0].b,out[0].a,sizeof out[0].b); out[0].radius=0.02f; return 1;
}
'''
    names=['st_dot','st_rotate','st_axis','st_env','st_place','st_palm','st_grip_distance',
           'st_hand_yaw','st_wrap','st_axis_value','test_stick_update']
    stick=stick.replace('void XwaVrStick_Update(', 'static void test_stick_update(')
    source=ROOT/'build/comfort-tests/seated_inertia_test.c'
    source.parent.mkdir(parents=True,exist_ok=True)
    source.write_text(prefix+'\n'.join(function(stick,name) for name in names)+'\n'+
                      (ROOT/'comfort/tests/seated_inertia_test.c').read_text())
    exe=source.with_suffix('.exe')
    subprocess.run([args.zig,'cc','-std=c11','-O2','-UNDEBUG','-I',str(ROOT/'src'),
                    '-I',str(ROOT/'aeron/include'),'-I',str(ROOT/'build/deps/SDL/include'),
                    str(source),str(ROOT/'src/xwa_remaster/vr_pilot_space.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

if __name__=='__main__': main()
