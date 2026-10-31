// shell-win32.cpp -- Windows testbed shell: a real window that draws the engine's draw list.
//
// WHY WIN32 + WGL RATHER THAN SDL2: the charter allows SDL2 for the Windows testbed, but SDL2 is
// not installed here and that install was an open decision. Win32 + WGL needs NO third-party
// dependency at all, which also matches the iOS zero-deps rule. The part that matters (the
// textured-quad blit over eng::state().drawList) is identical under either backend, so swapping
// in SDL2 later is a window/input change only. Plain GL 1.1 immediate mode is deliberate: no
// extension loading, and the iOS GLKit path will supply its own GL ES 2 equivalent geometry.
//
// Usage: build\baldi.exe [--scene School] [--frames N] [--shot out.raw] [--force-active]
//                         [--autowalk] [--dump-world] [--at x,y,z] [--size WxH]
//   --mode <m>     story | endless -- writes PlayerPrefs CurrentMode, which the menu normally
//                  sets. Story shows "n/7 Notebooks", endless shows "n Notebooks".
//   --automouse <dx> feed a constant Mouse X every frame; the turn is dx*0.25*MouseSensitivity
//                  (2 seeded) = dx*0.5 deg/frame, so --automouse 180 turns 90 deg/frame
//   --size WxH     render at another resolution. The CanvasScaler is resolution-DEPENDENT, so
//                  one size proves little; verified at 1280x720, 960x600 and 640x480 (4:3).
//   --frames N     run exactly N frames then exit (0 = until the window closes)
//   --at x,y,z     teleport the Player before frame 1. Debug aid: the level is 400 units across
//                  and the spawn faces an empty corridor, so without it the only way to look at a
//                  given piece of geometry is to autowalk into a wall.
//   --shot <path>  glReadPixels the last frame to raw RGBA8 + a .txt sidecar of the dimensions.
//                  This is how the GL path is verified without a human watching a window
//                  (tools/shot-to-png.js converts it).
//   --force-active enable every GameObject before building (MainMenu bakes its menus off)
//   --wav-out f    capture everything the mixer produced to f as a 44.1kHz stereo WAV (with
//                  --frames N; no live device during captures) -- the audio verification path
//   --debug        collision fly-through + diagnostics: CharacterController::Move skips blocker
//                  resolution AND prints a per-scene collider census plus CCDBG contact lines
//                  (collider name, push vector, penetration depth). -debug is accepted as alias.
// _CRT_SECURE_NO_WARNINGS must precede every include (MSVC C4996 on fopen under /W3).
#define _CRT_SECURE_NO_WARNINGS
// NOMINMAX is mandatory: windows.h otherwise defines min/max as macros, which breaks every
// standard header that declares std::min/std::max (here <set>, pulled in by eng-internal.h).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>  // waveOut (audio output; winmm.lib on the link line)
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>     // time() for the default RNG seed
#include <cstring>
#include <cmath>       // tanf (perspective)
#include <map>
#include <string>
#include <vector>
#include <algorithm>  // std::sort (billboard draw order)

#include "eng-internal.h"
#include "render-gl.h"
#include "render-scene.h"
#include "app-core.h"

namespace {

bool g_quit = false;

// ---- audio output (waveOut; the engine mixes into bb::audioPull) --------------------------
// The feed thread advances the engine's MIXER clock in wall time by calling audioPull repeatedly.
// AudioSim.Play()/PlayOneShot() stamps are stored against that clock, so voices start where the
// timeline currently is. Capture runs (--shot/--frames) do NOT start the device; --wav-out
// instead pulls inside the frame loop (fixed 1/50 dt = a deterministic 882 frames/frame), which
// makes the dumped WAV reproducible-run-to-run.
//
// Block size = Unity's DSP buffer: ProjectSettings/AudioManager.asset m_DSPBufferSize 1024
// (23.2 ms at 44.1 kHz). Voices can only start on a block boundary (PlayOneShot stamps the mix
// clock), so the block size decides how the game-over's 32 per-frame screech copies stack: at
// 1024 each frame's copy lands on its own block ~23 ms apart, as in Unity; the old 2048 started
// ~3 copies on the same sample, 46 ms apart, which changes how the stacked screech sounds.
// Four blocks in flight (FMOD's default buffer count); the thread polls WHDR_DONE and re-fills
// finished ones. Polling (not a callback) keeps the code single-context.
struct WavOut {
  static const int NBUF = 4;
  HWAVEOUT h = 0;
  WAVEHDR hdr[NBUF];
  static const int FRAMES = 1024;              // 23.2 ms per block at 44.1 kHz
  // 32-bit float blocks, unclamped (bb::audioPullFloat): Unity's FMOD hands Windows float too, and
  // the OS applies the user's volume before any clipping. 16-bit stays as the fallback for a
  // device that refuses float.
  float fblock[NBUF][FRAMES * 2];
  short sblock[NBUF][FRAMES * 2];
  bool isFloat = true;
  bool ok = false;

  void fill(int i) {
    if (isFloat) {
      bb::audioPullFloat(fblock[i], FRAMES);
      hdr[i].lpData = (LPSTR)fblock[i];
      hdr[i].dwBufferLength = FRAMES * 2 * sizeof(float);
    } else {
      bb::audioPull(sblock[i], FRAMES);
      hdr[i].lpData = (LPSTR)sblock[i];
      hdr[i].dwBufferLength = FRAMES * 2 * sizeof(short);
    }
    hdr[i].dwFlags = 0;
    waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    waveOutWrite(h, &hdr[i], sizeof(WAVEHDR));
  }

  bool openAs(bool flt) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = flt ? 3 /* WAVE_FORMAT_IEEE_FLOAT */ : WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = flt ? 32 : 16;
    wf.nBlockAlign = wf.nChannels * wf.wBitsPerSample / 8;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    isFloat = flt;
    return waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, 0) == MMSYSERR_NOERROR;
  }

  bool open() {
    if (!openAs(true) && !openAs(false)) return false;
    memset(hdr, 0, sizeof(hdr));
    for (int i = 0; i < NBUF; i++) fill(i);
    ok = true;
    return true;
  }

  void pump() {  // re-fill whichever block finished (called ~every few ms)
    for (int i = 0; i < NBUF; i++) {
      if (!ok) return;
      if (hdr[i].dwFlags & WHDR_DONE) {
        waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
        fill(i);
      }
    }
  }
};

WavOut g_wavOut;

DWORD CALLBACK audioFeedThread(LPVOID) {
  if (!g_wavOut.open()) {
    printf("shell: audio: waveOutOpen failed -- running silent\n");
    return 0;   // the game keeps running; failing to open a device is not an error the sim cares about
  }
  printf("shell: audio: waveOut 44100 Hz stereo %s live\n", g_wavOut.isFloat ? "float32" : "int16 (no float support)");
  fflush(stdout);
  while (!g_quit) {
    Sleep(2);   // a 23 ms block: poll well inside it
    g_wavOut.pump();
  }
  return 0;
}


// Win32 VK_* -> engine KeyCode indices (types.h). Only keys the game reads are mapped.
int keyCodeFor(WPARAM vk, LPARAM lp) {
  switch (vk) {
    case VK_SPACE:  return 32;
    case VK_ESCAPE: return 27;
    case 65: return 97;    // A
    case 68: return 100;   // D
    case 78: return 110;   // N
    case 83: return 115;   // S
    case 87: return 119;   // W
    case 89: return 121;   // Y
    // VK_SHIFT arrives for BOTH shift keys; Unity's KeyCode has separate LeftShift (304, the
    // sprint key the game reads) and RightShift (305). WPARAM cannot tell them apart -- the
    // extended-key bit is not set for shifts -- so decode the hard scancode from LPARAM:
    // 0x2A = left shift, 0x36 = right shift (NP pad 0x0C arrives as VK_SHIFT too but has
    // scancode 0x37 on the pad; map it right-shift as well).
    case VK_SHIFT: {
      const int scan = (int)((lp >> 16) & 0xFF);
      return scan == 0x2A ? 304 : 305;
    }
    case VK_BACK: return 8;         // Backspace (pad answer erase)
    case VK_RETURN: return 13;      // Return/Enter (pad answer submit)
    case VK_OEM_MINUS: return 45;   // Minus (pad negative numbers / impossible mode)
    case VK_OEM_2: return 47;       // the / key (Unity KeyCode.Slash: -debug cheat menu toggle)
    default: {
      if (vk >= '0' && vk <= '9') return 256 + (int)(vk - '0');   // Alpha0..9 (pad digits)
      return -1;
    }
  }
}

// MOUSE LOOK. Unity's "Mouse X"/"Mouse Y" axes are per-frame DELTAS, and PlayerScript reads
// Input::GetAxis("Mouse X") every frame to turn the player. The shell wired keys and mouse BUTTONS
// but never called eng::injectAxis, so the axes stayed 0 and the game could be walked but not
// LOOKED AROUND IN -- the single biggest blocker to actually playing it.
// Deltas accumulate here between frames and are drained once per frame (see the loop), because a
// frame can receive several WM_MOUSEMOVE messages and Unity reports their sum, not the last one.
float g_mouseDX = 0.0f, g_mouseDY = 0.0f;
// A pending UI click, consumed once per frame (see the loop). Unity routes clicks through
// EventSystem -> GraphicRaycaster; we hit-test Buttons directly, which is what a menu needs.
bool g_clickPending = false;
float g_clickX = 0.0f, g_clickY = 0.0f;
bool g_haveLastMouse = false;
int g_lastMouseX = 0, g_lastMouseY = 0;
// Client-space cursor position for the per-frame hover hit test. Updated by WM_MOUSEMOVE; until
// the first move arrives (pointer parked inside the client at startup) sampled from GetCursorPos.
float g_mouseX = 0.0f, g_mouseY = 0.0f;
bool g_mouseSeen = false;
// Capture runs (--shot) ignore the PHYSICAL mouse: the window opens wherever the user's cursor
// happens to be, and that WM_MOUSEMOVE hovered whatever lay under it (measured: the mode screen
// flipped between two captures 29541 px apart -- "Story Mode" underlined or not -- run to run) and
// would feed mouse-look into School captures. Scripted --hover/--click/--automouse still drive it.
bool g_ignoreOsMouse = false;
// --ios: run as the iOS build (eng::State::mobile): the touch UI, Rewired action reads, iOS-only
// script paths. The left mouse button is a FINGER (touch id 0); the PC mouse button, cursor lock
// and mouse-look recentring are off -- the iOS build has no mouse.
bool g_ios = false;
bool g_touchHeld = false;
bool g_lastCursorVisible = true;   // last value pushed to the OS cursor (see the frame loop)

// ---- Mouse-look confinement
// The real game grabs the mouse: without it, turning leaks moves to other windows and stops
// every time the pointer reaches the window edge. Windows' answer avoids both Capture and
// LowLevelHooks: ClipCursor pins the pointer to an on-screen rectangle while the shell window
// holds focus, and focus loss (Alt-Tab) releases it again. Runs without --shot/--frames are
// the interactive game; capture runs never take focus, and confining the pointer there would
// hijack the real desktop, so this only activates for interactive windows.
static RECT confineRect(HWND hwnd) {
  RECT r;
  GetClientRect(hwnd, &r);
  POINT tl = { r.left, r.top }, br = { r.right, r.bottom };
  ClientToScreen(hwnd, &tl);
  ClientToScreen(hwnd, &br);
  RECT s;
  s.left = tl.x; s.top = tl.y; s.right = br.x; s.bottom = br.y;
  return s;
}
void mouseConfine(HWND hwnd) {
  RECT s = confineRect(hwnd);
  ClipCursor(&s);
}
void mouseRelease() { ClipCursor(0); }

// ---- Mouse-look recentre ("anchor to the middle")
// Confine alone is not enough: the cursor walks to a clip edge and that direction's spin dies
// there (the user's exact "can't freely turn" report). The classic FPS answer without hooks is
// SetCursorPos back to the client centre every frame; the poke's own WM_MOUSEMOVE lands at the
// centre as a ZERO delta only if g_lastMouse* already records the centre, which this does.
// Never call it for capture runs (it would steal the desktop pointer) or whenever the game
// itself shows the OS cursor (menus AND the pause overlay: clicks and hover rely on free
// pointer travel -- this is now driven by the game's own Cursor flag rather than scene names).
bool recentreMouse(HWND hwnd) {
  if (GetForegroundWindow() != hwnd) return false;      // capture/headless runs fail this too
  if (bb::Cursor::visible) return false;                // menu/pause: pointer stays free
  RECT r;
  GetClientRect(hwnd, &r);
  POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
  ClientToScreen(hwnd, &c);
  SetCursorPos(c.x, c.y);
  g_lastMouseX = c.x;
  g_lastMouseY = c.y;
  g_haveLastMouse = true;
  return true;
}

// THE GAME'S OWN CURSOR. Unity takes this from Player Settings (`defaultCursor` +
// `cursorHotspot` in ProjectSettings.asset), NOT from any script or scene -- which is why
// nothing in the port ever set it and the player saw their own Windows arrow. The baker now
// writes the pixels to data/tex/<guid>.raw and the hotspot to data/cursor.json; this builds a
// real HCURSOR from them so the OS draws it, exactly as Unity does.
//
// The blob is top-row-first RGBA, which is what CreateIconIndirect wants for a 32-bit colour
// bitmap, so no vertical flip. The AND mask is required even for an alpha cursor: it is left
// all-zero so the colour bitmap's alpha decides every pixel.
HCURSOR g_gameCursor = 0;

// MOUSE WHEEL -> Unity's "Mouse ScrollWheel" axis. The decompile's InputManager.asset defines it
// as type 1 (mouse movement), axis 2 (the wheel), sensitivity 0.1: GetAxis returns notches * 0.1
// (+ up, - down) for the frame. GameControllerScript.Update reads it to step the item selector,
// and nothing fed it, so scrolling did nothing. Notches accumulate between frames, drained below.
static float g_wheelNotches = 0.0f;

// Wall-clock seconds from the performance counter (--debug profiler and the real-time frame delta).
static double qpcSeconds() {
  static LARGE_INTEGER freq = { 0 };
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / (double)freq.QuadPart;
}

static HCURSOR loadGameCursor() {
  bool ok = false;
  const std::string meta = eng::jsonReadFile("data/cursor.json", ok);
  if (!ok) return 0;
  eng::JsonValue root;
  std::string err;
  if (!eng::jsonParse(meta, root, err)) return 0;
  const eng::JsonValue* gv = root.find("guid");
  if (!gv || gv->type != eng::JsonValue::String) return 0;
  const eng::JsonValue* hx = root.find("hotspotX");
  const eng::JsonValue* hy = root.find("hotspotY");

  // Dimensions come from the texture index, the same table the renderer uses.
  int w = 0, h = 0;
  {
    bool iok = false;
    const std::string itext = eng::jsonReadFile("data/textures-index.json", iok);
    eng::JsonValue iroot;
    if (!iok || !eng::jsonParse(itext, iroot, err)) return 0;
    const eng::JsonValue* e = iroot.find(gv->str.c_str());
    if (!e) return 0;
    const eng::JsonValue* wv = e->find("w");
    const eng::JsonValue* hv = e->find("h");
    if (!wv || !hv) return 0;
    w = wv->asInt();
    h = hv->asInt();
  }
  if (w <= 0 || h <= 0) return 0;

  // SIZE IT LIKE A HARDWARE CURSOR. Unity's Player-Settings cursor goes through the platform
  // hardware-cursor path, and Windows draws those at the SYSTEM CURSOR METRIC -- SM_CXCURSOR,
  // which is 32x32 on a standard DPI desktop. The source texture is 64x64 (maxTextureSize: 64 in
  // its .meta, so Unity imports it at 64), and building a 64x64 HCURSOR from it made the pointer
  // exactly TWICE the size it is in the real game. Resample to the metric and scale the hotspot by
  // the same factor so it keeps pointing at the same pixel of the art.
  const int cw = GetSystemMetrics(SM_CXCURSOR), chh = GetSystemMetrics(SM_CYCURSOR);
  float hotScaleX = 1.0f, hotScaleY = 1.0f;

  // Read the blob directly rather than through TexCache: this runs BEFORE the window exists, so
  // before the GL texture cache is even constructed.
  std::vector<unsigned char> px;
  {
    const std::string blob = "data/tex/" + gv->str + ".raw";
    FILE* f = fopen(blob.c_str(), "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    px.resize(len > 0 ? (size_t)len : 0);
    const bool read = (len > 0) && (fread(&px[0], 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    if (!read) return 0;
  }
  if (px.size() != (size_t)(w * h * 4)) return 0;

  // RGBA -> BGRA, which is what a Windows DIB section stores.
  BITMAPV5HEADER bi;
  memset(&bi, 0, sizeof(bi));
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = cw;
  bi.bV5Height = -chh;                 // negative = top-down, matching the blob's row order
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask   = 0x00FF0000;
  bi.bV5GreenMask = 0x0000FF00;
  bi.bV5BlueMask  = 0x000000FF;
  bi.bV5AlphaMask = 0xFF000000;
  void* bits = 0;
  HDC screen = GetDC(0);
  HBITMAP colour = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, 0, 0);
  ReleaseDC(0, screen);
  if (!colour || !bits) { if (colour) DeleteObject(colour); return 0; }
  unsigned char* dst = (unsigned char*)bits;
  // Box-filter from the source texture into the cursor-sized DIB, converting RGBA -> BGRA.
  // Point sampling a 2:1 reduction of pixel art drops every other row and column, which eats the
  // 1px outline this cursor is drawn with.
  for (int y = 0; y < chh; y++) {
    for (int x = 0; x < cw; x++) {
      const int sx0 = x * w / cw, sx1 = ((x + 1) * w + cw - 1) / cw;
      const int sy0 = y * h / chh, sy1 = ((y + 1) * h + chh - 1) / chh;
      int r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int sy = sy0; sy < sy1 && sy < h; sy++) {
        for (int sx = sx0; sx < sx1 && sx < w; sx++) {
          const size_t si = ((size_t)sy * w + sx) * 4;
          r += px[si + 0]; g += px[si + 1]; b += px[si + 2]; a += px[si + 3];
          n++;
        }
      }
      if (n == 0) n = 1;
      const size_t di = ((size_t)y * cw + x) * 4;
      dst[di + 0] = (unsigned char)(b / n);
      dst[di + 1] = (unsigned char)(g / n);
      dst[di + 2] = (unsigned char)(r / n);
      dst[di + 3] = (unsigned char)(a / n);
    }
  }
  HBITMAP mask = CreateBitmap(cw, chh, 1, 1, 0);   // all zero: alpha decides every pixel
  ICONINFO ii;
  memset(&ii, 0, sizeof(ii));
  ii.fIcon = FALSE;                            // FALSE = cursor, so the hotspot is used
  hotScaleX = (float)cw / (float)w;
  hotScaleY = (float)chh / (float)h;
  ii.xHotspot = (DWORD)((hx ? hx->asInt() : 0) * hotScaleX + 0.5f);
  ii.yHotspot = (DWORD)((hy ? hy->asInt() : 0) * hotScaleY + 0.5f);
  ii.hbmMask = mask;
  ii.hbmColor = colour;
  HCURSOR cur = CreateIconIndirect(&ii);
  DeleteObject(colour);
  DeleteObject(mask);
  if (cur) printf("shell: game cursor %dx%d (source %dx%d, SM_CXCURSOR) hotspot (%d,%d)\n", cw, chh, w, h,
                  (int)ii.xHotspot, (int)ii.yHotspot);
  return cur;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
      mouseRelease();   // never leave the OS pointer boxed after the window dies
      g_quit = true;
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(wp) == WA_INACTIVE) mouseRelease();
      else if (!g_ios) mouseConfine(hwnd);
      return 0;
    case WM_KILLFOCUS:
      mouseRelease();
      return 0;
    case WM_SETCURSOR: {
      // The OS cursor state follows the GAME's own Cursor flags now (CursorControllerScript via
      // GameControllerScript.Lock/UnlockMouse): hidden while locked in gameplay, visible on the
      // menus. Mirrors the real Unity build, which has no Cursor.SetCursor call anywhere.
      // HTCLIENT only: over the borders let Windows draw whenever the cursor is visible.
      // WM_SETCURSOR fires per mouse move, so this re-evaluates on every move.
      if (LOWORD(lp) == HTCLIENT && !bb::Cursor::visible) {
        SetCursor(0);
        return TRUE;
      }
      // VISIBLE over the client area: draw the GAME's cursor, not the player's Windows arrow.
      // Unity has no Cursor.SetCursor call anywhere -- it takes the pointer from PLAYER SETTINGS
      // (`defaultCursor` + `cursorHotspot`), which nothing in the port had ever read. That is why
      // the menus showed whatever arrow the user had configured in Windows.
      if (LOWORD(lp) == HTCLIENT && g_gameCursor) {
        SetCursor(g_gameCursor);
        return TRUE;
      }
      break;
    }
    case WM_KEYDOWN: {
      if (lp & (1 << 30)) return 0;         // bit 30 set = auto-repeat, not a fresh edge
      const int kc = keyCodeFor(wp, lp);
      if (kc >= 0) eng::injectKey(kc, true);
      // Escape is the GAME's key (InputManager PauseOrCancel): GameControllerScript.Update opens
      // the pause menu on it, and Y/N in that menu exit or resume. The shell used to quit here,
      // so pressing Escape closed the game instead of pausing. Closing the window still quits.
      return 0;
    }
    case WM_KEYUP: {
      const int kc = keyCodeFor(wp, lp);
      if (kc >= 0) eng::injectKeyRelease(kc);
      return 0;
    }
    case WM_MOUSEWHEEL:
      g_wheelNotches += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
      return 0;
    case WM_MOUSEMOVE: {
      if (g_ignoreOsMouse) return 0;
      const int mx = (int)(short)LOWORD(lp), my = (int)(short)HIWORD(lp);
      if (g_ios && g_touchHeld) eng::touchMove(0, (float)mx, (float)my);
      if (wp & MK_LBUTTON) eng::uiSliderMove(0, (float)mx, (float)my);   // slider drag
      if (g_haveLastMouse) {
        g_mouseDX += (float)(mx - g_lastMouseX);
        g_mouseDY += (float)(my - g_lastMouseY);
      }
      g_lastMouseX = mx;
      g_lastMouseY = my;
      g_haveLastMouse = true;
      g_mouseX = (float)mx;
      g_mouseY = (float)my;
      g_mouseSeen = true;
      // Ask for one WM_MOUSELEAVE so a hover underline clears when the pointer leaves the
      // client towards another window (Unity fires IPointerExit there too).
      TRACKMOUSEEVENT tme;
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd;
      tme.dwHoverTime = 0;
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      g_mouseSeen = false;
      g_haveLastMouse = false;
      eng::uiPointerMove(-1.0f, -1.0f);   // no button there: dispatches the exit edge
      return 0;
    case WM_LBUTTONDOWN:
      if (g_ios) {
        eng::touchDown(0, (float)(int)(short)LOWORD(lp), (float)(int)(short)HIWORD(lp));
        g_touchHeld = true;
      } else {
        eng::injectMouse(0, true);
      }
      // Queue a UI click for the frame loop: the hit test needs the CURRENT layout, and WndProc can
      // run between frames. LOWORD/HIWORD of lParam are client-space, which is what the hit test
      // wants (row space, y down from the top-left).
      g_clickX = (float)(int)(short)LOWORD(lp);
      g_clickY = (float)(int)(short)HIWORD(lp);
      g_clickPending = true;
      eng::uiSliderDown(0, g_clickX, g_clickY);   // a slider under the press captures the drag
      return 0;
    case WM_LBUTTONUP:
      eng::uiSliderUp(0);
      if (g_ios) { eng::touchUp(0); g_touchHeld = false; }
      else eng::injectMouseRelease(0);
      return 0;
    case WM_RBUTTONDOWN: eng::injectMouse(1, true);  return 0;
    case WM_RBUTTONUP:   eng::injectMouseRelease(1); return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

// The render passes + texture cache live in render-scene.cpp (portable; the iOS shell shares
// them). This shell only owns the window, input, audio and the frame loop.

const char* argValue(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; i++) if (strcmp(argv[i], name) == 0) return argv[i + 1];
  return def;
}
bool argFlag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; i++) if (strcmp(argv[i], name) == 0) return true;
  return false;
}

} // namespace

// Every data path in this program is relative ("data/scene-X.bake.json", "data/tex/...") and was
// therefore relative to whatever directory the program was LAUNCHED from. Launch it from anywhere
// but the project root -- double-click it in Explorer, run it from another shell -- and the scene
// bake is not found, the scene loads 0 GameObjects, and you get a BLACK WINDOW with the only
// explanation printed to a console a /SUBSYSTEM:WINDOWS-style launch never shows. That is exactly
// what happened to the first person who tried to just run the game.
//
// So the program now finds its own data: the exe lives in <root>\build\, so the root is the
// parent of its directory, and we chdir there before touching anything. Launch location stops
// mattering, which is what you expect of a game.
// Find the game CONTENT and make it the working directory.
//
// Every data path in this program is relative ("data/scene-X.bake.json", "data/tex/..."), so
// before this existed they resolved against whatever directory the program was LAUNCHED from.
// Launch from anywhere but the project root -- double-click in Explorer, run from another shell --
// and the bake is not found, the scene loads 0 GameObjects, and you get a BLACK WINDOW with the
// only explanation printed to a console a windowed launch never shows. That is exactly what
// happened to the first person who tried to just run the game, and no amount of testing from the
// project root would ever have caught it.
//
// Search order, first hit wins, all relative to the EXE and not the CWD:
//   <exe>\content\data\   shipped layout: baldi.exe beside a content/ folder
//   <exe>\data\           flat layout: baldi.exe beside data/
//   <exe>\..\data\        the dev tree, where the exe lives in <root>\build\
//
// The marker is the MainMenu bake rather than the directory itself: a stray empty "data" folder
// would otherwise win and put us right back at the black window.
static bool dirHasBake(const char* dir) {
  char probe[MAX_PATH];
  _snprintf(probe, sizeof(probe), "%s\\data\\scene-MainMenu.bake.json", dir);
  probe[sizeof(probe) - 1] = 0;
  const DWORD a = GetFileAttributesA(probe);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void chdirToContentRoot() {
  char exeDir[MAX_PATH];
  const DWORD n = GetModuleFileNameA(0, exeDir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;                 // leave the CWD alone rather than guess
  char* lastSlash = 0;
  for (char* p = exeDir; *p; p++) if (*p == '\\' || *p == '/') lastSlash = p;
  if (!lastSlash) return;
  *lastSlash = 0;                                      // <exe directory>

  char cand[3][MAX_PATH];
  _snprintf(cand[0], MAX_PATH, "%s\\content", exeDir);
  _snprintf(cand[1], MAX_PATH, "%s", exeDir);
  _snprintf(cand[2], MAX_PATH, "%s\\..", exeDir);
  for (int i = 0; i < 3; i++) {
    cand[i][MAX_PATH - 1] = 0;
    if (!dirHasBake(cand[i])) continue;
    if (SetCurrentDirectoryA(cand[i])) {
      char full[MAX_PATH];
      const DWORD fn = GetCurrentDirectoryA(MAX_PATH, full);
      printf("shell: content root %s\n", (fn > 0 && fn < MAX_PATH) ? full : cand[i]);
      return;
    }
  }
  // Nothing found. Say so HERE rather than letting the scene load empty and the window go black:
  // "no content" is a sentence, a black window is a puzzle.
  printf("shell: NO CONTENT FOUND next to %s\n", exeDir);
  printf("shell: expected %s\\content\\data\\, %s\\data\\, or %s\\..\\data\\\n",
         exeDir, exeDir, exeDir);
}

int main(int argc, char** argv) {
#ifdef BB_RELEASE
  // RELEASE build (the packaged game, built by build-shell.ps1 as baldi-release.exe and shipped as
  // baldi.exe): a Windows GUI program with no console. Every development option still exists, but
  // ONLY with -debug / --debug on the command line: then a console opens with the full log and all
  // the dev options (--scene, --at, --frames, --shot, --catch, --spoop, --ios, ...) work exactly as
  // in the dev build, --debug's own debug mode included (no-clip, collision diagnostics, frame
  // profiler). Without it every argument is ignored and the game just starts at its menu.
  // BB_SELFTEST=<path> (package.ps1 only): render 20 frames to <path> and exit, to prove the
  // package runs; not a player-facing option.
  {
    static char* relArgv[8];
    static char framesArg[] = "--frames", twenty[] = "20", shotArg[] = "--shot";
    int relArgc = 0;
    relArgv[relArgc++] = argv[0];
    const bool debugMode = argFlag(argc, argv, "--debug") || argFlag(argc, argv, "-debug");
    if (debugMode) {
      if (AllocConsole()) {
        FILE* f = 0;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleTitleA("Baldi's Basics Classic -- debug log");
      }
    }
    if (!debugMode) {   // a player's launch: no options at all (only the package self-test hook)
      if (char* selftest = getenv("BB_SELFTEST")) {
        relArgv[relArgc++] = framesArg; relArgv[relArgc++] = twenty;
        relArgv[relArgc++] = shotArg; relArgv[relArgc++] = selftest;
      }
      relArgv[relArgc] = 0;
      argc = relArgc;
      argv = relArgv;
    }
  }
#endif
  chdirToContentRoot();
  // The game starts at its MENU. Defaulting to School meant the only way in was a flag, which is
  // a testbed habit, not a game.
  const char* scene = argValue(argc, argv, "--scene", "MainMenu");
  const int frames = atoi(argValue(argc, argv, "--frames", "0"));
  const char* shot = argValue(argc, argv, "--shot", 0);
  const char* wavOut = argValue(argc, argv, "--wav-out", 0);
  // Deterministic audio capture: each 1/50 s frame = 882 stereo frames through the mixer. 
  std::vector<int16_t> g_wavCap;
  const bool dumpWorld = argFlag(argc, argv, "--dump-world");
  const bool forceActive = argFlag(argc, argv, "--force-active");
  // --debug: collision fly-through + diagnostics + the frame profiler. The two collision bits go
  // through one engine switch so the
  // game code in CharacterController::Move only ever reads eng::debugFlags().
  if (argFlag(argc, argv, "--debug") || argFlag(argc, argv, "-debug")) {
    eng::setDebugFlags(eng::kDebugNoClip | eng::kDebugDiag | eng::kDebugUi);
    printf("shell: --debug: collision blocking DISABLED, collision diagnostics ON, frame profiler ON\n");
  }

  // SEED THE RNG. State::rng is initialised to FIXED constants (eng-core.cpp ~line 41) and only
  // sim.cpp ever called setRandomSeed -- so every launch of the GAME replayed the identical random
  // sequence. That is why the You Can Think Pad asked the same questions ("5-7", "7+4") every
  // single run: they were never pre-selected, they were the first draws of a stream that always
  // started from the same place.
  // --seed N keeps a run reproducible on purpose (bug repros, A/B captures); the default is the
  // clock, so a real play session differs each time as Unity's does.
  {
    unsigned int seed = (unsigned int)time(0) ^ (unsigned int)GetTickCount();
    if (const char* sd = argValue(argc, argv, "--seed", 0)) seed = (unsigned int)strtoul(sd, 0, 10);
    eng::setRandomSeed(seed);
    printf("shell: random seed %u%s\n", seed, argValue(argc, argv, "--seed", 0) ? " (--seed)" : "");
  }

  eng::setPrefsPath("run/prefs.ini");
  // --mode story|endless writes PlayerPrefs "CurrentMode", which is what the MENU normally sets
  // and what GameControllerScript reads in Awake. Launching a scene directly bypasses the menu, so
  // the pref stays unset and `mode` is "" -- neither branch -- which falls through to the Endless
  // notebook string. That is why our HUD matched an Endless reference screenshot without Endless
  // ever having been chosen. The mode logic itself was already ported (story shows "n/7
  // Notebooks", endless shows "n Notebooks"); only the selection was missing.
  if (const char* modeArg = argValue(argc, argv, "--mode", 0)) {
    bb::PlayerPrefs::SetString("CurrentMode", modeArg);
    printf("shell: CurrentMode = %s\n", modeArg);
  }
  // Audio assets live inside the content tree once tools\bake-audio.js ran (data/Assets/AudioClip).
  // content-root-RELATIVE so the packaged folder is self-contained; the decompile-tree fallback
  // keeps pre-bake dev builds working without a runtime dependency on bake output.
  {
    const DWORD fa = GetFileAttributesA("data\\Assets\\AudioClip");
    const bool haveAudio = fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY);
    eng::setAssetRoot(haveAudio ? "data/Assets/"
                                : "F:/baldidecomp/baldi_s_basics_classic_143_decompile_v5/Assets/");
    printf("shell: assetRoot %s\n", haveAudio ? "data/Assets/" : "(decompile tree)");
  }

  // --size WxH overrides the default resolution. The CanvasScaler is resolution-DEPENDENT logic
  // (scaleFactor is derived from the screen against the canvas reference resolution), so testing
  // it at one size proves almost nothing: a bug that happens to be identity at 1280x720 stays
  // invisible. This is what makes a second resolution checkable.
  // --ios [--dpi N]: emulate the iOS build on an iOS 6 device. Default iPhone 4 landscape, 960x640 at
  // 326 dpi, so the touch canvas (DpCanvasScaler: dpi / 160) comes out at its real physical size.
  if (argFlag(argc, argv, "--ios")) {
    g_ios = true;
    eng::state().mobile = true;
    const float dpi = (float)atof(argValue(argc, argv, "--dpi", "326"));
    eng::touchSetDpi(dpi);
    if (!argValue(argc, argv, "--size", 0)) eng::setScreenSize(960, 640);
    printf("shell: iOS mode (touch UI, Rewired actions), Screen.dpi %.0f\n", dpi);
  }
  if (const char* sizeArg = argValue(argc, argv, "--size", 0)) {
    int sw = 0, sh = 0;
    if (sscanf(sizeArg, "%dx%d", &sw, &sh) == 2 && sw > 0 && sh > 0) {
      eng::setScreenSize(sw, sh);
    } else {
      printf("shell: --size wants WxH (got '%s')\n", sizeArg);
    }
  }
  int W = eng::state().screenW, H = eng::state().screenH;
  // Report what the ENGINE actually holds, never what was requested. shell-smoke.ps1 matches on
  // this line, and echoing the request would let a broken --size pass the gate -- which is exactly
  // what happened the first time this was negative-tested.
  // The real game launches FULL SCREEN; the port used to open a 1280x720 testbed window. Capture
  // runs (--shot/--frames) keep that window on purpose: the gates compare pixels, and a monitor-
  // sized capture would change every golden ratio. Interactive launches go borderless at the
  // primary monitor's size -- no mode switch, Alt-Tab and the clip-cursor path stay intact.
  const bool autopilot = (shot != 0) || frames > 0;
  const bool fullscreenRun = !autopilot && !argFlag(argc, argv, "--windowed");
  if (fullscreenRun) {
    const int fsW = GetSystemMetrics(SM_CXSCREEN), fsH = GetSystemMetrics(SM_CYSCREEN);
    eng::setScreenSize(fsW, fsH);
    W = eng::state().screenW;
    H = eng::state().screenH;
    if (W != fsW || H != fsH)
      printf("shell: fullscreen request %dx%d, engine holds %dx%d\n", fsW, fsH, W, H);
  }
  printf("shell: screen %dx%d\n", W, H);

  WNDCLASSEX wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandle(0);
  g_gameCursor = loadGameCursor();   // Player Settings defaultCursor; 0 falls back to the arrow
  wc.hCursor = g_gameCursor ? g_gameCursor : LoadCursor(0, IDC_ARROW);
  wc.lpszClassName = "BaldiPortShell";
  // the genuine BALDI.exe icon (shell-res/baldi.rc, resource 1): taskbar, Alt+Tab, title bar
  wc.hIcon = (HICON)LoadImage(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                              GetSystemMetrics(SM_CYICON), 0);
  wc.hIconSm = (HICON)LoadImage(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON), 0);
  if (!RegisterClassEx(&wc)) { printf("shell: RegisterClassEx failed\n"); return 1; }

  const DWORD winStyle = fullscreenRun ? WS_POPUP : WS_OVERLAPPEDWINDOW;
  RECT r;
  r.left = 0; r.top = 0; r.right = W; r.bottom = H;
  if (!fullscreenRun) AdjustWindowRect(&r, winStyle, FALSE);
#ifdef BB_RELEASE
  static const char kWindowTitle[] = "Baldi's Basics Classic";   // PlayerSettings productName
#else
  static const char kWindowTitle[] = "Baldi C++ port testbed";
#endif
  HWND hwnd = CreateWindowEx(0, wc.lpszClassName, kWindowTitle,
                             winStyle, fullscreenRun ? 0 : CW_USEDEFAULT,
                             fullscreenRun ? 0 : CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
  if (!hwnd) { printf("shell: CreateWindowEx failed\n"); return 1; }

  // Windows CLAMPS a window that will not fit the display, so asking for a 1920x1080 client area
  // on a 1920x1080 screen gives a shorter one once the title bar and borders are added. Rendering
  // and reading back at the REQUESTED size then returns rows the GL never drew -- they come back
  // black, and a capture compared against a real screenshot shows a letterbox band at the top plus
  // a HUD that looks the wrong height. Adopt whatever client area actually exists.
  RECT cr;
  GetClientRect(hwnd, &cr);
  const int clientW = cr.right - cr.left, clientH = cr.bottom - cr.top;
  if (clientW > 0 && clientH > 0 && (clientW != W || clientH != H)) {
    printf("shell: window clamped to %dx%d (asked %dx%d) -- rendering at the real client size\n",
           clientW, clientH, W, H);
    eng::setScreenSize(clientW, clientH);
    W = clientW;
    H = clientH;
  }

  HDC dc = GetDC(hwnd);
  PIXELFORMATDESCRIPTOR pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  const int pf = ChoosePixelFormat(dc, &pfd);
  if (!pf || !SetPixelFormat(dc, pf, &pfd)) { printf("shell: pixel format failed\n"); return 1; }
  HGLRC rc = wglCreateContext(dc);
  if (!rc || !wglMakeCurrent(dc, rc)) { printf("shell: wglCreateContext failed\n"); return 1; }
  // --gles2: draw through rgl's OpenGL ES 2.0-subset backend (the iOS 6 renderer) instead of
  // fixed-function GL. Same passes, same data; only the GPU path differs.
  if (argFlag(argc, argv, "--gles2")) {
    if (!rgl::init(rgl::ES2)) { printf("shell: --gles2 backend failed to start\n"); return 1; }
    printf("shell: renderer = GLES2-subset backend\n");
  } else {
    rgl::init(rgl::FixedFunction);
  }
  // VSYNC, as the game ships it: QualitySettings.asset level 0 ("1999", the only level) has
  // vSyncCount: 1. Set it explicitly -- left alone, the driver default decides, and on a
  // "application-controlled" default the loop free-runs.
  {
    typedef BOOL (WINAPI* SwapIntervalFn)(int);
    SwapIntervalFn swapInterval = (SwapIntervalFn)wglGetProcAddress("wglSwapIntervalEXT");
    if (swapInterval) swapInterval(1);
    printf("shell: vsync %s\n", swapInterval ? "on (vSyncCount 1)" : "unavailable -- driver default");
  }
  // A scene swap blocks this thread for the whole load (seconds on a cold disk). Windows marks a
  // window "Not Responding" -- and ghosts it -- when its thread has not called PeekMessage for 5 s,
  // so the loader checks in here. PM_NOREMOVE: no message is dispatched mid-load.
  eng::state().loadPump = []() { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); };
  printf("shell: GL %s / %s\n", (const char*)glGetString(GL_VERSION),
         (const char*)glGetString(GL_RENDERER));

  ShowWindow(hwnd, frames > 0 ? SW_SHOWNA : SW_SHOW);  // headless capture must not steal focus

  // --appcore: run through src/app-core (the driver the iOS shell uses) instead of this shell's own
  // setup + frame loop, so tools/render-compare.py can prove the two produce the same frames.
  // Supports the capture switches only: --scene --mode --seed --frames --shot --ios --touch --gles2.
  if (argFlag(argc, argv, "--appcore")) {
    app::Config ac;
    ac.keepEngineSetup = true;              // seed / prefs / mode / asset root set above
    ac.initRenderer = false;                // rgl::init above (honours --gles2)
    ac.firstScene = scene;
    ac.width = W;
    ac.height = H;
    ac.mobile = g_ios;
    ac.dpi = eng::state().screenDpi;
    if (const char* pl = getenv("BB_PERFLOG")) { ac.perfLogPath = pl; ac.deviceInfo = "Windows testbed"; }   // the iOS perf log, testable here
    if (!app::start(ac)) return 1;
    float ta[4] = { 0, 0, 0, 0 };
    int ta0 = -1, ta1 = -1;
    if (const char* t = argValue(argc, argv, "--touch", 0))
      if (sscanf(t, "%f,%f,%f,%f@%d,%d", &ta[0], &ta[1], &ta[2], &ta[3], &ta0, &ta1) != 6) ta0 = ta1 = -1;
    const char* shotPath = argValue(argc, argv, "--shot", 0);
    for (int n = 0; (frames <= 0 || n < frames) && !g_quit && !app::quitRequested(); n++) {
      MSG msg;
      {
      eng::AudioLock audioLock;   // input handlers run game code (menu clicks play sounds)
      while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    }
      if (ta0 >= 0 && n >= ta0 && n <= ta1) {
        const float t = ta1 > ta0 ? (float)(n - ta0) / (float)(ta1 - ta0) : 1.0f;
        const float tx = ta[0] + (ta[2] - ta[0]) * t, ty = ta[1] + (ta[3] - ta[1]) * t;
        if (n == ta0) app::touchBegan(1, tx, ty);
        else app::touchMoved(1, tx, ty);
        if (n == ta1) app::touchEnded(1);
      }
      app::frame(1.0f / 50.0f);
      if (shotPath && frames > 0 && n + 1 >= frames) { rgl::Finish(); screenshot(shotPath, W, H); }
      SwapBuffers(dc);
    }
    return 0;
  }

  TexCache tex;
  tex.loadIndex();
  tex.loadSpriteIndex();
  tex.loadAtlas();

  // Mouse sensitivity seeding. The decompile's real chain: PlayerScript.MouseMove() does
  // yaw += GetAxis("Mouse X") * mouseSensitivity * Time.timeScale * flipaturn (NO deltaTime),
  // where mouseSensitivity = PlayerPrefs "MouseSensitivity". The MainMenu TurnSlider's
  // serialized range is 0.1..10 with m_Value 2, and OptionsManager (the only writer) publishes
  // that whenever the options panel is open -- the MouseSliderScript with its "< 100 -> 200"
  // branch is NOT attached in 1.4.3 (the bake shows empty value-change handlers, and a 200 seed
  // would fight the 0.1..10 slider). A fresh install therefore has NO key -> GetFloat 0 ->
  // PlayerScript turns exactly nothing until the options panel has been opened once in the real
  // game. Every used real install carries 2, so seed that when the key is absent; an existing
  // value (the user's own slider) always wins.
  if (!bb::PlayerPrefs::HasKey("MouseSensitivity")) {
    bb::PlayerPrefs::SetFloat("MouseSensitivity", 2.0f);
    printf("shell: seeded MouseSensitivity=2 (TurnSlider m_Value; key absent on fresh installs "
           "-- see run/prefs.ini)\n");
  }

  // Live device only for human launches: capture runs stay silent (and deterministic). The
  // thread must exist BEFORE loadScene so playOnAwake music/greeting stamps find a running clock.
  const bool audioLive = !autopilot && wavOut == 0 && getenv("BB_NOAUDIO") == 0;
  if (audioLive) CreateThread(0, 0, audioFeedThread, 0, 0, 0);
  else printf("shell: audio device off (%s)\n",
              wavOut ? "wav capture" : (getenv("BB_NOAUDIO") ? "BB_NOAUDIO" : "capture run"));

  eng::loadScene(scene);
  printf("shell: scene '%s' (%d GameObjects)\n", eng::currentSceneName().c_str(),
         (int)eng::state().gos.size());
  if (dumpWorld) {   // camera state straight after load, BEFORE any script Update has run
    bb::Camera* c0 = bb::Camera::main();
    if (c0 && c0->transform) {
      const bb::Quat q0 = c0->transform->rotation;
      const bb::Vec3 u0 = q0.rotate(bb::Vec3(0.0f,1.0f,0.0f));
      printf("shell: camera AT LOAD rot(%.3f,%.3f,%.3f,%.3f) up(%.2f,%.2f,%.2f)\n",
             q0.x,q0.y,q0.z,q0.w, u0.x,u0.y,u0.z);
    }
  }

  // --autowalk holds W (MoveForward binds to KeyCode::W = 119) for the whole run, so the full
  // chain can be exercised without a human at the keyboard: injected key -> InputManager ->
  // PlayerScript movement -> CharacterController -> CameraScript -> the rendered view. The
  // player's start and end position are printed so the motion is checkable, not just visible.
  // --at x,y,z teleports the Player before the first frame. The level is 400 units across and the
  // spawn faces an empty corridor, so without this the only way to LOOK at a piece of geometry is
  // to autowalk into a wall -- which is how the desks stayed unverified. Purely a debug aid: it
  // writes the transform directly and lets CameraScript follow.
  // --automouse <dx> feeds a constant Mouse X every frame, the same trick --autowalk uses for the
  // keyboard: it makes the input -> PlayerScript -> camera chain verifiable without a human on the
  // mouse. Without it, mouse look can only be tested by hand, which means it silently rots.
  // --click x,y fires one UI click on frame 2, so a menu can be driven headlessly the way
  // --autowalk drives movement. Without it a menu can only be tested by hand and silently rots.
  // A SEQUENCE: "x,y;x,y;..." -- each click fires 20 frames after the previous one, which is what
  // a menu needs (click Start, let the next screen appear, click Story Mode). One click could only
  // ever test one screen.
  std::vector<std::pair<float, float> > clickSeq;
  if (const char* cs = argValue(argc, argv, "--click", 0)) {
    std::string str(cs);
    size_t pos = 0;
    while (pos < str.size()) {
      const size_t semi = str.find(';', pos);
      const std::string part = str.substr(pos, semi == std::string::npos ? std::string::npos
                                                                        : semi - pos);
      float cx = 0, cy = 0;
      if (sscanf(part.c_str(), "%f,%f", &cx, &cy) == 2) clickSeq.push_back(std::make_pair(cx, cy));
      if (semi == std::string::npos) break;
      pos = semi + 1;
    }
  }
  float clickAtX = -1.0f, clickAtY = -1.0f;
  if (const char* ca = argValue(argc, argv, "--click-unused", 0)) {
    if (sscanf(ca, "%f,%f", &clickAtX, &clickAtY) != 2) {
      printf("shell: --click wants x,y (got '%s')\n", ca);
      clickAtX = clickAtY = -1.0f;
    }
  }
  // Real gameplay "Interact" click (Mouse0) at a given frame -- NOT a UI click: this injects the
  // same press/release the OS mouse produces, so it exercises the full
  // InputManager -> NotebookScript raycast -> MathGame chain headlessly. Release 2 frames later
  // (a human click at 60 fps spans a few frames; the held state is what GetKeyState reads).
  // --press <unity keycode>@<frame>: a scripted key press through the live input path (e.g. 27@20
  // presses Escape on frame 20), released two frames later. For captures of key-driven screens.
  int pressKey = -1, pressAt = -1;
  if (const char* pa = argValue(argc, argv, "--press", 0)) {
    if (sscanf(pa, "%d@%d", &pressKey, &pressAt) != 2) { pressKey = -1; pressAt = -1; }
  }
  // --touch x0,y0,x1,y1@f0,f1: a scripted finger (touch id 1, --ios): down at (x0,y0) on frame f0,
  // dragged in a straight line, lifted at (x1,y1) on frame f1. Deterministic touch captures.
  float tch[4] = { 0, 0, 0, 0 };
  int tchF0 = -1, tchF1 = -1;
  if (const char* ta = argValue(argc, argv, "--touch", 0)) {
    if (sscanf(ta, "%f,%f,%f,%f@%d,%d", &tch[0], &tch[1], &tch[2], &tch[3], &tchF0, &tchF1) != 6) tchF0 = tchF1 = -1;
  }
  int interactAt = -1;
  if (const char* ia = argValue(argc, argv, "--interact", 0)) {
    interactAt = atoi(ia);
    if (interactAt < 0) { printf("shell: --interact wants a frame number (got '%s')\n", ia); interactAt = -1; }
    else printf("shell: interact (Mouse0) press at frame %d\n", interactAt);
  }
  // --catch N: Baldi catches the player at frame N (eng::debugBaldiCatch), so the jumpscare can be
  // captured and compared with a recording of the genuine game.
  int catchAt = -1;
  if (const char* ct = argValue(argc, argv, "--catch", 0)) {
    catchAt = atoi(ct);
    printf("shell: Baldi catches the player at frame %d\n", catchAt);
  }
  float autoMouseDX = 0.0f;
  if (const char* am = argValue(argc, argv, "--automouse", 0)) autoMouseDX = (float)atof(am);
  // Debug hover pin (same reason --click exists): headless capture runs have no real cursor, so
  // the MouseoverScript enter path is otherwise unreachable. Feeds uiPointerMove every frame.
  if (argValue(argc, argv, "--shot", 0)) g_ignoreOsMouse = true;
  if (const char* hv = argValue(argc, argv, "--hover", 0)) {
    float hx = 0, hy = 0;
    if (sscanf(hv, "%f,%f", &hx, &hy) == 2) {
      g_mouseX = hx;
      g_mouseY = hy;
      g_mouseSeen = true;
      printf("shell: hover pinned at (%.0f,%.0f)\n", hx, hy);
    } else {
      printf("shell: --hover wants x,y (got '%s')\n", hv);
    }
  }
  const char* atArg = argValue(argc, argv, "--at", 0);
  const bool autowalk = argFlag(argc, argv, "--autowalk");
  bb::Transform* playerT = 0;
  if (autowalk || atArg) {
    for (size_t i = 0; i < eng::state().gos.size(); i++) {
      if (eng::state().gos[i]->name == "Player") { playerT = eng::state().gos[i]->transform; break; }
    }
    if (playerT && atArg) {
      float px = 0.0f, py = 0.0f, pz = 0.0f;
      if (sscanf(atArg, "%f,%f,%f", &px, &py, &pz) == 3) {
        playerT->position = bb::Vec3(px, py, pz);
        printf("shell: teleported player to (%.2f, %.2f, %.2f)\n", px, py, pz);
      } else {
        printf("shell: --at wants x,y,z (got '%s')\n", atArg);
      }
    }
    // --facing <deg> sets the player yaw directly (Unity euler y). Spawn faces +Z (yaw 0);
    // yaw 90 looks +X, yaw -90 looks -X. CameraScript::LateUpdate copies this each frame.
    if (const char* fArg = argValue(argc, argv, "--facing", 0)) {
      float yawDeg = (float)atof(fArg);
      if (playerT) {
        playerT->rotation.setEulerAngles(bb::Vec3(0.0f, yawDeg, 0.0f));
        eng::setPlayerFacing(yawDeg);   // ...and PlayerScript.playerRotation, which wins each Update
        printf("shell: player facing set to %.1f deg\n", yawDeg);
      } else {
        printf("shell: --facing ignored (no player)\n");
      }
    }
    if (playerT && autowalk) {
      printf("shell: autowalk start player (%.2f, %.2f, %.2f)\n",
             playerT->position.x, playerT->position.y, playerT->position.z);
    }
  }

  // Frame profiler, part of --debug: average wall-clock cost of each part of the frame, printed
  // every 100 frames (PROFILE lines). Measures instead of guessing where the lag is.
  const bool profile = argFlag(argc, argv, "--debug") || argFlag(argc, argv, "-debug");
  double profSim = 0.0, profDraw = 0.0, profSwap = 0.0, profGame = 0.0, profWall = qpcSeconds();
  int profN = 0;
  // Deterministic runs (the same rule as their fixed 1/50 dt below): LoadSceneAsync swaps on its
  // fixed frame, waiting for the parse worker there, so captures stay reproducible.
  eng::state().asyncWaitAtSwap = shot || wavOut || (frames > 0 && !profile);

  int n = 0;
  while (!g_quit) {
    MSG msg;
    {
      eng::AudioLock audioLock;   // input handlers run game code (menu clicks play sounds)
      while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    }
    if (g_quit) break;

    if (autowalk) eng::injectKey(119, true);   // hold W (MoveForward)
    // Scripted gameplay click: press on the requested frame (edge + held), release two frames
    // later. Injected through the LIVE input path so InputManager sees the real edge/held state.
    if (tchF0 >= 0 && n >= tchF0 && n <= tchF1) {
      const float t = tchF1 > tchF0 ? (float)(n - tchF0) / (float)(tchF1 - tchF0) : 1.0f;
      const float tx = tch[0] + (tch[2] - tch[0]) * t, ty = tch[1] + (tch[3] - tch[1]) * t;
      if (n == tchF0) eng::touchDown(1, tx, ty);
      else eng::touchMove(1, tx, ty);
      if (n == tchF1) eng::touchUp(1);
    }
    if (pressAt >= 0 && n == pressAt) eng::injectKey(pressKey, true);
    else if (pressAt >= 0 && n == pressAt + 2) eng::injectKeyRelease(pressKey);
    // --catch counts its own frames: `n` only advances in capture runs (--frames)
    static int catchFrame = 0;
    const int cf = catchFrame++;
    if (catchAt >= 0 && cf == catchAt && !eng::debugBaldiCatch()) printf("shell: --catch failed (no player/Baldi)\n");
    static const bool spoopArg = argFlag(argc, argv, "--spoop");
    if (spoopArg && cf == 5) eng::debugSpoopMode();   // every NPC awake: the real gameplay load
    static const bool redArg = argFlag(argc, argv, "--red");
    if (redArg && cf == 5) bb::setAmbientLight(bb::Color::red());   // TEMP BB_REDTEST probe: verify the finale red ambient (REMOVE with the flag once verified)
    if (catchAt >= 0 && cf >= catchAt - 5 && cf <= catchAt + 90) {   // frame pacing through the jumpscare
      static double lastLog = 0.0;
      const double now = qpcSeconds();
      if (lastLog > 0.0) printf("CATCHFRAME %d dt %.1f ms\n", cf - catchAt, (now - lastLog) * 1000.0);
      lastLog = now;
      fflush(stdout);
    }
    if (interactAt >= 0 && n == interactAt) eng::injectMouse(0, true);
    else if (interactAt >= 0 && n == interactAt + 2) eng::injectMouseRelease(0);
    // Drain the accumulated mouse delta into Unity's axes. The scale is the decompile's OWN
    // InputManager.asset: axes "Mouse X"/"Mouse Y" are type 1 (mouse movement) with sensitivity
    // 0.25, so Unity's GetAxis returns pixel-delta * 0.25 -- that is what PlayerScript.MouseMove
    // multiplies by mouseSensitivity (0.1..10 range, no deltaTime in 1.4.3's formula). Reset
    // afterwards: per-frame deltas, a stale value would keep turning the player forever.
    // Fire the scripted sequence: first click on frame 2 (one full frame to settle), then every
    // 20 frames so the screen opened by the previous click exists before the next one lands.
    {
      const int step = 20;
      if (n >= 1 && (n - 1) % step == 0) {
        const size_t idx = (size_t)((n - 1) / step);
        if (idx < clickSeq.size()) {
          g_clickX = clickSeq[idx].first;
          g_clickY = clickSeq[idx].second;
          g_clickPending = true;
        }
      }
    }
    // Hover edges for the menu (MouseoverScript enter/exit -> underline swap). The hit test needs
    // the CURRENT layout, same as the click below, so it runs inside the frame loop too.
    if (!g_mouseSeen && GetForegroundWindow() == hwnd) {
      POINT p;
      if (GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
        g_mouseX = (float)p.x;
        g_mouseY = (float)p.y;
        if (p.x >= 0 && p.y >= 0) g_mouseSeen = true;
      }
    }
    eng::uiPointerMove(g_mouseX, g_mouseY);
    // Apply the game's Cursor flag immediately (CursorControllerScript flips it via the scripts
    // between frames): the OS arrow vanishes the same tick the gameplay locks, instead of
    // waiting for the next WM_SETCURSOR from a mouse move.
    if (g_lastCursorVisible != bb::Cursor::visible) {
      g_lastCursorVisible = bb::Cursor::visible;
      SetCursor(bb::Cursor::visible ? LoadCursor(0, IDC_ARROW) : 0);
    }
    if (g_clickPending) {
      g_clickPending = false;
      const bool hit = eng::uiClick(g_clickX, g_clickY);
      printf("shell: click (%.0f,%.0f) -> %s\n", g_clickX, g_clickY,
                            hit ? "button" : "nothing");
    }
    eng::injectAxis("Mouse X", (g_mouseDX + autoMouseDX) * 0.25f);
    eng::injectAxis("Mouse Y", g_mouseDY * 0.25f);
    eng::injectAxis("Mouse ScrollWheel", g_wheelNotches * 0.1f);   // InputManager.asset: sensitivity 0.1
    g_wheelNotches = 0.0f;
    g_mouseDX = 0.0f;
    g_mouseDY = 0.0f;
    if (!g_ios) recentreMouse(hwnd);   // spin never dies at a clip edge (see the helper)
    // FRAME DELTA. Unity's Time.deltaTime is the REAL elapsed frame time, clamped by
    // TimeManager.asset `Maximum Allowed Timestep: 0.3333333`; FixedUpdate then runs however many
    // `Fixed Timestep: 0.02` steps that time accumulates (eng's runFixedSteps already does).
    // This loop used to pass a constant 1/50 per RENDERED frame, which tied game speed to frame
    // rate: measured 0.75x game speed at 37 fps, and 1.20x at 60 fps once the trigger phase was
    // fixed. A fixed 1/50 is kept only for deterministic runs (--shot / --wav-out / --frames
    // without --debug) so captures and the smoke gates stay reproducible.
    const double tSim0 = qpcSeconds();
    float frameDt = 1.0f / 50.0f;
    if (!(shot || wavOut || (frames > 0 && !profile))) {
      static double lastFrame = 0.0;
      if (lastFrame > 0.0) frameDt = (float)(tSim0 - lastFrame);
      lastFrame = tSim0;
      if (frameDt > 0.3333333f) frameDt = 0.3333333f;
      if (frameDt < 0.0f) frameDt = 0.0f;
    }
    profGame += frameDt;
    eng::runFullFrame(frameDt);
    // TEMP BB_DBGMENU (remove with the debug-feature probes): scripted '/' press at frames 4-5 so
    // --shot runs capture the bottom-left cheat panel for the handoff's visual proof.
    if (getenv("BB_DBGMENU") && n == 4) eng::injectKey(47, true);
    if (getenv("BB_DBGMENU") && n == 5) eng::injectKeyRelease(47);
    // TEMP BB_DBGESP (remove with the debug-feature probes): wallhack visual proof -- mark the
    // notebook billboards with the -debug ESP pass straight from the shell. The notebooks live in
    // classroom interiors, so most of them are behind walls from the spawn camera; the in-game
    // cheat marks the seven NPC roots instead (bb::debugUiEsp takes names root-or-ancestor).
    if (getenv("BB_DBGESP") && n == 3) {
      static const char* const kProbeEspNames[19] = {
        "Notebooks", "Notebooks (1)", "Notebooks (2)", "Notebooks (3)", "Notebooks (4)",
        "Notebooks (5)", "Notebooks (6)", "Notebooks (7)", "Notebooks (8)", "Notebooks (9)",
        "Notebooks (10)", "Notebooks (11)", "Notebooks (12)", "Notebooks (13)", "Notebooks (14)",
        "Notebooks (15)", "Notebooks (16)", "Notebooks (17)", "Notebooks (18)" };
      bb::debugUiEsp(true, kProbeEspNames, 19);
    }
    // Application.Quit() (the main menu's X button -> ExitButtonScript.ExitGame) only sets the
    // engine's quitting flag; nothing read it, so the button did nothing.
    if (eng::state().quitting) g_quit = true;
    const double tSim1 = qpcSeconds();
    if (wavOut) { // deterministic capture: fixed dt makes the dump reproducible
      short cap[882 * 2];
      bb::audioPull(cap, 882);
      g_wavCap.insert(g_wavCap.end(), cap, cap + 882 * 2);
    }
    if (forceActive) {
      eng::State& st = eng::state();
      for (size_t i = 0; i < st.gos.size(); i++) st.gos[i]->activeSelf = true;
    }
    {
      static std::string preloadedScene;
      const std::string sceneNow = eng::currentSceneName();
      if (sceneNow != preloadedScene) {
        preloadedScene = sceneNow;
        preloadSceneTextures(tex);
      }
    }
    eng::clearDrawList();
    eng::buildDrawList();
    if (frames > 0 && n + 1 >= frames) g_dumpWorld = dumpWorld;  // last frame only
    drawFrame(tex, W, H);
    const double tDraw1 = qpcSeconds();
    const double tFlush0 = qpcSeconds();
    const int drawCalls = rgl::flush();
    static double profFlush = 0.0;
    profFlush += qpcSeconds() - tFlush0;
    if (frames > 0 && n + 1 >= frames && argFlag(argc, argv, "--gles2")) printf("shell: last frame %d draw calls\n", drawCalls);
    SwapBuffers(dc);
    eng::sceneShown();   // a swapped-in scene is on screen now: its held audio may start
    const double tSwap1 = qpcSeconds();
    if (profile) {
      profSim += tSim1 - tSim0;
      profDraw += tDraw1 - tSim1;
      profSwap += tSwap1 - tDraw1;
      if (++profN == 100) {
        const double now = qpcSeconds();
        printf("PROFILE fps=%.1f  sim=%.2fms  draw=%.2fms  swap=%.2fms  (game time per real second: %.2f)\n",
               100.0 / (now - profWall), profSim * 10.0, profDraw * 10.0, profSwap * 10.0,
               profGame / (now - profWall));
        fflush(stdout);
        double ph[6];
        eng::readFrameProfile(ph);
        printf("PROFILE   texture first-use loads=%d costing %.1fms total\n", g_texLoads, g_texLoadSec * 1000.0);
        g_texLoads = 0;
        g_texLoadSec = 0.0;
        printf("PROFILE   fixed=%.2fms (agents=%.2f triggers=%.2f)  update=%.2fms  late=%.2fms  end=%.2fms\n",
               ph[0] * 10.0, ph[1] * 10.0, ph[2] * 10.0, ph[3] * 10.0, ph[4] * 10.0, ph[5] * 10.0);
        {
          double pp[4];
          long long drawn = 0, culled = 0;
          readPassProfile(pp, drawn, culled);
          printf("PROFILE   passes: sky=%.2fms world=%.2fms billboards=%.2fms ui=%.2fms  rgl::flush=%.2fms  "
                 "renderers drawn %lld culled %lld per frame\n", pp[0] * 10.0, pp[1] * 10.0, pp[2] * 10.0,
                 pp[3] * 10.0, profFlush * 10.0, drawn / 100, culled / 100);
          profFlush = 0.0;
        }
        profSim = profDraw = profSwap = profGame = 0.0;
        profN = 0;
        profWall = now;
      }
    }

    if (frames > 0 && ++n >= frames) {
      // facing provenance: the camera's final yaw where the shot was taken (Unity euler degrees:
      // spawn +Z is 0). Without it, an automouse-derived view angle is guesswork.
      if (bb::Camera::main() && bb::Camera::main()->transform) {
        const bb::Vec3 e = bb::Camera::main()->transform->rotation.eulerAngles();
        bb::Transform* pt = 0;
        for (size_t i = 0; i < eng::state().gos.size(); i++)
          if (eng::state().gos[i]->name == "Player") { pt = eng::state().gos[i]->transform; break; }
        const bb::Vec3 pe = pt ? pt->rotation.eulerAngles() : bb::Vec3(0, 0, 0);
        const bb::Vec3 pp = pt ? pt->position : bb::Vec3(0, 0, 0);
        const bb::Vec3 cp = bb::Camera::main()->transform->position;
        printf("shell: facing at shot: camera yaw %.1f pitch %.1f pos (%.2f,%.2f,%.2f) | player yaw %.1f pos (%.2f,%.2f,%.2f)\n",
               e.y, e.x, cp.x, cp.y, cp.z, pe.y, pp.x, pp.y, pp.z);
        const bb::Quat cq = bb::Camera::main()->transform->rotation;
        const bb::Vec3 fwd = cq.rotate(bb::Vec3(0.0f, 0.0f, 1.0f));
        printf("shell: camera forward (0,0,1 rotated) = (%.3f, %.3f, %.3f)\n", fwd.x, fwd.y, fwd.z);
      }
      if (shot) { glFinish(); screenshot(shot, W, H); }
      break;
    }
  }
  if (autowalk && playerT) {
    printf("shell: autowalk end   player (%.2f, %.2f, %.2f)\n",
           playerT->position.x, playerT->position.y, playerT->position.z);
  }
  printf("shell: ran %d frame(s), %d quads on the last build\n", n,
         (int)eng::state().drawList.size());

  if (wavOut) { // canonical 44-byte RIFF header + the captured interleaved stereo PCM16
    FILE* wf = fopen(wavOut, "wb");
    if (!wf) { printf("shell: --wav-out open failed: %s\n", wavOut); }
    else {
      const unsigned int dataBytes = (unsigned int)(g_wavCap.size() * sizeof(int16_t));
      unsigned char h[44];
      memcpy(h + 0, "RIFF", 4); *(unsigned int*)(h + 4) = 36 + dataBytes;
      memcpy(h + 8, "WAVE", 4);
      memcpy(h + 12, "fmt ", 4); *(unsigned int*)(h + 16) = 16;
      *(unsigned short*)(h + 20) = 1;                 // PCM
      *(unsigned short*)(h + 22) = 2;                 // stereo
      *(unsigned int*)(h + 24) = 44100;
      *(unsigned int*)(h + 28) = 44100 * 4;
      *(unsigned short*)(h + 32) = 4;
      *(unsigned short*)(h + 34) = 16;
      memcpy(h + 36, "data", 4); *(unsigned int*)(h + 40) = dataBytes;
      fwrite(h, 1, 44, wf);
      if (!g_wavCap.empty()) fwrite(&g_wavCap[0], 1, dataBytes, wf);
      fclose(wf);
      printf("shell: audio capture %.2f s -> %s\n", dataBytes / 176400.0, wavOut);
    }
  }

  eng::engineShutdown();
  wglMakeCurrent(0, 0);
  wglDeleteContext(rc);
  ReleaseDC(hwnd, dc);
  DestroyWindow(hwnd);
  return 0;
}
