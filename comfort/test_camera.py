"""Compile and exercise the actual production camera math on the Windows host."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    # Mask comments/string literals while preserving indices before matching braces.
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda match: ' ' * len(match[0]), source, flags=re.S)
    match = re.search(r'^static\s+(?:int|float|void)\s+' + re.escape(name) + r'\([^;]*?\)\s*\{', masked, re.M)
    if not match:
        raise RuntimeError(f"Production function not found: {name}")
    start = masked.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True)
    args = parser.parse_args()
    current = (ROOT / 'src/xwa_remaster/flight.c').read_text()
    upstream = subprocess.check_output(['git', 'show', 'v1.0.1:src/xwa_remaster/flight.c'], cwd=ROOT, text=True)
    for name in ('fl_xr_eye_camera', 'fl_quat_to_mat3', 'fl_rows_to_quat'):
        if function(current, name) != function(upstream, name):
            raise RuntimeError(f"Tracking math changed unexpectedly: {name}")
    names = ['fl_rows_to_quat', 'fl_quat_to_mat3', 'fl_comfort_shake_scale',
             'fl_curmat_from_cached', 'fl_transformaxes', 'fl_curmat_from_euler',
             'fl_object_world', 'fl_model_matrix', 'fl_world_to_view',
             'fl_cockpit_model_matrix', 'fl_xr_env', 'fl_xr_eye_camera']
    prefix = '''#include <string.h>
#include "aeron/scene/scene3d.h"
#include "aeron/xr.h"
#include "aeron/asset/opt_model.h"
#include "xwa_remaster/vr_comfort.h"
static const char* test_shake = "0.25";
static const char* test_getenv(const char* key) {
    return strcmp(key, "XWA_COMFORT_IMPACT_SHAKE") == 0 ? test_shake : NULL;
}
#define getenv test_getenv
#define Aeron_XrIsPresenting() 1
'''
    prefix += re.search(r'^#define FL_Q16_TO_RAD .+$', current, re.M)[0] + '\n'
    functions = '\n\n'.join(function(current, name) for name in names)
    legacy = function(upstream, 'fl_cockpit_model_matrix').replace('fl_cockpit_model_matrix', 'legacy_fl_cockpit_model_matrix', 1)
    source = ROOT / 'build/comfort-tests/camera_test.c'
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(prefix + functions + '\n' + legacy + '\n' + (ROOT / 'comfort/tests/camera_test.c').read_text())
    executable = source.with_suffix('.exe')
    subprocess.run([args.zig, 'cc', '-std=c11', '-O2', '-UNDEBUG',
                    '-I', str(ROOT / 'src'), '-I', str(ROOT / 'aeron/include'),
                    '-I', str(ROOT / 'build/deps/SDL/include'), str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
