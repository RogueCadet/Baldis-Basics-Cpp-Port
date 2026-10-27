# Make the iOS 6 icon + launch-image set for the Xcode project from the GENUINE iOS app's own images.
# The genuine bundle's PNGs are Apple "CgBI" (Xcode pngcrush): a CgBI chunk before IHDR, IDAT as RAW
# deflate (no zlib header), pixels BGRA with colour premultiplied by alpha, standard PNG row filters.
# They are decoded here, then resized to the names/sizes iOS 6 looks for:
#   icons: Icon.png 57, Icon@2x.png 114, Icon-72.png 72, Icon-72@2x.png 144 (CFBundleIconFiles)
#   launch: Default.png 320x480, Default@2x.png 640x960, Default-568h@2x.png 640x1136 (without it an
#   iPhone 5 runs the app letterboxed), Default-Landscape~ipad.png 1024x768,
#   Default-Landscape@2x~ipad.png 2048x1536.
# usage: py -3.10 -I tools/ios-app-images.py <dir with PIL (e.g. the UnityPy install)> <out dir>
import os
import struct
import sys
import zlib

sys.path.insert(0, sys.argv[1])
from PIL import Image  # noqa: E402

APP = 'F:/baldidecomp/mobile/BALDI_1.4.3_iOS/Payload/BaldisBasicsClassic.app'
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)


def read_cgbi(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'\x89PNG\r\n\x1a\n'
    pos, w, h, idat, cgbi = 8, 0, 0, b'', False
    while pos < len(b):
        ln, typ = struct.unpack('>I4s', b[pos:pos + 8])
        data = b[pos + 8:pos + 8 + ln]
        if typ == b'CgBI':
            cgbi = True
        elif typ == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', data[:10])
            assert depth == 8 and ctype == 6, (path, depth, ctype)   # 8-bit RGBA layout
        elif typ == b'IDAT':
            idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat, -15 if cgbi else 15)
    stride = w * 4
    px = bytearray(h * stride)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            up = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + up) & 255
            elif f == 3: line[x] = (line[x] + ((a + up) >> 1)) & 255
            elif f == 4:
                p = a + up - c
                pa, pb, pc = abs(p - a), abs(p - up), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else up if pb <= pc else c)) & 255
        px[y * stride:(y + 1) * stride] = line
        prev = line
    if cgbi:   # BGRA premultiplied -> RGBA straight
        for i in range(0, len(px), 4):
            bl, g, r, al = px[i], px[i + 1], px[i + 2], px[i + 3]
            if al:
                r, g, bl = min(255, r * 255 // al), min(255, g * 255 // al), min(255, bl * 255 // al)
            px[i], px[i + 1], px[i + 2], px[i + 3] = r, g, bl, al
    return Image.frombytes('RGBA', (w, h), bytes(px))


src_icon = read_cgbi(APP + '/AppIcon60x60@3x.png')      # the largest icon (180 px)
for name, size in (('Icon.png', 57), ('Icon@2x.png', 114), ('Icon-72.png', 72), ('Icon-72@2x.png', 144)):
    src_icon.resize((size, size), Image.LANCZOS).convert('RGB').save(os.path.join(OUT, name))
launch = {   # genuine launch image -> iOS 6 name, size
    'LaunchImage.png': ('Default.png', (320, 480)),
    'LaunchImage@2x.png': ('Default@2x.png', (640, 960)),
    'LaunchImage-568h@2x.png': ('Default-568h@2x.png', (640, 1136)),
    'LaunchImage-Landscape~ipad.png': ('Default-Landscape~ipad.png', (1024, 768)),
    'LaunchImage-Landscape@2x~ipad.png': ('Default-Landscape@2x~ipad.png', (2048, 1536)),
}
for src, (name, size) in launch.items():
    im = read_cgbi(APP + '/' + src)
    if im.size != size:
        print('  %s is %dx%d, resized to %dx%d' % (src, im.size[0], im.size[1], size[0], size[1]))
        im = im.resize(size, Image.LANCZOS)
    im.convert('RGB').save(os.path.join(OUT, name))
print('wrote', sorted(os.listdir(OUT)))
