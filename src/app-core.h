// app-core.h -- the platform-neutral game driver both shells run.
//
// Everything between "a platform gave us a GL context, a screen and some input" and "the game
// runs" lives here, written once: engine setup, the first scene, the per-frame sequence (update,
// scene-change texture preload, draw list, draw), touch input, app suspend/resume and the audio
// pull. The Windows testbed can run its frame loop through this (--appcore) and the capture gate
// compares it with the testbed's own loop; the iOS shell (ios/BaldiCpp/*.mm) is then only window,
// display link, touches, RemoteIO and lifecycle glue around these calls.
#pragma once
#include <string>

namespace app {

struct Config {
  std::string prefsPath = "run/prefs.ini";   // writable PlayerPrefs file
  std::string assetRoot = "data/Assets/";    // audio clips (relative to the content root)
  std::string firstScene = "MainMenu";
  int width = 1280, height = 720;            // drawable size in PIXELS
  float dpi = 0.0f;                          // Screen.dpi (0 = unknown)
  bool mobile = false;                       // run as the iOS build (touch UI, Rewired actions)
  bool gles2 = true;                         // rgl backend (iOS: always ES2)
  bool initRenderer = true;                  // false: the host already called rgl::init
  unsigned seed = 0;                         // 0 = time-based
  // The Windows testbed sets seed / prefs / asset root itself before the window exists; it passes
  // keepEngineSetup so start() leaves them alone (setPrefsPath would drop in-memory prefs).
  bool keepEngineSetup = false;
  // On-device profiler: every 60 frames one line appended here (empty = off). iOS: Documents/
  // perf.log, readable with Filza -- the phone's real frame breakdown instead of a PC estimate.
  std::string perfLogPath;
  std::string deviceInfo;                    // written in the log header (model / iOS version)
};

// Call with the GL context current and the process cwd at the content root (the folder that
// holds data/). Returns false if the renderer could not start.
bool start(const Config& cfg);
// One frame: advance by dt seconds (clamped like Unity's Maximum Allowed Timestep) and draw.
void frame(float dt);
// The drawable changed size (rotation, first layout).
void resize(int width, int height);
// Touches in drawable PIXELS, rows down. A touch also taps uGUI buttons (menus) on its down edge.
void touchBegan(long id, float x, float y);
void touchMoved(long id, float x, float y);
void touchEnded(long id);
// App lifecycle: suspended (true) / resumed (false) -> Unity OnApplicationPause.
void applicationPause(bool paused);
// Audio render callback body: interleaved stereo int16 at 44100 Hz.
void audioPull(short* stereo, int frames);
void audioPullFloat(float* stereo, int frames);   // same mix as float, unclamped (iOS RemoteIO Float32)
// The game asked to quit (Application.Quit). iOS apps never quit themselves; Windows does.
bool quitRequested();

}  // namespace app
