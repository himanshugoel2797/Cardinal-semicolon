#!/usr/bin/env bash
# Self-test for the simulator: build it, run the demo offscreen with a scripted
# input sequence, and assert the final framebuffer encodes that input (the box
# moved / recoloured / recentred as scripted). Deterministic and display-free,
# so it runs in CI. Exit non-zero on any mismatch.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

bash "$HERE/build.sh"

run() {  # run <script-or-empty> <ppm>
  local script="$1" ppm="$2"
  if [[ -n "$script" ]]; then
    SIM_BACKEND=offscreen SIM_SCRIPT="$script" SIM_PPM="$ppm" "$HERE/sim" >/dev/null
  else
    SIM_BACKEND=offscreen SIM_PPM="$ppm" "$HERE/sim" >/dev/null
  fi
}

echo "[selftest] scripted-input frame..."
run "$HERE/demo-script.txt" "$HERE/sim-test-scripted.ppm"
echo "[selftest] initial frame (no script)..."
run "" "$HERE/sim-test-initial.ppm"

python3 - "$HERE/sim-test-scripted.ppm" "$HERE/sim-test-initial.ppm" <<'PY'
import sys
def reader(p):
    f=open(p,'rb')
    assert f.readline().strip()==b'P6', "not a P6 ppm"
    w,h=map(int,f.readline().split())
    assert f.readline().strip()==b'255'
    d=f.read()
    return w,h,(lambda x,y:(d[(y*w+x)*3],d[(y*w+x)*3+1],d[(y*w+x)*3+2]))

fails=0
def ck(name,px,xy,want):
    global fails
    got=px(*xy)
    ok = got==want
    if not ok: fails+=1
    print(("  PASS" if ok else "  FAIL"), name, "at", xy, "got", got, "want", want)

# Scripted run: 3x Right + 2x Down from centre, colour cycled twice (-> blue),
# then a click recentres the box on (500,300). Final box covers x476..524,
# y276..324, blue interior with a white outline.
w,h,px=reader(sys.argv[1])
assert (w,h)==(640,480), (w,h)
print("[scripted]")
ck("box interior is blue (click recentred)", px, (500,300), (90,160,245))
ck("title bar",                              px, (10,5),    (40,60,110))
ck("background uncovered",                   px, (320,450), (20,24,40))
ck("box outline white",                      px, (476,300), (255,255,255))

# Initial frame: centred box (296..344, 216..264), default colour red.
w,h,px=reader(sys.argv[2])
print("[initial]")
ck("box interior is red (default)", px, (320,240), (235,80,80))
ck("title bar",                     px, (10,5),    (40,60,110))
ck("background uncovered",          px, (600,460), (20,24,40))

if fails:
    print(f"\n[selftest] {fails} check(s) FAILED")
    sys.exit(1)
print("\n[selftest] all checks passed")
PY
echo "[selftest] OK"
