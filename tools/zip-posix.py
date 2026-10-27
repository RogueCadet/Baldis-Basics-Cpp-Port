# Zip a folder with POSIX ('/') entry names, for unzipping on the Mac. Windows PowerShell 5.1's
# Compress-Archive writes '\' separators, which macOS Archive Utility / unzip keep as literal
# characters in flat file names (so BaldiCpp.xcodeproj never appears as a folder).
# usage: python3 tools/zip-posix.py <folder> <out.zip>   (the folder itself is the top-level entry)
import os
import sys
import zipfile

src, out = os.path.abspath(sys.argv[1]), sys.argv[2]
top = os.path.basename(src)
n = 0
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        rel = os.path.relpath(root, src).replace(os.sep, '/')
        base = top if rel == '.' else top + '/' + rel
        z.writestr(zipfile.ZipInfo(base + '/'), '')
        for f in sorted(files):
            if f.endswith(('.command', '.sh')):
                # Windows files carry no executable bit; set rwxr-xr-x (Unix mode in the high 16
                # bits, create_system 3 = Unix) so Finder can run the script after unzipping.
                zi = zipfile.ZipInfo.from_file(os.path.join(root, f), base + '/' + f)
                zi.create_system = 3
                zi.external_attr = (0o100755 << 16)
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(os.path.join(root, f), 'rb') as fh:
                    z.writestr(zi, fh.read())
            else:
                z.write(os.path.join(root, f), base + '/' + f)
            n += 1
with zipfile.ZipFile(out) as z:
    names = z.namelist()
    for zi in z.infolist():
        if zi.filename.endswith(('.command', '.sh')):
            assert (zi.external_attr >> 16) & 0o111, 'not executable in the zip: ' + zi.filename
bad = [x for x in names if '\\' in x]
assert not bad, 'backslash entry names: %s' % bad[:5]
need = top + '/BaldiCpp.xcodeproj/project.pbxproj'
assert need in names, 'missing ' + need
print('zip-posix: %d files, %d entries, all "/"-separated; %s present' % (n, len(names), need))
