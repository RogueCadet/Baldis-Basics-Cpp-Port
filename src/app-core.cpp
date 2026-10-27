// app-core.cpp -- see app-core.h. The sequence mirrors shell-win32.cpp's main() and frame loop
// step for step (same engine calls, same order), minus the Windows-only debug switches.
#include <chrono>
#include <cstdio>
#include <ctime>

#include "app-core.h"
#include "eng-internal.h"
#include "render-gl.h"
#include "render-scene.h"

namespace app {
namespace {

TexCache* g_tex = 0;
std::string g_preloadedScene;
int g_w = 0, g_h = 0;

// perf log (Config::perfLogPath): per-60-frame averages
std::string g_perfPath;
struct Perf { double wall0 = 0, sim = 0, preload = 0, build = 0, draw = 0, flush = 0; int frames = 0, steps = 0, calls = 0, maxSteps = 0; } g_perf;
double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

}  // namespace

bool start(const Config& cfg) {
  if (cfg.initRenderer && !rgl::init(cfg.gles2 ? rgl::ES2 : rgl::FixedFunction)) {
    printf("app: renderer failed to start\n");
    return false;
  }
  if (!cfg.keepEngineSetup) {
    eng::setRandomSeed(cfg.seed ? cfg.seed : (unsigned)time(0));
    eng::setPrefsPath(cfg.prefsPath);
    eng::setAssetRoot(cfg.assetRoot);
  }
  eng::state().mobile = cfg.mobile;
  if (cfg.mobile) eng::touchSetDpi(cfg.dpi);
  resize(cfg.width, cfg.height);
  g_tex = new TexCache();
  g_tex->loadIndex();
  g_tex->loadSpriteIndex();
  g_tex->loadAtlas();
  // Same seeding as the testbed (shell-win32.cpp): a fresh install has no MouseSensitivity key and
  // PlayerScript would turn by 0 until the options slider is first opened; every used install has
  // the slider default 2 (PC TurnSlider and the iOS one, level1 m_Value 2).
  if (!bb::PlayerPrefs::HasKey("MouseSensitivity")) bb::PlayerPrefs::SetFloat("MouseSensitivity", 2.0f);
  g_perfPath = cfg.perfLogPath;
  if (!g_perfPath.empty())
    if (FILE* f = fopen(g_perfPath.c_str(), "a")) {
      fprintf(f, "=== BaldiCpp perf log: %s, %dx%d px, dpi %.0f. Each line = average of 60 frames (ms).\n",
              cfg.deviceInfo.empty() ? "unknown device" : cfg.deviceInfo.c_str(), cfg.width, cfg.height, cfg.dpi);
      fclose(f);
    }
  eng::loadScene(cfg.firstScene);
  printf("app: scene '%s' (%d GameObjects), %dx%d, mobile %d\n", eng::currentSceneName().c_str(),
         (int)eng::state().gos.size(), g_w, g_h, cfg.mobile ? 1 : 0);
  return true;
}

void resize(int width, int height) {
  if (width <= 0 || height <= 0 || (width == g_w && height == g_h)) return;
  g_w = width;
  g_h = height;
  eng::setScreenSize(width, height);
}

void frame(float dt) {
  if (!g_tex) return;
  if (dt > 0.3333333f) dt = 0.3333333f;   // TimeManager.asset Maximum Allowed Timestep
  if (dt < 0.0f) dt = 0.0f;
  const double t0 = nowSec();
  eng::runFullFrame(dt);
  const double t1 = nowSec();
  const std::string sceneNow = eng::currentSceneName();
  if (sceneNow != g_preloadedScene) {     // the scene's textures before its first drawn frame
    g_preloadedScene = sceneNow;
    preloadSceneTextures(*g_tex);
  }
  const double t2 = nowSec();
  eng::clearDrawList();
  eng::buildDrawList();
  const double t3 = nowSec();
  drawFrame(*g_tex, g_w, g_h);
  const double t4 = nowSec();
  const int calls = rgl::flush();
  const double t5 = nowSec();
  eng::sceneShown();   // GLKit presents this frame when drawInRect returns
  if (!g_perfPath.empty()) {
    Perf& p = g_perf;
    if (p.frames == 0) p.wall0 = t0;
    p.sim += t1 - t0; p.preload += t2 - t1; p.build += t3 - t2; p.draw += t4 - t3; p.flush += t5 - t4;
    p.steps += eng::state().fixedStepsLastFrame;
    if (eng::state().fixedStepsLastFrame > p.maxSteps) p.maxSteps = eng::state().fixedStepsLastFrame;
    p.calls += calls;
    if (++p.frames == 60) {
      double ph[6];
      eng::readFrameProfile(ph);
      const double n = 60.0, ms = 1000.0 / n;
      if (FILE* f = fopen(g_perfPath.c_str(), "a")) {
        fprintf(f, "%s fps %.1f | sim %.1f (fixed %.1f: agents %.1f triggers %.1f; update %.1f late %.1f) | "
                   "preload %.1f | uiBuild %.1f | draw %.1f | glFlush %.1f | fixed steps %.1f/frame (max %d) | %.0f draw calls\n",
                sceneNow.c_str(), n / (nowSec() - p.wall0), p.sim * ms, ph[0] * ms, ph[1] * ms, ph[2] * ms, ph[3] * ms,
                ph[4] * ms, p.preload * ms, p.build * ms, p.draw * ms, p.flush * ms, p.steps / n, p.maxSteps, p.calls / n);
        fclose(f);
      }
      p = Perf();
    }
  }
}

void touchBegan(long id, float x, float y) {
  eng::AudioLock audioLock;   // uiClick runs button handlers (sounds) -- see State::audioMutex
  eng::touchDown((int)id, x, y);
  eng::uiSliderDown((int)id, x, y);   // options TurnSlider: press + drag
  eng::uiClick(x, y);   // menus: uGUI buttons take the tap (the testbed clicks on the press edge too)
}
void touchMoved(long id, float x, float y) {
  eng::AudioLock audioLock;
  eng::touchMove((int)id, x, y);
  eng::uiSliderMove((int)id, x, y);
}
void touchEnded(long id) { eng::AudioLock audioLock; eng::touchUp((int)id); eng::uiSliderUp((int)id); }

void applicationPause(bool paused) { eng::AudioLock audioLock; eng::applicationPause(paused); }

void audioPull(short* stereo, int frames) { bb::audioPull(stereo, frames); }
void audioPullFloat(float* stereo, int frames) { bb::audioPullFloat(stereo, frames); }

bool quitRequested() { return eng::state().quitting; }

}  // namespace app
