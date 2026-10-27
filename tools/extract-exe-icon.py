# Rebuild a multi-size .ico from a Windows executable's icon resources (RT_GROUP_ICON + its RT_ICON
# images), byte-for-byte as stored -- used to give the port's baldi.exe the genuine BALDI.exe icon.
# usage: python3 tools/extract-exe-icon.py <exe> <out.ico>
import struct
import sys

import pefile

pe = pefile.PE(sys.argv[1])
RT_ICON, RT_GROUP_ICON = 3, 14
icons, group = {}, None
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    for e in t.directory.entries:
        lang = e.directory.entries[0]
        data = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
        if t.id == RT_ICON:
            icons[e.id] = data
        elif t.id == RT_GROUP_ICON and group is None:
            group = data          # the first group is the application icon
assert group, 'no RT_GROUP_ICON'
_, _, count = struct.unpack_from('<HHH', group, 0)
entries = []
for i in range(count):
    w, h, colors, _, planes, bpp, size, rid = struct.unpack_from('<BBBBHHIH', group, 6 + i * 14)
    entries.append((w, h, colors, planes, bpp, icons[rid]))
out = bytearray(struct.pack('<HHH', 0, 1, len(entries)))
offset = 6 + 16 * len(entries)
for w, h, colors, planes, bpp, img in entries:
    out += struct.pack('<BBBBHHII', w, h, colors, 0, planes, bpp, len(img), offset)
    offset += len(img)
for e in entries:
    out += e[5]
open(sys.argv[2], 'wb').write(out)
print('%s: %d images (%s)' % (sys.argv[2], len(entries),
      ', '.join('%dx%d %dbpp' % (e[0] or 256, e[1] or 256, e[4]) for e in entries)))
