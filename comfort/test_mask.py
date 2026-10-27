"""Test production mask policy, ship sampling and GPU submission contracts."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_camera import function

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig',required=True)
    args=parser.parse_args()
    production=(ROOT/'src/xwa_remaster/flight.c').read_text()
    names=['fl_rows_to_quat','fl_quat_to_mat3','fl_curmat_from_cached','fl_transformaxes',
           'fl_curmat_from_euler','fl_comfort_ship_quaternion']
    prefix='#include <math.h>\n#include <string.h>\n#include "xwa_runtime/snapshot/snapshot.h"\n'
    prefix+=re.search(r'^#define FL_Q16_TO_RAD .+$',production,re.M)[0]+'\n'
    source=ROOT/'build/comfort-tests/mask_test.c'
    source.parent.mkdir(parents=True,exist_ok=True)
    source.write_text(prefix+'\n'.join(function(production,name) for name in names)+'\n'+
                      (ROOT/'comfort/tests/mask_test.c').read_text())
    executable=source.with_suffix('.exe')
    subprocess.run([args.zig,'cc','-std=c11','-O2','-UNDEBUG','-I',str(ROOT/'src'),
                    '-I',str(ROOT/'aeron/include'),str(source),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)

if __name__=='__main__': main()
