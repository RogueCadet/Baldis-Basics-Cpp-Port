// BBViewController.mm -- the game view: an OpenGL ES 2.0 GLKView driven by GLKViewController's
// display link. All game work goes through src/app-core (app::start / frame / touch*); this file
// only supplies the GL drawable, the pixel size, the frame time, touches and the orientation.
#import "BBViewController.h"
#import <OpenGLES/ES2/gl.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include "app-core.h"

static const int kMaxTouches = 16;

// Screen.dpi. iOS has no API for it; Unity's iOS player answers from a device table. The value
// only drives the touch pad's Physical value format (pixels / dpi * 100), so the device families
// are what matter: iPhone/iPod 163 ppi (326 retina), iPad 132 ppi (264 retina; the mini's 163 is
// indistinguishable from an iPad 2 here and reads 132).
static float BBScreenDpi(void) {
  const CGFloat scale = [[UIScreen mainScreen] scale];
  const BOOL pad = [[UIDevice currentDevice] userInterfaceIdiom] == UIUserInterfaceIdiomPad;
  if (pad) return scale > 1.5f ? 264.0f : 132.0f;
  return scale > 1.5f ? 326.0f : 163.0f;
}

@implementation BBViewController {
  EAGLContext* _context;
  BOOL _started;
  void* _touchSlots[kMaxTouches];   // UITouch identities (compared, never dereferenced)
}

- (BOOL)started {
  return _started;
}

- (void)loadView {
  self.view = [[GLKView alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  for (int i = 0; i < kMaxTouches; i++) _touchSlots[i] = 0;
  _context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
  GLKView* view = (GLKView*)self.view;
  view.context = _context;
  view.delegate = self;
  view.drawableColorFormat = GLKViewDrawableColorFormatRGBA8888;
  view.drawableDepthFormat = GLKViewDrawableDepthFormat24;
  view.multipleTouchEnabled = YES;
  self.preferredFramesPerSecond = 60;
  [EAGLContext setCurrentContext:_context];
}

// GLKView has bound its framebuffer and set the viewport before calling this.
- (void)glkView:(GLKView*)view drawInRect:(CGRect)rect {
  const int w = (int)view.drawableWidth, h = (int)view.drawableHeight;
  if (!_started) {
    // Landscape-only game: wait for the rotation, so the first scene is never laid out portrait.
    if (w < h) {
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      return;
    }
    // The engine reads everything from data/ relative to the working directory: the bundle's
    // "content" folder (a folder reference in the Xcode project). PlayerPrefs must be writable,
    // so they live in Documents.
    NSString* content = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:@"content"];
    chdir([content fileSystemRepresentation]);
    NSArray* dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString* prefs = [[dirs objectAtIndex:0] stringByAppendingPathComponent:@"prefs.ini"];
    app::Config cfg;
    cfg.prefsPath = std::string([prefs fileSystemRepresentation]);
    // DEBUG MODE (the iOS counterpart of the Windows build's -debug: an app has no command line):
    // only when a file named "debug" exists in Documents (create it with Filza) does the app
    // write Documents/perf.log, the frame-time breakdown every 60 frames.
    const std::string docs = std::string([[dirs objectAtIndex:0] fileSystemRepresentation]);
    if (FILE* dbg = fopen((docs + "/debug").c_str(), "r")) {
      fclose(dbg);
      cfg.perfLogPath = docs + "/perf.log";
      UIDevice* dev = [UIDevice currentDevice];
      cfg.deviceInfo = std::string([[dev model] UTF8String]) + " iOS " + [[dev systemVersion] UTF8String];
    }
    cfg.assetRoot = "data/Assets/";
    cfg.firstScene = "MainMenu";
    cfg.width = w;
    cfg.height = h;
    cfg.dpi = BBScreenDpi();
    cfg.mobile = true;
    cfg.gles2 = true;
    if (!app::start(cfg)) {
      NSLog(@"BaldiCpp: app::start failed (see the log above)");
      return;
    }
    _started = YES;
  }
  app::resize(w, h);
  app::frame((float)self.timeSinceLastDraw);
}

// ---- touches: points -> drawable pixels, UITouch -> small stable ids ----
- (int)slotFor:(UITouch*)touch create:(BOOL)create {
  void* key = (__bridge void*)touch;
  for (int i = 0; i < kMaxTouches; i++)
    if (_touchSlots[i] == key) return i;
  if (!create) return -1;
  for (int i = 0; i < kMaxTouches; i++)
    if (_touchSlots[i] == 0) {
      _touchSlots[i] = key;
      return i;
    }
  return -1;
}

- (void)touchesBegan:(NSSet*)touches withEvent:(UIEvent*)event {
  if (!_started) return;
  const CGFloat s = self.view.contentScaleFactor;
  for (UITouch* t in touches) {
    const int slot = [self slotFor:t create:YES];
    if (slot < 0) continue;
    const CGPoint p = [t locationInView:self.view];
    app::touchBegan(slot, (float)(p.x * s), (float)(p.y * s));
  }
}

- (void)touchesMoved:(NSSet*)touches withEvent:(UIEvent*)event {
  if (!_started) return;
  const CGFloat s = self.view.contentScaleFactor;
  for (UITouch* t in touches) {
    const int slot = [self slotFor:t create:NO];
    if (slot < 0) continue;
    const CGPoint p = [t locationInView:self.view];
    app::touchMoved(slot, (float)(p.x * s), (float)(p.y * s));
  }
}

- (void)endTouches:(NSSet*)touches {
  for (UITouch* t in touches) {
    const int slot = [self slotFor:t create:NO];
    if (slot < 0) continue;
    if (_started) app::touchEnded(slot);
    _touchSlots[slot] = 0;
  }
}

- (void)touchesEnded:(NSSet*)touches withEvent:(UIEvent*)event {
  [self endTouches:touches];
}

- (void)touchesCancelled:(NSSet*)touches withEvent:(UIEvent*)event {
  [self endTouches:touches];
}

// ---- landscape only (iOS 6 API, plus the iOS 5 one) ----
- (NSUInteger)supportedInterfaceOrientations {
  return UIInterfaceOrientationMaskLandscape;
}

- (BOOL)shouldAutorotate {
  return YES;
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)orientation {
  return UIInterfaceOrientationIsLandscape(orientation);
}

- (BOOL)prefersStatusBarHidden {   // iOS 7+ (harmless on 6)
  return YES;
}

@end
