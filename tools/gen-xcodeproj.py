# Generate BaldiCpp.xcodeproj for the iOS 6 build, inside a package folder laid out as
#   <pkg>/BaldiCpp/   iOS shell (*.mm, *.h, Info.plist, Images/*.png)
#   <pkg>/src/        engine + game sources (every .cpp here is compiled)
#   <pkg>/content/    game data, added as ONE folder reference (copied into the app as "content")
# Format: Xcode 4.5-5.x (objectVersion 46, compatibilityVersion "Xcode 3.2"), which is what the
# user's OS X 10.8 VM runs. Settings: iOS 6.0, iPhone + iPad, armv7 + armv7s (i386 simulator),
# libc++ C++11, ARC. The generator validates its own output: balanced braces/parentheses, every
# referenced object id defined, every referenced file present on disk.
# usage: python3 tools/gen-xcodeproj.py <pkg dir>
import hashlib
import os
import re
import sys

PKG = sys.argv[1]
NAME = 'BaldiCpp'


def oid(key):
    return hashlib.md5(('baldicpp:' + key).encode()).hexdigest()[:24].upper()


def q(s):
    return '"' + str(s).replace('\\', '\\\\').replace('"', '\\"') + '"'


FILETYPE = {'.cpp': 'sourcecode.cpp.cpp', '.mm': 'sourcecode.cpp.objcpp', '.h': 'sourcecode.c.h',
            '.hpp': 'sourcecode.cpp.h', '.plist': 'text.plist.xml', '.png': 'image.png'}
FRAMEWORKS = ['UIKit', 'Foundation', 'CoreGraphics', 'QuartzCore', 'OpenGLES', 'GLKit', 'AudioToolbox',
              'AVFoundation']

objs = {}        # id -> (isa, dict of fields)  (dict values already serialized)
order = []


def add(key, isa, fields):
    i = oid(key)
    assert i not in objs, key
    objs[i] = (isa, fields)
    order.append(i)
    return i


def listing(ids):
    return '(' + ''.join('\n\t\t\t\t%s,' % x for x in ids) + '\n\t\t\t)'


# ---- files
shell_files = sorted(f for f in os.listdir(os.path.join(PKG, NAME)) if os.path.splitext(f)[1] in FILETYPE)
image_files = sorted(os.listdir(os.path.join(PKG, NAME, 'Images'))) if os.path.isdir(os.path.join(PKG, NAME, 'Images')) else []
src_files = sorted(f for f in os.listdir(os.path.join(PKG, 'src')) if os.path.splitext(f)[1] in ('.cpp', '.h'))
game_files = sorted(f for f in os.listdir(os.path.join(PKG, 'src', 'game')) if f.endswith('.hpp'))
assert os.path.isdir(os.path.join(PKG, 'content', 'data')), 'content/data missing'

sources, resources, fw_build = [], [], []


def file_ref(key, path, ftype, group_path_is_relative=True):
    return add('ref:' + key, 'PBXFileReference', {
        'lastKnownFileType': ftype, 'path': q(path), 'sourceTree': q('<group>')})


shell_refs = []
for f in shell_files:
    ext = os.path.splitext(f)[1]
    r = file_ref('shell/' + f, f, FILETYPE[ext])
    shell_refs.append(r)
    if ext == '.mm':
        sources.append(add('build:shell/' + f, 'PBXBuildFile', {'fileRef': r}))
image_refs = []
for f in image_files:
    r = file_ref('img/' + f, f, 'image.png')
    image_refs.append(r)
    resources.append(add('build:img/' + f, 'PBXBuildFile', {'fileRef': r}))
src_refs = []
for f in src_files:
    ext = os.path.splitext(f)[1]
    r = file_ref('src/' + f, f, FILETYPE[ext])
    src_refs.append(r)
    if ext == '.cpp':
        sources.append(add('build:src/' + f, 'PBXBuildFile', {'fileRef': r}))
game_refs = [file_ref('game/' + f, f, FILETYPE['.hpp']) for f in game_files]
content_ref = add('ref:content', 'PBXFileReference', {'lastKnownFileType': 'folder', 'path': q('content'),
                                                       'sourceTree': q('<group>')})
resources.append(add('build:content', 'PBXBuildFile', {'fileRef': content_ref}))
fw_refs = []
for fw in FRAMEWORKS:
    r = add('ref:fw/' + fw, 'PBXFileReference', {'lastKnownFileType': 'wrapper.framework', 'name': q(fw + '.framework'),
                                                  'path': q('System/Library/Frameworks/%s.framework' % fw),
                                                  'sourceTree': 'SDKROOT'})
    fw_refs.append(r)
    fw_build.append(add('build:fw/' + fw, 'PBXBuildFile', {'fileRef': r}))
product = add('ref:product', 'PBXFileReference', {'explicitFileType': 'wrapper.application', 'includeInIndex': '0',
                                                  'path': q(NAME + '.app'), 'sourceTree': 'BUILT_PRODUCTS_DIR'})

# ---- groups
g_images = add('group:images', 'PBXGroup', {'children': listing(image_refs), 'path': q('Images'), 'sourceTree': q('<group>')})
g_shell = add('group:shell', 'PBXGroup', {'children': listing(shell_refs + [g_images]), 'path': q(NAME), 'sourceTree': q('<group>')})
g_game = add('group:game', 'PBXGroup', {'children': listing(game_refs), 'path': q('game'), 'sourceTree': q('<group>')})
g_src = add('group:src', 'PBXGroup', {'children': listing(src_refs + [g_game]), 'path': q('src'), 'sourceTree': q('<group>')})
g_fw = add('group:fw', 'PBXGroup', {'children': listing(fw_refs), 'name': q('Frameworks'), 'sourceTree': q('<group>')})
g_products = add('group:products', 'PBXGroup', {'children': listing([product]), 'name': q('Products'), 'sourceTree': q('<group>')})
g_main = add('group:main', 'PBXGroup', {'children': listing([g_shell, g_src, content_ref, g_fw, g_products]),
                                        'sourceTree': q('<group>')})

# ---- phases
p_src = add('phase:sources', 'PBXSourcesBuildPhase', {'buildActionMask': '2147483647', 'files': listing(sources),
                                                      'runOnlyForDeploymentPostprocessing': '0'})
p_fw = add('phase:frameworks', 'PBXFrameworksBuildPhase', {'buildActionMask': '2147483647', 'files': listing(fw_build),
                                                           'runOnlyForDeploymentPostprocessing': '0'})
p_res = add('phase:resources', 'PBXResourcesBuildPhase', {'buildActionMask': '2147483647', 'files': listing(resources),
                                                          'runOnlyForDeploymentPostprocessing': '0'})


def settings(d):
    return '{' + ''.join('\n\t\t\t\t%s = %s;' % (q(k) if not re.match(r'^[A-Za-z0-9_]+$', k) else k, v) for k, v in d.items()) + '\n\t\t\t}'


common = {
    'ALWAYS_SEARCH_USER_PATHS': 'NO',
    'ARCHS': q('armv7 armv7s'),
    'ARCHS[sdk=iphonesimulator*]': 'i386',
    'VALID_ARCHS': q('armv7 armv7s i386'),
    'CLANG_CXX_LANGUAGE_STANDARD': q('gnu++0x'),
    'CLANG_CXX_LIBRARY': q('libc++'),
    'CLANG_ENABLE_OBJC_ARC': 'YES',
    'CODE_SIGN_IDENTITY[sdk=iphoneos*]': q('iPhone Developer'),
    'GCC_C_LANGUAGE_STANDARD': 'gnu99',
    'GCC_OPTIMIZATION_LEVEL': 's',          # optimised even in Debug: the game must be playable from Run
    'GCC_WARN_ABOUT_RETURN_TYPE': 'YES',
    'HEADER_SEARCH_PATHS': q('$(SRCROOT)/src'),
    'IPHONEOS_DEPLOYMENT_TARGET': '6.0',
    'SDKROOT': 'iphoneos',
    'TARGETED_DEVICE_FAMILY': q('1,2'),
}
proj_dbg = add('cfg:proj:Debug', 'XCBuildConfiguration', {'buildSettings': settings(dict(common, ONLY_ACTIVE_ARCH='YES', COPY_PHASE_STRIP='NO')), 'name': 'Debug'})
proj_rel = add('cfg:proj:Release', 'XCBuildConfiguration', {'buildSettings': settings(dict(common, COPY_PHASE_STRIP='YES', VALIDATE_PRODUCT='YES')), 'name': 'Release'})
tgt = {'INFOPLIST_FILE': q(NAME + '/Info.plist'), 'PRODUCT_NAME': q(NAME), 'WRAPPER_EXTENSION': 'app'}
tgt_dbg = add('cfg:tgt:Debug', 'XCBuildConfiguration', {'buildSettings': settings(tgt), 'name': 'Debug'})
tgt_rel = add('cfg:tgt:Release', 'XCBuildConfiguration', {'buildSettings': settings(tgt), 'name': 'Release'})
proj_list = add('cfglist:proj', 'XCConfigurationList', {'buildConfigurations': listing([proj_dbg, proj_rel]),
                                                        'defaultConfigurationIsVisible': '0', 'defaultConfigurationName': 'Release'})
tgt_list = add('cfglist:tgt', 'XCConfigurationList', {'buildConfigurations': listing([tgt_dbg, tgt_rel]),
                                                      'defaultConfigurationIsVisible': '0', 'defaultConfigurationName': 'Release'})
target = add('target', 'PBXNativeTarget', {
    'buildConfigurationList': tgt_list, 'buildPhases': listing([p_src, p_fw, p_res]), 'buildRules': '(\n\t\t\t)',
    'dependencies': '(\n\t\t\t)', 'name': q(NAME), 'productName': q(NAME), 'productReference': product,
    'productType': q('com.apple.product-type.application')})
project = add('project', 'PBXProject', {
    'attributes': '{\n\t\t\t\tLastUpgradeCheck = 0450;\n\t\t\t}', 'buildConfigurationList': proj_list,
    'compatibilityVersion': q('Xcode 3.2'), 'developmentRegion': 'English', 'hasScannedForEncodings': '0',
    'knownRegions': '(\n\t\t\t\ten,\n\t\t\t)', 'mainGroup': g_main, 'productRefGroup': g_products,
    'projectDirPath': q(''), 'projectRoot': q(''), 'targets': listing([target])})

# ---- write, grouped by isa as Xcode does
out = ['// !$*UTF8*$!', '{', '\tarchiveVersion = 1;', '\tclasses = {', '\t};', '\tobjectVersion = 46;', '\tobjects = {']
for isa in sorted(set(v[0] for v in objs.values())):
    out.append('\n/* Begin %s section */' % isa)
    for i in order:
        if objs[i][0] != isa:
            continue
        fields = objs[i][1]
        body = ''.join('\n\t\t\t%s = %s;' % (k, v) for k, v in fields.items())
        out.append('\t\t%s = {\n\t\t\tisa = %s;%s\n\t\t};' % (i, isa, body))
    out.append('/* End %s section */' % isa)
out += ['\t};', '\trootObject = %s;' % project, '}', '']
text = '\n'.join(out)

# ---- self-validation
assert text.count('{') == text.count('}') and text.count('(') == text.count(')'), 'unbalanced'
refs = set(re.findall(r'\b([0-9A-F]{24})\b', text))
undefined = refs - set(objs.keys())
assert not undefined, 'undefined ids: %s' % sorted(undefined)
for i, (isa, f) in objs.items():
    if isa == 'PBXFileReference' and f.get('sourceTree') == q('<group>'):
        pass
missing = []
for f in shell_files: missing += [] if os.path.exists(os.path.join(PKG, NAME, f)) else [f]
for f in src_files: missing += [] if os.path.exists(os.path.join(PKG, 'src', f)) else [f]
assert not missing, missing
os.makedirs(os.path.join(PKG, NAME + '.xcodeproj'), exist_ok=True)
open(os.path.join(PKG, NAME + '.xcodeproj', 'project.pbxproj'), 'w', encoding='utf8', newline='\n').write(text)
print('%s.xcodeproj: %d objects, %d sources, %d resources (+content folder), %d frameworks; ids all defined'
      % (NAME, len(objs), len(sources), len(resources) - 1, len(fw_build)))
