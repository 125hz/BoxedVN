"""Exercise the production display placement without an Apple SDK."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'ios/runtime/src/BVNAppDelegate.mm').read_text()
start = source.index('extern "C" CGRect BVNGuestContentFrame(')
function = source[start:source.index('\n}', start) + 2]
code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
using CGFloat = double;
struct CGSize { double width, height; };
struct CGPoint { double x, y; };
struct CGRect { CGPoint origin; CGSize size; };
double CGRectGetMidX(CGRect r) { return r.origin.x+r.size.width/2; }
double CGRectGetMidY(CGRect r) { return r.origin.y+r.size.height/2; }
CGRect CGRectMake(double x,double y,double w,double h) { return {{x,y},{w,h}}; }
#define MIN std::min
#define MAX std::max
int mode=0,crop=5;
int BVNGuestPresentationMode() { return mode; }
int BVNGuestFillCropPercent() { return crop; }
'''+function+r'''
int main() {
 const CGSize pixels{800,600};
 const CGRect available{{20,40},{1200,600}};
 auto fit=BVNGuestContentFrame(available,pixels);
 assert(fit.origin.x==220 && fit.origin.y==40);
 assert(fit.size.width==800 && fit.size.height==600);
 mode=2; auto stretch=BVNGuestContentFrame(available,pixels);
 assert(stretch.origin.x==20 && stretch.origin.y==40);
 assert(stretch.size.width==1200 && stretch.size.height==600);
 mode=1; crop=25; auto fill=BVNGuestContentFrame(available,pixels);
 assert(fill.origin.x==20 && fill.origin.y==-110);
 assert(fill.size.width==1200 && fill.size.height==900);
 crop=0; fill=BVNGuestContentFrame(available,pixels);
 assert(fill.size.width==fit.size.width && fill.size.height==fit.size.height);
 mode=0; auto portrait=BVNGuestContentFrame({{0,0},{400,800}},pixels);
 assert(portrait.origin.y==250 && portrait.size.height==300);
 auto empty=BVNGuestContentFrame(available,{0,0});
 assert(empty.size.width==available.size.width);
 assert(pixels.width==800 && pixels.height==600);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'placement.cpp'; exe=Path(tmp)/'placement'
    path.write_text(code)
    subprocess.run(['c++','-std=c++17',str(path),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Production Fit/Fill/Stretch placement and unchanged drawable extent passed')
