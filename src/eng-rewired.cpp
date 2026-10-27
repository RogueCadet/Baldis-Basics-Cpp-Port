// eng-rewired.cpp -- the iOS build's input path: Rewired actions fed by the touch controls.
//
// The PC build reads keys through the game's own InputManager; the genuine iOS build instead reads
// Rewired ACTIONS by name (PlayerScript: GetAxis("Forward"/"Strafe"/"Turn"), GetButton("Run"),
// GetButtonDown("Interact"/"Item"/"Pause"...) -- ios-ref/classes/*.ios.c with the metadata-usage
// slots resolved). On iOS those actions come from one Rewired custom controller, "TouchControl",
// whose elements are written by the on-screen Rewired touch components. Everything below is copied
// from the genuine data or code:
//   - element -> action map + per-axis coordinate mode: data/rewired-touch.json, baked from the
//     iOS level0 Rewired Input Manager (tools/ios-bake-rewired.py);
//   - TouchJoystick: value = ClampMagnitude(touch - rect centre, range) / range (Analog, Circle
//     bounds; Rewired_Core TouchJoystick drag handler), range = _stickRange 60 canvas units;
//   - TouchPad: Delta mode, Physical format: per-frame finger delta in pixels / Screen.dpi * 100,
//     dpi < 10 -> 96 (TouchPad value formatter); _useInertia 0, so no smoothing while dragging;
//   - TouchButton: pressed only by a touch that STARTS inside (activateOnSwipeIn 0), held until
//     that touch ends even if it slides out (stayActiveOnSwipeOut 1). _buttonType ToggleSwitch
//     (Run Button) flips its value on each press instead and keeps it through the release;
//   - uGUI raycasting: a touch over a raycast-target graphic of a HIGHER-sorted canvas (the
//     ThinkPad's full-screen backdrop on iOS) reaches that canvas, not the controls below.
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "eng-internal.h"
#include "eng-json.h"

namespace eng {

namespace {

struct Link { int element; std::string action; bool invert; };
struct Element { int id; std::string name; bool axis; int coordinateMode; };

struct RewiredState {
  bool loaded = false;
  std::vector<Element> elements;
  std::vector<Link> links;
  std::map<int, float> axisValue;            // element id -> this frame's axis value
  std::map<int, bool> buttonValue;           // element id -> held this frame
  std::map<std::string, float> actAxis;      // action -> combined axis value
  std::map<std::string, bool> actHeld, actHeldPrev;
  std::vector<TouchControlDef> layout;
  bool explicitLayout = false;               // set by touchSetLayout (tests); else built from the scene
  struct Ptr { int control; const void* owner; float x, y, lastX, lastY; };
  std::map<int, Ptr> pointers;               // pointer id -> claimed control
  float dpi = 0.0f;
};
RewiredState& rs() { static RewiredState s; return s; }

void loadOnce() {
  RewiredState& r = rs();
  if (r.loaded) return;
  r.loaded = true;
  bool ok = false;
  const std::string text = jsonReadFile("data/rewired-touch.json", ok);
  if (!ok) return;
  JsonValue root;
  std::string err;
  if (!jsonParse(text, root, err)) return;
  if (const JsonValue* els = root.find("elements"))
    for (int i = 0; i < els->size(); i++) {
      const JsonValue* e = els->at((size_t)i);
      Element el;
      el.id = e->find("id")->asInt(-1);
      el.name = e->find("name")->str;
      el.axis = e->find("axis")->asBool(false);          // JSON true/false (asInt reads a bool as 0)
      el.coordinateMode = e->find("coordinateMode")->asInt(0);
      r.elements.push_back(el);
    }
  if (const JsonValue* m = root.find("map"))
    for (int i = 0; i < m->size(); i++) {
      const JsonValue* l = m->at((size_t)i);
      Link k;
      k.element = l->find("element")->asInt(-1);
      k.action = l->find("action")->str;
      k.invert = l->find("invert")->asBool(false);
      r.links.push_back(k);
    }
}

const Element* elementById(int id) {
  RewiredState& r = rs();
  for (size_t i = 0; i < r.elements.size(); i++) if (r.elements[i].id == id) return &r.elements[i];
  return 0;
}

int controlAt(float x, float y) {          // topmost = last in layout order (uGUI sibling order)
  const std::vector<TouchControlDef>& L = rs().layout;
  for (int i = (int)L.size() - 1; i >= 0; i--)
    if (x >= L[(size_t)i].x0 && x <= L[(size_t)i].x1 && y >= L[(size_t)i].y0 && y <= L[(size_t)i].y1) return i;
  return -1;
}

}  // namespace

void touchSetLayout(const std::vector<TouchControlDef>& controls) {
  rs().layout = controls;
  rs().explicitLayout = !controls.empty();
  rs().pointers.clear();
}
void touchSetDpi(float dpi) { rs().dpi = dpi; state().screenDpi = dpi; }

// The live layout: every enabled RewiredTouchControl on an active canvas, in registry (hierarchy)
// order, so later = on top as in uGUI raycasting. A non-interactable control is a Blocker.
// "Child/Grandchild" below `root` (""= root itself). Transforms keep only parent links, so match a
// candidate's name chain upward; run once per invisible control instance (hiddenApplied).
static bb::GameObject* findRelative(bb::Transform* root, const std::string& path) {
  if (!root) return 0;
  if (path.empty()) return root->gameObject();
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos <= path.size()) {
    const size_t slash = path.find('/', pos);
    parts.push_back(path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos));
    if (slash == std::string::npos) break;
    pos = slash + 1;
  }
  State& g = state();
  for (size_t gi = 0; gi < g.gos.size(); gi++) {
    bb::Transform* t = g.gos[gi]->transform;
    bool ok = true;
    for (int k = (int)parts.size() - 1; k >= 0 && ok; k--) {
      if (!t || !t->gameObject() || t->gameObject()->name != parts[(size_t)k]) ok = false;
      else t = t->parent;
    }
    if (ok && t == root) return g.gos[gi];
  }
  return 0;
}

static void buildSceneLayout() {
  RewiredState& r = rs();
  State& g = state();
  r.layout.clear();
  for (size_t gi = 0; gi < g.gos.size(); gi++) {
    bb::GameObject* go = g.gos[gi];
    if (!go->activeInHierarchy()) continue;
    for (size_t ci = 0; ci < go->components.size(); ci++) {
      bb::RewiredTouchControl* tc = dynamic_cast<bb::RewiredTouchControl*>(go->components[ci]);
      if (!tc || !tc->enabled) continue;
      if (!tc->visible && !tc->hiddenApplied) {   // Rewired: target graphic + transitioner graphics
        tc->hiddenApplied = true;
        for (size_t h = 0; h < tc->hideOnInvisible.size(); h++) {
          bb::GameObject* target = findRelative(tc->transform, tc->hideOnInvisible[h]);
          if (!target) continue;
          for (size_t k = 0; k < target->components.size(); k++)
            if (bb::Image* im = dynamic_cast<bb::Image*>(target->components[k])) im->enabled = false;
        }
      }
      TouchControlDef d;
      if (!uiScreenRect(tc->transform, d.x0, d.y0, d.x1, d.y1)) continue;
      d.owner = tc;
      if (!tc->interactable) d.kind = TouchControlDef::Blocker;
      else if (tc->kind == bb::RewiredTouchControl::Joystick) d.kind = TouchControlDef::Joystick;
      else if (tc->kind == bb::RewiredTouchControl::Pad) d.kind = TouchControlDef::Pad;
      else d.kind = TouchControlDef::Button;
      d.elemX = tc->elemX;
      d.elemY = tc->elemY;
      d.elemButton = tc->elemButton;
      // _scaleStickRange: the 60 canvas units follow the canvas scale -> screen pixels here
      const float h = d.y1 - d.y0, rh = tc->transform && dynamic_cast<bb::RectTransform*>(tc->transform)
          ? static_cast<bb::RectTransform*>(tc->transform)->sizeDelta.y : 0.0f;
      d.rangePx = (rh > 0.0f) ? tc->stickRange * (h / rh) : tc->stickRange;
      r.layout.push_back(d);
    }
  }
  // keep each live touch on the control it went down on (indices shift as controls toggle)
  for (std::map<int, RewiredState::Ptr>::iterator it = r.pointers.begin(); it != r.pointers.end();) {
    int found = -1;
    for (size_t i = 0; i < r.layout.size(); i++) if (r.layout[i].owner == it->second.owner) found = (int)i;
    if (found < 0) r.pointers.erase(it++);
    else { it->second.control = found; ++it; }
  }
}

// uGUI's EventSystem takes the topmost raycast hit across canvases, higher sortingOrder first.
// True when an enabled raycast-target Image/RawImage on a canvas sorted above `order` covers (x,y).
static bool coveredByHigherCanvas(float x, float y, int order) {
  State& g = state();
  for (size_t gi = 0; gi < g.gos.size(); gi++) {
    bb::GameObject* go = g.gos[gi];
    if (!go->activeInHierarchy()) continue;
    for (size_t ci = 0; ci < go->components.size(); ci++) {
      bb::Component* c = go->components[ci];
      bool target = false;
      if (bb::Image* im = dynamic_cast<bb::Image*>(c)) target = im->enabled && im->raycastTarget;
      else if (bb::RawImage* ri = dynamic_cast<bb::RawImage*>(c)) target = ri->enabled && ri->raycastTarget;
      if (!target) continue;
      int o = 0;
      float x0, y0, x1, y1;
      if (!uiCanvasOrder(go->transform, o) || o <= order) continue;
      if (uiScreenRect(go->transform, x0, y0, x1, y1) && x >= x0 && x <= x1 && y >= y0 && y <= y1) return true;
    }
  }
  return false;
}

void touchDown(int id, float x, float y) {
  state().usingTouch = true;                 // InputTypeManager: any touch switches to touch mode
  if (!rs().explicitLayout) buildSceneLayout();
  const int c = controlAt(x, y);
  if (c < 0) return;                         // not on a control: the touch is not claimed
  bb::RewiredTouchControl* tc = rs().explicitLayout ? 0
      : (bb::RewiredTouchControl*)const_cast<void*>(rs().layout[(size_t)c].owner);
  if (tc) {
    int order = 0;
    if (uiCanvasOrder(tc->transform, order) && coveredByHigherCanvas(x, y, order)) return;
    if (tc->toggle && rs().layout[(size_t)c].kind == TouchControlDef::Button) tc->toggledOn = !tc->toggledOn;
  }
  RewiredState::Ptr p;
  p.control = c;
  p.owner = rs().layout[(size_t)c].owner;
  p.x = p.lastX = x;
  p.y = p.lastY = y;
  rs().pointers[id] = p;                     // a Blocker claims too (it absorbs the touch)
}
void touchMove(int id, float x, float y) {
  std::map<int, RewiredState::Ptr>::iterator it = rs().pointers.find(id);
  if (it == rs().pointers.end()) return;     // activateOnSwipeIn 0: a moving touch never claims
  it->second.x = x;
  it->second.y = y;
}
void touchUp(int id) { rs().pointers.erase(id); }

// Once per frame, before scripts run: control state -> element values -> action values.
void rewiredBeginFrame() {
  RewiredState& r = rs();
  loadOnce();
  if (!r.explicitLayout) buildSceneLayout();
  // InputTypeManager.Update: a button on a non-touch controller (keyboard/mouse here) while no touch
  // is down switches back out of touch mode.
  if (r.pointers.empty() && bb::Input::anyKey()) state().usingTouch = false;
  r.axisValue.clear();
  r.buttonValue.clear();
  const float sdpi = state().screenDpi;
  const float dpi = sdpi < 10.0f ? 96.0f : sdpi;
  for (std::map<int, RewiredState::Ptr>::iterator it = r.pointers.begin(); it != r.pointers.end(); ++it) {
    RewiredState::Ptr& p = it->second;
    if (p.control < 0 || (size_t)p.control >= r.layout.size()) continue;
    const TouchControlDef& c = r.layout[(size_t)p.control];
    if (c.kind == TouchControlDef::Joystick) {
      const float cx = (c.x0 + c.x1) * 0.5f, cy = (c.y0 + c.y1) * 0.5f;
      float ox = p.x - cx, oy = cy - p.y;    // screen rows run DOWN; Unity's y runs up
      const float len = sqrtf(ox * ox + oy * oy);
      if (len > c.rangePx && len > 0.0f) { ox *= c.rangePx / len; oy *= c.rangePx / len; }
      if (c.rangePx > 0.0f) {
        if (c.elemX >= 0) r.axisValue[c.elemX] = ox / c.rangePx;
        if (c.elemY >= 0) r.axisValue[c.elemY] = oy / c.rangePx;
      }
    } else if (c.kind == TouchControlDef::Pad) {
      if (c.elemX >= 0) r.axisValue[c.elemX] = (p.x - p.lastX) / dpi * 100.0f;
      if (c.elemY >= 0) r.axisValue[c.elemY] = (p.lastY - p.y) / dpi * 100.0f;
    } else if (c.kind == TouchControlDef::Button) {
      const bb::RewiredTouchControl* tc = r.explicitLayout ? 0 : (const bb::RewiredTouchControl*)c.owner;
      if (c.elemButton >= 0 && !(tc && tc->toggle)) r.buttonValue[c.elemButton] = true;
    }
    p.lastX = p.x;
    p.lastY = p.y;
  }
  // a ToggleSwitch button's value is its latched state, touched or not
  if (!r.explicitLayout)
    for (size_t i = 0; i < r.layout.size(); i++) {
      const bb::RewiredTouchControl* tc = (const bb::RewiredTouchControl*)r.layout[i].owner;
      if (tc && tc->toggle && tc->toggledOn && r.layout[i].kind == TouchControlDef::Button && r.layout[i].elemButton >= 0)
        r.buttonValue[r.layout[i].elemButton] = true;
    }
  // STICK GRAPHIC (TouchJoystick value setter): stick.anchoredPosition = original + value * range,
  // range in the stick parent's units (the stick is the joystick's direct child, scale 1 -> the
  // serialized _stickRange). No touch on it -> value 0 -> centred (_centerStickOnRelease 1).
  if (!r.explicitLayout)
    for (size_t i = 0; i < r.layout.size(); i++) {
      const TouchControlDef& c = r.layout[i];
      bb::RewiredTouchControl* tc = (bb::RewiredTouchControl*)const_cast<void*>(c.owner);
      if (!tc || c.kind != TouchControlDef::Joystick || !tc->stick) continue;
      if (!tc->stickOrigSet) { tc->stickOrigSet = true; tc->stickOrig = tc->stick->anchoredPosition; }
      float vx = 0.0f, vy = 0.0f;
      for (std::map<int, RewiredState::Ptr>::iterator it = r.pointers.begin(); it != r.pointers.end(); ++it)
        if (it->second.owner == tc && c.rangePx > 0.0f) {
          const float cx = (c.x0 + c.x1) * 0.5f, cy = (c.y0 + c.y1) * 0.5f;
          float ox = it->second.x - cx, oy = cy - it->second.y;
          const float len = sqrtf(ox * ox + oy * oy);
          if (len > c.rangePx) { ox *= c.rangePx / len; oy *= c.rangePx / len; }
          vx = ox / c.rangePx;
          vy = oy / c.rangePx;
        }
      tc->stick->anchoredPosition = bb::Vec2(tc->stickOrig.x + vx * tc->stickRange, tc->stickOrig.y + vy * tc->stickRange);
    }
  // COLOUR TINT transitions (Rewired ColorTint): pressed while a touch holds the control, disabled
  // when not interactable, else normal (touch never hovers, so Highlighted never applies). Unity
  // CrossFadeColor: a linear fade over m_FadeDuration of unscaled time; the first state is instant.
  if (!r.explicitLayout)
    for (size_t i = 0; i < r.layout.size(); i++) {
      bb::RewiredTouchControl* tc = (bb::RewiredTouchControl*)const_cast<void*>(r.layout[i].owner);
      if (!tc || tc->tints.empty()) continue;
      // TouchButton.IsPressed() is the button VALUE: a ToggleSwitch stays "pressed" while on
      bool held = false;
      for (std::map<int, RewiredState::Ptr>::iterator it = r.pointers.begin(); it != r.pointers.end(); ++it)
        if (it->second.owner == tc) held = true;
      if (tc->toggle && r.layout[i].kind == TouchControlDef::Button) held = tc->toggledOn;
      for (size_t k = 0; k < tc->tints.size(); k++) {
        bb::RewiredTouchControl::TintTarget& tt = tc->tints[k];
        if (!tt.resolved) {
          tt.resolved = true;
          if (bb::GameObject* go = findRelative(tc->transform, tt.path)) tt.image = go->GetComponent<bb::Image>();
        }
        if (!tt.image) continue;
        const bb::Color& c = !tc->interactable ? tt.disabled : held ? tt.pressed : tt.normal;
        const bb::Color want(c.r * tt.multiplier, c.g * tt.multiplier, c.b * tt.multiplier, c.a * tt.multiplier);
        if (!tt.started) { tt.started = true; tt.from = tt.to = want; tt.t = 1.0f; }
        else if (want.r != tt.to.r || want.g != tt.to.g || want.b != tt.to.b || want.a != tt.to.a) {
          const float u = tt.t < 1.0f ? tt.t : 1.0f;     // restart from where the fade has got to
          tt.from = bb::Color(tt.from.r + (tt.to.r - tt.from.r) * u, tt.from.g + (tt.to.g - tt.from.g) * u,
                              tt.from.b + (tt.to.b - tt.from.b) * u, tt.from.a + (tt.to.a - tt.from.a) * u);
          tt.to = want;
          tt.t = 0.0f;
        }
        if (tt.t < 1.0f) tt.t += tt.fade > 0.0f ? state().dtUnscaled / tt.fade : 1.0f;
        const float u = tt.t < 1.0f ? tt.t : 1.0f;
        tt.image->tint = bb::Color(tt.from.r + (tt.to.r - tt.from.r) * u, tt.from.g + (tt.to.g - tt.from.g) * u,
                                   tt.from.b + (tt.to.b - tt.from.b) * u, tt.from.a + (tt.to.a - tt.from.a) * u);
      }
    }
  r.actHeldPrev = r.actHeld;
  r.actAxis.clear();
  r.actHeld.clear();
  for (size_t i = 0; i < r.links.size(); i++) {
    const Link& k = r.links[i];
    const Element* e = elementById(k.element);
    if (!e) continue;
    if (e->axis) {
      std::map<int, float>::iterator v = r.axisValue.find(k.element);
      if (v != r.axisValue.end()) r.actAxis[k.action] += k.invert ? -v->second : v->second;
    } else if (r.buttonValue.count(k.element)) {
      r.actHeld[k.action] = true;
    }
  }
}

}  // namespace eng

namespace bb {

float RewiredPlayer::GetAxis(const char* action) const {
  eng::RewiredState& r = eng::rs();
  std::map<std::string, float>::const_iterator a = r.actAxis.find(action);
  if (a != r.actAxis.end()) return a->second;
  return r.actHeld.count(action) ? 1.0f : 0.0f;
}
bool RewiredPlayer::GetButton(const char* action) const {
  eng::RewiredState& r = eng::rs();
  if (r.actHeld.count(action)) return true;
  std::map<std::string, float>::const_iterator a = r.actAxis.find(action);
  return a != r.actAxis.end() && a->second != 0.0f;
}
bool RewiredPlayer::GetButtonDown(const char* action) const {
  eng::RewiredState& r = eng::rs();
  return r.actHeld.count(action) && !r.actHeldPrev.count(action);
}
AxisCoordinateMode RewiredPlayer::GetAxisCoordinateMode(const char* action) const {
  eng::RewiredState& r = eng::rs();
  eng::loadOnce();
  for (size_t i = 0; i < r.links.size(); i++)
    if (r.links[i].action == action)
      if (const eng::Element* e = eng::elementById(r.links[i].element))
        if (e->axis) return e->coordinateMode == 1 ? AxisCoordinateMode::Relative : AxisCoordinateMode::Absolute;
  return AxisCoordinateMode::Absolute;
}
RewiredPlayer& ReInputPlayer() { static RewiredPlayer p; return p; }
bool ApplicationIsMobilePlatform() { return eng::state().mobile; }
bool InputTypeManagerUsingTouch() { return eng::state().usingTouch; }

}  // namespace bb
