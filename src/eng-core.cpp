// eng-core.cpp -- Phase 2 engine-lite implementation, part 1: object lifecycle, frame loop,
// PlayerPrefs, Random/Debug/Time/Cursor/Input, exact ray math (OBB slab + capsules), NavMeshAgent
// straight-line integration (grid A* is Phase 4), AudioSource playback simulation with real WAV
// durations, Animator/EventSystem/UI state, and the coroutine registry.
// Windows-first; the iOS build links this file unchanged (no platform headers included here).
// Must precede every include: MSVC flags getenv (the ENG_TRACE_START switch) as C4996 under /W3.
#define _CRT_SECURE_NO_WARNINGS

#include <queue>
#include <chrono>
#include "eng-internal.h"
#include <cstdio>
#include <typeinfo>   // ENG_TRACE_START names components by dynamic type
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <cmath>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <set>          // baked-texture guid lookup (buildDrawList)

#ifdef _MSC_VER
static FILE* fopenSafe(const char* path, const char* mode) {
  FILE* f = 0;
  if (fopen_s(&f, path, mode) != 0) return 0;
  return f;
}
#else
static FILE* fopenSafe(const char* path, const char* mode) { return fopen(path, mode); }
#endif

// ---------------------------------------------------------------------------
// engine state + frame loop (single translation unit; accessors keep other TUs opaque)
// ---------------------------------------------------------------------------
namespace eng {

State::State() {
  for (int i = 0; i < bb::kKeySlots; i++) keyEdge[i] = false;
  for (int i = 0; i < bb::kKeySlots; i++) keyHeld[i] = false;
  for (int i = 0; i < 8; i++) mouseEdge[i] = false;
  for (int i = 0; i < 8; i++) mouseHeld[i] = false;
  rng[0] = 0x9E3779B9u;
  rng[1] = 0x85EBCA6Bu;
  rng[2] = 0xC2B2AE35u;
  rng[3] = 0x27D4EB2Fu;
}

State& state() { static State g; return g; }

// ---- registries ------------------------------------------------------------
std::vector<bb::GameObject*>& liveGameObjects() { return state().gos; }

void noteRuntimeObject(bb::Object* o) {
  if (o) state().ownedObjects.push_back(o);
}

// Bumped on every transform add/remove/re-parent: propagateHierarchy's cached parent-first order
// must never hold a freed pointer (a destroy + create in one frame keeps the registry SIZE equal).
static unsigned int s_hierGen = 1;

void noteTransform(bb::Transform* t) {
  if (!t) return;
  std::vector<bb::Transform*>& v = state().transforms;
  if (std::find(v.begin(), v.end(), t) == v.end()) { v.push_back(t); s_hierGen++; } // dedupe: ownership stays w/ GO
}

void noteParent(bb::Transform* child, bb::Transform* parent) {
  if (!child || !parent) return;
  s_hierGen++;
  state().childOf[parent].push_back(child);
  child->parent = parent;
}

void noteGameObject(bb::GameObject* go) {
  if (!go) return;
  if (go->transform) noteTransform(go->transform);
  state().gos.push_back(go);
  state().componentVersion++;
}

void noteComponent(bb::Component* c) {
  if (!c) return;
  State& g = state();
  // Engine-created components (Singleton::CreateInstance) may carry an unregistered GO -- adopt it
  // so Find/FixedUpdate dispatch and shutdown see them.
  if (c->gameObject &&
      std::find(g.gos.begin(), g.gos.end(), c->gameObject) == g.gos.end()) {
    noteGameObject(c->gameObject);
  }
  if (dynamic_cast<bb::Collider*>(c)) g.colliderVersion++;
  g.componentVersion++;
  if (bb::Rigidbody* rb = dynamic_cast<bb::Rigidbody*>(c)) g.rigidbodies.push_back(rb);
  if (bb::BoxCollider* b = dynamic_cast<bb::BoxCollider*>(c)) g.boxColliders.push_back(b);
  else if (bb::CapsuleCollider* s = dynamic_cast<bb::CapsuleCollider*>(c)) g.capsuleColliders.push_back(s);
  else if (bb::MeshCollider* m = dynamic_cast<bb::MeshCollider*>(c)) g.meshColliders.push_back(m);
  else if (bb::NavMeshAgent* a = dynamic_cast<bb::NavMeshAgent*>(c)) g.agents.push_back(a);
  else if (bb::AudioSource* s = dynamic_cast<bb::AudioSource*>(c)) g.audioSources.push_back(s);
  else if (bb::Camera* cam = dynamic_cast<bb::Camera*>(c)) g.cameras.push_back(cam);
}

template <class V> void eraseOne(V& vec, bb::Component* n) { // swap-remove a registry slot
  for (size_t i = 0; i < vec.size(); i++) {
    if (vec[i] == n) { vec[i] = vec.back(); vec.pop_back(); break; }
  }
}

// Order-preserving removal. pendingStarts must keep creation order (Unity's Start order), so it
// cannot use eraseOne's swap-remove.
template <class V> void eraseKeepOrder(V& vec, bb::Component* n) {
  for (size_t i = 0; i < vec.size(); i++) {
    if (static_cast<bb::Component*>(vec[i]) == n) { vec.erase(vec.begin() + i); return; }
  }
}

void unnoteComponent(bb::Component* c) {
  State& g = state();
  eraseKeepOrder(g.pendingStarts, c); // destroyed before its first Start -> it never gets one

  // Drop any trigger overlap naming this collider: the set holds raw pointers, so a destroyed
  // collider left in it would be dereferenced on the next Exit dispatch. No Exit message is sent
  // (Unity does not deliver one to a destroyed object either).
  if (bb::Collider* col = dynamic_cast<bb::Collider*>(c)) {
    std::set<std::pair<bb::Collider*, bb::Collider*> >::iterator it = g.triggerOverlaps.begin();
    while (it != g.triggerOverlaps.end()) {
      if (it->first == col || it->second == col) g.triggerOverlaps.erase(it++);
      else ++it;
    }
    // If this happened during a dispatch, the dispatcher is walking snapshots taken before the
    // destroy; mark the collider so it skips both sides of any pair naming it.
    g.triggerDead.insert(col);
  }

  if (dynamic_cast<bb::Collider*>(c)) g.colliderVersion++;
  g.componentVersion++;
  eraseOne(g.boxColliders, c);
  eraseOne(g.capsuleColliders, c);
  eraseOne(g.meshColliders, c);
  eraseOne(g.agents, c);
  eraseOne(g.audioSources, c);
  eraseOne(g.cameras, c);
  eraseKeepOrder(g.rigidbodies, c);
  // The render lists too: a destroyed object's renderers stayed in them, and drawBillboards /
  // drawWorld later read the freed GameObject (crash in GameObject::activeInHierarchy, user's
  // minidump 2026-10-26). The BSODA spray carries both kinds and destroys itself after 30 s.
  eraseKeepOrder(g.spriteRenderers, c);   // (MeshRenderers are not components: destroyObjectNow)
  if (g.dragSlider == c) { g.dragSlider = 0; g.dragPointer = -1; }
  if (bb::NavMeshAgent* a = dynamic_cast<bb::NavMeshAgent*>(c)) g.agentSim.erase(a);
  if (bb::AudioSource* s = dynamic_cast<bb::AudioSource*>(c)) g.audioSim.erase(s);
  if (bb::Animator* an = dynamic_cast<bb::Animator*>(c)) { g.animBools.erase(an); g.animTriggers.erase(an); }
  if (bb::CharacterController* cc = dynamic_cast<bb::CharacterController*>(c)) {
    g.moveSim.erase(cc);
    // Tear down the mirrored capsule with its CharacterController (see State::ccShell): it is not
    // in go->components, so the destroy sweep never reaches it on its own.
    std::unordered_map<bb::CharacterController*, bb::CapsuleCollider*>::iterator sh =
        g.ccShell.find(cc);
    if (sh != g.ccShell.end()) {
      bb::CapsuleCollider* shell = sh->second;
      g.ccShell.erase(sh);
      unnoteComponent(shell);   // CapsuleCollider, not a CharacterController -> no recursion
      delete shell;
    }
  }
}

// ---- deferred object destruction (Unity end-of-frame semantics) ------------
static void destroyObjectNow(bb::GameObject* go) {
  State& g = state();
  if (!go) return;
  // Unity: a UI object's transform IS its RectTransform, which is ALSO a component. The
  // components loop below releases it ONCE; the transform block must not delete it again.
  const bool transformAlsoComponent =
      go->transform != 0 && dynamic_cast<bb::RectTransform*>(go->transform) != 0;
  if (go->transform) { // children first (Unity Destroy tears the whole subtree)
    std::unordered_map<bb::Transform*, std::vector<bb::Transform*>>::iterator it = g.childOf.find(go->transform);
    if (it != g.childOf.end()) {
      std::vector<bb::Transform*> kids = it->second; // copy: destroying mutates the map
      for (size_t i = 0; i < kids.size(); i++) {
        if (kids[i] && kids[i]->go) destroyObjectNow(kids[i]->go);
      }
    }
  }
  if (go->activeSelf) {
    for (size_t i = 0; i < go->components.size(); i++) go->components[i]->OnDestroy(); // Unity: active objects only
  }
  // detach from the parent's child list so later subtree destroys never see stale pointers
  if (go->transform && go->transform->parent) {
    std::unordered_map<bb::Transform*, std::vector<bb::Transform*>>::iterator pit = g.childOf.find(go->transform->parent);
    if (pit != g.childOf.end()) {
      std::vector<bb::Transform*>& sibs = pit->second;
      for (size_t i = 0; i < sibs.size(); i++) {
        if (sibs[i] == go->transform) { sibs[i] = sibs.back(); sibs.pop_back(); break; }
      }
    }
  }
  for (size_t i = 0; i < go->components.size(); i++) {
    unnoteComponent(go->components[i]);
    delete go->components[i];
  }
  go->components.clear();
  if (go->transform) {
    g.childOf.erase(go->transform);
    std::vector<bb::Transform*>& trs = g.transforms;
    for (size_t i = 0; i < trs.size(); i++) {
      if (trs[i] == go->transform) { trs[i] = trs.back(); trs.pop_back(); s_hierGen++; break; }
    }
    if (!transformAlsoComponent) delete go->transform;  // rect form: released with components
    go->transform = 0;
  }
  // Order-preserving erase, NOT swap-remove: g.gos IS the creation order, and both
  // GameObject::Find and FindObjectOfType are specified to return the first match in that order
  // (the loader's goBase watermark also assumes survivors stay a prefix). A swap-remove here
  // silently scrambled that ordering after any runtime Destroy.
  for (size_t i = 0; i < g.gos.size(); i++) {
    if (g.gos[i] == go) { g.gos.erase(g.gos.begin() + i); break; }
  }
  if (g.selectedGO == go) g.selectedGO = 0;
  g.componentVersion++;
  // MeshRenderers live outside go->components (Object-derived, owned by the scene list) and point
  // back at their GameObject: drop this object's, or drawWorld reads the freed GameObject.
  for (size_t i = 0; i < g.meshRenderers.size();) {
    if (g.meshRenderers[i]->gameObject == go) g.meshRenderers.erase(g.meshRenderers.begin() + i);
    else i++;
  }
  delete go;
}

void queueDestroy(bb::GameObject* go, float delay) {
  if (!go) return;
  std::vector<DoomEntry>& d = state().doomed;
  for (size_t i = 0; i < d.size(); i++) { // Unity dedupes repeat destroys on the same object
    if (d[i].go == go) return;
  }
  d.push_back(DoomEntry(go, state().doomClock + delay));
}

static void purgeDoomed() {
  State& g = state();
  if (g.doomed.empty()) return;
  std::vector<DoomEntry> keep;
  for (size_t i = 0; i < g.doomed.size(); i++) {
    if (g.doomed[i].t <= g.doomClock) destroyObjectNow(g.doomed[i].go);
    else keep.push_back(g.doomed[i]);
  }
  g.doomed.swap(keep);
}

// ---- agent + audio integration ---------------------------------------------
// ---- NavMesh pathing ----------------------------------------------------------------------
// Agents used to moveTowards their destination in a straight 3D line: through walls, and up toward
// the player's eye height (Baldi "chases me floating in the air"). They now walk the scene's real
// navmesh: A* over its polygons, the funnel ("string pulling") algorithm through the portals for
// the corner path, and the transform kept on the surface at y = surface + baseOffset, as Unity's
// agent does. Every agent here has acceleration >= 1000, so speed changes are treated as instant.
void applicationPause(bool pauseStatus) {
  State& g = state();
  std::vector<bb::GameObject*> gos(g.gos);       // a handler may activate/destroy objects
  for (size_t i = 0; i < gos.size(); i++) {
    bb::GameObject* go = gos[i];
    if (!go->activeInHierarchy()) continue;
    std::vector<bb::Component*> cs(go->components);
    for (size_t k = 0; k < cs.size(); k++) cs[k]->OnApplicationPause(pauseStatus);
  }
}

void loadNavMesh(const std::string& scene) {
  State& g = state();
  g.navPolys.clear();
  bool ok = false;
  const std::string text = jsonReadFile("data/navmesh-" + scene + ".json", ok);
  if (!ok) return;
  JsonValue root;
  std::string err;
  if (!jsonParse(text, root, err)) return;
  const JsonValue* polys = root.find("polys");
  if (!polys) return;
  g.navPolys.resize((size_t)polys->size());
  for (int i = 0; i < polys->size(); i++) {
    const JsonValue* pj = polys->at((size_t)i);
    NavPoly& p = g.navPolys[(size_t)i];
    bb::Vec3 c;
    if (const JsonValue* vv = pj->find("v"))
      for (int k = 0; k < vv->size(); k++) {
        const JsonValue* q = vv->at((size_t)k);
        p.v.push_back(bb::Vec3(q->at(0)->asFloat(), q->at(1)->asFloat(), q->at(2)->asFloat()));
        c = c + p.v.back();
      }
    if (!p.v.empty()) p.centre = c * (1.0f / (float)p.v.size());
    if (const JsonValue* nn = pj->find("n"))
      for (int k = 0; k < nn->size(); k++) {
        const JsonValue* l = nn->at((size_t)k);
        NavLink L;
        L.poly = l->find("p")->asInt(-1);
        L.ax = l->find("a")->at(0)->asFloat(); L.az = l->find("a")->at(1)->asFloat();
        L.bx = l->find("b")->at(0)->asFloat(); L.bz = l->find("b")->at(1)->asFloat();
        p.links.push_back(L);
      }
  }
}

namespace {
bool navInside(const NavPoly& p, float x, float z) {     // convex polygon, either winding
  bool pos = false, neg = false;
  for (size_t i = 0; i < p.v.size(); i++) {
    const bb::Vec3& a = p.v[i];
    const bb::Vec3& b = p.v[(i + 1) % p.v.size()];
    const float cr = (b.x - a.x) * (z - a.z) - (b.z - a.z) * (x - a.x);
    if (cr > 1e-4f) pos = true; else if (cr < -1e-4f) neg = true;
    if (pos && neg) return false;
  }
  return true;
}
// nearest navmesh point (Unity SamplePosition / the projection SetDestination does)
int navNearest(float x, float z, float& outX, float& outZ) {
  const std::vector<NavPoly>& P = state().navPolys;
  int best = -1;
  float bestD = 1e30f;
  for (size_t i = 0; i < P.size(); i++) {
    if (navInside(P[i], x, z)) { outX = x; outZ = z; return (int)i; }
    for (size_t k = 0; k < P[i].v.size(); k++) {
      const bb::Vec3& a = P[i].v[k];
      const bb::Vec3& b = P[i].v[(k + 1) % P[i].v.size()];
      const float ex = b.x - a.x, ez = b.z - a.z, l2 = ex * ex + ez * ez;
      float t = l2 > 0.0f ? ((x - a.x) * ex + (z - a.z) * ez) / l2 : 0.0f;
      t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
      const float px = a.x + ex * t, pz = a.z + ez * t;
      const float d = (px - x) * (px - x) + (pz - z) * (pz - z);
      if (d < bestD) { bestD = d; best = (int)i; outX = px; outZ = pz; }
    }
  }
  return best;
}
float navHeight(int poly) { return poly >= 0 ? state().navPolys[(size_t)poly].centre.y : 0.0f; }
float cross2(float ax, float az, float bx, float bz, float cx, float cz) {   // (b-a) x (c-a), XZ
  return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}
}  // namespace

// Plan start -> dest. Returns false when there is no navmesh (caller keeps the old behaviour).
static bool agentRepath(AgentSim& s, const bb::Vec3& start, const bb::Vec3& dest) {
  const std::vector<NavPoly>& P = state().navPolys;
  if (P.empty()) return false;
  float sx, sz, ex, ez;
  const int sp = navNearest(start.x, start.z, sx, sz);
  const int ep = navNearest(dest.x, dest.z, ex, ez);
  s.corners.clear();
  s.corner = 0;
  if (sp < 0 || ep < 0) return true;
  // A* over polygons (edge-midpoint distance), heuristic = straight distance to the goal
  std::vector<float> gc(P.size(), 1e30f);
  std::vector<int> from(P.size(), -1), via(P.size(), -1);
  std::vector<bb::Vec3> at(P.size());
  std::vector<char> closed(P.size(), 0);
  typedef std::pair<float, int> QE;
  std::priority_queue<QE, std::vector<QE>, std::greater<QE> > open;
  gc[(size_t)sp] = 0.0f;
  at[(size_t)sp] = bb::Vec3(sx, 0.0f, sz);
  open.push(QE(0.0f, sp));
  while (!open.empty()) {
    const int u = open.top().second;
    open.pop();
    if (closed[(size_t)u]) continue;
    closed[(size_t)u] = 1;
    if (u == ep) break;
    for (size_t k = 0; k < P[(size_t)u].links.size(); k++) {
      const NavLink& L = P[(size_t)u].links[k];
      if (L.poly < 0 || closed[(size_t)L.poly]) continue;
      const bb::Vec3 m((L.ax + L.bx) * 0.5f, 0.0f, (L.az + L.bz) * 0.5f);
      const float cost = gc[(size_t)u] + sqrtf((m.x - at[(size_t)u].x) * (m.x - at[(size_t)u].x) +
                                               (m.z - at[(size_t)u].z) * (m.z - at[(size_t)u].z));
      if (cost < gc[(size_t)L.poly]) {
        gc[(size_t)L.poly] = cost;
        from[(size_t)L.poly] = u;
        via[(size_t)L.poly] = (int)k;
        at[(size_t)L.poly] = m;
        const float h = sqrtf((m.x - ex) * (m.x - ex) + (m.z - ez) * (m.z - ez));
        open.push(QE(cost + h, L.poly));
      }
    }
  }
  if (sp != ep && from[(size_t)ep] < 0) return true;         // unreachable: stand still
  // portal list start -> end, each oriented LEFT/RIGHT for the direction of travel
  std::vector<std::pair<int, int> > chain;                    // (poly, link index into it)
  for (int p = ep; p != sp; p = from[(size_t)p]) chain.push_back(std::make_pair(from[(size_t)p], via[(size_t)p]));
  std::reverse(chain.begin(), chain.end());
  std::vector<float> lx, lz, rx, rz;
  lx.push_back(sx); lz.push_back(sz); rx.push_back(sx); rz.push_back(sz);
  for (size_t c = 0; c < chain.size(); c++) {
    const NavPoly& fromP = P[(size_t)chain[c].first];
    const NavLink& L = fromP.links[(size_t)chain[c].second];
    // the portal's "left" end is on the left of the line from this polygon's centre to the next's
    const NavPoly& toP = P[(size_t)L.poly];
    const bool aLeft = cross2(fromP.centre.x, fromP.centre.z, toP.centre.x, toP.centre.z, L.ax, L.az) > 0.0f;
    lx.push_back(aLeft ? L.ax : L.bx); lz.push_back(aLeft ? L.az : L.bz);
    rx.push_back(aLeft ? L.bx : L.ax); rz.push_back(aLeft ? L.bz : L.az);
  }
  lx.push_back(ex); lz.push_back(ez); rx.push_back(ex); rz.push_back(ez);
  // simple stupid funnel algorithm (Mononen), left = positive cross
  float ax = sx, az = sz, flx = lx[1], flz = lz[1], frx = rx[1], frz = rz[1];
  size_t apexI = 0, leftI = 1, rightI = 1;
  for (size_t i = 1; i < lx.size(); i++) {
    // tighten right
    if (cross2(ax, az, frx, frz, rx[i], rz[i]) >= 0.0f) {
      if ((ax == frx && az == frz) || cross2(ax, az, flx, flz, rx[i], rz[i]) < 0.0f) {
        frx = rx[i]; frz = rz[i]; rightI = i;
      } else {
        ax = flx; az = flz; apexI = leftI;
        s.corners.push_back(bb::Vec3(ax, 0.0f, az));
        flx = frx = ax; flz = frz = az; leftI = rightI = apexI;
        i = apexI; continue;
      }
    }
    // tighten left
    if (cross2(ax, az, flx, flz, lx[i], lz[i]) <= 0.0f) {
      if ((ax == flx && az == flz) || cross2(ax, az, frx, frz, lx[i], lz[i]) > 0.0f) {
        flx = lx[i]; flz = lz[i]; leftI = i;
      } else {
        ax = frx; az = frz; apexI = rightI;
        s.corners.push_back(bb::Vec3(ax, 0.0f, az));
        flx = frx = ax; flz = frz = az; leftI = rightI = apexI;
        i = apexI; continue;
      }
    }
  }
  s.corners.push_back(bb::Vec3(ex, 0.0f, ez));
  const float hy = navHeight(ep);
  for (size_t c = 0; c < s.corners.size(); c++) s.corners[c].y = hy;
  return true;
}

static void integrateAgents(float dt) {
  State& g = state();
  for (size_t i = 0; i < g.agents.size(); i++) {
    bb::NavMeshAgent* a = g.agents[i];
    AgentSim& s = g.agentSim[a];
    s.lastStepVel = bb::Vec3::zero();
    if (!a->gameObject || !a->gameObject->activeSelf || !a->enabled || a->isStopped) continue;
    if (!a->transform) continue;
    // agent.velocity = v (BsodaEffectScript, every Update while in a BSODA or Gotta Sweep's wake):
    // Unity's agent then MOVES at that velocity -- it replaces the steering's motion, it does not
    // add to it. Added on top, the Principal's own 30 u/s path walked him straight back against
    // the spray's 20 and the BSODA pushed nobody. The agent stays on its navmesh, as in Unity.
    if (s.hasPendingVel && bb::Time::frameCount - s.pendingFrame <= 1) {
      const bb::Vec3 before = a->transform->position;
      bb::Vec3 pos = before + bb::Vec3(s.pendingVel.x, 0.0f, s.pendingVel.z) * dt;
      if (!g.navPolys.empty()) {
        float nx, nz;
        const int np = navNearest(pos.x, pos.z, nx, nz);
        pos = bb::Vec3(nx, navHeight(np) + a->baseOffset * fabsf(a->transform->scale.y), nz);
      }
      a->transform->position = pos;
      s.planned = false;                       // re-plan from wherever the push leaves it
      s.lastStepVel = (pos - before) / dt;
      continue;
    }
    s.hasPendingVel = false;
    if (!s.hasDest) continue;
    const bb::Vec3 before = a->transform->position;
    if (!g.navPolys.empty()) {
      // replan when the goal moved (Baldi re-targets the player every frame) or none exists yet
      if (!s.planned || bb::length(s.dest - s.plannedFor) > 0.25f ||
          (s.corner >= s.corners.size() && bb::length(bb::Vec3(s.dest.x - before.x, 0.0f, s.dest.z - before.z)) > 0.5f)) {
        agentRepath(s, before, s.dest);
        s.plannedFor = s.dest;
        s.planned = true;
      }
      // ACCELERATION: the agent's actual speed ramps toward `speed` at `acceleration` (both ways).
      // Effectively instant for agents at >= 1000; 1st Prize is authored at 10 and, with speed
      // set to 0 every time it turns (FirstPrizeScript), never reaches full speed straight away.
      {
        const float target = a->speed > 0.0f ? a->speed : 0.0f;
        const float step = (a->acceleration > 0.0f ? a->acceleration : 1e6f) * dt;
        if (s.curSpeed < target) s.curSpeed = (std::min)(target, s.curSpeed + step);
        else s.curSpeed = (std::max)(target, s.curSpeed - step);
      }
      float budget = s.curSpeed * dt;
      bb::Vec3 pos = before;
      while (budget > 0.0f && s.corner < s.corners.size()) {
        const bb::Vec3 tgt(s.corners[s.corner].x, pos.y, s.corners[s.corner].z);
        const float d = bb::length(tgt - pos);
        if (d <= budget) { pos = tgt; budget -= d; s.corner++; }
        else { pos = pos + (tgt - pos) * (budget / d); budget = 0.0f; }
      }
      // ON the surface: y = navmesh height + baseOffset, and Unity scales the agent's dimensions --
      // baseOffset included -- by the transform's scale. Every School agent has baseOffset 1 at
      // scale y 2 (Baldi 1.6): 0.017 + 1*2 = 2.017 and 0.017 + 1*1.6 = 1.617, their authored
      // heights exactly. Unscaled they sat 1 (Baldi 0.6) units sunk into the floor.
      float nx, nz;
      const int np = navNearest(pos.x, pos.z, nx, nz);
      pos.y = navHeight(np) + a->baseOffset * fabsf(a->transform->scale.y);
      a->transform->position = pos;
      // An agent's velocity IS its motion: one that has arrived or is held at the navmesh edge
      // (1st Prize pinning the player, whose destination lies off the mesh by the wall) stands
      // still, and from there accelerates again at `acceleration`. Carrying the old cruise speed
      // through the stop let 1st Prize resume at 100 u/s the instant the player slid free -- a
      // re-hug 0.02 s later -- where the genuine agent needs ~0.5 s at 10 u/s^2 to pass the
      // hug threshold (PlayerScript: velocity > 5).
      {
        const float actual = bb::length(bb::Vec3(pos.x - before.x, 0.0f, pos.z - before.z)) / dt;
        if (actual < s.curSpeed) s.curSpeed = actual;
      }
      // updateRotation: turn toward the motion at angularSpeed (1st Prize has 0 and steers itself)
      const bb::Vec3 mv(pos.x - before.x, 0.0f, pos.z - before.z);
      if (a->angularSpeed > 0.0f && bb::length(mv) > 1e-4f) {
        const float want = atan2f(mv.x, mv.z) * 57.29578f;
        const float have = a->transform->rotation.eulerAngles().y;
        float diff = fmodf(want - have + 540.0f, 360.0f) - 180.0f;
        const float step = a->angularSpeed * dt;
        if (diff > step) diff = step; else if (diff < -step) diff = -step;
        a->transform->rotation.setEulerAngles(bb::Vec3(0.0f, have + diff, 0.0f));
      }
    } else if (a->speed > 0.0f) {
      a->transform->position = bb::moveTowards(before, s.dest, a->speed * dt);
    }
    const bb::Vec3 moved = a->transform->position - before;
    s.lastStepVel = moved / dt;
  }
}

static const int kExpiryMixRate = 44100;  // == the mixer's kMixRate (output samples per second)

// VOICE EXPIRY. A voice used to end when GAME time said it was over (elapsed/t += dtUnscaled), but
// the mixer plays it on the AUDIO timeline (mixClock), which only advances when the device pulls
// the next block -- up to a buffer behind wall time. Removing the voice on game time therefore
// dropped whatever the mixer had not rendered yet: the last fraction of every ThinkPad line
// ("Baldi is cut off when he's about to finish a sentence"), since MathGameScript queues the next
// clip the moment isPlaying goes false. With the mixer live a voice now ends when its last sample
// has been mixed: mixClock >= start + clipLength * kMixRate / pitch. Headless and capture runs
// (mixer never pulled, mixClock stays 0) keep the game-time rule, so they are unchanged.
static bool voiceDoneOnMix(double startSamples, float lenSeconds, float rate) {
  const double now = (double)state().mixClock.load(std::memory_order_relaxed);
  return now - startSamples >= (double)lenSeconds * (double)kExpiryMixRate / (double)rate;
}

static void advanceAudio(float dtUnscaled) {
  State& g = state();
  const bool mixLive = g.mixClock.load(std::memory_order_relaxed) > 0;
  for (size_t i = 0; i < g.audioSources.size(); i++) {
    bb::AudioSource* src = g.audioSources[i];
    AudioSim& s = g.audioSim[src];
    if (s.paused) continue;
    const float rate = src->pitch > 0.0f ? src->pitch : 1.0f;
    if (s.playing && src->clip) {
      float len = clipLength(src->clip);
      if (len <= 0.0f) len = 1.0f;
      s.elapsed += dtUnscaled * rate;
      if (src->loop) { while (s.elapsed >= len) s.elapsed -= len; }
      else if (mixLive && s.mainStart >= 0.0) {
        if (voiceDoneOnMix(s.mainStart, len, rate)) { s.elapsed = 0.0f; s.playing = false; }
        else if (s.elapsed >= len) s.elapsed = len;       // the mixer still owes its tail
      }
      else if (s.elapsed >= len) { s.elapsed = 0.0f; s.playing = false; }
    }
    for (size_t j = 0; j < s.shots.size();) {
      s.shots[j].t += dtUnscaled * rate;
      const float slen = clipLength(s.shots[j].clip);
      const bool done = mixLive ? voiceDoneOnMix(s.shots[j].startSamples, slen, rate) : (s.shots[j].t >= slen);
      if (done) {
        s.shots[j] = s.shots.back();
        s.shots.pop_back();
      } else {
        j++;
      }
    }
  }
}

// ---- frame loop -------------------------------------------------------------
void beginFrame(float unscaledDt) {
  State& g = state();
  g.dtUnscaled = unscaledDt;
  g.dtScaled = unscaledDt * bb::Time::timeScale;
  bb::Time::frameCount++;
}

// Phase profile (shell --profile): wall-clock seconds accumulated per phase since the last
// read. Static rather than in State so it does not change State's layout.
static double s_prof[6] = { 0, 0, 0, 0, 0, 0 };
static double profNow() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void readFrameProfile(double out[6]) {
  for (int i = 0; i < 6; i++) { out[i] = s_prof[i]; s_prof[i] = 0.0; }
}

// Unity's physics step moves every DYNAMIC (non-kinematic) Rigidbody by its velocity after
// FixedUpdate and before the trigger callbacks. The BSODA spray (BsodaSparyScript sets
// rb.velocity = forward * 20 each frame) is the only dynamic body in the game; without this it hung
// where it was thrown and never pushed anyone. Its collider is a trigger, so nothing blocks it.
// Gravity is Unity's default -9.81 (useGravity is off on the spray); drag per PhysX: v *= 1/(1+drag*dt).
static void integrateRigidbodies(float dt) {
  State& g = state();
  for (size_t i = 0; i < g.rigidbodies.size(); i++) {
    bb::Rigidbody* rb = g.rigidbodies[i];
    bb::GameObject* go = rb->gameObject;
    if (rb->isKinematic || !go || !go->transform || !go->activeInHierarchy()) continue;
    if (rb->useGravity) rb->velocity.y -= 9.81f * dt;
    if (rb->drag > 0.0f) rb->velocity = rb->velocity * (1.0f / (1.0f + rb->drag * dt));
    go->transform->position += rb->velocity * dt;
  }
}

// Per-class script cost (BB_SCRIPTPROF=1): FixedUpdate / Update / LateUpdate seconds by dynamic
// type, printed every 500 frames -- which game scripts dominate a physics step.
static std::map<std::string, double> s_scriptCost[3];
static bool scriptProf() { static const bool on = getenv("BB_SCRIPTPROF") != 0; return on; }
static void scriptProfReport() {
  static int frames = 0;
  if (!scriptProf() || ++frames % 500 != 0) return;
  const char* names[3] = { "FixedUpdate", "Update", "LateUpdate" };
  for (int k = 0; k < 3; k++) {
    std::vector<std::pair<double, std::string> > v;
    for (std::map<std::string, double>::iterator it = s_scriptCost[k].begin(); it != s_scriptCost[k].end(); ++it)
      v.push_back(std::make_pair(it->second, it->first));
    std::sort(v.rbegin(), v.rend());
    printf("SCRIPTPROF %s per frame:", names[k]);
    for (size_t i = 0; i < v.size() && i < 8; i++) printf("  %s %.3fms", v[i].second.c_str(), v[i].first * 1000.0 / 500.0);
    printf("\n");
    s_scriptCost[k].clear();
  }
  fflush(stdout);
}

// State::componentVersion: the per-phase dispatch list (bit 1 Update, 2 FixedUpdate, 4 LateUpdate).
static std::vector<bb::Component*>& phaseList(int bit) {
  static std::vector<bb::Component*> lists[3];
  static size_t built[3] = { (size_t)-1, (size_t)-1, (size_t)-1 };
  const int k = bit == 1 ? 0 : bit == 2 ? 1 : 2;
  State& g = state();
  if (built[k] != g.componentVersion) {
    lists[k].clear();
    for (size_t i = 0; i < g.gos.size(); i++)
      for (size_t j = 0; j < g.gos[i]->components.size(); j++)
        if (!(g.gos[i]->components[j]->noOp & bit)) lists[k].push_back(g.gos[i]->components[j]);
    built[k] = g.componentVersion;
  }
  return lists[k];
}
// after a dispatch: components that just discovered they implement nothing drop out next time
static void phaseListPrune(int bit) {
  std::vector<bb::Component*>& l = phaseList(bit);
  size_t w = 0;
  for (size_t i = 0; i < l.size(); i++) if (!(l[i]->noOp & bit)) l[w++] = l[i];
  l.resize(w);
}

void runFixedSteps() {
  State& g = state();
  g.acc += g.dtScaled;
  int steps = 0;
  struct StepCount { State& g; int& n; ~StepCount() { g.fixedStepsLastFrame = n; } } stepCount = { g, steps };
  while (g.acc >= kFixedDt && steps < 8) { // 8-step spiral-of-death guard
    steps++;
    g.acc -= kFixedDt;
    g.dtContext = kFixedDt;
    g.doomClock += kFixedDt;                // Unity Time.time advances on fixed steps
    {
      const std::vector<bb::Component*> list = phaseList(2);   // copy: a handler may add objects
      for (size_t i = 0; i < list.size(); i++) {
        bb::Component* c = list[i];
        if (!c->gameObject || !c->gameObject->activeSelf) continue;
        if (c->enabled && !c->startPending && !(c->noOp & 2)) {
          if (scriptProf()) {
            const double t = profNow();
            c->FixedUpdate();
            s_scriptCost[0][typeid(*c).name()] += profNow() - t;
          } else {
            c->FixedUpdate();
          }
        }
      }
      phaseListPrune(2);
    }
    const double tA = profNow();
    integrateAgents(kFixedDt);
    integrateRigidbodies(kFixedDt);
    const double tB = profNow();
    // Unity dispatches trigger messages from the physics step, after motion is applied -- so this
    // sits at the end of the fixed step, downstream of FixedUpdate and the agent integration.
    runTriggerPhase();
    const double tC = profNow();
    s_prof[1] += tB - tA;
    s_prof[2] += tC - tB;
  }
}

// Unity runs Start just before an object's first Update, never at creation. Anything
// Instantiate()d since the last frame starts here, after its spawner had the chance to wire it up
// (see State::pendingStarts).
//
// Pops from the FRONT rather than indexing: a Start body may both append (it Instantiates more
// objects -- those start in this same drain, in creation order) and remove (it Destroys something
// still queued, which unnoteComponent erases). Index iteration would skip entries when the vector
// shifts under it. The queue is per-frame-small, so the erase cost is irrelevant.
// A component that is not READY (its object inactive in the hierarchy, or itself disabled) is
// NOT dropped: it stays queued, in order, and starts on the first frame it is ready -- Unity's
// rule. Dropping it (the old behaviour) meant anything inactive at load never got a Start at all.
// After each Start the scan restarts from the front: that Start may have destroyed a queued entry
// (unnoteComponent erases it, shifting indices) or instantiated new ones (appended).
void drainPendingStarts() {
  State& g = state();
  static const bool trace = getenv("ENG_TRACE_START") != 0;
  size_t i = 0;
  while (i < g.pendingStarts.size()) {
    bb::Component* c = g.pendingStarts[i];
    if (!c || !c->gameObject) { g.pendingStarts.erase(g.pendingStarts.begin() + i); continue; }
    if (!c->enabled || !c->gameObject->activeInHierarchy()) { i++; continue; }   // not yet: keep
    g.pendingStarts.erase(g.pendingStarts.begin() + i);
    c->startPending = false;
    if (trace) {
      printf("[start] go '%s' comp %s\n", c->gameObject->name.c_str(), typeid(*c).name());
      fflush(stdout);
    }
    c->Start();
    i = 0;
  }
}

void runUpdatePhase() {
  State& g = state();
  // Time.deltaTime is the SCALED delta, and it is 0 while Time.timeScale is 0 -- the ThinkPad
  // (GameControllerScript pins timeScale 0 while learningActive), the pause menu and the game-over
  // all rely on that to freeze the world. Substituting 0.02 for a zero delta kept every script that
  // moves by deltaTime running at full speed through the pad and the pause menu.
  g.dtContext = g.dtScaled;

  // TMP_InputField typed input (MathGame pad answer): Unity's TMP_InputField reads
  // Input.inputString during Update when it holds focus. The pad's answer field belongs to that
  // family: MainKeyboard digits, the minus key and backspace append/erase the draft. Return is
  // left to MathGameScript (it submits its own way) and Backspace previews nothing: TMP erases
  // the LAST CHARACTER.
  if (g.inputFocus != 0) {
    bb::InputField* f = g.inputFocus;
    static const bb::KeyCode mini[] = {
      bb::KeyCode::Alpha0, bb::KeyCode::Alpha1, bb::KeyCode::Alpha2, bb::KeyCode::Alpha3,
      bb::KeyCode::Alpha4, bb::KeyCode::Alpha5, bb::KeyCode::Alpha6, bb::KeyCode::Alpha7,
      bb::KeyCode::Alpha8, bb::KeyCode::Alpha9, bb::KeyCode::Minus,
    };
    for (size_t i = 0; i < sizeof(mini) / sizeof(mini[0]); i++) {
      const int slot = (int)mini[i];
      if (g.keyEdge[slot]) {
        g.keyEdge[slot] = false;
        if (mini[i] == bb::KeyCode::Minus) f->SetText(f->text + "-");
        else f->SetText(f->text + (char)('0' + (int)mini[i] - (int)bb::KeyCode::Alpha0));
      }
    }
    if (g.keyEdge[(int)bb::KeyCode::Backspace]) {
      g.keyEdge[(int)bb::KeyCode::Backspace] = false;
      if (!f->text.empty()) f->SetText(f->text.substr(0, f->text.size() - 1));
    }
  }

  drainPendingStarts();
  const size_t coroutineCount = g.coroutines.size(); // fresh starts got their sync tick already
  {
    const std::vector<bb::Component*> list = phaseList(1);
    for (size_t i = 0; i < list.size(); i++) {
      bb::Component* c = list[i];
      if (!c->gameObject || !c->gameObject->activeSelf) continue;
      if (c->enabled && !c->startPending && !(c->noOp & 1)) {
        if (scriptProf()) {
          const double t = profNow();
          c->Update(g.dtScaled);
          s_scriptCost[1][typeid(*c).name()] += profNow() - t;
        } else {
          c->Update(g.dtScaled);
        }
      }
    }
    phaseListPrune(1);
  }
  const size_t n = (std::min)(coroutineCount, g.coroutines.size());
  std::vector<bb::Coroutine*> batch(g.coroutines.begin(), g.coroutines.begin() + n);
  for (size_t i = 0; i < batch.size(); i++) {
    bb::Coroutine* c = batch[i];
    if (!c->tick(g.dtContext)) {
      delete c;
      for (size_t k = 0; k < g.coroutines.size(); k++) {
        if (g.coroutines[k] == c) { g.coroutines.erase(g.coroutines.begin() + k); break; }
      }
    }
  }
}

// ---- 3D audio spatial cache (main thread; the mix thread only reads these copies) ------------
// Unity attenuates every AudioSource by its "Spatial Blend" x distance-to-listener roll-off curve.
// Our bake carries that as spatial/rolloff/minDist/maxDist (AudioSource in eng.h); the listener is
// the main camera. runLateUpdatePhase-end is the last main-thread point before endFrame hands
// anything to the audio thread, so positions are one-frame fresh at worst.
// NOTE: the roll-off formulas (audioRolloffAtten) live further down, next to the audioPull mixer,
// so the definition sits beside its only caller there; refreshAudioSpatial above is the sole
// position-refresher. The definition is at file scope and the helper is static in this TU, so
// both sides see it via the forward declaration above.
static float audioRolloffAtten(const bb::AudioSource& s, float d) {
  const float min_ = s.minDist > 0.0f ? s.minDist : 1.0f;
  const float max_ = s.maxDist > min_ ? s.maxDist : min_ + 0.001f;
  if (d <= min_) return 1.0f;
  if (d >= max_) return 0.0f;   // Linear exact; Log's far tail in Unity also lands at ~0 there
  if (s.rolloff == 1 || s.rolloff == 2) {
    // Unity Linear rolloff: exact ramp 1 at min -> 0 at max.
    return 1.0f - (d - min_) / (max_ - min_);
  }
  // Unity Logarithmic rolloff (rolloff 0, the Unity default): 1/(1 + rolloffFactor*(d/min - 1)),
  // rolloffFactor 1 (this build serializes no factor; the classic game relies on the default).
  const float r = d / min_;
  return 1.0f / (1.0f + (r - 1.0f));
}

void refreshAudioSpatial() {
  State& g = state();
  bb::Camera* cam = bb::Camera::main();
  if (!cam || !cam->transform) {
    if (getenv("BB_AUDIODBG")) printf("AUDIODBG refresh: no camera, listener stays unset\n");
    return;   // keep listenerValid false: mixer passes at full volume
  }
  g.listenerPos = cam->transform->position;
  int cached3d = 0;
  for (size_t i = 0; i < g.audioSources.size(); i++) {
    bb::AudioSource* src = g.audioSources[i];
    // 2D sources (spatial == 0) never cache: audioPull only consults rollPos when spatial>0, and
    // an inactive-hierarchy 3D source stays at its last position (Unity attenuates the same way:
    // the voice keeps the last valid world position across the deactivation frame; our roll-off
    // only matters while a voice is actually live).
    if (!src || !src->transform || !src->spatial) continue;
    src->rollPos = src->transform->position;
    cached3d++;
  }
  g.listenerValid.store(1, std::memory_order_release);
  if (getenv("BB_AUDIODBG")) {
    printf("AUDIODBG refresh: listener=(%.0f,%.0f,%.0f) cached3d=%d of %d\n",
           g.listenerPos.x, g.listenerPos.y, g.listenerPos.z, cached3d, (int)g.audioSources.size());
    fflush(stdout);
  }
}

void runLateUpdatePhase() {
  State& g = state();
  // Time.deltaTime is the SCALED delta, and it is 0 while Time.timeScale is 0 -- the ThinkPad
  // (GameControllerScript pins timeScale 0 while learningActive), the pause menu and the game-over
  // all rely on that to freeze the world. Substituting 0.02 for a zero delta kept every script that
  // moves by deltaTime running at full speed through the pad and the pause menu.
  g.dtContext = g.dtScaled;
  {
    const std::vector<bb::Component*> list = phaseList(4);
    for (size_t i = 0; i < list.size(); i++) {
      bb::Component* c = list[i];
      if (!c->gameObject || !c->gameObject->activeSelf) continue;
      if (c->enabled && !c->startPending && !(c->noOp & 4)) {
        if (scriptProf()) {
          const double t = profNow();
          c->LateUpdate();
          s_scriptCost[2][typeid(*c).name()] += profNow() - t;
        } else {
          c->LateUpdate();
        }
      }
    }
    phaseListPrune(4);
  }
  refreshAudioSpatial();
  scriptProfReport();
}

void endFrame() {
  State& g = state();
  processPendingScenes(); // queued LoadScene swaps land here (Unity end-of-frame)
  purgeDoomed();
  advanceAudio(g.dtUnscaled);
}

// TRANSFORM HIERARCHY AT RUNTIME. The loader composes world transforms once; nothing moved a child
// when its parent moved afterwards. Every character's visible sprite is a CHILD of the object its
// NavMeshAgent / script moves (BaldiSprite under Baldi, PlaytimeSprite under Playtime, ... -- all
// eight in School.unity), and a notebook's art is the child _nbSprite of the root NotebookScript
// sinks to y=-20 when it is used. So the sprites stayed at their spawn points while the invisible
// colliders roamed, and a used notebook never vanished. Scripts here write WORLD fields directly
// (transform->position = ...), so Unity's setter-driven bookkeeping is rebuilt as a pass:
//   - a transform whose own localPosition was written (and world was not) takes its world position
//     from its parent: PickupAnimationScript bobs itemPosition->localPosition, which did nothing;
//   - a transform whose PARENT moved, and that was not itself written, follows rigidly -- its
//     pose relative to the parent is unchanged, exactly Unity's semantics;
//   - a direct world write wins, as Unity's position setter does.
// localPosition is then re-derived from the world pose so script READS of it are right too.
// RectTransforms are skipped: uGUI layout comes from their anchored fields, not these.
namespace {
bool vNear(const bb::Vec3& a, const bb::Vec3& b) {
  return fabsf(a.x - b.x) < 1e-5f && fabsf(a.y - b.y) < 1e-5f && fabsf(a.z - b.z) < 1e-5f;
}
bool qNear(const bb::Quat& a, const bb::Quat& b) {
  return fabsf(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) > 0.999999f;
}
float safeDiv(float a, float b) { return fabsf(b) > 1e-8f ? a / b : a; }
}  // namespace

namespace {
static float canvasScaleFactor(const bb::RectTransform* canvasRoot, float screenW, float screenH);  // below
}  // namespace

// UI POSITION WRITES. uGUI layout reads a RectTransform's anchored fields, so a script writing its
// position / localPosition did nothing. On a Screen Space Overlay canvas world units ARE screen
// pixels, so a world move maps to anchoredPosition += delta / canvas scale factor, and a local move
// (canvas units) maps one to one. The game's writers: MathGameScript.Start hides Baldi's pad face
// with baldiFeedTransform.position = (-1000,-1000) in spoop mode (it stayed visible), UiManager
// shifts a rect by Screen.height/8, and GameController's BootAnimation slides the Big Ol' Boots
// overlay through localPosition.
static size_t s_hierTouched = 0;   // slow-path transforms, summed (BB_HIERPROF)
static void propagateRectWrites(State& g) {
  // The RectTransforms among g.transforms, cached per registry generation (s_hierGen) like the
  // hierarchy order below: a dynamic_cast on every School transform, three times a frame, was a
  // real cost on ARM for the few hundred that are rects.
  static std::vector<bb::RectTransform*> rects;
  static unsigned int rectsFor = (unsigned int)-1;
  if (rectsFor != s_hierGen) {
    rects.clear();
    for (size_t i = 0; i < g.transforms.size(); i++)
      if (bb::RectTransform* r = dynamic_cast<bb::RectTransform*>(g.transforms[i])) rects.push_back(r);
    rectsFor = s_hierGen;
  }
  for (size_t i = 0; i < rects.size(); i++) {
    bb::RectTransform* r = rects[i];
    if (r->hierValid) {
      const bb::Vec3 dl = r->localPosition - r->hierLocal;
      const bb::Vec3 dw = r->position - r->hierPos;
      if (fabsf(dl.x) > 1e-5f || fabsf(dl.y) > 1e-5f) {
        r->anchoredPosition = bb::Vec2(r->anchoredPosition.x + dl.x, r->anchoredPosition.y + dl.y);
      } else if (fabsf(dw.x) > 1e-5f || fabsf(dw.y) > 1e-5f) {
        const bb::RectTransform* root = r;
        for (bb::Transform* t = r->parent; t; t = t->parent)
          if (bb::RectTransform* rr = dynamic_cast<bb::RectTransform*>(t)) root = rr;
        float csf = canvasScaleFactor(root, (float)g.screenW, (float)g.screenH);
        if (csf <= 0.0f) csf = 1.0f;
        r->anchoredPosition = bb::Vec2(r->anchoredPosition.x + dw.x / csf, r->anchoredPosition.y + dw.y / csf);
      }
    }
    r->hierPos = r->position;
    r->hierLocal = r->localPosition;
    r->hierValid = true;
  }
}

void propagateHierarchy() {
  State& g = state();
  propagateRectWrites(g);
  static std::vector<bb::Transform*> order;
  static std::vector<bb::Transform*> touched;   // transforms the slow path visited this pass
  touched.clear();
  static unsigned int builtFor = 0;
  if (builtFor != s_hierGen) {
    // parents before children: sort by depth (rebuilt only when the registry changes size)
    std::vector<std::pair<int, bb::Transform*> > d;
    d.reserve(g.transforms.size());
    for (size_t i = 0; i < g.transforms.size(); i++) {
      bb::Transform* t = g.transforms[i];
      if (!t || dynamic_cast<bb::RectTransform*>(t)) continue;
      int depth = 0;
      for (bb::Transform* p = t->parent; p; p = p->parent) depth++;
      d.push_back(std::make_pair(depth, t));
    }
    std::stable_sort(d.begin(), d.end(),
                     [](const std::pair<int, bb::Transform*>& a, const std::pair<int, bb::Transform*>& b) {
                       return a.first < b.first;
                     });
    order.clear();
    for (size_t i = 0; i < d.size(); i++) order.push_back(d[i].second);
    builtFor = s_hierGen;
  }
  for (size_t i = 0; i < order.size(); i++) {
    bb::Transform* t = order[i];
    bb::Transform* p = t->parent;
    if (!t->hierValid) {
      t->hierPos = t->position; t->hierRot = t->rotation; t->hierScale = t->scale;
      t->hierLocal = t->localPosition; t->hierValid = true; t->hierMoved = false;
      continue;
    }
    // FAST PATH: a pose BIT-IDENTICAL to its snapshot under a parent that did not move was not
    // written and has nothing to propagate (every tolerance test below would come out "unchanged"
    // anyway). The School's ~5200 static transforms took the full test three times a frame --
    // 1.0 ms, 57% of the game logic on a desktop CPU, far more on an iOS-6-class one.
    if (!(p && p->hierMoved) &&
        t->position.x == t->hierPos.x && t->position.y == t->hierPos.y && t->position.z == t->hierPos.z &&
        t->rotation.x == t->hierRot.x && t->rotation.y == t->hierRot.y && t->rotation.z == t->hierRot.z &&
        t->rotation.w == t->hierRot.w &&
        t->scale.x == t->hierScale.x && t->scale.y == t->hierScale.y && t->scale.z == t->hierScale.z &&
        t->localPosition.x == t->hierLocal.x && t->localPosition.y == t->hierLocal.y &&
        t->localPosition.z == t->hierLocal.z) {
      t->hierMoved = false;
      continue;
    }
    touched.push_back(t);
    // Position, rotation and scale are separate channels, as in Unity: every character sprite
    // carries a Billboard script that writes its ROTATION each frame, and that must not stop its
    // POSITION following the parent (it did, on the first version of this pass).
    const bool posWritten = !vNear(t->position, t->hierPos);
    const bool rotWritten = !qNear(t->rotation, t->hierRot);
    const bool sclWritten = !vNear(t->scale, t->hierScale);
    const bool localWritten = !vNear(t->localPosition, t->hierLocal);
    if (p && p->hierValid) {
      if (!posWritten && localWritten) {
        t->position = p->position + p->rotation.rotate(bb::compMul(p->scale, t->localPosition));
      } else if (p->hierMoved) {
        // pose relative to the parent's OLD pose, re-applied on its NEW pose, per channel
        const bb::Quat invOld = bb::conj(p->hierRot);
        if (!posWritten) {
          const bb::Vec3 rel = invOld.rotate(t->position - p->hierPos);
          const bb::Vec3 relS(safeDiv(rel.x, p->hierScale.x), safeDiv(rel.y, p->hierScale.y),
                              safeDiv(rel.z, p->hierScale.z));
          t->position = p->position + p->rotation.rotate(bb::compMul(p->scale, relS));
        }
        if (!rotWritten) t->rotation = p->rotation * (invOld * t->rotation);
        if (!sclWritten)
          t->scale = bb::Vec3(t->scale.x * safeDiv(p->scale.x, p->hierScale.x),
                              t->scale.y * safeDiv(p->scale.y, p->hierScale.y),
                              t->scale.z * safeDiv(p->scale.z, p->hierScale.z));
      }
    }
    t->hierMoved = !vNear(t->position, t->hierPos) || !qNear(t->rotation, t->hierRot) ||
                   !vNear(t->scale, t->hierScale);
    // keep localPosition truthful for script reads -- recomputed only when this transform, its
    // localPosition or its parent changed: the School's ~5200 static transforms used to pay a
    // quaternion rotate here three times a frame for a value that could not have changed.
    if (t->hierMoved || localWritten || (p && p->hierMoved)) {
      if (p) {
        const bb::Vec3 rel = bb::conj(p->rotation).rotate(t->position - p->position);
        t->localPosition = bb::Vec3(safeDiv(rel.x, p->scale.x), safeDiv(rel.y, p->scale.y), safeDiv(rel.z, p->scale.z));
      } else {
        t->localPosition = t->position;
      }
    }
  }
  // snapshot for the next pass (after the whole sweep, so every child saw its parent's move) --
  // only what the sweep touched: an untouched transform's snapshot already equals its pose
  s_hierTouched += touched.size();
  for (size_t i = 0; i < touched.size(); i++) {
    bb::Transform* t = touched[i];
    t->hierPos = t->position; t->hierRot = t->rotation; t->hierScale = t->scale;
    t->hierLocal = t->localPosition;
  }
}

AudioLock::AudioLock() { state().audioMutex.lock(); }
AudioLock::~AudioLock() { state().audioMutex.unlock(); }

// ---- Legacy UnityEngine.Input surface (InputManager.asset axes/buttons) ----------------------
// Most of the game reads input through Rewired (the game's InputManager re-dispatches the
// Rewired actions), but the 31718 easter-egg TestRoom scripts read the LEGACY Input surface
// directly: PlayerMovement (GetAxis "Forward"/"Strafe", GetButton "Run"), and AgentTest/
// ClickableTest/CameraScript_Simple around it. Nothing populated those axes, so the TestRoom
// player could look but not walk (user report). Unity ramps held keys with
// gravity/sensitivity; the port snaps to the held key (a sub-second ramp is invisible here).
// Keys are Unity's legacy axis definitions for this project: Forward = W/S, Strafe = D/A,
// Run = the shifts.
static void pumpLegacyAxes() {
  State& g = state();
  g.axes["Forward"] = (g.keyHeld[(int)bb::KeyCode::W] ? 1.0f : 0.0f) - (g.keyHeld[(int)bb::KeyCode::S] ? 1.0f : 0.0f);
  g.axes["Strafe"] = (g.keyHeld[(int)bb::KeyCode::D] ? 1.0f : 0.0f) - (g.keyHeld[(int)bb::KeyCode::A] ? 1.0f : 0.0f);
  g.namedButtons["Run"] = g.keyHeld[(int)bb::KeyCode::LeftShift];
}

void runFullFrame(float unscaledDt) {
  std::lock_guard<std::recursive_mutex> audioLock(state().audioMutex);   // see State::audioMutex
  const double t0 = profNow();
  beginFrame(unscaledDt);
  if (state().mobile) rewiredBeginFrame();   // touch controls -> Rewired actions, before scripts
  pumpLegacyAxes();   // legacy InputManager axes, fresh for both the fixed and update phases
  static double hierCost = 0.0;
  static int hierFrames = 0;
  runFixedSteps();
  double th = profNow();
  propagateHierarchy();
  hierCost += profNow() - th;
  const double t1 = profNow();
  runUpdatePhase();
  th = profNow();
  propagateHierarchy();
  hierCost += profNow() - th;
  const double t2 = profNow();
  runLateUpdatePhase();
  th = profNow();
  propagateHierarchy();
  hierCost += profNow() - th;
  const double t3 = profNow();
  if (++hierFrames == 500) {
    if (getenv("BB_HIERPROF")) { printf("HIERPROF propagateHierarchy x3 = %.3f ms per frame\n", hierCost * 2.0); printf("HIERPROF slow-path transforms per pass: %.1f\n", s_hierTouched / 1500.0); fflush(stdout); }
    s_hierTouched = 0;
    hierCost = 0.0;
    hierFrames = 0;
  }
  endFrame();
  const double t4 = profNow();
  s_prof[0] += t1 - t0;   // whole fixed phase (includes [1] agents and [2] triggers)
  s_prof[3] += t2 - t1;   // Update
  s_prof[4] += t3 - t2;   // LateUpdate
  s_prof[5] += t4 - t3;   // endFrame
}

// ---- engine services ---------------------------------------------------------
void setRandomSeed(unsigned int seed) {
  unsigned int x = seed ? seed : 1u;
  unsigned int* s = state().rng;
  for (int i = 0; i < 4; i++) { // splitmix32 expansion so low seeds still scramble state 0 ans 1
    x += 0x9E3779B9u;
    unsigned int z = x;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    s[i] = z ^ (z >> 15);
  }
}

// ---- debug switches (shell --debug) -------------------------------------------------------------
void setDebugFlags(int flags) { state().dbgFlags = flags; }
int debugFlags() { return state().dbgFlags; }

void registerClipGuid(const std::string& guid, const std::string& relPath) {
  State& g = state();
  if (g.clipByGuid.count(guid)) return;
  bb::AudioClip* clip = new bb::AudioClip();
  const size_t slash = relPath.find_last_of('/');
  clip->name = (slash == std::string::npos) ? relPath : relPath.substr(slash + 1);
  g.clipByGuid[guid] = clip;
  g.clipPath[clip] = relPath;
  noteRuntimeObject(clip);
}

bb::AudioClip* clipForGuid(const std::string& guid) {
  State& g = state();
  std::unordered_map<std::string, bb::AudioClip*>::iterator it = g.clipByGuid.find(guid);
  if (it != g.clipByGuid.end()) return it->second;
  // Resolve the guid to its real path under assetRoot so clipLength() can read the WAV header.
  // This used to pass `guid` as the path: nothing ever pre-registered clips with real paths, so
  // fopen always failed and EVERY clip in the game silently took parseWavDuration's 1.0s fallback.
  // An unknown guid still registers (keyed by guid, pathless) and keeps that documented fallback.
  const std::string rel = assetRelForGuid(guid);
  registerClipGuid(guid, rel.empty() ? guid : rel);
  return g.clipByGuid[guid];
}

bb::AudioClip* registerSceneClip(long long fid) { // scene-local clip node (rare serialization)
  State& g = state();
  const std::string key = "scene:" + std::to_string(fid);
  std::unordered_map<std::string, bb::AudioClip*>::iterator it = g.clipByGuid.find(key);
  if (it != g.clipByGuid.end()) return it->second;
  registerClipGuid(key, key); // no asset path: clipLength falls back to 1.0
  return g.clipByGuid[key];
}

static float parseWavDuration(const std::string& relPath) {
  State& g = state();
  const std::string full = g.assetRoot + relPath;
  FILE* f = fopenSafe(full.c_str(), "rb");
  if (!f) return 1.0f;
  unsigned char header[12];
  if (fread(header, 1, 12, f) != 12) { fclose(f); return 1.0f; }
  if (memcmp(header, "RIFF", 4) != 0) { fclose(f); return 1.0f; }
  int bytesPerSec = 0;
  long long dataSize = 0;
  for (;;) {
    unsigned char chunk[8];
    if (fread(chunk, 1, 8, f) != 8) break;
    const unsigned int len = (unsigned int)chunk[4] | ((unsigned int)chunk[5] << 8) |
                             ((unsigned int)chunk[6] << 16) | ((unsigned int)chunk[7] << 24);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      const unsigned int got = (unsigned int)fread(fmt, 1, sizeof(fmt), f);
      if (len > got && fseek(f, (long)(len - got), SEEK_CUR) != 0) break;
      if (got >= 12) {
        bytesPerSec = (int)fmt[8] | ((int)fmt[9] << 8) | ((int)fmt[10] << 16) | ((int)fmt[11] << 24);
      }
    } else if (memcmp(chunk, "data", 4) == 0) {
      dataSize = len; // data is the last chunk for these assets; duration is decided here
      break;
    } else {
      if (fseek(f, (long)len, SEEK_CUR) != 0) break;
    }
  }
  fclose(f);
  if (bytesPerSec > 0 && dataSize > 0) return (float)((double)dataSize / (double)bytesPerSec);
  return 1.0f;
}

float clipLength(bb::AudioClip* c) {
  State& g = state();
  if (!c) return 1.0f;
  std::unordered_map<bb::AudioClip*, float>::iterator it = g.clipLen.find(c);
  if (it != g.clipLen.end()) return it->second;
  std::unordered_map<bb::AudioClip*, std::string>::iterator p = g.clipPath.find(c);
  const float len = (p != g.clipPath.end()) ? parseWavDuration(p->second) : 1.0f;
  g.clipLen[c] = len;
  return len;
}



void destroyGameObjectNow(bb::GameObject* go) { destroyObjectNow(go); } // loader/scene-unload sweep

void engineShutdown() {
  State& g = state();
  // GameObjects own their comps + transforms; ownedObjects carry the engine-built nodes.
  // Unity: a UI object's transform IS its RectTransform (also a component) -- the components
  // sweep releases it once, the transform delete must skip the rect form.
  for (size_t i = 0; i < g.gos.size(); i++) {
    bb::GameObject* go = g.gos[i];
    const bool transformAlsoComponent =
        go->transform != 0 && dynamic_cast<bb::RectTransform*>(go->transform) != 0;
    for (size_t j = 0; j < go->components.size(); j++) {
      unnoteComponent(go->components[j]);
      delete go->components[j];
    }
    go->components.clear();
    if (!transformAlsoComponent) delete go->transform;
    delete go;
  }
  g.gos.clear();
  g.transforms.clear();
  g.childOf.clear();
  for (size_t i = 0; i < g.ownedObjects.size(); i++) delete g.ownedObjects[i];
  g.ownedObjects.clear();
  freeMeshAssets();        // shared mesh blobs: outside the scene registry, freed with the engine
  for (size_t i = 0; i < g.ownedActions.size(); i++) delete g.ownedActions[i];
  g.ownedActions.clear();
  for (size_t i = 0; i < g.coroutines.size(); i++) delete g.coroutines[i];
  g.coroutines.clear();
  g.boxColliders.clear();
  g.capsuleColliders.clear();
  g.meshColliders.clear();
  g.agents.clear();
  g.audioSources.clear();
  g.cameras.clear();
  g.agentSim.clear();
  g.audioSim.clear();
  g.moveSim.clear();
  g.animBools.clear();
  g.animTriggers.clear();
  g.inputFieldLabel.clear();
  g.inputFieldPlaceholder.clear();
  g.inputFocus = 0;
  g.doomed.clear();
}

// ---- platform shell plumbing -------------------------------------------------
void setScreenSize(int w, int h) { state().screenW = w; state().screenH = h; }
void setPrefsPath(const std::string& p) { state().prefsPath = p; state().prefsLoaded = false; }
void setAssetRoot(const std::string& root) { state().assetRoot = root; }

void injectKey(int keyCodeIndex, bool pressedEdge) {
  State& g = state();
  if (keyCodeIndex < 0 || keyCodeIndex >= bb::kKeySlots) return;
  if (pressedEdge) {
    g.keyEdge[keyCodeIndex] = true;
    g.keyHeld[keyCodeIndex] = true;   // held until injectKeyRelease / a future SDL up-event
    g.anyKeyFlag = true;
  }
}
void injectKeyRelease(int keyCodeIndex) {
  State& g = state();
  if (keyCodeIndex < 0 || keyCodeIndex >= bb::kKeySlots) return;
  g.keyHeld[keyCodeIndex] = false;    // no edge: Unity reports no GetKeyDown on release
}
void injectAxis(const char* axisName, float value) {
  if (!axisName) return;
  state().axes[axisName] = value;
}
void injectMouse(int button, bool pressedEdge) {
  State& g = state();
  if (button < 0 || button >= 8) return;
  if (pressedEdge) { g.mouseEdge[button] = true; g.mouseHeld[button] = true; g.anyKeyFlag = true; }
  // Unity parity bridge: KeyCode Mouse0..Mouse2 (slots 323..325) report the same state as
  // GetMouseButton(0..2) -- Input.GetKeyDown(Mouse0) is true on the press frame alongside
  // GetMouseButtonDown(0), each API consuming its own edge flag (separate storage, like
  // Unity's independent polls). InputManager::GetKeyState(Mouse0) (Interact = Mouse0 default
  // binding) reads this slot via Input::GetKey, so without it NotebookScript never fires.
  if (button < 3) {
    const int slot = (int)bb::KeyCode::Mouse0 + button;
    if (pressedEdge) { g.keyEdge[slot] = true; g.keyHeld[slot] = true; }
  }
}
void injectMouseRelease(int button) {
  State& g = state();
  if (button < 0 || button >= 8) return;
  g.mouseHeld[button] = false;        // no edge: Unity reports no GetMouseButtonDown on release
  if (button < 3) g.keyHeld[(int)bb::KeyCode::Mouse0 + button] = false;
}
void injectAnyKey() { state().anyKeyFlag = true; }

// ---------------------------------------------------------------------------
// uGUI overlay capture -- TMP labels recorded as screen-space glyph quads
// ---------------------------------------------------------------------------
// The headless draw list is the backend-agnostic render surface: a host (SDL/GL shell, iOS
// GLKit, or the sim's assertions) clears + builds + reads it once per rendered frame. Labels
// resolve against the ROOT canvas ancestor (Unity ScreenSpaceOverlay: the root rect covers the
// screen); anchor/sizeDelta/pivot math is Unity's exact RectTransform algebra (design space,
// y UP from the canvas bottom), with the final flip into row space (y DOWN, origin top-left)
// matching the atlas blob orientation so a backend blits straight onto a framebuffer.
//
// TMP text layout uses the baked face metrics: pointScale = fontSize / pointSize; per line,
// baseline = lineTop - ascender*pointScale; per glyph, quadTop = baseline + yOffset*scale
// (pixel-proven against atlas ink for the shipped face -- font-bake.js). Space characters
// advance the pen but emit no quad (their ink is empty; Unity generates vertices for them, the
// visible result is identical). Fonts without shipped atlas pixels emit no quads at all --
// advance/metrics stay correct, ink does not exist (COMIC / LiberationSans SDF labels).
void clearDrawList() { state().drawList.clear(); state().drawTextures.clear(); }

namespace {

// Guids that actually have baked pixels (data/tex/<guid>.raw, written by tools/bake-textures.js).
// A quad only claims textured=true when the blob exists, so a backend is never asked to bind an
// image that was never baked -- those stay solid tint fills, which is also what an unauthored
// (sprite-less) Image is.
static const std::set<std::string>& bakedTextureGuids() {
  static std::set<std::string> ids;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    bool ok = false;
    const std::string text = jsonReadFile("data/textures-index.json", ok);
    if (ok) {
      JsonValue root;
      std::string err;
      if (jsonParse(text, root, err) && root.type == JsonValue::Object) {
        for (size_t i = 0; i < root.obj.size(); i++) ids.insert(root.obj[i].first);
      }
    }
  }
  return ids;
}

// Design-space rect: origin at the canvas bottom-left, y UP (Unity RectTransform space).
struct RectD {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

static bool goHasComponent(const bb::GameObject* go, const bb::Canvas* unused) {
  (void)unused;
  for (size_t i = 0; i < go->components.size(); i++) {
    if (dynamic_cast<bb::Canvas*>(go->components[i]) != 0) return true;
  }
  return false;
}

// Collects the label's UI chain root-first. Unity UI invariant: EVERY node from the label up to
// AND INCLUDING the canvas ancestor is a RectTransform (mid-chain rects carry the anchor math);
// the first canvas-bearing ancestor is the overlay root -- Unity's ScreenSpaceOverlay covers the
// screen wherever the canvas sits in the OBJECT hierarchy (School's Hud/JumpRope/PauseMenu/
// Debugger/HighScore canvases all live mid-tree under plain 'UIElements'-style GO organizers).
// Those plain parents above the canvas gate ACTIVITY (Unity activeInHierarchy) but contribute no
// layout, which is why the layout chain stops at the canvas. (Simplification: nested-canvas
// trees -- a canvas below another canvas -- would need the walk to continue to the outermost
// canvas; the game ships none.)
// Unity paints Screen Space - Overlay canvases in Canvas.sortingOrder order, and only WITHIN a
// canvas by hierarchy. The draw key used to be hierarchy alone, so separate canvases interleaved
// by where their roots happened to sit: with the ThinkPad open (MathGame canvas sortingOrder 1)
// the School HUD (Hud canvas, sortingOrder 0) still drew its stamina bar, item slots and item
// label OVER the pad's full-screen black backdrop. The key now leads with the canvas order.
static int canvasSortingOrder(const bb::RectTransform* canvasRoot) {
  if (!canvasRoot || !canvasRoot->gameObject) return 0;
  const bb::GameObject* go = canvasRoot->gameObject;
  for (size_t i = 0; i < go->components.size(); i++)
    if (const bb::Canvas* cv = dynamic_cast<const bb::Canvas*>(go->components[i])) return cv->sortingOrder;
  return 0;
}

static bool labelRectChain(bb::Transform* leaf, std::vector<bb::RectTransform*>& chainOut) {
  chainOut.clear();
  std::vector<bb::Transform*> upward;
  for (bb::Transform* t = leaf; t != 0; t = t->parent) upward.push_back(t);
  if (upward.empty() || !upward[upward.size() - 1]->go) return false;
  for (size_t i = 0; i < upward.size(); i++) {
    if (!upward[i]->go || !upward[i]->go->activeSelf) return false; // active-in-hierarchy gate
  }
  // rect walk up to the canvas. Unity renders a Graphic whenever a Canvas is ANY ancestor; a plain
  // Transform in between is a zero-size rect at its own position. The only such nodes in this game
  // are the ThinkPad prefabs' `BG` (MathGame / MathGameNoKeypad), parent of the full-screen black
  // YCTPBG and the black BaldiFeedBG, and it is an IDENTITY node -- which lays out exactly as if it
  // were absent for children anchored at the centre of a centre-pivot canvas. Stopping the walk at
  // it dropped both panels, so the 3D world and the whole HUD showed around the pad. Identity plain
  // nodes are now passed through; a non-identity one still ends the walk (none exists to verify).
  size_t canvasIdx = upward.size();                            // index of the overlay root, once found
  for (size_t i = 0; i < upward.size(); i++) {
    if (dynamic_cast<bb::RectTransform*>(upward[i]) == 0) {
      const bb::Transform* t = upward[i];
      const bb::Transform* par = t->parent;
      const bool identity = par &&
          fabsf(t->localPosition.x) < 1e-4f && fabsf(t->localPosition.y) < 1e-4f &&
          fabsf(t->localPosition.z) < 1e-4f &&
          fabsf(t->scale.x - par->scale.x) < 1e-4f && fabsf(t->scale.y - par->scale.y) < 1e-4f &&
          fabsf(t->scale.z - par->scale.z) < 1e-4f &&
          fabsf(t->rotation.x * par->rotation.x + t->rotation.y * par->rotation.y +
                t->rotation.z * par->rotation.z + t->rotation.w * par->rotation.w) > 0.99999f;
      if (!identity) break;                                    // plain, offset node BELOW the canvas
      continue;
    }
    if (goHasComponent(upward[i]->go, (const bb::Canvas*)0)) {
      canvasIdx = i;                                           // first canvas ancestor = overlay root
      break;
    }
  }
  if (canvasIdx == upward.size()) return false;                // rects all the way up but no canvas
  // A DISABLED Canvas component draws nothing and takes no input (Unity), even with its GameObject
  // active. PlayerScript.KeepTheHudOff hides the HUD exactly this way (hud.enabled = false every
  // frame of the game over); ignoring the flag left the notebook count, stamina bar and item slots
  // on screen over the jumpscare.
  for (size_t ci = 0; ci < upward[canvasIdx]->go->components.size(); ci++)
    if (const bb::Canvas* cv = dynamic_cast<const bb::Canvas*>(upward[canvasIdx]->go->components[ci]))
      if (!cv->enabled) return false;
  for (size_t i = canvasIdx + 1; i-- > 0;) {                   // unrolled reverse: ROOT first,
    bb::RectTransform* r = dynamic_cast<bb::RectTransform*>(upward[i]);
    if (r) chainOut.push_back(r);                              // label LAST (compose order)
  }
  return true;
}

// Unity CanvasScaler: pixels per DESIGN unit for the canvas a chain hangs from. The canvas is laid
// out at screenSize/scaleFactor design units and rasterised at scaleFactor px per unit. Assuming
// the canvas IS the screen (scaleFactor 1) is only correct for ConstantPixelSize; School's Hud is
// ScaleWithScreenSize 640x480 Expand, which is 853.33x480 design units at 1280x720, so the whole
// HUD was laid out 1.5x too small and the boots overlay -- parked above a 480-tall canvas --
// peeked into the frame.
static float canvasScaleFactor(const bb::RectTransform* canvasRoot, float screenW, float screenH) {
  if (!canvasRoot || !canvasRoot->go) return 1.0f;
  const bb::CanvasScaler* cs = 0;
  for (size_t i = 0; i < canvasRoot->go->components.size(); i++) {
    cs = dynamic_cast<const bb::CanvasScaler*>(canvasRoot->go->components[i]);
    // a DISABLED scaler does nothing: the iOS touch canvas SimpleTouchDPI carries a CanvasScaler
    // (640x480, match width) AND a DpCanvasScaler with m_Enabled 0 -- only the enabled one scales.
    if (cs && !cs->enabled) cs = 0;
    if (cs) break;
  }
  if (const bb::DpCanvasScaler* dp = dynamic_cast<const bb::DpCanvasScaler*>(cs)) {
    const float dpi = state().screenDpi != 0.0f ? state().screenDpi : dp->fallbackScreenDPI;
    return dpi / 160.0f;
  }
  if (!cs || cs->uiScaleMode != 1) return (cs && cs->uiScaleMode == 0 && cs->scaleFactor > 0.0f)
                                            ? cs->scaleFactor : 1.0f;
  const float rw = cs->referenceResolution.x, rh = cs->referenceResolution.y;
  if (rw <= 0.0f || rh <= 0.0f) return 1.0f;
  const float sx = screenW / rw, sy = screenH / rh;
  if (cs->screenMatchMode == 1) return (sx < sy) ? sx : sy;          // Expand: show at least the ref
  if (cs->screenMatchMode == 2) return (sx > sy) ? sx : sy;          // Shrink
  // MatchWidthOrHeight: Unity lerps the LOG2 ratios, then exponentiates.
  const float m = (cs->matchWidthOrHeight < 0.0f) ? 0.0f
                : (cs->matchWidthOrHeight > 1.0f) ? 1.0f : cs->matchWidthOrHeight;
  const float lg = std::log(sx) / std::log(2.0f) * (1.0f - m) + std::log(sy) / std::log(2.0f) * m;
  return std::pow(2.0f, lg);
}

static bool traceDraw() {  // ENG_TRACE_DRAW=1: why each label does or does not reach the list
  static int v = -1;
  if (v < 0) {
    const char* e = std::getenv("ENG_TRACE_DRAW");
    v = (e && e[0] == '1') ? 1 : 0;
  }
  return v == 1;
}

// layRect: Unity's exact RectTransform algebra against the parent design rect.
//   size      = (anchorMax - anchorMin) * parentSize + sizeDelta
//   anchorRef = lerp(anchorMinPoint, anchorMaxPoint, pivot)     (yields parent-center offsets
//              for stretch anchors and point-anchoring for collapsed anchors -- both Unity modes)
//   rect.min  = anchorRef + anchoredPosition - pivot * size
// Unity VerticalLayoutGroup, LayoutGroup.SetChildrenAlongAxis, for children whose size the group
// does NOT control (m_ChildControlWidth/Height 0 -- the only configuration in this game). Children
// keep their sizeDelta; the group anchors them top-left and offsets them:
//   cross axis (x): required = Clamp(inner, childW, forceExpandW ? size : childW);
//                   x = padL + (size - padH - required) * alignH + (required - childW) * alignH
//   main axis (y):  each slot = childH (+ surplus share when force-expanded); a child sits at
//                   pos + (slot - childH) * alignV, pos += slot + spacing
// with alignH = (alignment % 3) * 0.5, alignV = (alignment / 3) * 0.5. Inactive children are
// skipped (Unity lays out only active ones). Returns false when r is not a laid-out child.
static bool verticalLayoutChild(const bb::RectTransform* group, const bb::RectTransform* r,
                                const RectD& pr, RectD& out) {
  std::vector<const bb::RectTransform*> kids;
  const std::vector<bb::Transform*>& trs = eng::state().transforms;
  for (size_t i = 0; i < trs.size(); i++) {
    if (trs[i] && trs[i]->parent == group) {
      const bb::RectTransform* k = dynamic_cast<const bb::RectTransform*>(trs[i]);
      if (k && k->go && k->go->activeSelf) kids.push_back(k);
    }
  }
  std::stable_sort(kids.begin(), kids.end(), [](const bb::RectTransform* a, const bb::RectTransform* b) {
    return a->siblingIndex < b->siblingIndex;
  });
  int idx = -1;
  for (size_t i = 0; i < kids.size(); i++) if (kids[i] == r) idx = (int)i;
  if (idx < 0) return false;
  const float alignH = (float)(group->childAlignment % 3) * 0.5f;
  const float alignV = (float)(group->childAlignment / 3) * 0.5f;
  // cross axis
  const float cw = r->sizeDelta.x, ch = r->sizeDelta.y;
  const float innerW = pr.w - group->padL - group->padR;
  const float maxW = group->forceExpandW ? pr.w : cw;
  float required = innerW;
  if (required < cw) required = cw; else if (required > maxW) required = maxW;   // Mathf.Clamp
  const float x = group->padL + (pr.w - group->padL - group->padR - required) * alignH + (required - cw) * alignH;
  // main axis
  float totalPref = group->padT + group->padB, totalFlex = 0.0f;
  for (size_t i = 0; i < kids.size(); i++) {
    totalPref += kids[i]->sizeDelta.y + (i ? group->spacing : 0.0f);
    if (group->forceExpandH) totalFlex += 1.0f;
  }
  const float surplus = pr.h - totalPref;
  float pos = group->padT, mult = 0.0f;
  if (surplus > 0.0f) {
    if (totalFlex == 0.0f) pos = group->padT + (pr.h - group->padT - group->padB - (totalPref - group->padT - group->padB)) * alignV;
    else mult = surplus / totalFlex;
  }
  for (int i = 0; i < idx; i++) pos += kids[i]->sizeDelta.y + (group->forceExpandH ? mult : 0.0f) + group->spacing;
  const float slot = ch + (group->forceExpandH ? mult : 0.0f);
  const float yFromTop = pos + (slot - ch) * alignV;
  out.w = cw;
  out.h = ch;
  out.x = pr.x + x;
  out.y = pr.y + pr.h - yFromTop - ch;
  return true;
}

static RectD layRect(const bb::RectTransform* r, const RectD& pr) {
  RectD out;
  if (const bb::RectTransform* grp = r->parent ? dynamic_cast<const bb::RectTransform*>(r->parent) : 0) {
    if (grp->verticalLayout && !grp->controlW && !grp->controlH && verticalLayoutChild(grp, r, pr, out)) return out;
  }
  out.w = (r->anchorMax.x - r->anchorMin.x) * pr.w + r->sizeDelta.x;
  out.h = (r->anchorMax.y - r->anchorMin.y) * pr.h + r->sizeDelta.y;
  const float refX = pr.x + pr.w * (r->anchorMin.x + r->pivot.x * (r->anchorMax.x - r->anchorMin.x));
  const float refY = pr.y + pr.h * (r->anchorMin.y + r->pivot.y * (r->anchorMax.y - r->anchorMin.y));
  out.x = refX + r->anchoredPosition.x - r->pivot.x * out.w;
  out.y = refY + r->anchoredPosition.y - r->pivot.y * out.h;
  return out;
}

// RectTransform SCALE, which layRect alone ignored. Unity scales a rect -- and everything under it --
// about its own pivot by its LOCAL scale (world scale / parent world scale here, since the loader
// stores composed world scales). Playtime's jump rope is the case that showed it: Rope is a 100x100
// rect at localScale (8, 6, 1), i.e. 800x600, drawn as a 100x100 square ("small and cropped").
// 16 UI objects in these scenes carry a non-unit scale (Story/Endless buttons + text 0.84, Back
// buttons 0.84/0.85, the slider Handle 1.5, ...). Rects are laid out in the UNSCALED frame (so
// children anchor exactly as before) and an accumulated per-axis map world = o + s * local is
// applied to the result. *outScale gets the accumulated vertical scale (labels scale their font).
static RectD layChainScaled(const std::vector<bb::RectTransform*>& chain, const RectD& root, float* outScale) {
  RectD loc = root;
  float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
  for (size_t i = 1; i < chain.size(); i++) {
    const bb::RectTransform* r = chain[i];
    loc = layRect(r, loc);
    const float lx = r->localScale.x, ly = r->localScale.y;   // chain[0] (the canvas) is excluded
    const float px = loc.x + r->pivot.x * loc.w;
    const float py = loc.y + r->pivot.y * loc.h;
    ox += sx * px * (1.0f - lx);
    oy += sy * py * (1.0f - ly);
    sx *= lx;
    sy *= ly;
  }
  RectD out;
  out.x = ox + sx * loc.x;
  out.y = oy + sy * loc.y;
  out.w = sx * loc.w;
  out.h = sy * loc.h;
  if (out.w < 0.0f) { out.x += out.w; out.w = -out.w; }   // negative scale mirrors; keep a positive rect
  if (out.h < 0.0f) { out.y += out.h; out.h = -out.h; }
  if (outScale) *outScale = fabsf(sy);
  return out;
}


// Pen advance of one character (baked advance * pointScale; TMP's missing-glyph fallback is a
// plain half-size step -- only ever hit for atlas-less or off-table codepoints).
static float charAdvance(const TMPFontData* font, int codepoint, float fontSize,
                         float charSpacing, float wordSpacing) {
  // TMP: the pen advances by the glyph advance PLUS m_characterSpacing, and a space additionally
  // by m_wordSpacing. Both are in font units like the advance itself, so they take the same
  // pointScale. Dropping them made every label with non-zero spacing render too loose.
  const TMPFontGlyph* gl = font ? font->glyphFor(codepoint) : 0;
  const float ps = font ? font->pointScaleFor(fontSize) : 1.0f;
  const float base = gl ? gl->adv * ps : fontSize * 0.5f;
  const float extra = (charSpacing + (codepoint == ' ' ? wordSpacing : 0.0f)) * ps;
  return base + extra;
}

} // namespace

// ---- UI pointer hit test ---------------------------------------------------------------------
// Unity routes a click through EventSystem -> GraphicRaycaster -> the Graphic under the pointer ->
// Selectable. We do not model that chain; what a MENU needs is "which Button is under the cursor",
// and the rect maths for that already exists in buildDrawList. Reusing layRect + canvasScaleFactor
// here keeps the hit test and the drawn rect IDENTICAL by construction -- a separate copy would
// drift and buttons would stop matching their own pixels.
//
// Row space (y DOWN from the top-left), same as DrawQuad and the same as a Win32 client click.
// Later entries win: buildDrawList emits in hierarchy order, so the LAST match is the topmost.
// Canvas.sortingOrder of the overlay canvas `t` draws under (false: not under an active canvas).
bool uiCanvasOrder(const bb::Transform* t, int& order) {
  std::vector<bb::RectTransform*> chain;
  if (!labelRectChain(const_cast<bb::Transform*>(t), chain)) return false;
  order = canvasSortingOrder(chain[0]);
  return true;
}

bool uiScreenRect(const bb::Transform* t, float& x0, float& y0, float& x1, float& y1) {
  State& g = state();
  const float screenW = (float)g.screenW, screenH = (float)g.screenH;
  std::vector<bb::RectTransform*> chain;
  if (!labelRectChain(const_cast<bb::Transform*>(t), chain)) return false;
  const float csf = canvasScaleFactor(chain[0], screenW, screenH);
  const float designH = screenH / csf;
  RectD rect;
  rect.w = screenW / csf;
  rect.h = designH;
  rect = layChainScaled(chain, rect, 0);
  if (rect.w <= 0.0f || rect.h <= 0.0f) return false;
  x0 = rect.x * csf;
  x1 = (rect.x + rect.w) * csf;
  y0 = (designH - (rect.y + rect.h)) * csf;
  y1 = (designH - rect.y) * csf;
  return true;
}

bb::Button* uiButtonAt(float px, float py) {
  State& g = state();
  const float screenW = (float)g.screenW, screenH = (float)g.screenH;
  bb::Button* hit = 0;
  for (size_t gi = 0; gi < g.gos.size(); gi++) {
    bb::GameObject* go = g.gos[gi];
    if (!go->activeInHierarchy()) continue;
    for (size_t ci = 0; ci < go->components.size(); ci++) {
      bb::Button* btn = dynamic_cast<bb::Button*>(go->components[ci]);
      if (!btn || !btn->enabled) continue;   // Selectable has no interactable flag in our model yet
      std::vector<bb::RectTransform*> chain;
      if (!labelRectChain(btn->transform, chain)) continue;   // not under an active canvas
      const float csf = canvasScaleFactor(chain[0], screenW, screenH);
      const float designH = screenH / csf;
      RectD rect;
      rect.w = screenW / csf;
      rect.h = designH;
      rect = layChainScaled(chain, rect, 0);   // RectTransform scale (see layChainScaled)
      if (rect.w <= 0.0f || rect.h <= 0.0f) continue;
      const float x0 = rect.x * csf, x1 = (rect.x + rect.w) * csf;
      const float y0 = (designH - (rect.y + rect.h)) * csf, y1 = (designH - rect.y) * csf;
      if (px >= x0 && px <= x1 && py >= y0 && py <= y1) hit = btn;
    }
  }
  return hit;
}

// Toggle rows in the options menu are NOT Buttons: Unity's Toggle handles its own press -> isOn
// flips and onValueChanged fires (UiSettings.UpdateState writes the pref). Same hit test and the
// same "later entry wins" layering rule as uiButtonAt, so a toggle under a button never misfires.
bb::Toggle* uiToggleAt(float px, float py) {
  State& g = state();
  const float screenW = (float)g.screenW, screenH = (float)g.screenH;
  bb::Toggle* hit = 0;
  for (size_t gi = 0; gi < g.gos.size(); gi++) {
    bb::GameObject* go = g.gos[gi];
    if (!go->activeInHierarchy()) continue;
    for (size_t ci = 0; ci < go->components.size(); ci++) {
      bb::Toggle* tg = dynamic_cast<bb::Toggle*>(go->components[ci]);
      if (!tg || !tg->enabled) continue;
      std::vector<bb::RectTransform*> chain;
      if (!labelRectChain(tg->transform, chain)) continue;   // not under an active canvas
      const float csf = canvasScaleFactor(chain[0], screenW, screenH);
      const float designH = screenH / csf;
      RectD rect;
      rect.w = screenW / csf;
      rect.h = designH;
      rect = layChainScaled(chain, rect, 0);   // RectTransform scale (see layChainScaled)
      if (rect.w <= 0.0f || rect.h <= 0.0f) continue;
      const float x0 = rect.x * csf, x1 = (rect.x + rect.w) * csf;
      const float y0 = (designH - (rect.y + rect.h)) * csf, y1 = (designH - rect.y) * csf;
      if (px >= x0 && px <= x1 && py >= y0 && py <= y1) hit = tg;
    }
  }
  return hit;
}

// Unity fires onClick on pointer UP over the same control; one call per click is what a menu needs,
// so the shell calls this on the press edge and we keep it that simple until something needs more.
// ---- uGUI Slider dragging -------------------------------------------------------------------
// Unity's Slider takes OnPointerDown + OnDrag: the value follows the pointer's position along the
// slider's "click rect" -- the PARENT of the handle (Handle Slide Area), else of the fill (Fill
// Area) -- normalised over that rect's extent on the slider's axis (Slider.UpdateDrag). The iOS
// options TurnSlider (0.1..10, OptionsManager.Update copies slider.value into MouseSensitivity
// every frame) could not be moved at all: touches only ever reached buttons.
static bool sliderTrackRect(bb::Slider* sl, float& x0, float& y0, float& x1, float& y1) {
  bb::Transform* track = sl->handleRect ? sl->handleRect->parent : sl->fillRect ? sl->fillRect->parent : 0;
  if (!track) track = sl->transform;
  return uiScreenRect(track, x0, y0, x1, y1) && x1 > x0 && y1 > y0;
}

static void sliderSetFromPointer(bb::Slider* sl, float px, float py) {
  float x0, y0, x1, y1;
  if (!sliderTrackRect(sl, x0, y0, x1, y1)) return;
  const int axis = sl->direction >= 2 ? 1 : 0;
  const bool reverse = sl->direction == 1 || sl->direction == 3;
  // screen rows run DOWN; a vertical slider's normalised value runs UP (Unity's y)
  float t = axis == 0 ? (px - x0) / (x1 - x0) : (y1 - py) / (y1 - y0);
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  if (reverse) t = 1.0f - t;
  float v = sl->minValue + t * (sl->maxValue - sl->minValue);
  if (sl->wholeNumbers) v = floorf(v + 0.5f);
  sl->value = v;
}

bool uiSliderDown(int pointer, float px, float py) {
  State& g = state();
  bb::Slider* hit = 0;
  for (size_t gi = 0; gi < g.gos.size(); gi++) {         // topmost = last in hierarchy order
    bb::GameObject* go = g.gos[gi];
    if (!go->activeInHierarchy()) continue;
    for (size_t ci = 0; ci < go->components.size(); ci++) {
      bb::Slider* sl = dynamic_cast<bb::Slider*>(go->components[ci]);
      if (!sl || !sl->enabled || !sl->interactable) continue;
      float x0, y0, x1, y1;
      if (uiScreenRect(sl->transform, x0, y0, x1, y1) && px >= x0 && px <= x1 && py >= y0 && py <= y1) hit = sl;
    }
  }
  if (!hit) return false;
  g.dragSlider = hit;
  g.dragPointer = pointer;
  sliderSetFromPointer(hit, px, py);
  return true;
}

void uiSliderMove(int pointer, float px, float py) {
  State& g = state();
  if (!g.dragSlider || g.dragPointer != pointer) return;
  if (!g.dragSlider->gameObject || !g.dragSlider->gameObject->activeInHierarchy()) { g.dragSlider = 0; return; }
  sliderSetFromPointer(g.dragSlider, px, py);
}

void uiSliderUp(int pointer) {
  State& g = state();
  if (g.dragPointer == pointer) { g.dragSlider = 0; g.dragPointer = -1; }
}

bool uiClick(float px, float py) {
  if (bb::Button* btn = uiButtonAt(px, py)) {
    // Unity: a non-interactable Selectable still takes the raycast but does not fire onClick
    // (ControlsMenu.SetButtonsInteractable(false) while a rebind/confirm panel is up).
    if (btn->interactable) btn->onClick.Invoke();
    return true;
  }
  // Options menu radio rows: press -> flip -> onValueChanged (Unity's Toggle handles its own
  // press, there is no separate Button under it in these scenes).
  if (bb::Toggle* tg = uiToggleAt(px, py)) {
    tg->isOn = !tg->isOn;
    tg->onValueChanged.Invoke();
    return true;
  }
  return false;
}

// Per-frame hover dispatch: Unity's EventSystem raises IPointerEnterHandler/IPointerExitHandler on
// the components of the object under the pointer as it crosses controls. We hit-test the Button
// the same way uiClick does, keep the hovered pointer in State and fire the game-layer seam only
// on the enter/exit EDGES. Locked cursor (in-game play) can never rest on a control, and asking
// for hover then would underline from a stale play position, so it is skipped while invisible.
void applySelectableState(bb::Selectable* s, bool highlighted) {
  if (!s) return;
  if (s->transition == 3) {            // Animation: Unity resets the other state triggers, sets one
    if (bb::Animator* an = s->GetComponent<bb::Animator>()) {
      std::map<std::string, bool>& trig = state().animTriggers[an];
      trig.erase(s->normalTrigger);
      trig.erase(s->highlightedTrigger);
      trig.erase("Pressed");
      trig.erase("Disabled");
      an->SetTrigger(highlighted ? s->highlightedTrigger.c_str() : s->normalTrigger.c_str());
    }
    return;
  }
  if (!s->targetImage) return;
  if (s->transition == 1) {            // ColorTint: CanvasRenderer colour = state colour x multiplier
    const float* c = highlighted ? s->highlightedColor : s->normalColor;
    const float m = s->colorMultiplier;
    s->targetImage->tint = bb::Color(c[0] * m, c[1] * m, c[2] * m, c[3] * m);
  } else if (s->transition == 2) {     // SpriteSwap: overrideSprite, cleared again for Normal
    s->targetImage->overrideSprite = highlighted ? s->highlightedSprite : 0;
  }
}

void uiPointerMove(float px, float py) {
  State& g = state();
  g.uiPointerX = px;                 // the -debug menu's hover/click hit test reads these
  g.uiPointerY = py;
  if (!bb::Cursor::visible) {
    if (g.hoveredButton) {
      dispatchPointerEvent(g.hoveredButton->gameObject, false);
      applySelectableState(g.hoveredButton, false);
      g.hoveredButton = 0;
    }
    return;
  }
  bb::Button* btn = uiButtonAt(px, py);
  static int uiHoverDbg = -1;
  if (uiHoverDbg < 0) {
    const char* e = std::getenv("BB_UIHDBG");
    uiHoverDbg = e && *e == '1' ? 1 : 0;
  }
  if (uiHoverDbg) {
    printf("[uihover] move (%.0f,%.0f) -> %s\n", px, py,
           btn && btn->gameObject ? btn->gameObject->name.c_str() : "(none)");
  }
  if (btn != g.hoveredButton) {
    if (g.hoveredButton) {
      dispatchPointerEvent(g.hoveredButton->gameObject, false);
      applySelectableState(g.hoveredButton, false);
    }
    g.hoveredButton = btn;
    if (btn) {
      dispatchPointerEvent(btn->gameObject, true);
      if (btn->interactable) applySelectableState(btn, true);   // no Highlighted while disabled
    }
  }
}

// ---- -debug cheat menu (the /-toggled bottom-left panel) -------------------------------------
// GameControllerScript calls bb::debugUiFrame every frame of a story/endless run while the host
// turned on the debug UI; this module draws the bottom-left hint + the panel and routes clicks.
// Rendering is plain DrawQuads APPENDED to the canvas pass output (after the last sorted entry)
// so the menu always paints over everything, including panels: it is a debug overlay, not a
// Unity-style node. Text shapes with the COMIC_Pro bake (the game's own in-game face, always in
// data/fonts-index.json), the same glyph math as the canvas TMP pass, expressed directly in row
// space: glyph top edge y0 = baselineRow - yo*pointScale, height h*pointScale down.
static const char* kDebugUiFontGuid = "21749360ea10093439a1f9266c91c718";  // COMIC_Pro

static TMPFontData* debugUiFont() {
  return fontForGuid(kDebugUiFontGuid);
}

static float debugUiTextWidth(const char* text, float size) {
  TMPFontData* font = debugUiFont();
  if (!font || !text) return 0.0f;
  const float ps = font->pointScaleFor(size);
  float w = 0.0f;
  for (const char* p = text; *p; p++) {
    const TMPFontGlyph* gl = font->glyphFor((int)(unsigned char)*p);
    w += gl ? gl->adv * ps : size * 0.5f;
  }
  return w;
}

static void debugUiPushText(float baselineRow, float penX, float size, const bb::Color& c,
                            const char* text) {
  State& g = state();
  TMPFontData* font = debugUiFont();
  if (!font || !text) return;
  const float ps = font->pointScaleFor(size);
  for (const char* p = text; *p; p++) {
    const TMPFontGlyph* gl = font->glyphFor((int)(unsigned char)*p);
    if (!gl) { penX += size * 0.5f; continue; }
    if (*p != ' ') {
      DrawQuad q;
      q.x0 = penX + gl->xo * ps;
      q.y0 = baselineRow - gl->yo * ps;
      q.x1 = q.x0 + gl->w * ps;
      q.y1 = q.y0 + gl->h * ps;
      q.r = c.r; q.g = c.g; q.b = c.b; q.a = c.a;
      q.u0 = gl->u0; q.v0 = gl->v0; q.u1 = gl->u1; q.v1 = gl->v1;
      q.canvasLayer = 0;
      q.textured = true;
      q.font = font;
      g.drawList.push_back(q);
    }
    penX += gl->adv * ps;
  }
}

static void debugUiMiniQuad(float x0, float y0, float x1, float y1, const bb::Color& c) {
  State& g = state();
  DrawQuad q;
  q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
  q.r = c.r; q.g = c.g; q.b = c.b; q.a = c.a;
  q.canvasLayer = 0;
  q.textured = false;                      // solid fill: the menu's panel/hover/chips
  g.drawList.push_back(q);
}

// ENGINE-SIDE impls of the bb::debug* API (eng.h). The bb:: functions that game scripts call live
// next to bb::setAmbientLight at the bottom of this file -- every other engine API for game code
// follows the same split.
void debugUiFrameImpl(bb::DebugUiSink* sink, bool open) {
  State& g = state();
  g.debugUiSink = sink;                    // buildDrawList draws from this later in-frame
  g.debugUiOpen = open;                    // also arms the game-side mouse swallow
  g.dbgHoverRow = -1;

  // Click routing happens HERE (in the game's Update), hit-testing against the PREVIOUS build's
  // row rects: rows are fixed-size and static-labelled, so the one-frame lag is invisible. The
  // raw edge is read (not Input::GetMouseButtonDown) because that API suppresses game reads
  // while open -- but any edge the menu consumes must be cleared so nothing replays it later.
  if (open && sink && g.mouseEdge[0]) {
    g.mouseEdge[0] = false;
    const float mx = g.uiPointerX, my = g.uiPointerY;
    for (int i = 0; i < g.dbgRowCount && i < State::kDebugUiMaxRows; i++) {
      if (mx >= g.dbgRowX[i] && mx <= g.dbgRowX[i] + g.dbgRowW[i] &&
          my >= g.dbgRowY[i] && my <= g.dbgRowY[i] + g.dbgRowH[i]) {
        sink->debugUiActivate(i);
        break;
      }
    }
  }
  // Hover highlight index for this frame (buildDrawList uses it to fill the row).
  if (open && sink) {
    const float mx = g.uiPointerX, my = g.uiPointerY;
    for (int i = 0; i < g.dbgRowCount && i < State::kDebugUiMaxRows; i++) {
      if (mx >= g.dbgRowX[i] && mx <= g.dbgRowX[i] + g.dbgRowW[i] &&
          my >= g.dbgRowY[i] && my <= g.dbgRowY[i] + g.dbgRowH[i]) {
        g.dbgHoverRow = i;
        break;
      }
    }
  }
}

// (bb::debugUiAvailable / bb::cheatNpcIgnore / bb::setCheatNpcIgnore live in the bb section at
// the bottom of this file; they only need State flags, so their bodies are one-liners there.)
// buildDrawList's tail (called from buildDrawList after the canvas emission): lays out the hint
// and, when open, the panel -- bottom-left, over everything.
static void buildDebugUiOverlay() {
  State& g = state();
  if (!(debugFlags() & kDebugUi) || !g.debugUiSink) return;   // -debug off, or not in game
  const float sh = (float)g.screenH;

  // The HINT is always there in-game so the / shortcut is discoverable ("debug menu - / opens
  // it" / "debug menu - / closes it" once opened). A dark backing strip keeps it readable over
  // any floor texture (the pause HUD's trick, at one line's height).
  const float hintSize = 16.0f, hintBaseline = sh - 16.0f;
  const char* hint = g.debugUiOpen ? "- / closes it" : "- / opens it";
  const float hintW = debugUiTextWidth("debug menu", hintSize) + 7.0f + debugUiTextWidth(hint, hintSize);
  debugUiMiniQuad(10.0f, sh - 34.0f, 14.0f + hintW + 10.0f, sh - 4.0f, bb::Color(0.0f, 0.0f, 0.0f, 0.55f));
  debugUiPushText(hintBaseline, 14.0f, hintSize, bb::Color(1.0f, 1.0f, 1.0f, 0.95f), "debug menu");
  debugUiPushText(hintBaseline, 14.0f + debugUiTextWidth("debug menu", hintSize) + 7.0f,
                  hintSize, bb::Color(1.0f, 0.85f, 0.4f, 0.95f), hint);
  if (!g.debugUiOpen) { g.dbgRowCount = 0; return; }

  // The PANEL. Fixed geometry (no per-frame meat): title bar + one 26px row per cheat, anchored
  // above the hint on the left edge. Rows re-fill the hit-rect table every build.
  const int rows = g.debugUiSink->debugUiRows();
  const float rowH = 26.0f, titleH = 30.0f, pad = 6.0f;
  const float panelW = 310.0f, panelX = 12.0f;
  const float panelH = titleH + rows * rowH + pad * 2.0f;
  const float panelY = sh - 44.0f - panelH;      // anchors above the hint, grows upward
  debugUiMiniQuad(panelX, panelY, panelX + panelW, panelY + panelH, bb::Color(0.0f, 0.0f, 0.0f, 0.68f));
  debugUiPushText(panelY + 23.0f, panelX + pad + 2.0f, 19.0f,
                  bb::Color(1.0f, 0.9f, 0.3f, 1.0f), "DEBUG MENU");
  int stored = 0;
  for (int i = 0; i < rows && i < State::kDebugUiMaxRows; i++) {
    bool toggle = false, on = false;
    const char* label = g.debugUiSink->debugUiRow(i, &toggle, &on);
    if (!label) continue;
    const float rx = panelX + pad, ry = panelY + titleH + pad + (float)i * rowH;
    g.dbgRowX[i] = rx; g.dbgRowY[i] = ry; g.dbgRowW[i] = panelW - pad * 2.0f; g.dbgRowH[i] = rowH;
    stored = i + 1;
    if (i == g.dbgHoverRow)
      debugUiMiniQuad(rx, ry, rx + panelW - pad * 2.0f, ry + rowH, bb::Color(1.0f, 1.0f, 1.0f, 0.18f));
    debugUiPushText(ry + 20.0f, rx + 8.0f, 18.0f, bb::Color(1.0f, 1.0f, 1.0f, 1.0f), label);
    if (toggle) {
      const char* chip = on ? "[ X ]" : "[   ]";
      debugUiPushText(ry + 20.0f, rx + panelW - pad * 2.0f - 8.0f - debugUiTextWidth(chip, 18.0f),
                      18.0f, on ? bb::Color(0.4f, 1.0f, 0.45f, 1.0f) : bb::Color(1.0f, 1.0f, 1.0f, 0.55f),
                      chip);
    }
  }
  g.dbgRowCount = stored;                 // hit table refreshed; stale old rows drop out
}

// the full walks used), cached per State::componentVersion. Three full walks over every School
// component with dynamic_casts, every frame, found the HUD's few dozen UI elements.
struct UiLists {
  size_t version = (size_t)-1;
  std::vector<bb::Slider*> sliders;
  std::vector<bb::Component*> graphics;     // Image / RawImage
  std::vector<EngineUiText*> labels;
};
static const UiLists& uiLists() {
  static UiLists u;
  State& g = state();
  if (u.version != g.componentVersion) {
    u.sliders.clear(); u.graphics.clear(); u.labels.clear();
    for (size_t gi = 0; gi < g.gos.size(); gi++)
      for (size_t ci = 0; ci < g.gos[gi]->components.size(); ci++) {
        bb::Component* c = g.gos[gi]->components[ci];
        if (bb::Slider* sl = dynamic_cast<bb::Slider*>(c)) u.sliders.push_back(sl);
        if (dynamic_cast<bb::Image*>(c) || dynamic_cast<bb::RawImage*>(c)) u.graphics.push_back(c);
        if (EngineUiText* t = dynamic_cast<EngineUiText*>(c)) u.labels.push_back(t);
      }
    u.version = g.componentVersion;
  }
  return u;
}

void buildDrawList() {
  State& g = state();
  const UiLists& ul = uiLists();
  g.drawList.clear();
  const float screenW = (float)g.screenW;
  const float screenH = (float)g.screenH;

  // Unity's Slider DRIVES its fillRect before the canvas is drawn: the fill's anchors come from
  // the normalised value, which is why the bake stores that rect with collapsed anchors and
  // sizeDelta (0,0). Without this the stamina bar is its empty red track and nothing else -- the
  // green fill is a zero-width quad. Horizontal left-to-right is the only direction this game
  // authors (PlayerScript writes staminaBar.value every frame, 0..100).
  for (size_t ui = 0; ui < ul.sliders.size(); ui++) {
    {
      bb::Slider* sl = ul.sliders[ui];
      if (!sl || (!sl->fillRect && !sl->handleRect)) continue;
      // Unity's Slider.UpdateVisuals writes ONLY the anchors. anchoredPosition, sizeDelta and
      // pivot stay exactly as authored -- and they matter: this slider's Fill Area carries
      // anchoredPosition (+5,0) and its Fill carries (-5,0), which cancel. Zeroing the Fill's
      // anchoredPosition (as a first attempt did) destroys that cancellation and slides the green
      // fill 5 design units -- 10px at this canvas scale -- off the red track.
      // Slider.UpdateVisuals for every direction: axis 0 = horizontal (0 LeftToRight, 1 Right-
      // ToLeft), 1 = vertical (2 BottomToTop, 3 TopToBottom); reversed directions grow from the
      // far end. The handle sits at the value along the same axis (the options TurnSlider's knob:
      // m_HandleRect, anchors 0.1919 = (2 - 0.1) / (10 - 0.1) as authored).
      const float t = sl->normalized();
      const int axis = sl->direction >= 2 ? 1 : 0;
      const bool reverse = sl->direction == 1 || sl->direction == 3;
      if (sl->fillRect) {
        bb::Vec2 mn(0.0f, 0.0f), mx(1.0f, 1.0f);
        if (reverse) { if (axis == 0) mn.x = 1.0f - t; else mn.y = 1.0f - t; }
        else { if (axis == 0) mx.x = t; else mx.y = t; }
        sl->fillRect->anchorMin = mn;
        sl->fillRect->anchorMax = mx;
      }
      if (sl->handleRect) {
        const float at = reverse ? 1.0f - t : t;
        bb::Vec2 mn(0.0f, 0.0f), mx(1.0f, 1.0f);
        if (axis == 0) { mn.x = mx.x = at; } else { mn.y = mx.y = at; }
        sl->handleRect->anchorMin = mn;
        sl->handleRect->anchorMax = mx;
      }
    }
  }

  // BOTH passes collect into this, keyed by hierarchy position, and it is sorted and emitted once
  // at the very end. Unity paints a canvas depth-first by sibling index; these walks are in g.gos
  // (creation) order, and a label is a separate pass only because glyph layout needs its own code
  // -- not because Unity draws all text last.
  std::vector<std::pair<std::vector<int>, DrawQuad> > uiSorted;

  // ---- pass 1: uGUI GRAPHICS (Image / RawImage) --------------------------------------------
  // Unity draws a Canvas in hierarchy order, so a panel's child label paints over its parent
  // panel. Emitting every graphic before every label reproduces that for the shape the game
  // actually uses (labels parented under their panel) without a full depth-first walk.
  //
  // These are SOLID quads (textured = false): Image/RawImage carry a bound sprite/texture node,
  // but those nodes are identity-only -- no pixel decoding exists yet -- so the authored tint is
  // all the colour information there is. Once texture pixels are baked, fill in the UVs here and
  // flip textured on; the rect math below does not change.
  for (size_t ui = 0; ui < ul.graphics.size(); ui++) {
    bb::GameObject* go = ul.graphics[ui]->gameObject;
    (void)go;
    {
      bb::Component* c = ul.graphics[ui];
      bb::Image* im = dynamic_cast<bb::Image*>(c);
      bb::RawImage* ri = im ? 0 : dynamic_cast<bb::RawImage*>(c);
      if (!im && !ri) continue;
      if (!c->enabled) continue;
      const bb::Color col = im ? bb::Color(im->color.r * im->tint.r, im->color.g * im->tint.g,
                                           im->color.b * im->tint.b, im->color.a * im->tint.a)
                               : ri->color;    // Image: colour x CanvasRenderer tint (Color Tint)
      if (col.a <= 0.0f) continue;                  // fully transparent graphic draws nothing
      std::vector<bb::RectTransform*> chain;
      if (!labelRectChain(c->transform, chain)) continue;  // not under an active overlay canvas
      // Design space: the canvas is screen/scaleFactor units across, and every coordinate below
      // is multiplied back up by scaleFactor at emit.
      const float csf = canvasScaleFactor(chain[0], screenW, screenH);
      const float designH = screenH / csf;
      RectD rect;
      rect.w = screenW / csf;
      rect.h = designH;
      rect = layChainScaled(chain, rect, 0);   // RectTransform scale (see layChainScaled)
      if (rect.w <= 0.0f || rect.h <= 0.0f) continue;
      DrawQuad q;
      // Hierarchy sort key: the sibling index of every rect from the canvas root down. Unity
      // paints a canvas depth-first by sibling index; this list is built in g.gos (creation)
      // order, so without the sort MainMenu's Background (rootOrder 0) emitted LAST and covered
      // the title and every button. Comparing the key lexicographically reproduces depth-first.
      std::vector<int> key;
      key.reserve(chain.size() + 1);
      key.push_back(canvasSortingOrder(chain[0]));   // CANVAS ORDER FIRST, then hierarchy
      for (size_t ki = 0; ki < chain.size(); ki++) {
        key.push_back(chain[ki] ? chain[ki]->siblingIndex : 0);
      }
      // FLIP into row space, exactly as the label pass does at its own emit site. layRect is
      // Unity's algebra and therefore yields a DESIGN rect: y UP from the canvas bottom. DrawQuad
      // is row space: y DOWN from the top-left. Emitting the design y directly mirrors every
      // graphic about the screen's horizontal centre line.
      //
      // This was invisible for two sessions because the HUD this game authors is nearly
      // symmetric: the reticle is 2x2 at dead centre, and everything else is anchored in a corner
      // where "wrong corner" still looks like a HUD. What exposed it was the BOOTS overlay --
      // parked at anchoredPosition.y = +375 (above the screen, waiting for BootAnimation to slide
      // it in) and mirrored to +735, so it sat half-visible at the bottom of every frame. The
      // confirming case is ItemSlots: it is the background FOR the "Nothing" item label, yet the
      // label (flipped, correct) drew top-right while its own background drew bottom-right.
      q.x0 = rect.x * csf;                     q.y0 = (designH - (rect.y + rect.h)) * csf;
      q.x1 = (rect.x + rect.w) * csf;          q.y1 = (designH - rect.y) * csf;
      q.r = col.r; q.g = col.g; q.b = col.b; q.a = col.a;
      // A bound Sprite/Texture node carries its asset guid as its name. If that guid was baked
      // (data/tex/<guid>.raw), the quad samples the whole image; otherwise it stays a solid tint
      // fill -- which is also exactly what a sprite-less Image is in Unity.
      const bb::Object* src = im ? (const bb::Object*)(im->overrideSprite ? im->overrideSprite : im->sprite)
                                 : (const bb::Object*)ri->texture;   // overrideSprite wins (Sprite Swap)
      const std::string guid = src ? src->name : std::string();
      q.u0 = q.v0 = 0.0f; q.u1 = q.v1 = 0.0f;       // no sample; solid fill
      q.textured = false;
      q.texId = -1;
      if (!guid.empty() && bakedTextureGuids().count(guid)) {
        int id = -1;
        for (size_t t = 0; t < g.drawTextures.size(); t++) {
          if (g.drawTextures[t] == guid) { id = (int)t; break; }
        }
        if (id < 0) { id = (int)g.drawTextures.size(); g.drawTextures.push_back(guid); }
        q.texId = id;
        q.u0 = 0.0f; q.v0 = 0.0f; q.u1 = 1.0f; q.v1 = 1.0f;   // whole image
        q.textured = true;
      }
      uiSorted.push_back(std::make_pair(key, q));         // emitted in hierarchy order at the end
      if (traceDraw()) {
        printf("  draw: graphic '%s' %s rect %.1f,%.1f %.1fx%.1f rgba %.2f,%.2f,%.2f,%.2f\n",
               go->name.c_str(), im ? "Image" : "RawImage", rect.x, rect.y, rect.w, rect.h,
               col.r, col.g, col.b, col.a);
      }
    }
  }

  for (size_t li = 0; li < ul.labels.size(); li++) {
    bb::GameObject* go = ul.labels[li]->gameObject;
    {
      EngineUiText* ui = ul.labels[li];
      if (ui == 0 || !ui->enabled || ui->text.empty()) continue;
      if (!ui->font || ui->font->rgba.empty()) continue;   // no atlas pixels -> no ink to draw
      std::vector<bb::RectTransform*> chain;
      if (!labelRectChain(ui->transform, chain)) {
        if (traceDraw()) {
          printf("  draw: label '%s' (text '%s', font %s) REJECTED: ", go->name.c_str(),
                 ui->text.c_str(), ui->font->name.c_str());
          int nA = 0;
          std::string chainDbg;
          for (bb::Transform* t = ui->transform; t != 0 && nA < 12; t = t->parent, nA++) {
            chainDbg += std::string(" <- ") + (t->go ? t->go->name : std::string("{null go}")) +
                        (t->go ? (t->go->activeSelf ? "[ok]" : "[off]") : "");
          }
          printf("no canvas root (%d ancestors)%s\n", nA, chainDbg.c_str());
        }
        continue; // not under an active overlay canvas
      }
      if (traceDraw()) {
        printf("  draw: label '%s' text '%s' -> %d chain nodes\n", go->name.c_str(),
               ui->text.c_str(), (int)chain.size());
      }

      // resolve the label rect from the (screen-covering) canvas root down
      // Canvas root in DESIGN units (see canvasScaleFactor); glyph geometry below is laid out in
      // those units and multiplied by csf at emit, so the text scales with the canvas exactly as
      // its background graphic does.
      const float csf = canvasScaleFactor(chain[0], screenW, screenH);
      const float designH = screenH / csf;
      // Same hierarchy key the graphics pass builds: every glyph of this label carries it, and the
      // stable sort keeps them in pen order within the label.
      std::vector<int> key;
      key.reserve(chain.size() + 1);
      key.push_back(canvasSortingOrder(chain[0]));   // CANVAS ORDER FIRST, then hierarchy
      for (size_t ki = 0; ki < chain.size(); ki++) {
        key.push_back(chain[ki] ? chain[ki]->siblingIndex : 0);
      }
      RectD rect;  // canvas root covers the screen (Unity ScreenSpaceOverlay semantics)
      rect.w = screenW / csf;
      rect.h = designH;
      float labelScale = 1.0f;
      rect = layChainScaled(chain, rect, &labelScale);   // RectTransform scale (see layChainScaled)

      // TMP text layout; the face's pointScale maps every baked metric to pixels
      const TMPFontData* font = ui->font;
      // a scaled label is laid out at its scaled size: identical line breaks for a uniform scale
      const float fontSize = ui->fontSize * labelScale;
      const float ps = font->pointScaleFor(fontSize > 0.0f ? fontSize : 0.01f);
      const float lineH = font->lineHeight * ps;

      // hard breaks: '\n' (tmp escapes already decoded by the JSON parser)
      std::vector<std::string> lines;
      {
        std::string cur;
        for (size_t i = 0; i < ui->text.size(); i++) {
          if (ui->text[i] == '\n') {
            lines.push_back(cur);
            cur.clear();
          } else {
            cur += ui->text[i];
          }
        }
        lines.push_back(cur);
      }

      // word wrap (Unity: break on spaces so no word splits; the space at a break vanishes)
      if (ui->wordWrap && rect.w > 0.0f) {
        if (traceDraw()) {
          printf("  draw: wrap check '%s' rect.w=%.1f\n", ui->text.c_str(), rect.w);
        }
        std::vector<std::string> wrapped;
        for (size_t li = 0; li < lines.size(); li++) {
          const std::string& raw = lines[li];
          std::vector<std::string> words;
          {
            std::string w;
            for (size_t i = 0; i <= raw.size(); i++) {
              if (i == raw.size() || raw[i] == ' ') {
                words.push_back(w);
                w.clear();
              } else {
                w += raw[i];
              }
            }
          }
          std::string cur;
          float pen = 0.0f;
          // TMP's wrap test (TextMeshProUGUI.GenerateTextMesh, genuine Unity.TextMeshPro.dll):
          // |m_xAdvance| + the NEXT glyph's bare xAdvance * scale > width -- the last character's
          // m_characterSpacing is not part of the measured width. The loader's "LOAD" (spacing
          // 11 at size 48 in a 200-unit box) measures 198 by that rule and stays on one line;
          // counting the trailing spacing made 220 and wrapped the D onto a second line.
          const float trailSp = ui->characterSpacing * ps;
          for (size_t wi = 0; wi < words.size(); wi++) {
            float wordW = 0.0f;
            for (size_t k = 0; k < words[wi].size(); k++) {
              wordW += charAdvance(font, (unsigned char)words[wi][k], fontSize, ui->characterSpacing, ui->wordSpacing);
            }
            // A word WIDER THAN THE WHOLE LINE: TMP moves it to a fresh line like any other word and
            // then breaks it at the last character that fits, repeatedly. The ThinkPad's impossible
            // question is exactly this -- "2834.344+(8464.174X3607.208=" is one 439-unit word in a
            // 296-unit box, and without the break it ran off the right edge of the screen box.
            if (wordW - trailSp > rect.w) {
              if (!cur.empty()) { wrapped.push_back(cur); cur.clear(); pen = 0.0f; }
              std::string chunk;
              float cw = 0.0f;
              for (size_t k = 0; k < words[wi].size(); k++) {
                const float adv = charAdvance(font, (unsigned char)words[wi][k], fontSize, ui->characterSpacing, ui->wordSpacing);
                if (!chunk.empty() && cw + adv - trailSp > rect.w) { wrapped.push_back(chunk); chunk.clear(); cw = 0.0f; }
                chunk += words[wi][k];
                cw += adv;
              }
              cur = chunk;
              pen = cw;
              continue;
            }
            float need = wordW;
            if (!cur.empty()) need += charAdvance(font, ' ', fontSize, ui->characterSpacing, ui->wordSpacing);
            if (!cur.empty() && pen + need - trailSp > rect.w) {
              wrapped.push_back(cur);
              cur = words[wi];
              pen = wordW;
            } else {
              if (!cur.empty()) {
                cur += ' ';
                pen += charAdvance(font, ' ', fontSize, ui->characterSpacing, ui->wordSpacing);
              }
              cur += words[wi];
              pen += wordW;
            }
          }
          wrapped.push_back(cur);
        }
        lines.swap(wrapped);
        if (traceDraw()) {
          printf("  draw: wrapped '%s' rect.w=%.1f -> %d lines:", ui->text.c_str(), rect.w,
                 (int)lines.size());
          for (size_t li = 0; li < lines.size() && li < 8; li++) {
            float lw = 0.0f;
            for (size_t k = 0; k < lines[li].size(); k++) {
              lw += charAdvance(font, (unsigned char)lines[li][k], fontSize, ui->characterSpacing, ui->wordSpacing);
            }
            printf(" [%d]'%.40s'w%.1f", (int)li, lines[li].c_str(), lw);
          }
          printf("\n");
        }
      }

      // vertical band: 1/8/0x100 == Top, 0x200 == Middle, 0x400 == Bottom (TextAlignmentOptions)
      const int vband = ui->alignment & 0x0700;
      const float totalH = (float)lines.size() * lineH;
      float firstTop = rect.y + rect.h;
      if (vband == 0x0200) {
        firstTop = rect.y + (rect.h + totalH) * 0.5f;
      } else if (vband == 0x0400) {
        firstTop = rect.y + totalH;
      }

      // horizontal band: 1 == Left, 2 == Center, 4 == Right (anything else reads Unknown/L/T)
      const int hband = ui->alignment & 0xFF;

      for (size_t li = 0; li < lines.size(); li++) {
        const std::string& line = lines[li];
        float lineWidth = 0.0f;
        for (size_t k = 0; k < line.size(); k++) {
          lineWidth += charAdvance(font, (unsigned char)line[k], fontSize, ui->characterSpacing, ui->wordSpacing);
        }
        // TMP aligns by lineInfo.maxAdvance = the last visible character's xAdvance MINUS its
        // (m_characterSpacing + normalSpacingOffset) * scale: no trailing spacing either.
        if (!line.empty()) lineWidth -= ui->characterSpacing * ps;
        float penX = rect.x;
        if (hband == 2) penX = rect.x + (rect.w - lineWidth) * 0.5f;
        else if (hband == 4) penX = rect.x + rect.w - lineWidth;
        const float baseline = firstTop - (float)li * lineH - font->ascender * ps;

        // FontStyle Underline (MouseoverScript hover swap -> TextUnderliner): TMP draws its
        // underline from face metrics we do not bake, so mirror its look with the same
        // proportions it ships at: offset ~11% of the font size below the baseline, thickness
        // ~8% (never thinner than one screen pixel). Emitted BEFORE the glyphs so the sort
        // keeps it under the ink (stable order within an equal key).
        if ((ui->fontStyle & 0x04) != 0 && lineWidth > 0.0f) {
          const float uOffset = fontSize * 0.11f;
          const float uH = (std::max)(fontSize * 0.08f, 1.0f / csf);
          DrawQuad q;
          q.x0 = penX * csf;
          q.y0 = (designH - (baseline - uOffset - uH)) * csf;
          q.x1 = (penX + lineWidth) * csf;
          q.y1 = (designH - (baseline - uOffset)) * csf;
          q.r = ui->colorR; q.g = ui->colorG; q.b = ui->colorB; q.a = ui->colorA;
          q.canvasLayer = 0;
          q.textured = false;
          uiSorted.push_back(std::make_pair(key, q));
        }

        for (size_t k = 0; k < line.size(); k++) {
          const int code = (int)(unsigned char)line[k];
          const TMPFontGlyph* gl = font->glyphFor(code);
          if (gl == 0) {
            penX += fontSize * 0.5f;  // TMP missing-glyph fallback step (documented)
            continue;
          }
          if (line[k] != ' ') {           // space advances only; no vertices (empty ink)
            const float quadL = penX + gl->xo * ps;
            const float quadTop = baseline + gl->yo * ps;
            const float quadW = gl->w * ps;
            const float quadH = gl->h * ps;
            DrawQuad q;
            // flip design y-up -> row space, then rasterise at the canvas scale factor
            q.x0 = quadL * csf;
            q.y0 = (designH - quadTop) * csf;
            q.x1 = (quadL + quadW) * csf;
            q.y1 = q.y0 + quadH * csf;
            q.r = ui->colorR;
            q.g = ui->colorG;
            q.b = ui->colorB;
            q.a = ui->colorA;
            q.u0 = gl->u0;
            q.v0 = gl->v0;
            q.u1 = gl->u1;
            q.v1 = gl->v1;
            q.canvasLayer = 0;            // single overlay ordering today (chain order suffices)
            q.textured = true;
            q.font = font;                // which atlas: this label's own font (texId stays -1)
            uiSorted.push_back(std::make_pair(key, q));   // sorted with the graphics below
          }
          // same advance rule as charAdvance(): glyph + characterSpacing (+ wordSpacing on a
          // space), all in font units scaled by ps. The measuring pass and the emitting pass
          // MUST agree or centred/right-aligned text drifts from its own measured width.
          penX += gl->adv * ps +
                  (ui->characterSpacing + (line[k] == ' ' ? ui->wordSpacing : 0.0f)) * ps;
        }
      }
    }
  }

  // ---- emit, in HIERARCHY order ---------------------------------------------------------------
  // One sort over BOTH passes. The key is the sibling index of every rect from the canvas root
  // down, compared lexicographically, which is exactly depth-first order. std::stable_sort keeps
  // equal-key quads in the order they were produced, so a label's glyphs stay in pen order and an
  // Image sharing a GameObject with a label still paints under it (pass 1 runs first).
  //
  // Emitting all graphics and then all labels -- which is what this did before -- is only right
  // when every label sits above every panel. It is a real difference for a panel that should
  // cover text behind it, which is why the two passes now share one ordering.
  std::stable_sort(uiSorted.begin(), uiSorted.end(),
                   [](const std::pair<std::vector<int>, DrawQuad>& a,
                      const std::pair<std::vector<int>, DrawQuad>& b) { return a.first < b.first; });
  for (size_t i = 0; i < uiSorted.size(); i++) g.drawList.push_back(uiSorted[i].second);

  // The -debug cheat menu LAST: it is a debug overlay, never an authored Unity canvas, and must
  // paint over the HUD (and any pause/menu canvas) on every resolution.
  buildDebugUiOverlay();
}

// ---------------------------------------------------------------------------
// raycast providers -- exact OBB slab + capsules (Unity containment/skip rules)
// ---------------------------------------------------------------------------
namespace {

struct WorldCapsule { bb::Vec3 c0, c1; float radius; };

static WorldCapsule makeCapsule(const bb::Transform* tr, const bb::Vec3& localCenter,
                                int axisIdx, float radius, float height) {
  WorldCapsule w;
  const bb::Vec3 axisLocal = (axisIdx == 0) ? bb::Vec3(1.0f, 0.0f, 0.0f)
                           : (axisIdx == 2) ? bb::Vec3(0.0f, 0.0f, 1.0f)
                                            : bb::Vec3(0.0f, 1.0f, 0.0f);
  const bb::Vec3 s = tr->scale;
  // Unity: radius follows the largest scale perpendicular to the capsule axis; length follows
  // the axis scale. Headless keeps both rules.
  float flat = (axisIdx == 1) ? (std::max)(fabsf(s.x), fabsf(s.z))
             : (axisIdx == 0) ? (std::max)(fabsf(s.y), fabsf(s.z))
                              : (std::max)(fabsf(s.x), fabsf(s.y));
  float lenScale = (axisIdx == 0) ? fabsf(s.x) : (axisIdx == 1) ? fabsf(s.y) : fabsf(s.z);
  w.radius = radius * (std::max)(flat, 1.0e-6f);
  const float hh = height * (std::max)(lenScale, 1.0e-6f);
  const bb::Vec3 axisWorld = tr->rotation.rotate(axisLocal);
  const bb::Vec3 centerWorld = tr->position + tr->rotation.rotate(bb::compMul(localCenter, s));
  const float halfSeg = (std::max)(0.0f, hh * 0.5f - w.radius);
  w.c0 = centerWorld - axisWorld * halfSeg;
  w.c1 = centerWorld + axisWorld * halfSeg;
  return w;
}

static float raySphereT(const bb::Vec3& o, const bb::Vec3& d, const bb::Vec3& c, float r) {
  const bb::Vec3 m = o - c;
  const float b = bb::dot(m, d);
  const float cc = bb::dot(m, m) - r * r;
  const float disc = b * b - cc;
  if (disc < 0.0f) return -1.0f;
  const float sq = sqrtf(disc);
  float t = -b - sq;      // d is normalized
  if (t < 0.0f) t = -b + sq;
  return t > 0.0f ? t : -1.0f;
}

static float pointSegDist(const bb::Vec3& p, const bb::Vec3& c0, const bb::Vec3& c1) {
  const bb::Vec3 ab = c1 - c0;
  const float len2 = bb::dot(ab, ab);
  if (len2 < bb::kEps) return bb::length(p - c0);
  float t = bb::dot(p - c0, ab) / len2;
  t = bb::clampf(t, 0.0f, 1.0f);
  return bb::length(p - (c0 + ab * t));
}

static bool slabSegment(float e, float ro, float rd, float& tmin, float& tmax) {
  if (fabsf(rd) < bb::kEps) {        // parallel to this slab pair
    return fabsf(ro) <= e;           // miss if outside the slab band
  }
  float t1 = (-e - ro) / rd;
  float t2 = (e - ro) / rd;
  if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
  if (t1 > tmin) tmin = t1;
  if (t2 < tmax) tmax = t2;
  return tmax >= tmin;
}

// entry t (>0) for an OBox, or -2 when the ray origin sits inside (PhysX containment skip)
static float boxEntryT(const bb::BoxCollider* b, const bb::Vec3& o, const bb::Vec3& d) {  const bb::Transform* tr = b->transform;
  const bb::Vec3 s = tr->scale;
  if (fabsf(s.x) < bb::kEps || fabsf(s.y) < bb::kEps || fabsf(s.z) < bb::kEps) return -1.0f; // degenerate scale
  const bb::Quat invR = bb::conj(tr->rotation);
  // pLocal = S^-1 R^-1 (p - T), then shift by the collider's local center ( regained scale units:
  // the box lives in pre-scale local space with half-extents 0.5*size)
  const bb::Vec3 rel = bb::compDiv(invR.rotate(o - tr->position), s) - b->center;
  const bb::Vec3 dirL = bb::compDiv(invR.rotate(d), s); // unnormalized: t stays world-param
  const bb::Vec3 e = b->size * 0.5f;
  float tmin = -FLT_MAX, tmax = FLT_MAX;
  const bool ok = slabSegment(e.x, rel.x, dirL.x, tmin, tmax) &&
                  slabSegment(e.y, rel.y, dirL.y, tmin, tmax) &&
                  slabSegment(e.z, rel.z, dirL.z, tmin, tmax);
  if (!ok || tmax < 0.0f) return -1.0f;
  if (tmin <= 0.0f) return (tmax > 0.0f) ? -2.0f : -1.0f;
  return tmin;
}

// entry t (>0) for a ZERO-THICKNESS built-in surface (Quad/Plane), else -1.
// These have no interior, so there is no containment (-2) convention, and they must not be
// slab-tested as OBBs -- a degenerate extent makes the slab miss. Exact + cheap: map the ray
// into the collider's pre-scale local space exactly like boxEntryT (S^-1 R^-1 (p - T), the
// compDiv folding world scale into the ray parameter so t stays a world-distance), intersect
// the panel plane (z=0 for Quad, y=0 for Plane), bounds-check the two in-plane axes.
// DOUBLE-SIDED by choice: Unity mesh raycasts default to queriesHitBackfaces=false, but this
// engine's box/capsule tests are boundary crossings from any side, and the dominant consumers
// (occlusion/line-of-sight, door clicks) read better with walls blocking reliably from both
// directions. Revisit only if a script provably relies on a backface pass-through.
static float meshEntryT(const bb::MeshCollider* m, const bb::Vec3& o, const bb::Vec3& d) {
  const bb::Transform* tr = m->transform;
  if (!tr) return -1.0f;
  const bb::Vec3 s = tr->scale;
  if (fabsf(s.x) < bb::kEps || fabsf(s.y) < bb::kEps || fabsf(s.z) < bb::kEps) return -1.0f;
  const bool quad = (m->surface == bb::MeshCollider::SurfaceQuad);
  // Quad spans local XY, Plane spans local XZ; halfU follows X either way, halfV is Y or Z.
  const bb::Quat invR = bb::conj(tr->rotation);
  const bb::Vec3 rel = bb::compDiv(invR.rotate(o - tr->position), s);
  const bb::Vec3 dirL = bb::compDiv(invR.rotate(d), s); // unnormalized: t stays a world parameter
  const float ow = quad ? rel.z : rel.y;
  const float dw = quad ? dirL.z : dirL.y;
  if (fabsf(dw) < bb::kEps) return -1.0f;           // parallel to the panel: never crosses
  const float t = -ow / dw;
  if (t <= 0.0f) return -1.0f;                      // crossing at/behind the origin
  const float u = rel.x + t * dirL.x;
  const float v = quad ? (rel.y + t * dirL.y) : (rel.z + t * dirL.z);
  const float pad = 1.0e-4f;                        // float-slack margin (local pre-scale units)
  if (fabsf(u) > m->halfU + pad || fabsf(v) > m->halfV + pad) return -1.0f;
  return t;
}

static float capsuleEntryT(const WorldCapsule& w, const bb::Vec3& o, const bb::Vec3& d) {
  if (pointSegDist(o, w.c0, w.c1) < w.radius) return -2.0f; // origin inside the capsule
  float best = -1.0f;
  float t = raySphereT(o, d, w.c0, w.radius);
  if (t > 0.0f) best = t;
  t = raySphereT(o, d, w.c1, w.radius);
  if (t > 0.0f && (best < 0.0f || t < best)) best = t;
  // side wall: infinite cylinder around the axis, clipped to the segment span
  const bb::Vec3 axis = w.c1 - w.c0;
  const float L = bb::length(axis);
  if (L > bb::kEps) {
    const bb::Vec3 a = axis / L;
    const bb::Vec3 m = o - w.c0;
    const bb::Vec3 n = m - a * bb::dot(m, a);   // perpendicular part of the origin offset
    const bb::Vec3 u = d - a * bb::dot(d, a);   // perpendicular part of the direction
    const float A = bb::dot(u, u);
    if (A > bb::kEps) {
      const float B = 2.0f * bb::dot(n, u);
      const float C = bb::dot(n, n) - w.radius * w.radius;
      const float disc = B * B - 4.0f * A * C;
      if (disc >= 0.0f) {
        const float sq = sqrtf(disc);
        float tt = (-B - sq) / (2.0f * A);
        if (tt <= 0.0f) tt = (-B + sq) / (2.0f * A);
        if (tt > 0.0f) {
          const float proj = bb::dot((o + d * tt) - w.c0, a);
          if (proj >= 0.0f && proj <= L && (best < 0.0f || tt < best)) best = tt;
        }
      }
    }
  }
  return best;
}

// ---- trigger overlap (OnTriggerEnter / Stay / Exit) -------------------------------------------
//
// SCOPE (documented Phase 3 limitation): overlap is only evaluated for pairs where at least one
// side is a CAPSULE. Every moving actor in this game is a capsule -- the player (CharacterController
// shell), the NPCs, and the thrown BSODA spray -- so capsule-vs-capsule and capsule-vs-box cover
// every trigger the game can actually fire. Box-vs-box pairs are static-vs-static in these scenes
// and could never produce an Enter event, so an OBB-vs-OBB SAT test would be dead code.
// MeshColliders are excluded for the same reason raycasts skip them: no real bounds yet.

// squared distance between two segments (exact; the standard clamped-parametric solution)
static float segSegDist2(const bb::Vec3& p1, const bb::Vec3& q1,
                         const bb::Vec3& p2, const bb::Vec3& q2) {
  const bb::Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  const float a = bb::dot(d1, d1), e = bb::dot(d2, d2), f = bb::dot(d2, r);
  float s, t;
  if (a < bb::kEps && e < bb::kEps) return bb::dot(r, r);     // both degenerate -> point/point
  if (a < bb::kEps) { s = 0.0f; t = bb::clampf(f / e, 0.0f, 1.0f); }
  else {
    const float c = bb::dot(d1, r);
    if (e < bb::kEps) { t = 0.0f; s = bb::clampf(-c / a, 0.0f, 1.0f); }
    else {
      const float b = bb::dot(d1, d2);
      const float denom = a * e - b * b;
      s = (denom > bb::kEps) ? bb::clampf((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
      t = (b * s + f) / e;
      if (t < 0.0f)      { t = 0.0f; s = bb::clampf(-c / a, 0.0f, 1.0f); }
      else if (t > 1.0f) { t = 1.0f; s = bb::clampf((b - c) / a, 0.0f, 1.0f); }
    }
  }
  const bb::Vec3 c1 = p1 + d1 * s, c2 = p2 + d2 * t;
  return bb::dot(c1 - c2, c1 - c2);
}

// Exact: two capsules overlap when their axis segments are closer than the sum of their radii.
static bool capsuleCapsuleOverlap(const WorldCapsule& a, const WorldCapsule& b) {
  const float r = a.radius + b.radius;
  return segSegDist2(a.c0, a.c1, b.c0, b.c1) <= r * r;
}

// Shared capsule-vs-oriented-box core (pre-scale local slab, radius-expanded Minkowski).
// CONSERVATIVE: the true Minkowski sum has rounded edges/corners, so this over-reports within one
// radius of a box corner. Acceptable here -- the game's trigger boxes are room/area volumes
// entered through their faces.
static bool capsuleObbOverlap(const WorldCapsule& w, const bb::Transform* tr,
                              const bb::Vec3& localCenter, const bb::Vec3& localHalf) {
  const bb::Vec3 s = tr->scale;
  if (fabsf(s.x) < bb::kEps || fabsf(s.y) < bb::kEps || fabsf(s.z) < bb::kEps) return false;
  const bb::Quat invR = bb::conj(tr->rotation);
  const bb::Vec3 p0 = bb::compDiv(invR.rotate(w.c0 - tr->position), s) - localCenter;
  const bb::Vec3 p1 = bb::compDiv(invR.rotate(w.c1 - tr->position), s) - localCenter;
  const bb::Vec3 dir = p1 - p0;
  // radius expressed per-axis in local units (non-uniform scale shrinks/stretches it)
  const bb::Vec3 e(localHalf.x + w.radius / fabsf(s.x),
                   localHalf.y + w.radius / fabsf(s.y),
                   localHalf.z + w.radius / fabsf(s.z));
  float tmin = 0.0f, tmax = 1.0f;     // segment parameter range, not an infinite ray
  return slabSegment(e.x, p0.x, dir.x, tmin, tmax) &&
         slabSegment(e.y, p0.y, dir.y, tmin, tmax) &&
         slabSegment(e.z, p0.z, dir.z, tmin, tmax);
}

static bool capsuleBoxOverlap(const WorldCapsule& w, const bb::BoxCollider* b) {
  if (!b->transform) return false;
  return capsuleObbOverlap(w, b->transform, b->center, b->size * 0.5f);
}

// Capsule vs a mesh collider's built-in surface, used by the trigger phase ONLY for isTrigger
// meshes (plain walls never pair with a trigger and would be dead math). The panel is tested as
// an oriented box of HALF-EXTENTS (halfU, halfV, 0) -- the thin axis has zero extent, but the
// radius expansion keeps it alive: slabSegment on the thin axis passes exactly when the capsule's
// axis segment lies within one radius of the panel plane, i.e. the EXACT capsule-vs-zero-thickness
// surface overlap (segment-to-plane distance <= radius), with the same conservative in-plane
// corner rule as boxes. Unmodelled (.asset) meshes are skipped silently here -- the raycast path
// owns the one-time notice. Quad: local XY, normal z. Plane: local XZ, normal y.
static bool capsuleMeshOverlap(const WorldCapsule& w, const bb::MeshCollider* m) {
  if (!m->transform) return false;
  if (m->surface == bb::MeshCollider::SurfaceQuad) {
    return capsuleObbOverlap(w, m->transform, bb::Vec3(),
                             bb::Vec3(m->halfU, m->halfV, 0.0f));
  }
  if (m->surface == bb::MeshCollider::SurfacePlane) {
    return capsuleObbOverlap(w, m->transform, bb::Vec3(),
                             bb::Vec3(m->halfU, 0.0f, m->halfV));
  }
  return false;
}

// Unity's collision action matrix: trigger messages are sent only when at least one collider of
// the pair is attached to a Rigidbody (kinematic or not; on its own object or a parent) or is a
// CharacterController. Two static colliders never message each other. The player carries a
// CharacterController AND a static trigger CapsuleCollider; without this rule the static capsule
// fired a second OnTriggerEnter at every static trigger -- the yellow swinging doors played
// doorOpen twice per pass (SwingingDoorScript.OnTriggerEnter, one PlayOneShot per message).
static bool attachedToBody(bb::Collider* c) {
  State& g = state();
  for (std::unordered_map<bb::CharacterController*, bb::CapsuleCollider*>::const_iterator it = g.ccShell.begin();
       it != g.ccShell.end(); ++it)
    if (it->second == c) return true;
  for (const bb::Transform* t = c->transform; t; t = t->parent) {
    if (!t->go) continue;
    for (size_t i = 0; i < t->go->components.size(); i++)
      if (dynamic_cast<bb::Rigidbody*>(t->go->components[i])) return true;
  }
  return false;
}

static bool triggerCandidate(bb::Collider* c) {
  return c && c->enabled && c->gameObject && c->gameObject->activeSelf && c->transform;
}

// ---- CharacterController capsule sweep (depenetration Move; Unity CC parity) --------------------
//
// The actor capsule (makeCapsule on the CC's baked dims; every CC in this game is Y-axis) is
// stepped along the requested motion and pushed back out of any non-trigger blocker it overlaps.
// Depenetration-only resolution is enough for this game: every scripted speed keeps ONE max step
// far below the radius-expanded thickness of the zero-thickness wall quads (<= ~0.44 u/frame at
// 50 Hz vs 0.64 u of expanded slab), so a static push-out both stops the actor AND leaves the
// tangential part of the motion intact -- which is wall sliding. Subdividing motions larger than
// one expanded slab keeps any single Move from tunneling through a panel.
//
// Blockers: non-trigger BoxColliders, non-trigger CapsuleColliders, and built-in Quad panels
// (SurfaceQuad). SurfacePlane panels are NOT blockers: the game's scripts hold the actor y
// constant (no gravity integration in any shipped scene), and a floor slab inside the radius
// expansion would push the capsule up every step and make walking jitter. Ceilings (Quad panels
// facing down) resolve harmlessly  -  the capsule can never reach them. The CC's own query shell
// (g.ccShell, deliberately outside go->components) is skipped as self.
//
// Diagnostics (eng::kDebugDiag): every contact is appended to a collector; the Move body prints
// the last batch on a 25-frame cadence. Under kDebugNoClip the contacts are computed at the RAW
// target (nothing is applied), so debug flight still reports what a real walk would have hit.

struct CCContact {
  bb::Collider* collider;
  bb::Vec3 push;     // world-space push that frees the capsule (points out of the blocker)
  float depth;       // penetration = push magnitude
};

static bool ccContactSeen(const std::vector<CCContact>& v, bb::Collider* c) {
  for (size_t i = 0; i < v.size(); i++) if (v[i].collider == c) return true;
  return false;
}

static bb::Vec3 closestOnObb(const bb::Vec3& ctr, const bb::Vec3 axis[3], const float half[3],
                             const bb::Vec3& p) {
  const bb::Vec3 d = p - ctr;
  bb::Vec3 q = ctr;
  for (int i = 0; i < 3; i++) q = q + axis[i] * bb::clampf(bb::dot(axis[i], d), -half[i], half[i]);
  return q;
}

// closest pair between a segment and a world OBB: alternately project the segment point onto the
// box and the box point onto the segment. Both steps are convex projections, so for the shallow
// overlaps this game produces the alternation lands in a few rounds.
static void segObbClosest(const bb::Vec3& c0, const bb::Vec3& c1, const bb::Vec3& ctr,
                          const bb::Vec3 axis[3], const float half[3],
                          bb::Vec3& pSeg, bb::Vec3& pBox) {
  const bb::Vec3 ab = c1 - c0;
  const float len2 = bb::dot(ab, ab);
  float t = len2 > bb::kEps ? 0.5f : 0.0f;
  for (int it = 0; it < 4; it++) {
    pSeg = c0 + ab * t;
    pBox = closestOnObb(ctr, axis, half, pSeg);
    t = len2 > bb::kEps ? bb::clampf(bb::dot(pBox - c0, ab) / len2, 0.0f, 1.0f) : 0.0f;
  }
  pSeg = c0 + ab * t;
  pBox = closestOnObb(ctr, axis, half, pSeg);
}

// push that frees a capsule from a world OBB (boxes and zero-thickness panels share this; the
// thin axis' half-extent of 0 only ever passes the containment branch when the axis midpoint is
// EXACTLY on the plane, which the sub-stepped motion never produces).
static bool pushOutObbSeg(const WorldCapsule& w, const bb::Vec3& ctr,
                          const bb::Vec3 axis[3], const float half[3],
                          bb::Vec3& push, float& depth) {
  const bb::Vec3 mid = (w.c0 + w.c1) * 0.5f;
  const bb::Vec3 dm = mid - ctr;
  float tm[3];
  bool midInside = true;
  for (int i = 0; i < 3; i++) {
    tm[i] = bb::dot(axis[i], dm);
    if (fabsf(tm[i]) > half[i]) midInside = false;
  }
  if (midInside) {
    int mi = 0;
    float pen = half[0] - fabsf(tm[0]);
    for (int i = 1; i < 3; i++) {
      const float pi = half[i] - fabsf(tm[i]);
      if (pi < pen) { pen = pi; mi = i; }
    }
    depth = pen + w.radius;
    push = axis[mi] * ((tm[mi] >= 0.0f ? 1.0f : -1.0f) * depth);
    return true;
  }
  bb::Vec3 ps, pb;
  segObbClosest(w.c0, w.c1, ctr, axis, half, ps, pb);
  const bb::Vec3 delta = ps - pb;
  const float dist = bb::length(delta);
  if (dist <= bb::kEps || dist >= w.radius) return false;
  depth = w.radius - dist;
  push = delta * (depth / dist);
  return true;
}

// push between two capsules (their axis segments share the segSegDist2 clamped-parametric walk,
// but this returns the closest POINTS so the push has a direction).
static bool pushOutCapsuleCapsule(const WorldCapsule& a, const WorldCapsule& b,
                                  bb::Vec3& push, float& depth) {
  const bb::Vec3 d1 = a.c1 - a.c0, d2 = b.c1 - b.c0, r0 = a.c0 - b.c0;
  const float aa = bb::dot(d1, d1), ee = bb::dot(d2, d2), ff = bb::dot(d2, r0);
  float s = 0.0f, t = 0.0f;
  if (aa >= bb::kEps) {
    const float c = bb::dot(d1, r0);
    if (ee < bb::kEps) { t = 0.0f; s = bb::clampf(-c / aa, 0.0f, 1.0f); }
    else {
      const float bb2 = bb::dot(d1, d2);
      const float den = aa * ee - bb2 * bb2;
      s = (den > bb::kEps) ? bb::clampf((bb2 * ff - c * ee) / den, 0.0f, 1.0f) : 0.0f;
      t = (bb2 * s + ff) / ee;
      if (t < 0.0f)      { t = 0.0f; s = bb::clampf(-c / aa, 0.0f, 1.0f); }
      else if (t > 1.0f) { t = 1.0f; s = bb::clampf((bb2 - c) / aa, 0.0f, 1.0f); }
    }
  } else if (ee >= bb::kEps) {
    s = 0.0f; t = bb::clampf(ff / ee, 0.0f, 1.0f);
  }
  const bb::Vec3 delta = (a.c0 + d1 * s) - (b.c0 + d2 * t);
  const float dist = bb::length(delta);
  const float sum = a.radius + b.radius;
  if (dist <= bb::kEps) {
    // coaxial pair (measure-rare): fall back to pushing along the actor's own axis
    const float len = bb::length(d1);
    if (len < bb::kEps) return false;
    depth = sum;
    push = d1 * (sum / len);
    return true;
  }
  if (dist >= sum) return false;
  depth = sum - dist;
  push = delta * (depth / dist);
  return true;
}

// BoxCollider and MeshCollider share the world-OBB shape: TRS transforms are orthonormal (no
// shear), so the world axes are the rotated unit basis and each world half-extent folds the
// transform's |scale| for its own axis. `half` stays pre-scale local so it composes exactly the
// way boxEntryT/capsuleObbOverlap see these colliders.
static bool colliderWorldObb(const bb::Collider* c, bb::Vec3& ctr, bb::Vec3 axis[3], float half[3]) {
  const bb::Transform* tr = c->transform;
  if (!tr) return false;
  const bb::Vec3 s = tr->scale;
  if (fabsf(s.x) < bb::kEps || fabsf(s.y) < bb::kEps || fabsf(s.z) < bb::kEps) return false;
  const bb::Quat r = tr->rotation;
  axis[0] = r.rotate(bb::Vec3(1.0f, 0.0f, 0.0f));
  axis[1] = r.rotate(bb::Vec3(0.0f, 1.0f, 0.0f));
  axis[2] = r.rotate(bb::Vec3(0.0f, 0.0f, 1.0f));
  bb::Vec3 lc(0.0f, 0.0f, 0.0f);
  if (const bb::BoxCollider* bx = dynamic_cast<const bb::BoxCollider*>(c)) {
    lc = bx->center;
    half[0] = bx->size.x * 0.5f * fabsf(s.x);
    half[1] = bx->size.y * 0.5f * fabsf(s.y);
    half[2] = bx->size.z * 0.5f * fabsf(s.z);
  } else if (const bb::MeshCollider* mm = dynamic_cast<const bb::MeshCollider*>(c)) {
    if (mm->surface == bb::MeshCollider::SurfaceQuad) {
      half[0] = mm->halfU * fabsf(s.x);
      half[1] = mm->halfV * fabsf(s.y);
      half[2] = 0.0f;
    } else {
      return false; // Plane = floor (not a blocker), SurfaceNone = unmodelled
    }
  } else {
    return false;
  }
  ctr = tr->position + r.rotate(bb::compMul(lc, s));
  return true;
}


// ---- static-collider broadphase ------------------------------------------------------------
// Every NPC raycast, every trigger-phase capsule and every CharacterController sweep used to test
// ALL ~3150 School colliders (2618 wall/floor panels + 529 boxes). Per 1/50 s physics step that
// was ~3.5-4.5 ms on a desktop CPU; an iOS-6-class CPU is ~20x slower, so one step outlasted the
// step itself, every frame hit the 8-step catch-up cap and the game ran at 1-2 fps (user report,
// real device). The broadphase only PRUNES: static colliders are bucketed by their XZ footprint
// in a 10-unit grid, a query returns every collider in the cells it touches (a superset of what it
// can hit), and the callers test those candidates in their ORIGINAL list order with the original
// comparisons -- so ties (raycast's strict <, the sweep's <=) resolve exactly as before.
// "Static" = no script, agent, rigidbody, CC or animator on the collider's object or any ancestor
// (walls, floors, ceilings, hall props); everything else is always a candidate. Rebuilt whenever a
// collider is added or removed (State::colliderVersion). BB_BRUTE=1: every collider, every query.
struct Broadphase {
  size_t version = (size_t)-1;
  float minX = 0.0f, minZ = 0.0f, cell = 10.0f;
  int nx = 0, nz = 0;
  std::vector<std::vector<int> > boxCells, meshCells;
  std::vector<int> dynBoxes, dynMeshes;
  std::vector<unsigned> boxStamp, meshStamp;
  unsigned stamp = 0;
  std::vector<int> outBoxes, outMeshes;     // last query's candidates (ascending list indices)
  bool brute = false;
};
static Broadphase& bphase() { static Broadphase b; return b; }
static bool s_bphaseForceBrute = false;

static bool colliderIsStatic(const bb::Collider* c) {
  for (const bb::Transform* t = c->transform; t; t = t->parent) {
    if (!t->go) continue;
    for (size_t i = 0; i < t->go->components.size(); i++) {
      const bb::Component* k = t->go->components[i];
      if (dynamic_cast<const bb::BoxCollider*>(k) || dynamic_cast<const bb::CapsuleCollider*>(k) ||
          dynamic_cast<const bb::MeshCollider*>(k) || dynamic_cast<const bb::AudioSource*>(k) ||
          dynamic_cast<const bb::SpriteRenderer*>(k) || dynamic_cast<const bb::NavMeshObstacle*>(k))
        continue;
      return false;   // a script / agent / rigidbody / CC / animator / anything that can move it
    }
  }
  return true;
}

// world XZ footprint of a box / quad / plane collider (false: unmodelled -> always a candidate)
static bool colliderXZ(const bb::Collider* c, float& x0, float& z0, float& x1, float& z1) {
  bb::Vec3 ctr, axis[3];
  float half[3];
  if (!colliderWorldObb(c, ctr, axis, half)) {
    const bb::MeshCollider* m = dynamic_cast<const bb::MeshCollider*>(c);
    if (!m || m->surface != bb::MeshCollider::SurfacePlane || !m->transform) return false;
    const bb::Vec3 s = m->transform->scale;
    const bb::Quat r = m->transform->rotation;
    ctr = m->transform->position;
    axis[0] = r.rotate(bb::Vec3(1.0f, 0.0f, 0.0f));
    axis[1] = r.rotate(bb::Vec3(0.0f, 1.0f, 0.0f));
    axis[2] = r.rotate(bb::Vec3(0.0f, 0.0f, 1.0f));
    half[0] = m->halfU * fabsf(s.x);
    half[1] = 0.0f;
    half[2] = m->halfV * fabsf(s.z);
  }
  const float ex = fabsf(axis[0].x) * half[0] + fabsf(axis[1].x) * half[1] + fabsf(axis[2].x) * half[2];
  const float ez = fabsf(axis[0].z) * half[0] + fabsf(axis[1].z) * half[1] + fabsf(axis[2].z) * half[2];
  x0 = ctr.x - ex; x1 = ctr.x + ex;
  z0 = ctr.z - ez; z1 = ctr.z + ez;
  return true;
}

static void bphaseBuild() {
  State& g = state();
  Broadphase& b = bphase();
  b.version = g.colliderVersion;
  b.brute = getenv("BB_BRUTE") != 0 || s_bphaseForceBrute;
  b.dynBoxes.clear(); b.dynMeshes.clear();
  b.boxStamp.assign(g.boxColliders.size(), 0u);
  b.meshStamp.assign(g.meshColliders.size(), 0u);
  struct Fp { int idx; bool mesh; float x0, z0, x1, z1; };
  std::vector<Fp> st;
  for (size_t i = 0; i < g.boxColliders.size(); i++) {
    Fp f = { (int)i, false, 0, 0, 0, 0 };
    if (!b.brute && colliderIsStatic(g.boxColliders[i]) && colliderXZ(g.boxColliders[i], f.x0, f.z0, f.x1, f.z1)) st.push_back(f);
    else b.dynBoxes.push_back((int)i);
  }
  for (size_t i = 0; i < g.meshColliders.size(); i++) {
    Fp f = { (int)i, true, 0, 0, 0, 0 };
    if (!b.brute && colliderIsStatic(g.meshColliders[i]) && colliderXZ(g.meshColliders[i], f.x0, f.z0, f.x1, f.z1)) st.push_back(f);
    else b.dynMeshes.push_back((int)i);
  }
  float mnx = 1e30f, mnz = 1e30f, mxx = -1e30f, mxz = -1e30f;
  for (size_t i = 0; i < st.size(); i++) {
    mnx = (std::min)(mnx, st[i].x0); mnz = (std::min)(mnz, st[i].z0);
    mxx = (std::max)(mxx, st[i].x1); mxz = (std::max)(mxz, st[i].z1);
  }
  if (st.empty()) { b.nx = b.nz = 0; b.boxCells.clear(); b.meshCells.clear(); return; }
  const float pad = 0.05f;                    // float slack at cell boundaries
  b.minX = mnx - 1.0f; b.minZ = mnz - 1.0f;
  const float spanX = mxx + 1.0f - b.minX, spanZ = mxz + 1.0f - b.minZ;
  b.cell = (std::max)(10.0f, (std::max)(spanX, spanZ) / 256.0f);   // at most 256 x 256 cells
  b.nx = (int)ceilf(spanX / b.cell);
  b.nz = (int)ceilf(spanZ / b.cell);
  b.boxCells.assign((size_t)b.nx * b.nz, std::vector<int>());
  b.meshCells.assign((size_t)b.nx * b.nz, std::vector<int>());
  for (size_t i = 0; i < st.size(); i++) {
    const int cx0 = (std::max)(0, (int)floorf((st[i].x0 - pad - b.minX) / b.cell));
    const int cx1 = (std::min)(b.nx - 1, (int)floorf((st[i].x1 + pad - b.minX) / b.cell));
    const int cz0 = (std::max)(0, (int)floorf((st[i].z0 - pad - b.minZ) / b.cell));
    const int cz1 = (std::min)(b.nz - 1, (int)floorf((st[i].z1 + pad - b.minZ) / b.cell));
    for (int cz = cz0; cz <= cz1; cz++)
      for (int cx = cx0; cx <= cx1; cx++)
        (st[i].mesh ? b.meshCells : b.boxCells)[(size_t)cz * b.nx + cx].push_back(st[i].idx);
  }
}

static void bphaseBegin() {
  Broadphase& b = bphase();
  if (b.version != state().colliderVersion) bphaseBuild();
  b.outBoxes.assign(b.dynBoxes.begin(), b.dynBoxes.end());
  b.outMeshes.assign(b.dynMeshes.begin(), b.dynMeshes.end());
  if (++b.stamp == 0) {                       // stamp wrapped: clear the marks
    std::fill(b.boxStamp.begin(), b.boxStamp.end(), 0u);
    std::fill(b.meshStamp.begin(), b.meshStamp.end(), 0u);
    b.stamp = 1;
  }
}
static void bphaseCell(int cx, int cz) {
  Broadphase& b = bphase();
  if (cx < 0 || cz < 0 || cx >= b.nx || cz >= b.nz) return;
  const size_t k = (size_t)cz * b.nx + cx;
  for (size_t i = 0; i < b.boxCells[k].size(); i++) {
    const int idx = b.boxCells[k][i];
    if (b.boxStamp[idx] != b.stamp) { b.boxStamp[idx] = b.stamp; b.outBoxes.push_back(idx); }
  }
  for (size_t i = 0; i < b.meshCells[k].size(); i++) {
    const int idx = b.meshCells[k][i];
    if (b.meshStamp[idx] != b.stamp) { b.meshStamp[idx] = b.stamp; b.outMeshes.push_back(idx); }
  }
}
static void bphaseEnd() {                     // original list order (tie rules depend on it)
  Broadphase& b = bphase();
  std::sort(b.outBoxes.begin(), b.outBoxes.end());
  std::sort(b.outMeshes.begin(), b.outMeshes.end());
}

// candidates whose footprint may touch the XZ box [x0,x1]x[z0,z1]
static void bphaseQueryBox(float x0, float z0, float x1, float z1) {
  bphaseBegin();
  Broadphase& b = bphase();
  if (b.nx > 0) {
    const int cx0 = (int)floorf((x0 - b.minX) / b.cell), cx1 = (int)floorf((x1 - b.minX) / b.cell);
    const int cz0 = (int)floorf((z0 - b.minZ) / b.cell), cz1 = (int)floorf((z1 - b.minZ) / b.cell);
    for (int cz = (std::max)(0, cz0); cz <= (std::min)(b.nz - 1, cz1); cz++)
      for (int cx = (std::max)(0, cx0); cx <= (std::min)(b.nx - 1, cx1); cx++) bphaseCell(cx, cz);
  }
  bphaseEnd();
}

// candidates along the ray origin + dir * t, t in [0, maxT] (dir normalised; maxT may be FLT_MAX):
// Amanatides-Woo cell walk in XZ, clipped to the grid
static void bphaseQueryRay(const bb::Vec3& o, const bb::Vec3& d, float maxT) {
  bphaseBegin();
  Broadphase& b = bphase();
  if (b.nx > 0) {
    const float gx0 = b.minX, gz0 = b.minZ, gx1 = b.minX + b.nx * b.cell, gz1 = b.minZ + b.nz * b.cell;
    float t0 = 0.0f, t1 = maxT;
    bool hitGrid = true;
    const float ox[2] = { o.x, o.z }, dx[2] = { d.x, d.z }, lo[2] = { gx0, gz0 }, hi[2] = { gx1, gz1 };
    for (int a = 0; a < 2 && hitGrid; a++) {
      if (fabsf(dx[a]) < 1e-12f) { if (ox[a] < lo[a] || ox[a] > hi[a]) hitGrid = false; continue; }
      float ta = (lo[a] - ox[a]) / dx[a], tb = (hi[a] - ox[a]) / dx[a];
      if (ta > tb) { const float s = ta; ta = tb; tb = s; }
      t0 = (std::max)(t0, ta); t1 = (std::min)(t1, tb);
      if (t0 > t1) hitGrid = false;
    }
    if (hitGrid) {
      const float sx = o.x + d.x * t0, sz = o.z + d.z * t0;
      int cx = (std::min)(b.nx - 1, (std::max)(0, (int)floorf((sx - b.minX) / b.cell)));
      int cz = (std::min)(b.nz - 1, (std::max)(0, (int)floorf((sz - b.minZ) / b.cell)));
      const int stepX = d.x > 0.0f ? 1 : -1, stepZ = d.z > 0.0f ? 1 : -1;
      const float inf = 1e30f;
      float tMaxX = fabsf(d.x) < 1e-12f ? inf : ((b.minX + (cx + (stepX > 0 ? 1 : 0)) * b.cell) - o.x) / d.x;
      float tMaxZ = fabsf(d.z) < 1e-12f ? inf : ((b.minZ + (cz + (stepZ > 0 ? 1 : 0)) * b.cell) - o.z) / d.z;
      const float tDX = fabsf(d.x) < 1e-12f ? inf : b.cell / fabsf(d.x);
      const float tDZ = fabsf(d.z) < 1e-12f ? inf : b.cell / fabsf(d.z);
      for (int guard = 0; guard < b.nx + b.nz + 4; guard++) {
        bphaseCell(cx, cz);
        // the cell walk can graze a corner: also take the cell diagonal neighbours it touches
        if (fabsf(tMaxX - tMaxZ) < 1e-4f) { bphaseCell(cx + stepX, cz); bphaseCell(cx, cz + stepZ); }
        if (tMaxX < tMaxZ) { if (tMaxX > t1) break; cx += stepX; tMaxX += tDX; }
        else { if (tMaxZ > t1) break; cz += stepZ; tMaxZ += tDZ; }
        if (cx < 0 || cz < 0 || cx >= b.nx || cz >= b.nz) break;
      }
    }
  }
  bphaseEnd();
}

// One resolve pass with the actor capsule at `pos`: accumulates and applies every blocker push.
// collect (optional) stores the contacts deduped by collider.
//
// The capsule is composed from `pos` directly, NOT the transform: Move only writes the transform
// back after the whole sweep lands, and mid-sweep the transform is still at the previous
// position. Unity CC dimensions (radius/height/center) are world-absolute -- a CharacterController
// capsule ignores transform scale, so nothing is folded in here (colliders below DO fold it).
static bb::Vec3 resolveContactsAt(eng::State& g, bb::CharacterController* cc, const bb::Vec3& pos,
                                  std::vector<CCContact>* collect, float rEffOverride = -1.0f) {
  bb::Vec3 total(0.0f, 0.0f, 0.0f);
  const bb::Transform* tr = cc->transform;
  if (!tr) return total;
  const bb::Vec3 center = pos + tr->rotation.rotate(bb::compMul(cc->center, bb::Vec3::one()));
  const bb::Vec3 up = tr->rotation.rotate(bb::Vec3(0.0f, 1.0f, 0.0f));
  const float halfSeg = (std::max)(0.0f, cc->height * 0.5f - cc->radius);
  WorldCapsule w;
  w.c0 = center - up * halfSeg;
  w.c1 = center + up * halfSeg;
  w.radius = rEffOverride >= 0.0f ? rEffOverride : cc->radius;
  const bb::CapsuleCollider* selfShell = 0;
  {
    std::unordered_map<bb::CharacterController*, bb::CapsuleCollider*>::const_iterator sh =
        g.ccShell.find(cc);
    if (sh != g.ccShell.end()) selfShell = sh->second;
  }

  {
    const float r = w.radius + 0.1f;
    bphaseQueryBox((std::min)(w.c0.x, w.c1.x) - r, (std::min)(w.c0.z, w.c1.z) - r,
                   (std::max)(w.c0.x, w.c1.x) + r, (std::max)(w.c0.z, w.c1.z) + r);
  }
  const std::vector<int> candBoxes = bphase().outBoxes, candMeshes = bphase().outMeshes;
  for (size_t ci = 0; ci < candBoxes.size(); ci++) {
    bb::BoxCollider* b = g.boxColliders[(size_t)candBoxes[ci]];
    if (!triggerCandidate(b) || b->isTrigger || (bb::Collider*)b == (bb::Collider*)selfShell) continue;
    bb::Vec3 ctr, axis[3]; float half[3];
    if (!colliderWorldObb(b, ctr, axis, half)) continue;
    bb::Vec3 push; float depth;
    if (pushOutObbSeg(w, ctr, axis, half, push, depth)) {
      total = total + push;
      if (collect && !ccContactSeen(*collect, b)) collect->push_back(CCContact{ b, push, depth });
    }
  }
  // Wall/ceiling QUAD panels (SurfaceQuad only; floors are SurfacePlane and skipped above). A
  // cheap sphere reject first: the panels dominate the collider list, and most are far away.
  if (!g.meshColliders.empty()) {
    const bb::Vec3 mid = (w.c0 + w.c1) * 0.5f;
    for (size_t ci = 0; ci < candMeshes.size(); ci++) {
      bb::MeshCollider* mcol = g.meshColliders[(size_t)candMeshes[ci]];
      if (!triggerCandidate(mcol) || mcol->isTrigger) continue;
      if (mcol->surface != bb::MeshCollider::SurfaceQuad || !mcol->transform) continue;
      const bb::Vec3 s = mcol->transform->scale;
      const float rough = mcol->halfU * (std::max)(fabsf(s.x), 1.0f) +
                          mcol->halfV * (std::max)(fabsf(s.y), 1.0f) +
                          cc->height * 0.5f + w.radius + 0.05f;
      const bb::Vec3 dc = mid - mcol->transform->position;
      if (bb::dot(dc, dc) > rough * rough) continue;
      bb::Vec3 ctr, axis[3]; float half[3];
      if (!colliderWorldObb(mcol, ctr, axis, half)) continue;
      bb::Vec3 push; float depth;
      if (pushOutObbSeg(w, ctr, axis, half, push, depth)) {
        total = total + push;
        if (collect && !ccContactSeen(*collect, mcol)) collect->push_back(CCContact{ mcol, push, depth });
      }
    }
  }
  for (size_t i = 0; i < g.capsuleColliders.size(); i++) {
    bb::CapsuleCollider* c = g.capsuleColliders[i];
    if (!triggerCandidate(c) || c->isTrigger || c == selfShell) continue;
    const WorldCapsule wb = makeCapsule(c->transform, c->center, c->direction, c->radius, c->height);
    bb::Vec3 push; float depth;
    if (pushOutCapsuleCapsule(w, wb, push, depth)) {
      total = total + push;
      if (collect && !ccContactSeen(*collect, c)) collect->push_back(CCContact{ c, push, depth });
    }
  }
  return total;
}

// Unity CharacterController parity: a capsule COLLIDE-AND-SLIDE (Quake lineage -- what Unity's CC
// actually implements). Unity does not push stages of motion out of overlaps: it sweeps the
// capsule along the motion, stops at the FIRST surface contact (leaving a skinWidth gap), drops
// the motion's component along the contact normal, and sweeps the remainder ALONG the wall
// (slide), a few bounces at most. That ordering is why Unity walking feels smooth: no summed
// per-panel pushes, no shove battles between coplanar wall quads, and the tangential motion is
// preserved exactly by the normal-clip. Sequence per Move:
//   1. depenetrate any pre-existing overlap (teleport-in, NPC jam) -- full radius, capped rounds,
//      as Unity resolves penetration before sweeping;
//   2. collide-and-slide the motion against world blockers (non-trigger boxes + SurfaceQuad
//      panels; floors are genuinely Quads here and a radius-2 capsule rests 2 units above them --
//      authentic against School.unity m_Radius: 2);
//   3. depenetrate dynamic capsule movers (NPCs) at the landed position, full radius.
// No substep machinery is needed: the swept-OBB slab test catches a zero-thickness panel at
// exactly its time-of-impact, so no motion can tunnel through one.
static bool sweepObbTOI(const bb::Vec3& c0, const bb::Vec3& c1, float rEff,
                        const bb::Vec3& dir, float maxT,
                        const bb::Vec3& ctr, const bb::Vec3 axis[3], const float half[3],
                        bb::Vec3& outN, float& outT) {
  // Capsule slab interval = segment endpoints +/- rEff on this axis (the radius belongs HERE
  // only); the blocker band is the bare [-half, +half]. Counting rEff in both places double-
  // counted it and made contacts begin at ~2x the true distance.
  float tEnter = -1e30f, tExit = maxT;
  int hitAxis = -1;
  float hitSign = 0.0f;
  // TOUCHING IS OUTSIDE. A sweep lands the capsule EXACTLY on a face, so the next Move starts
  // with the slab edge equal to the band edge (traced: hi = -0.10 against -half = -0.1). The
  // strict comparisons below then called that "inside the band", and while sliding PARALLEL to
  // the face the parallel branch can never clear it -- so the capsule was reported as a t=0 hit
  // on the first overlapping axis, which for a long wall is its LENGTH. The expel then pushed it
  // along that axis by the wall's whole half-extent: the corner trace jumps z 901.556 -> 896.680
  // in one step (0.556 + rEff 0.32 + half 4 = 4.876). That is the teleport-and-stuck report.
  // Treating contact within this tolerance as OUTSIDE turns it into either "clear" (parallel or
  // moving away) or a real time-of-impact on the CORRECT axis (moving in).
  const float kTouch = 1e-3f;
  // Axes the capsule is ALREADY INSIDE (on that axis alone) were entered in the past, so they
  // must never compete for hitAxis. They used to take tFirst = 0.0, and a real entry TOI that
  // float noise left a hair negative (~-1e-8) then LOST to them: traced in the corner test as
  // ProbeCornerWallX reported with a Z normal -- the wall's LENGTH -- after which the expel
  // shoved the capsule 4.876 out the wall's end. They now record their penetration instead, and
  // are used only when EVERY axis overlaps (genuine penetration -> least-penetration axis).
  int penAxis = -1;
  float penDepth = 1e30f, penSign = 0.0f;
  for (int i = 0; i < 3; i++) {
    const float d0 = bb::dot(axis[i], c0 - ctr);
    const float d1 = bb::dot(axis[i], c1 - ctr);
    const float lo = (d0 < d1 ? d0 : d1) - rEff;
    const float hi = (d0 > d1 ? d0 : d1) + rEff;   // MAX: `hi` took the min a second time, which
                                                   // collapsed the capsule's extent on this axis
                                                   // to a sphere around its LOWER endpoint
    const float a = bb::dot(axis[i], dir);
    float tFirst, tLast;
    if (fabsf(a) < bb::kEps) {
      if (lo > half[i] - kTouch || hi < -half[i] + kTouch) return false; // parallel and clear
      tFirst = -1e30f; tLast = 1e30f;                    // always inside this slab's band
    } else if (lo > half[i] - kTouch) {                  // capsule slab above the band
      if (a >= 0.0f) return false;                       // ...moving further away
      tFirst = (half[i] - lo) / a;                       // lo descends onto the band top
      tLast = (-half[i] - hi) / a;                       // hi passes the band bottom later
    } else if (hi < -half[i] + kTouch) {                 // capsule slab below the band
      if (a <= 0.0f) return false;
      tFirst = (-half[i] - hi) / a;                      // hi rises onto the band bottom
      tLast = (half[i] - lo) / a;                        // lo passes the band top later
    } else {                                             // already overlapping this slab band
      tFirst = -1e30f;                                   // entered in the PAST: not an entry axis
      tLast = (a > 0.0f) ? (half[i] - lo) / a : (-half[i] - hi) / a;
      const float mid = 0.5f * (lo + hi);
      const float pen = mid >= 0.0f ? half[i] - lo : hi + half[i];
      if (pen < penDepth) { penDepth = pen; penAxis = i; penSign = mid >= 0.0f ? 1.0f : -1.0f; }
    }
    if (tFirst > tEnter) { tEnter = tFirst; hitAxis = i; hitSign = (a > 0.0f ? -1.0f : 1.0f); }
    if (tLast < tExit) tExit = tLast;
  }
  if (hitAxis < 0) {
    // Inside on every axis that is not parallel-and-inside: genuine penetration. Report a t=0
    // contact on the LEAST-penetration axis, pointing to the side the capsule is on -- unless the
    // motion is already carrying it out that way, in which case let it leave.
    if (penAxis < 0) return false;
    const bb::Vec3 n = axis[penAxis] * penSign;
    if (bb::dot(dir, n) >= 0.0f) return false;
    outT = 0.0f;
    outN = n;
    return true;
  }
  if (tEnter > tExit + bb::kEps || tEnter > maxT + bb::kEps) return false;
  if (tEnter < 0.0f) tEnter = 0.0f;
  outT = tEnter;
  outN = axis[hitAxis] * hitSign;   // faces the capsule: clip dot(dir, outN) < 0
  if (bb::dot(dir, outN) > 0.0f) outN = outN * -1.0f;
  return true;
}

static bb::Vec3 ccMoveCollide(eng::State& g, bb::CharacterController* cc,
                              const bb::Vec3& start, const bb::Vec3& motion,
                              std::vector<CCContact>* collect, bool& blocked) {
  blocked = false;
  const float total = bb::length(motion);
  if (total < bb::kEps) return motion;
  const bb::Transform* tr = cc->transform;
  if (!tr) return motion;
  bb::Vec3 pos = start;
  const float skin = (std::max)(cc->skinWidth, bb::kEps);
  const float rEff = (std::max)(cc->radius - skin, bb::kEps);
  const bb::Vec3 centerOff = tr->rotation.rotate(cc->center);
  const bb::Vec3 up = tr->rotation.rotate(bb::Vec3(0.0f, 1.0f, 0.0f));
  const float halfSeg = (std::max)(0.0f, cc->height * 0.5f - cc->radius);
  const bb::CapsuleCollider* selfShell = 0;
  {
    std::unordered_map<bb::CharacterController*, bb::CapsuleCollider*>::const_iterator sh =
        g.ccShell.find(cc);
    if (sh != g.ccShell.end()) selfShell = sh->second;
  }

  // 1. pre-existing overlap (teleport-in, squeezed by movers): depenetrate with the SKIN-SHRUNK
  //    effective radius -- the same geometry the sweep plans against. Unity keeps these consistent
  //    so a resting capsule is never re-resolved away from its sweep landing point (full-radius
  //    resolution against skin-shrunk sweep contact re-pushes every Move forever = jitter).
  for (int iter = 0; iter < 4; iter++) {
    const bb::Vec3 push = resolveContactsAt(g, cc, pos, 0, rEff);
    if (fabsf(push.x) <= bb::kEps && fabsf(push.y) <= bb::kEps && fabsf(push.z) <= bb::kEps) break;
    blocked = true;
    pos = pos + push;
  }

  // 2. collide-and-slide the motion against world blockers.
  bb::Vec3 dir = motion * (1.0f / total);
  float remaining = total;
  int bounces = 0;
  while (remaining > bb::kEps && bounces < 4) {
    const bb::Vec3 center = pos + centerOff;
    const WorldCapsule w = { center - up * halfSeg, center + up * halfSeg, rEff };
    float bestT = remaining;
    bb::Collider* bestCol = 0;
    bb::Vec3 bestN(0.0f, 0.0f, 0.0f);
    {   // the swept capsule's XZ footprint
      const bb::Vec3 e0 = w.c0 + dir * remaining, e1 = w.c1 + dir * remaining;
      const float r = rEff + 0.1f;
      bphaseQueryBox((std::min)((std::min)(w.c0.x, w.c1.x), (std::min)(e0.x, e1.x)) - r,
                     (std::min)((std::min)(w.c0.z, w.c1.z), (std::min)(e0.z, e1.z)) - r,
                     (std::max)((std::max)(w.c0.x, w.c1.x), (std::max)(e0.x, e1.x)) + r,
                     (std::max)((std::max)(w.c0.z, w.c1.z), (std::max)(e0.z, e1.z)) + r);
    }
    const std::vector<int> sweepBoxes = bphase().outBoxes, sweepMeshes = bphase().outMeshes;
    for (size_t ci = 0; ci < sweepBoxes.size(); ci++) {
      bb::BoxCollider* b = g.boxColliders[(size_t)sweepBoxes[ci]];
      if (!triggerCandidate(b) || b->isTrigger || (bb::Collider*)b == (bb::Collider*)selfShell) continue;
      bb::Vec3 ctr, axis[3]; float half[3];
      if (!colliderWorldObb(b, ctr, axis, half)) continue;
      bb::Vec3 n; float t;
      if (sweepObbTOI(w.c0, w.c1, rEff, dir, bestT, ctr, axis, half, n, t) && t <= bestT) {
        bestT = t; bestCol = b; bestN = n;
      }
    }
    if (!g.meshColliders.empty()) {
      for (size_t ci = 0; ci < sweepMeshes.size(); ci++) {
        bb::MeshCollider* mcol = g.meshColliders[(size_t)sweepMeshes[ci]];
        if (!triggerCandidate(mcol) || mcol->isTrigger) continue;
        if (mcol->surface != bb::MeshCollider::SurfaceQuad || !mcol->transform) continue;
        // reject sphere must include the PANEL's own extent too -- a wall quad's center sits at
        // mid-wall height (~5 up) while the capsule is ~2, and only its oversized radius brings
        // that panel into the loop at all (tested: dropping the halfU/halfV terms rejected tall
        // walls and every wall contact silently vanished, leaving the depenetration to jitter).
        const bb::Vec3 s = mcol->transform->scale;
        const float rough = mcol->halfU * fabsf(s.x) + mcol->halfV * fabsf(s.y) +
                            cc->height * 0.5f + rEff + total + 0.05f;
        const bb::Vec3 dc = center - mcol->transform->position;
        if (bb::dot(dc, dc) > rough * rough) continue;
        bb::Vec3 ctr, axis[3]; float half[3];
        if (!colliderWorldObb(mcol, ctr, axis, half)) continue;
        bb::Vec3 n; float t;
        if (sweepObbTOI(w.c0, w.c1, rEff, dir, bestT, ctr, axis, half, n, t) && t <= bestT) {
          bestT = t; bestCol = mcol; bestN = n;
        }
      }
    }
    if (!bestCol) { pos = pos + dir * remaining; remaining = 0.0f; break; }
    if (bestT > bb::kEps) {
      pos = pos + dir * bestT;
    } else {
      // t=0 contact = the slide carried the capsule INTO this blocker's band (corners: after
      // clipping wall A the remainder still moves into wall B). Clip alone would wedge the
      // capsule inside the band forever; Unity/Quake expel to the contact surface first, then
      // clip the remaining motion. Depth = distance from the capsule slab to the surface face
      // along the entry normal, on the axis the normal came from.
      bb::Vec3 ctr2, axis2[3];
      float half2[3];
      if (colliderWorldObb(bestCol, ctr2, axis2, half2)) {
        int mi = 0;
        float bestProject = fabsf(bb::dot(axis2[0], bestN));
        for (int i = 1; i < 3; i++) {
          const float pr = fabsf(bb::dot(axis2[i], bestN));
          if (pr > bestProject) { bestProject = pr; mi = i; }
        }
        const float d0 = bb::dot(axis2[mi], (pos + centerOff - up * halfSeg) - ctr2);
        const float d1 = bb::dot(axis2[mi], (pos + centerOff + up * halfSeg) - ctr2);
        const float mn = d0 < d1 ? d0 : d1;
        const float mx = d0 > d1 ? d0 : d1;
        // WHICH SIDE TO EXPEL TOWARDS is decided by where the capsule ACTUALLY IS, not by the
        // sign of bestN. At t=0 the sweep is degenerate -- it is reporting the normal of a
        // contact the capsule is already inside -- and that sign can point INTO the box. Trusting
        // it pushed the capsule out through the FAR face: in the corner test the capsule ended at
        // z=902.62, past a wall spanning 901.9..902.1, and then slid unobstructed out past the
        // wall's end to x=908.4. That is the "teleported through a wall, then stuck" report.
        // Projecting the capsule's own centre picks the NEAR face, so the expel is always the
        // shortest way out and can never cross the box.
        const float depth = bb::dot(axis2[mi], bestN) > 0.0f
                          ? half2[mi] + rEff - mn
                          : mx + rEff + half2[mi];
        if (depth > bb::kEps) pos = pos + bestN * depth;
        // GATED. This and the CCBOUNCE line below were left UNGATED by the collision rewrite, so
        // every bounce of every CharacterController wrote to stdout every frame -- the player plus
        // each NPC, several lines per frame while sliding along a wall. Console writes on Windows
        // are slow enough for that to be felt as stutter. Every other debug print in this file is
        // behind `diag`/getenv; these now match.
        if (collect) printf("CCEXPEL pos=(%.3f,%.3f,%.3f) n=(%.2f,%.2f,%.2f) depth=%.4f col='%s'\n",
               pos.x, pos.y, pos.z, bestN.x, bestN.y, bestN.z, depth,
               bestCol->gameObject ? bestCol->gameObject->name.c_str() : "?");
      }
    }
    if (collect) printf("CCBOUNCE b=%d t=%.4f rem=%.4f pos=(%.3f,%.3f,%.3f) n=(%.2f,%.2f,%.2f) col='%s'\n",
           bounces, bestT, remaining, pos.x, pos.y, pos.z, bestN.x, bestN.y, bestN.z,
           bestCol->gameObject ? bestCol->gameObject->name.c_str() : "?");
    remaining = (std::max)(0.0f, remaining - bestT);
    blocked = true;
    if (collect && !ccContactSeen(*collect, bestCol)) collect->push_back(CCContact{ bestCol, bestN, bestT });
    // clip the motion's component along the contact normal, keep the tangential slide
    bb::Vec3 mv = dir * remaining;
    mv = mv - bestN * bb::dot(bestN, mv);
    remaining = bb::length(mv);
    if (remaining > bb::kEps) dir = mv * (1.0f / remaining);
    bounces++;
  }

  // 3. dynamic capsule movers (NPCs) depenetrate at the landed position, full radius (their
  //    positions changed this step too; the sweep saw them static).
  for (int iter = 0; iter < 4; iter++) {
    const bb::Vec3 center = pos + centerOff;
    const WorldCapsule w = { center - up * halfSeg, center + up * halfSeg, cc->radius };
    bb::Vec3 push(0.0f, 0.0f, 0.0f);
    bb::Collider* pushCol = 0;
    float pushDepth = 0.0f;
    for (size_t i = 0; i < g.capsuleColliders.size(); i++) {
      bb::CapsuleCollider* c = g.capsuleColliders[i];
      if (!triggerCandidate(c) || c->isTrigger || c == selfShell) continue;
      const WorldCapsule wb = makeCapsule(c->transform, c->center, c->direction, c->radius, c->height);
      bb::Vec3 p; float depth;
      if (pushOutCapsuleCapsule(w, wb, p, depth)) { push = push + p; pushDepth = (std::max)(pushDepth, depth); pushCol = c; }
    }
    if (fabsf(push.x) <= bb::kEps && fabsf(push.y) <= bb::kEps && fabsf(push.z) <= bb::kEps) break;
    blocked = true;
    if (collect && pushCol && !ccContactSeen(*collect, pushCol)) collect->push_back(CCContact{ pushCol, push, pushDepth });
    pos = pos + push;
  }
  return pos - start;
}


// Unity sends the message to BOTH objects, each receiving the other's collider, and to every
// component on the object.
static void sendTrigger(int kind, bb::Collider* self, bb::Collider* other) {
  bb::GameObject* go = self->gameObject;
  if (!go || !go->activeSelf) return;
  // snapshot: a handler may add/remove components on this object (pickups destroy themselves)
  std::vector<bb::Component*> comps(go->components.begin(), go->components.end());
  for (size_t i = 0; i < comps.size(); i++) {
    bb::Component* c = comps[i];
    if (!c->enabled) continue;
    if (kind == 0) c->OnTriggerEnter(other);
    else if (kind == 1) c->OnTriggerStay(other);
    else c->OnTriggerExit(other);
  }
}

static std::pair<bb::Collider*, bb::Collider*> orderedPair(bb::Collider* a, bb::Collider* b) {
  return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

// Body lives here with the collision helpers; eng::runTriggerPhase (declared in eng-internal.h
// and called from runFixedSteps) forwards to it just past the end of this anonymous namespace.
// Defining runTriggerPhase itself in here compiles fine and then fails to link, because an
// anonymous-namespace definition has internal linkage.
void runTriggerPhaseImpl() {
  State& g = state();

  // Build this step's overlap set. Capsules are the driver (see SCOPE above), so iterate capsules
  // against the other capsules and against every box; keep only pairs with a trigger on one side.
  std::set<std::pair<bb::Collider*, bb::Collider*> > now;
  for (size_t i = 0; i < g.capsuleColliders.size(); i++) {
    bb::CapsuleCollider* ca = g.capsuleColliders[i];
    if (!triggerCandidate(ca)) continue;
    const WorldCapsule wa = makeCapsule(ca->transform, ca->center, ca->direction, ca->radius, ca->height);
    const bool bodyA = attachedToBody(ca);
    // Bounding sphere of this capsule, for the panel/box early-out below.
    const bb::Vec3 waMid = (wa.c0 + wa.c1) * 0.5f;
    const float waR = bb::length(wa.c1 - wa.c0) * 0.5f + wa.radius;
    bphaseQueryBox(waMid.x - waR - 0.1f, waMid.z - waR - 0.1f, waMid.x + waR + 0.1f, waMid.z + waR + 0.1f);
    const std::vector<int> trigBoxes = bphase().outBoxes, trigMeshes = bphase().outMeshes;

    for (size_t j = i + 1; j < g.capsuleColliders.size(); j++) {
      bb::CapsuleCollider* cb = g.capsuleColliders[j];
      if (!triggerCandidate(cb)) continue;
      if (!ca->isTrigger && !cb->isTrigger) continue;
      const WorldCapsule wb = makeCapsule(cb->transform, cb->center, cb->direction, cb->radius, cb->height);
      if (capsuleCapsuleOverlap(wa, wb) && (bodyA || attachedToBody(cb))) now.insert(orderedPair(ca, cb));
    }
    for (size_t jj = 0; jj < trigBoxes.size(); jj++) {
      bb::BoxCollider* bx = g.boxColliders[(size_t)trigBoxes[jj]];
      if (!triggerCandidate(bx)) continue;
      if (!ca->isTrigger && !bx->isTrigger) continue;
      {
        // Conservative sphere around the box: offset centre plus the sum of its half-extents.
        const bb::Vec3 sc = bx->transform->scale;
        const bb::Vec3 off(bx->center.x * sc.x, bx->center.y * sc.y, bx->center.z * sc.z);
        const float br = bb::length(off) + 0.5f * (fabsf(bx->size.x * sc.x) + fabsf(bx->size.y * sc.y) +
                                                   fabsf(bx->size.z * sc.z));
        const bb::Vec3 d = waMid - bx->transform->position;
        if (bb::dot(d, d) > (waR + br) * (waR + br)) continue;
      }
      if (capsuleBoxOverlap(wa, bx) && (bodyA || attachedToBody(bx))) now.insert(orderedPair(ca, bx));
    }
    // Mesh panels join the trigger set for their isTrigger members only (the 23 School wall
    // triggers); wall quads are not triggers and would be dead math. Worst case is ~32 capsules
    // x 2618 panels of cheap float slabs per step at 50 Hz -- comfortably kept simple.
    for (size_t jj = 0; jj < trigMeshes.size(); jj++) {
      bb::MeshCollider* m = g.meshColliders[(size_t)trigMeshes[jj]];
      if (!triggerCandidate(m)) continue;
      if (!ca->isTrigger && !m->isTrigger) continue;
      // EARLY-OUT, measured: 32 of the School's 33 capsules are triggers (every notebook, pickup
      // and NPC trigger shell), so each was run through the full OBB test against ALL 2618 wall
      // panels -- 65,448 tests per physics step, 17 of the 20 ms the game logic cost per frame.
      // A panel whose bounding sphere does not reach the capsule's cannot overlap it, so this
      // skips only tests that were going to return false: the overlap set is unchanged.
      if (m->surface == bb::MeshCollider::SurfaceQuad) {
        const bb::Vec3 sc = m->transform->scale;
        const float pr = m->halfU * fabsf(sc.x) + m->halfV * fabsf(sc.y);
        const bb::Vec3 d = waMid - m->transform->position;
        if (bb::dot(d, d) > (waR + pr) * (waR + pr)) continue;
      }
      if (capsuleMeshOverlap(wa, m) && (bodyA || attachedToBody(m))) now.insert(orderedPair(ca, m));
    }
  }

  // Diff against last step: gone -> Exit, new -> Enter, surviving -> Stay.
  //
  // The diff is SNAPSHOTTED into vectors and the new state COMMITTED before anything is
  // dispatched. Handlers are allowed to destroy objects (a pickup destroys itself on contact),
  // and unnoteComponent erases from g.triggerOverlaps -- so dispatching while iterating that set
  // invalidated the iterator and corrupted the heap, surfacing as a crash much later in an
  // unrelated phase. Vectors are immune to that, and triggerDead covers the pointers going stale.
  typedef std::pair<bb::Collider*, bb::Collider*> Pair;
  std::vector<Pair> exits, enters, stays;
  typedef std::set<Pair>::iterator It;
  for (It it = g.triggerOverlaps.begin(); it != g.triggerOverlaps.end(); ++it) {
    if (now.find(*it) == now.end()) exits.push_back(*it);
  }
  for (It it = now.begin(); it != now.end(); ++it) {
    if (g.triggerOverlaps.find(*it) != g.triggerOverlaps.end()) stays.push_back(*it);
    else enters.push_back(*it);
  }
  g.triggerOverlaps.swap(now);      // commit before dispatch
  g.triggerDead.clear();

  const std::vector<Pair>* phases[3] = { &exits, &enters, &stays };
  const int kinds[3] = { 2, 0, 1 };  // Exit, Enter, Stay (Unity's order within a step)
  for (int p = 0; p < 3; p++) {
    const std::vector<Pair>& list = *phases[p];
    for (size_t i = 0; i < list.size(); i++) {
      bb::Collider* a = list[i].first;
      bb::Collider* b = list[i].second;
      if (g.triggerDead.count(a) || g.triggerDead.count(b)) continue; // destroyed mid-dispatch
      sendTrigger(kinds[p], a, b);
      if (g.triggerDead.count(a) || g.triggerDead.count(b)) continue; // ...by the call above
      sendTrigger(kinds[p], b, a);
    }
  }
}

static bool raycastCandidate(bb::Component* c, int layerMask, bb::QueryTriggerInteraction query) {
  State& g = state();
  if (!c->gameObject || !c->gameObject->activeSelf) return false;
  if (!c->enabled) return false;
  const int bit = 1 << c->gameObject->layer;
  if ((layerMask & bit) == 0) return false;
  bb::Collider* col = dynamic_cast<bb::Collider*>(c); // only true shapes can be triggers
  if (col && col->isTrigger) {
    if (query == bb::QueryTriggerInteraction::Ignore) return false;
    if (query == bb::QueryTriggerInteraction::UseGlobal && !g.queriesHitTriggers) return false;
  }
  return true;
}

} // namespace

// External-linkage entry point for the trigger phase (body above, inside the anonymous namespace
// alongside the collision helpers it uses).
void runTriggerPhase() { runTriggerPhaseImpl(); }
// sim: compare the broadphase against brute force in-process
void setBroadphaseBrute(bool brute) { s_bphaseForceBrute = brute; bphase().version = (size_t)-1; }

bool raycastImpl(const bb::Vec3& origin, const bb::Vec3& direction, bb::RaycastHit* hit,
                 float maxDistance, int layerMask, bb::QueryTriggerInteraction query) {
  State& g = state();
  const bb::Vec3 dir = bb::normalized(direction);
  if (dir.magnitude() < bb::kEps) return false;
  // Unity docs: maxDistance 0 behaves like infinity
  float best = maxDistance <= 0.0f ? FLT_MAX : maxDistance;
  bb::RaycastHit bestHit;
  bool found = false;
  bb::Collider* foundCol = 0;
  bb::GameObject* foundGO = 0;

  bphaseQueryRay(origin, dir, best);
  const std::vector<int> rayBoxes = bphase().outBoxes, rayMeshes = bphase().outMeshes;
  for (size_t ci = 0; ci < rayBoxes.size(); ci++) {
    bb::BoxCollider* b = g.boxColliders[(size_t)rayBoxes[ci]];
    if (!raycastCandidate(b, layerMask, query)) continue;
    const float t = boxEntryT(b, origin, dir);
    if (t > 0.0f && t < best) {
      best = t; found = true; foundCol = b; foundGO = b->gameObject;
    }
  }
  for (size_t i = 0; i < g.capsuleColliders.size(); i++) {
    bb::CapsuleCollider* c = g.capsuleColliders[i];
    if (!raycastCandidate(c, layerMask, query)) continue;
    const WorldCapsule w = makeCapsule(c->transform, c->center, c->direction, c->radius, c->height);
    const float t = capsuleEntryT(w, origin, dir);
    if (t > 0.0f && t < best) {
      best = t; found = true; foundCol = c; foundGO = c->gameObject;
    }
  }
  for (size_t ci = 0; ci < rayMeshes.size(); ci++) {
    bb::MeshCollider* m = g.meshColliders[(size_t)rayMeshes[ci]];
    if (!raycastCandidate(m, layerMask, query)) continue;
    if (m->surface == bb::MeshCollider::SurfaceQuad || m->surface == bb::MeshCollider::SurfacePlane) {
      float t = meshEntryT(m, origin, dir);
      // CONVEX = SOLID. Every School door is three COPLANAR built-in quads at one position:
      // `barrier` (_DoorIn) and `invisibleBarrier` (_Barrier) are plain triangle meshes, while
      // `trigger` (_DoorOut) has m_Convex: 1 -- PhysX cooks it as a solid hull, so its surface
      // is reached BEFORE the zero-thickness quads it shares a plane with. DoorScript opens
      // only when the click ray's FIRST hit is `trigger`; with an exact tie decided by list
      // order, the sim measured 2 of 15 doors openable (13 returned _DoorIn first). The skin is
      // the smallest margin that settles an exact tie: it cannot reorder surfaces really apart.
      if (t > 0.0f && m->convex) t = (std::max)(t - 1.0e-3f, 1.0e-6f);
      if (t > 0.0f && t < best) {
        best = t; found = true; foundCol = m; foundGO = m->gameObject;
      }
    } else if (!g.meshWarned) {  // unmodelled .asset mesh: skip with a one-time notice
      g.meshWarned = true;
      bb::Debug::Log("eng: some mesh colliders carry real .asset meshes (raycast skips them until mesh bounds land)");
    }
  }

  if (!found) return false;
  if (hit) {
    *hit = bb::RaycastHit();
    hit->collider = foundCol;
    hit->gameObject = foundGO;
    hit->transform = foundGO ? foundGO->transform : 0;
    hit->point = origin + dir * best;
    hit->distance = best;
  }
  return true;
}

} // namespace eng

// ---------------------------------------------------------------------------
// bb API surface implementations
// ---------------------------------------------------------------------------
namespace bb {

float Time::timeScale = 1.0f;
int Time::frameCount = 0;
CursorLockMode Cursor::lockState = CursorLockMode::None;
bool Cursor::visible = true;

bool isPlaying() { return eng::state().playing && !eng::state().quitting; }
float Time::deltaTime() { return eng::state().dtContext; }
float Time::unscaledDeltaTime() { return eng::state().dtUnscaled; }

void Destroy(GameObject* obj) { eng::queueDestroy(obj, 0.0f); }   // Unity: end of current frame
void Destroy(GameObject* obj, float delay) { eng::queueDestroy(obj, delay); }

void QuitApplication() { eng::state().quitting = true; }

void LoadScene(const std::string& name) {
  eng::State& g = eng::state();
  g.pendingScene = name;        // eng::processPendingScenes consumes it at end of frame
  g.hasPendingScene = true;
}
void LoadSceneAsync(const std::string& name) {
  eng::State& g = eng::state();
  g.asyncScene = name;
  g.asyncFramesLeft = 2;        // this frame ends (and is drawn), the next one performs the swap
  eng::startAsyncSceneParse(name);   // read + parse on a worker meanwhile (State::asyncLoad)
}

void DontDestroyOnLoad(Object* obj) {
  if (!obj) return;
  if (GameObject* go = dynamic_cast<GameObject*>(obj)) { go->dontDestroy = true; return; }
  if (Component* c = dynamic_cast<Component*>(obj)) {
    if (c->gameObject) c->gameObject->dontDestroy = true;
  }
}

// Unity activeInHierarchy: self AND every ancestor. Walks transform parents, so it costs the
// hierarchy depth (3-4 here) and is only called from the render passes, once per renderer.
bool GameObject::activeInHierarchy() const {
  if (!activeSelf) return false;
  for (const Transform* t = transform ? transform->parent : 0; t != 0; t = t->parent) {
    if (t->go && !t->go->activeSelf) return false;
  }
  return true;
}

GameObject* GameObject::Find(const char* name) {
  if (!name) return 0;
  for (size_t i = 0; i < eng::liveGameObjects().size(); i++) {
    GameObject* go = eng::liveGameObjects()[i];
    if (go->activeSelf && go->name == name) return go; // Unity Find: active objects only
  }
  return 0;
}

GameObject* GameObject::FindGameObjectWithTag(const char* tag_) {
  if (!tag_) return 0;
  for (size_t i = 0; i < eng::liveGameObjects().size(); i++) {
    GameObject* go = eng::liveGameObjects()[i];
    if (go->activeSelf && go->tag == tag_) return go; // Unity: active objects only, first match
  }
  return 0;
}

// ---- Transform / Component plumbing ----------------------------------------
std::string Transform::name() const { return go ? go->name : std::string(); }
std::string Transform::tag() const { return go ? go->tag : std::string("Untagged"); }
GameObject* Transform::gameObject() const { return go; }

void Transform::Rotate(const Vec3& euler, bool worldSpace) {
  const Quat q = Quat::euler(euler);
  rotation = worldSpace ? (q * rotation) : (rotation * q); // Unity Rotate: self-space right-multiplies
}

void Transform::LookAt(const Vec3& target, const Vec3& worldUp) {
  const Vec3 f = normalized(target - position);
  if (f.magnitude() < kEps) return;
  const Vec3 r = normalized(cross(worldUp, f));
  const Vec3 u = cross(f, r);
  // basis columns (right, up, forward) -> quaternion; verified against Unity convention (identity
  // and axis cases checked; forward = rotation.rotate(0,0,1))
  const float m00 = r.x, m01 = u.x, m02 = f.x;
  const float m10 = r.y, m11 = u.y, m12 = f.y;
  const float m20 = r.z, m21 = u.z, m22 = f.z;
  const float tr = m00 + m11 + m22;
  Quat q;
  if (tr > 0.0f) {
    const float s = 0.5f / sqrtf(tr + 1.0f);
    q = Quat((m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25f / s);
  } else if (m00 > m11 && m00 > m22) {
    const float s = 2.0f * sqrtf(1.0f + m00 - m11 - m22);
    q = Quat(0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s);
  } else if (m11 > m22) {
    const float s = 2.0f * sqrtf(1.0f + m11 - m00 - m22);
    q = Quat((m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s);
  } else {
    const float s = 2.0f * sqrtf(1.0f + m22 - m00 - m11);
    q = Quat((m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s);
  }
  rotation = q;
}

bool Component::isActiveAndEnabled() const {
  return enabled && gameObject && gameObject->activeSelf;
}
std::string Component::tag() {
  return gameObject ? gameObject->tag : std::string("Untagged");
}

// ---- coroutines --------------------------------------------------------------
void StartCoroutine(Coroutine* c) {
  if (!c) return;
  eng::State& g = eng::state();
  g.coroutines.push_back(c);
  const float dt = g.dtContext > 0.0f ? g.dtContext : eng::kFixedDt;
  if (!c->tick(dt)) { // body finished during the synchronous prefix
    for (size_t i = 0; i < g.coroutines.size(); i++) {
      if (g.coroutines[i] == c) { g.coroutines.erase(g.coroutines.begin() + i); break; }
    }
    delete c; // Unity contract: engine frees the machine at completion
  }
}

// ---- Instantiate (deep component cloning lands with the prefab loader) --------
GameObject* Instantiate(GameObject* prefab, const Vec3& pos, const Quat& rot) {
  if (!prefab) return 0;

  // Deep clone when a prefab bake exists for this template. The inert template shell created by
  // BindCtx::prefabFor is named after its .prefab file (basenameOf), which is exactly the bake
  // key, so the template's name is the lookup. The returned subtree is real: components, bound
  // script fields and a fired Awake/OnEnable/Start, registered in the scene registry.
  if (GameObject* clone = eng::instantiatePrefab(prefab->name, pos, rot)) {
    clone->name = clone->name + "(Clone)";   // Unity's clone naming
    return clone;
  }

  // Fallback: shallow inert shell (prefabs with no bake -- e.g. a template built at runtime).
  // Keeps the pre-prefab-loader behaviour rather than returning null to a deref site.
  GameObject* go = new GameObject();
  Transform* t = new Transform();
  t->go = go;
  go->transform = t;
  go->name = prefab->name + "(Clone)";
  go->tag = prefab->tag;
  go->layer = prefab->layer;
  t->position = pos;
  t->rotation = rot;
  t->scale = prefab->transform ? prefab->transform->scale : Vec3::one();
  eng::noteGameObject(go); // Find/dispatch see the clone
  eng::noteRuntimeObject(go);
  return go;
}
GameObject* Instantiate(GameObject* prefab) { return Instantiate(prefab, Vec3::zero(), Quat::identity()); }

// ---- csharpToString: .NET Single.ToString() G7 parity -------------------------
std::string csharpToString(float f) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%.7g", (double)f);
  std::string s(buf);
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == 'e') { s[i] = 'E'; break; } // C# prints E+06 / E-07 (uppercase, 2-digit exponent)
  }
  return s;
}

// ---- Random (deterministic xorshift128+) --------------------------------------
static unsigned int xrNext() {
  eng::State& g = eng::state();
  unsigned int* s = g.rng;
  unsigned int t = s[3];
  const unsigned int x = s[0];
  s[3] = s[2];
  s[2] = s[1];
  s[1] = x;
  t ^= t << 11;
  t ^= t >> 8;
  s[0] = t ^ x ^ (x >> 19);
  return s[0];
}
static float xrUnit01() { return (float)(xrNext() >> 8) * (1.0f / 16777216.0f); }

float Random::range(float a, float b) {
  if (b < a) { const float tmp = a; a = b; b = tmp; }
  return a + (b - a) * xrUnit01();
}
int Random::range(int a, int b) {
  if (b < a) { const int tmp = a; a = b; b = tmp; }
  if (b == a) return a;
  return a + (int)(xrUnit01() * (float)(b - a)); // Unity int overload: [a, b)
}

// ---- Screen / Input ------------------------------------------------------------
int Screen::width() { return eng::state().screenW; }
int Screen::height() { return eng::state().screenH; }

bool Input::anyKey() {
  eng::State& g = eng::state();
  for (int i = 0; i < kKeySlots; i++) if (g.keyHeld[i]) return true;
  for (int i = 0; i < 8; i++) if (g.mouseHeld[i]) return true;
  return false;
}

void GameObject::SetActive(bool v) {
  if (activeSelf == v) return;
  eng::State& g = eng::state();
  // this object and its descendants, in creation (= hierarchy, parents first) order
  std::vector<GameObject*> affected;
  for (size_t i = 0; i < g.gos.size(); i++) {
    GameObject* o = g.gos[i];
    bool under = (o == this);
    for (Transform* t = o->transform ? o->transform->parent : 0; t && !under; t = t->parent)
      if (t->go == this) under = true;
    if (under) affected.push_back(o);
  }
  std::vector<char> before(affected.size());
  for (size_t i = 0; i < affected.size(); i++) before[i] = affected[i]->activeInHierarchy() ? 1 : 0;
  activeSelf = v;
  for (size_t i = 0; i < affected.size(); i++) {
    const bool now = affected[i]->activeInHierarchy();
    if ((now ? 1 : 0) == before[i]) continue;
    // snapshot: a handler may add or remove components
    std::vector<Component*> comps(affected[i]->components.begin(), affected[i]->components.end());
    for (size_t c = 0; c < comps.size(); c++) {
      if (!comps[c]->enabled) continue;
      if (now) comps[c]->OnEnable(); else comps[c]->OnDisable();
    }
  }
}

bool Input::anyKeyDown() {
  eng::State& g = eng::state();
  const bool r = g.anyKeyFlag;
  g.anyKeyFlag = false; // latch consumed by read (shell injects between frames)
  return r;
}
bool Input::GetKeyDown(KeyCode k) {
  eng::State& g = eng::state();
  const int i = (int)k;
  if (i < 0 || i >= bb::kKeySlots) return false;
  const bool e = g.keyEdge[i];
  g.keyEdge[i] = false;
  return e;
}
bool Input::GetKey(KeyCode k) {       // Unity held-state form (InputManager.cs GetKeyState)
  eng::State& g = eng::state();
  const int i = (int)k;
  if (i < 0 || i >= bb::kKeySlots) return false;
  return g.keyHeld[i];
}
bool Input::GetKeyDown(const char* name) {
  if (!name) return false;
  eng::State& g = eng::state();
  // Unity: GetKeyDown ALSO takes KeyCode.ToString() names ("return", "enter", "escape", "3"...).
  // The only caller-side dependency lives in MathGameScript (Submit the answer) plus TMP's
  // InputField MiniChar key set, all reachable through this same bridge.
  static const struct { const char* n; bb::KeyCode k; } named[] = {
    { "return",    bb::KeyCode::Return },
    { "enter",     bb::KeyCode::Return },
    { "escape",    bb::KeyCode::Escape },
  };
  const std::string l = name;
  for (size_t k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
    if (l == named[k].n) {
      const int i = (int)named[k].k;
      const bool e = g.keyEdge[i];
      g.keyEdge[i] = false;
      return e;
    }
  }
  std::map<std::string, bool>::iterator it = g.namedKeyEdge.find(name);
  if (it == g.namedKeyEdge.end()) return false;
  const bool e = it->second;
  g.namedKeyEdge.erase(it);
  return e;
}
bool Input::GetButton(const char* button) {
  if (!button) return false;
  eng::State& g = eng::state();
  std::map<std::string, bool>::iterator it = g.namedButtons.find(button);
  return it != g.namedButtons.end() ? it->second : false;
}
float Input::GetAxis(const char* axis) {
  if (!axis) return 0.0f;
  eng::State& g = eng::state();
  std::map<std::string, float>::iterator it = g.axes.find(axis);
  return it != g.axes.end() ? it->second : 0.0f;
}
bool Input::GetMouseButtonDown(int button) {
  eng::State& g = eng::state();
  if (button < 0 || button >= 8) return false;
  // While the -debug menu is open a tap is the menu's click, not the game's: game readers get
  // no edge (the panel reads g.mouseEdge directly in debugUiFrameImpl). Same for the held form:
  // a click-drag across the panel must not "use item"/shoot mid-drag.
  if (g.debugUiOpen) { g.mouseEdge[button] = false; return false; }
  const bool e = g.mouseEdge[button];
  g.mouseEdge[button] = false;
  return e;
}
bool Input::GetMouseButton(int button) {  // Unity held-state form (Interact/UseItem: Mouse0/Mouse1)
  eng::State& g = eng::state();
  if (button < 0 || button >= 8) return false;
  if (g.debugUiOpen) { g.mouseEdge[button] = false; return false; }
  return g.mouseHeld[button];
}

Vec3 Input::mousePosition() {
  // Unity Input.mousePosition: mouse pixels in the client area, ORIGIN AT THE BOTTOM-LEFT. The
  // engine's pointer store (uiPointerMove) is top-left (screen/Win32 convention, used by the UI
  // hit tests), so flip y; no pointer seen yet reads (0,0,0), like Unity's first frame.
  eng::State& g = eng::state();
  return Vec3(g.uiPointerX, (float)g.screenH - g.uiPointerY, 0.0f);
}

// ---- PlayerPrefs (file-backed INI at run/prefs.ini) -----------------------------
static void prefsEnsureLoaded() {
  eng::State& g = eng::state();
  if (g.prefsLoaded) return;
  g.prefsLoaded = true;
  FILE* f = fopenSafe(g.prefsPath.c_str(), "rb");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    const std::string k(line);
    std::string v(eq + 1);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    g.prefs[k] = v;
  }
  fclose(f);
}
static void prefsSave() {
  eng::State& g = eng::state();
  FILE* f = fopenSafe(g.prefsPath.c_str(), "wb");
  if (!f) return;
  for (std::map<std::string, std::string>::iterator it = g.prefs.begin(); it != g.prefs.end(); ++it) {
    fprintf(f, "%s=%s\n", it->first.c_str(), it->second.c_str());
  }
  fclose(f);
}
std::string PlayerPrefs::GetString(const std::string& k, const std::string& def) {
  prefsEnsureLoaded();
  std::map<std::string, std::string>::iterator it = eng::state().prefs.find(k);
  return it != eng::state().prefs.end() ? it->second : def;
}
int PlayerPrefs::GetInt(const std::string& k, int def) {
  prefsEnsureLoaded();
  std::map<std::string, std::string>::iterator it = eng::state().prefs.find(k);
  return it != eng::state().prefs.end() ? atoi(it->second.c_str()) : def;
}
float PlayerPrefs::GetFloat(const std::string& k, float def) {
  prefsEnsureLoaded();
  std::map<std::string, std::string>::iterator it = eng::state().prefs.find(k);
  return it != eng::state().prefs.end() ? (float)atof(it->second.c_str()) : def;
}
void PlayerPrefs::SetString(const std::string& k, const std::string& v) {
  prefsEnsureLoaded();
  eng::state().prefs[k] = v;
  prefsSave();
}
void PlayerPrefs::SetInt(const std::string& k, int v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", v);
  PlayerPrefs::SetString(k, buf);
}
void PlayerPrefs::SetFloat(const std::string& k, float v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%g", (double)v);
  PlayerPrefs::SetString(k, buf);
}
void PlayerPrefs::Save() { prefsSave(); } // explicit Unity flush; setters already persist
bool PlayerPrefs::HasKey(const std::string& k) {
  prefsEnsureLoaded();
  return eng::state().prefs.count(k) != 0;
}

// ---- Camera ----------------------------------------------------------------------
Camera* Camera::main() {
  eng::State& g = eng::state();
  if (g.camMainCachedAtFrame == Time::frameCount) return g.camMainCache; // per-frame memo
  g.camMainCachedAtFrame = Time::frameCount;
  g.camMainCache = 0;
  for (size_t i = 0; i < eng::liveGameObjects().size(); i++) {
    GameObject* go = eng::liveGameObjects()[i];
    if (!go->activeSelf || go->tag != "MainCamera") continue;
    for (size_t j = 0; j < go->components.size(); j++) {
      Camera* cam = dynamic_cast<Camera*>(go->components[j]);
      if (cam && cam->enabled) { g.camMainCache = cam; return cam; }
    }
  }
  return 0;
}

Ray Camera::ScreenPointToRay(const Vec3& screenPos) const {
  // Unity: fov is vertical; NDC y is up; the center pixel ray is straight ahead
  const int w = eng::state().screenW;
  const int h = eng::state().screenH;
  const float ndcx = (screenPos.x / (float)w) * 2.0f - 1.0f;
  const float ndcy = (screenPos.y / (float)h) * 2.0f - 1.0f;
  const float aspect = (float)w / (float)h;
  const float tanHalf = tanf(fov * kDeg2Rad * 0.5f);
  Ray r;
  r.origin = transform ? transform->position : Vec3::zero();
  r.direction = transform ? normalized(transform->rotation.rotate(Vec3(ndcx * tanHalf * aspect, ndcy * tanHalf, 1.0f)))
                          : Vec3(0.0f, 0.0f, 1.0f);
  return r;
}

// ---- uGUI selection / events -------------------------------------------------------
EventSystem* EventSystem::current() { return eng::state().esCurrent; }
GameObject* EventSystem::currentSelectedGameObject() const { return eng::state().selectedGO; }
void Selectable::Select() { eng::state().selectedGO = gameObject; }
void Selectable::OnSelect(EventSystem* e) { (void)e; eng::state().selectedGO = gameObject; }

void UnityEvent::Invoke() {
  std::vector<UnityAction*> snapshot = listeners; // listeners may mutate during invoke
  for (size_t i = 0; i < snapshot.size(); i++) if (snapshot[i]) snapshot[i]->Invoke();
}
void UnityEvent::AddListener(UnityAction* a) { if (a) listeners.push_back(a); }

void InputField::ActivateInputField() {
  // Unity TMP_InputField.ActivateInputField: this field becomes the caret/focus owner --- TMP
  // uses it to decide which field receives typed characters. Keep one focus (the pad is a
  // single-field UI today); C# never calls Deactivate here (the pad closes by canvas, and the
  // next Activate re-points).
  eng::state().inputFocus = this;
}

void InputField::SetText(const std::string& value) {
  text = value;
  // TMP_InputField.text setter parity: the value lands in m_TextComponent's label so the pad
  // answer box shows the typed digits (and the focused placeholder swap stays simple).
  const std::unordered_map<bb::InputField*, eng::EngineUiText*>& outer = eng::state().inputFieldLabel;
  std::unordered_map<bb::InputField*, eng::EngineUiText*>::const_iterator it = outer.find(this);
  if (it != outer.end() && it->second) it->second->text = value;
  // TMP parity: the Placeholder graphic shows ONLY while the field is empty (TMP_InputField
  // UpdateLabel toggles it on every text change). Same engine map, resolved beside the label.
  const std::unordered_map<bb::InputField*, eng::EngineUiText*>& ph = eng::state().inputFieldPlaceholder;
  std::unordered_map<bb::InputField*, eng::EngineUiText*>::const_iterator pi = ph.find(this);
  if (pi != ph.end() && pi->second) pi->second->enabled = value.empty();
}

void Animator::SetBool(const char* name, bool value) {
  if (!name) return;
  eng::state().animBools[this][name] = value;  // replace Unity animator params
}
bool Animator::GetBool(const char* name) const {
  if (!name) return false;
  const std::unordered_map<Animator*, std::map<std::string, bool>>& outer = eng::state().animBools;
  std::unordered_map<Animator*, std::map<std::string, bool>>::const_iterator it = outer.find(const_cast<Animator*>(this));
  if (it == outer.end()) return false;
  std::map<std::string, bool>::const_iterator b = it->second.find(name);
  return b != it->second.end() ? b->second : false;
}
void Animator::SetTrigger(const char* name) {
  if (!name) return;
  eng::state().animTriggers[this][name] = true; // latched; the renderer consumes
}

// ---- Sprite-frame animation driver ---------------------------------------------------------------
// The game's Animators animate ONE thing: a SpriteRenderer's m_Sprite via PPtr curves (bake-anims
// tears those into data/anims-index.json). Unity plays the controller's default state on start;
// scripts swap states via SetTrigger/SetBool later (BaldiFeed's talk/frown), which lands in Play().
// stateSpeed AnimatorState.m_Speed scales clip time; BaldiWave's 0.5 is why the greeting wave runs
// 3.3 s, not the clip's nominal 1.65 s. Non-looping clips hold their LAST key (Unity freezes the
// state on the final frame; GameController's ActivateSpoopMode deactivates baldiTutor regardless).

// The bb namespace closes around this block: eng::loadAnims must define at namespace eng scope.
} // namespace bb

namespace {

// Last key at or before t (keys are non-decreasing). Linear scan; the longest clip is 100 keys.
const std::pair<float, std::string>* animKeyAt(const eng::State::SpriteAnimKeys& clip, float t) {
  if (clip.keys.empty()) return 0;
  const std::pair<float, std::string>* hit = &clip.keys[0];
  for (size_t i = 0; i < clip.keys.size(); i++) {
    if (clip.keys[i].first <= t) hit = &clip.keys[i];
    else break;
  }
  return hit;
}

} // namespace

void eng::loadAnims() {
  eng::State& g = eng::state();
  if (g.animsLoaded) return;
  g.animsLoaded = true;                 // set first: a broken bake must not retry every scene load
  bool ok = false;
  const std::string animsText = eng::jsonReadFile("data/anims-index.json", ok);
  if (!ok) { printf("[anims] data/anims-index.json missing -- run: node tools\\bake-anims.js\n"); return; }
  eng::JsonValue root;
  std::string errText;
  if (!eng::jsonParse(animsText, root, errText) || root.type != eng::JsonValue::Object) {
    printf("[anims] anims-index parse failed\n");
    return;
  }
  for (size_t i = 0; i < root.obj.size(); i++) {
    const std::string& name = root.obj[i].first;
    const eng::JsonValue& c = root.obj[i].second;
    State::SpriteAnimKeys clip;
    const eng::JsonValue* len = c.find("length");
    clip.length = len ? len->asFloat(0.0f) : 0.0f;
    clip.loop = c.find("loop") ? c.find("loop")->asBool(false) : false;
    const eng::JsonValue* tracks = c.find("tracks");
    if (tracks && tracks->type == eng::JsonValue::Array) {
      for (size_t k = 0; k < tracks->arr.size(); k++) {
        const eng::JsonValue* keys = tracks->arr[k].find("keys");   // m_Sprite tracks only today
        if (!keys || keys->type != eng::JsonValue::Array) continue;
        for (size_t j = 0; j < keys->arr.size(); j++) {
          const eng::JsonValue* t = keys->arr[j].find("t");
          const eng::JsonValue* guid = keys->arr[j].find("guid");
          if (t && guid && guid->type == eng::JsonValue::String)
            clip.keys.push_back(std::make_pair(t->asFloat(0.0f), guid->str));
        }
      }
    }
    if (!clip.keys.empty()) g.spriteAnims[name] = clip;
  }
  bool ok2 = false;
  const std::string ctrText = eng::jsonReadFile("data/anims-controllers.json", ok2);
  if (!ok2) { printf("[anims] data/anims-controllers.json missing -- run: node tools\\bake-controllers.js\n"); return; }
  eng::JsonValue root2;
  std::string err2;
  if (!eng::jsonParse(ctrText, root2, err2) || root2.type != eng::JsonValue::Object) {
    printf("[anims] anims-controllers parse failed\n");
    return;
  }
  for (size_t i = 0; i < root2.obj.size(); i++) {
    const eng::JsonValue* def = root2.obj[i].second.find("default");
    if (!def) continue;
    const eng::JsonValue* cn = def->find("clip");
    if (!cn || cn->type != eng::JsonValue::String) continue;
    g.controllerClips[root2.obj[i].first] = cn->str;
    const eng::JsonValue* sp = def->find("speed");
    g.controllerSpeeds[root2.obj[i].first] = sp ? sp->asFloat(1.0f) : 1.0f;

    // the graph: states + transitions + Any State + parameters
    const eng::JsonValue& cj = root2.obj[i].second;
    eng::State::AnimCtrlDef cd;
    if (const eng::JsonValue* ds = cj.find("defaultState")) cd.defaultState = ds->asString();
    struct TransReader {
      static void read(const eng::JsonValue* arr, std::vector<eng::State::AnimTrans>& out) {
        if (!arr) return;
        for (int t = 0; t < arr->size(); t++) {
          const eng::JsonValue* tj = arr->at((size_t)t);
          if (!tj) continue;
          eng::State::AnimTrans tr;
          if (const eng::JsonValue* to = tj->find("to")) tr.to = to->asString();
          if (const eng::JsonValue* he = tj->find("hasExitTime")) tr.hasExitTime = he->asBool(false);
          if (const eng::JsonValue* et = tj->find("exitTime")) tr.exitTime = et->asFloat(0.0f);
          if (const eng::JsonValue* cs = tj->find("canTransitionToSelf")) tr.canTransitionToSelf = cs->asBool(true);
          if (const eng::JsonValue* conds = tj->find("conds")) {
            for (int c = 0; c < conds->size(); c++) {
              const eng::JsonValue* cv = conds->at((size_t)c);
              if (!cv || cv->size() < 3) continue;
              eng::State::AnimCond ac;
              ac.mode = cv->at(0)->asInt();
              ac.param = cv->at(1)->asString();
              ac.threshold = cv->at(2)->asFloat();
              tr.conds.push_back(ac);
            }
          }
          if (!tr.to.empty()) out.push_back(tr);
        }
      }
    };
    if (const eng::JsonValue* sts = cj.find("states")) {
      for (size_t k = 0; k < sts->obj.size(); k++) {
        eng::State::AnimStateDef sd;
        const eng::JsonValue& sv = sts->obj[k].second;
        if (const eng::JsonValue* c = sv.find("clip")) sd.clip = c->asString();
        if (const eng::JsonValue* v = sv.find("speed")) sd.speed = v->asFloat(1.0f);
        TransReader::read(sv.find("transitions"), sd.trans);
        cd.states[sts->obj[k].first] = sd;
      }
    }
    TransReader::read(cj.find("anyState"), cd.any);
    if (const eng::JsonValue* ps = cj.find("params")) {
      for (size_t k = 0; k < ps->obj.size(); k++) {
        const eng::JsonValue& pv = ps->obj[k].second;
        const eng::JsonValue* ty = pv.find("type");
        const eng::JsonValue* df = pv.find("def");
        cd.paramTypes[ps->obj[k].first] = ty ? ty->asInt() : 4;
        cd.boolDefaults[ps->obj[k].first] = df ? df->asBool(false) : false;
      }
    }
    g.animControllers[root2.obj[i].first] = cd;
  }
}

namespace bb { // reopened for the Animator sprite-frame driver + the types that follow

static void animEnterState(Animator* an, const eng::State::AnimCtrlDef& cd, const std::string& name) {
  std::map<std::string, eng::State::AnimStateDef>::const_iterator st = cd.states.find(name);
  if (st == cd.states.end()) return;
  an->stateName = name;
  an->clipName = st->second.clip;
  an->clipSpeed = st->second.speed;
  an->clipTime = 0.0f;
  an->clipPlaying = true;
}

void Animator::Play(const char* name) {
  eng::State& g = eng::state();
  if (controllerGuid.empty()) return;
  if (g.controllerClips.empty()) eng::loadAnims();
  std::map<std::string, eng::State::AnimCtrlDef>::const_iterator c = g.animControllers.find(controllerGuid);
  if (c != g.animControllers.end() && name && c->second.states.count(name)) {
    animEnterState(this, c->second, name);   // Unity Animator.Play(stateName)
    return;
  }
  clipName.clear();                         // unknown state: re-resolve the default next Update
  clipPlaying = true;
  clipTime = 0.0f;
}

// Unity condition semantics for the modes this game's controllers use: If (1) is a true bool or a
// set trigger, IfNot (2) a false bool. Numeric modes (3 Greater, 4 Less, 6 Equals, 7 NotEqual)
// have no parameter to read -- no script here calls SetFloat/SetInteger -- and are never met.
static bool animCondMet(Animator* an, const eng::State::AnimCtrlDef& cd, const eng::State::AnimCond& c) {
  eng::State& g = eng::state();
  std::map<std::string, int>::const_iterator ty = cd.paramTypes.find(c.param);
  const int type = ty != cd.paramTypes.end() ? ty->second : 4;
  bool v = false;
  if (type == 9) {
    std::unordered_map<Animator*, std::map<std::string, bool> >::const_iterator t = g.animTriggers.find(an);
    if (t != g.animTriggers.end()) {
      std::map<std::string, bool>::const_iterator b = t->second.find(c.param);
      v = b != t->second.end() && b->second;
    }
  } else if (type == 4) {
    std::map<std::string, bool>::const_iterator d = cd.boolDefaults.find(c.param);
    v = d != cd.boolDefaults.end() && d->second;
    std::unordered_map<Animator*, std::map<std::string, bool> >::const_iterator t = g.animBools.find(an);
    if (t != g.animBools.end()) {
      std::map<std::string, bool>::const_iterator b = t->second.find(c.param);
      if (b != t->second.end()) v = b->second;
    }
  } else {
    return false;
  }
  if (c.mode == 1) return v;
  if (c.mode == 2) return !v;
  return false;
}

static bool animTransitionReady(Animator* an, const eng::State::AnimCtrlDef& cd,
                                const eng::State::AnimTrans& t, float normTime) {
  if (t.hasExitTime && normTime < t.exitTime) return false;
  if (t.conds.empty() && !t.hasExitTime) return false;    // Unity rejects a transition with neither
  for (size_t i = 0; i < t.conds.size(); i++) if (!animCondMet(an, cd, t.conds[i])) return false;
  return true;
}

void Animator::Update(float dt) {
  eng::State& g = eng::state();
  if (clipName.empty()) {
    // lazy bind: the controller table (data/anims-controllers.json) resolves guid -> default clip
    if (controllerGuid.empty()) return;
    if (g.controllerClips.empty()) eng::loadAnims();
    std::map<std::string, std::string>::const_iterator it = g.controllerClips.find(controllerGuid);
    if (it == g.controllerClips.end()) { controllerGuid.clear(); return; }
    clipName = it->second;
    std::map<std::string, float>::const_iterator sp = g.controllerSpeeds.find(controllerGuid);
    clipSpeed = sp != g.controllerSpeeds.end() ? sp->second : 1.0f;
    clipTime = 0.0f;
    clipPlaying = true;
  }
  // STATE MACHINE. Unity evaluates Any State transitions first, then the current state's; the
  // first ready one fires, consuming the triggers its conditions used. Evaluated even after a
  // non-looping clip has finished (Frown/Slap hold their last frame until the next transition).
  std::map<std::string, eng::State::AnimCtrlDef>::const_iterator cdi = g.animControllers.find(controllerGuid);
  if (cdi != g.animControllers.end()) {
    const eng::State::AnimCtrlDef& cd = cdi->second;
    if (stateName.empty()) {
      // bound through the default-clip path above: name the state it is
      stateName = cd.defaultState;
      std::map<std::string, eng::State::AnimStateDef>::const_iterator ds = cd.states.find(stateName);
      if (ds == cd.states.end() || ds->second.clip != clipName) {
        for (ds = cd.states.begin(); ds != cd.states.end(); ++ds)
          if (ds->second.clip == clipName) { stateName = ds->first; break; }
      }
    }
    float normTime = 0.0f;
    std::map<std::string, eng::State::SpriteAnimKeys>::const_iterator ck = g.spriteAnims.find(clipName);
    if (ck != g.spriteAnims.end() && ck->second.length > 0.0f) normTime = clipTime / ck->second.length;
    const eng::State::AnimTrans* fire = 0;
    for (size_t i = 0; i < cd.any.size() && !fire; i++) {
      const eng::State::AnimTrans& t = cd.any[i];
      if (!t.canTransitionToSelf && t.to == stateName) continue;
      if (animTransitionReady(this, cd, t, normTime)) fire = &t;
    }
    std::map<std::string, eng::State::AnimStateDef>::const_iterator cur = cd.states.find(stateName);
    if (!fire && cur != cd.states.end()) {
      for (size_t i = 0; i < cur->second.trans.size() && !fire; i++)
        if (animTransitionReady(this, cd, cur->second.trans[i], normTime)) fire = &cur->second.trans[i];
    }
    if (fire) {
      for (size_t i = 0; i < fire->conds.size(); i++) {
        std::map<std::string, int>::const_iterator ty = cd.paramTypes.find(fire->conds[i].param);
        if (ty != cd.paramTypes.end() && ty->second == 9) g.animTriggers[this].erase(fire->conds[i].param);
      }
      animEnterState(this, cd, fire->to);
    }
  }
  if (!clipPlaying) return;
  std::map<std::string, eng::State::SpriteAnimKeys>::const_iterator it = g.spriteAnims.find(clipName);
  if (it == g.spriteAnims.end()) { clipName.clear(); return; }
  const eng::State::SpriteAnimKeys& clip = it->second;
  // A SINGLE-POSE clip (one key; Baldi_Idle is m_StopTime 1/60, baked length 0) is still sampled:
  // Unity shows its key at t=0 and holds it. Returning early here left the ThinkPad Baldi on
  // whatever frame the previous state (Talk) happened to end on.
  // UNSCALED TIME (updateMode 2) keeps animating while Time.timeScale is 0. The ThinkPad freezes
  // the world with timeScale 0, and BaldiFeed is authored UnscaledTime -- on the scaled dt its Talk
  // clip sat on one frame (traced: state Baldi_Talk, frame 2285a8ec, every frame of the question).
  const float step = updateMode == 2 ? eng::state().dtUnscaled : dt;
  if (clip.length <= 0.0f) clipTime = 0.0f;
  else clipTime += step * (clipSpeed > 0.0f ? clipSpeed : 0.0f);
  if (clip.length <= 0.0f) {
    // nothing to advance; fall through to sample key 0
  } else if (clip.loop) {
    if (clipTime > clip.length) clipTime = fmodf(clipTime, clip.length);
  } else if (clipTime > clip.length) {
    clipTime = clip.length;                 // hold the final frame
    clipPlaying = false;
  }
  const std::pair<float, std::string>* k = animKeyAt(clip, clipTime);
  if (!k || k->second.empty()) return;
  // The frame goes to a SpriteRenderer (world characters) OR a uGUI Image: the ThinkPad's Baldi
  // (BaldiFeed) and the pause menu's Baldi head are Images animated through m_Sprite, and with
  // SpriteRenderer-only output they never changed frame at all.
  SpriteRenderer* sr = GetComponent<SpriteRenderer>();
  Image* im = sr ? 0 : GetComponent<Image>();
  if (!sr && !im) return;
  Sprite* curSprite = sr ? sr->sprite : im->sprite;
  if (curSprite && curSprite->name == k->second) return;
  // Sprite interning is BindCtx::spriteFor at load time; the runtime swap interns the same way so
  // the shell's billboard pass resolves Sprite::name -> data/tex/<guid>.raw unchanged.
  Sprite* s = 0;
  std::unordered_map<std::string, Sprite*>::const_iterator sit = g.sprites.find(k->second);
  if (sit != g.sprites.end()) s = sit->second;
  if (!s) {
    s = new Sprite();
    s->name = k->second;
    eng::noteRuntimeObject(s);
    g.sprites[k->second] = s;
  }
  if (sr) sr->sprite = s;
  else im->sprite = s;
}

// ---- CharacterController (capsule mover: depenetration sweep vs the baked colliders) ------------
// Unity CharacterController.Move semantics: the requested motion is attempted, blockers steer the
// result onto their surfaces (with the tangential motion preserved = sliding), and the component's
// velocity() reports the ACTUAL displacement afterwards. --debug's noclip bit skips resolution
// (raw fly-through); the diag bit still reports the hypothetical contacts at the raw target.
void CharacterController::Move(const Vec3& motion) {
  eng::State& g = eng::state();
  eng::MoveSim& m = g.moveSim[this];
  m.lastDt = g.dtContext > 0.0f ? g.dtContext : eng::kFixedDt;
  Vec3 pos = transform ? transform->position : Vec3::zero();
  const Vec3 start = pos;
  const int flags = eng::debugFlags();
  const bool diag = (flags & eng::kDebugDiag) != 0;
  const bool noclip = (flags & eng::kDebugNoClip) != 0;
  std::vector<eng::CCContact> contacts;
  Vec3 applied = motion;
  // Unity: the entire collision pass is skipped for motions under minMoveDistance (pure jitter);
  // Move is then verbatim. Zero-length steps fall out of ccMoveCollide as `motion` anyway.
  if (motion.magnitude() >= minMoveDistance) {
    if (noclip) {
      if (diag) eng::resolveContactsAt(g, this, pos + motion, &contacts);
    } else {
      bool blocked = false;
      applied = eng::ccMoveCollide(g, this, pos, motion, diag ? &contacts : 0, blocked);
    }
  }
  if (transform) transform->position = pos + applied;
  m.lastMotion = applied;  // ACTUAL displacement: velocity() is Unity-actual; the game scripts
                           // (stamina regen against a wall, sweep/hug ride math) read it as such
  if (diag && !contacts.empty() && bb::Time::frameCount % 25 == 0) {
    const Vec3 end = pos + applied;
    printf("CCDBG f=%d go='%s' pos=(%.2f,%.2f,%.2f) motion=(%.2f,%.2f,%.2f) applied=(%.2f,%.2f,%.2f) contacts=%d\n",
           bb::Time::frameCount,
           gameObject ? gameObject->name.c_str() : "(?)",
           end.x, end.y, end.z,
           motion.x, motion.y, motion.z, applied.x, applied.y, applied.z,
           (int)contacts.size());
    for (size_t i = 0; i < contacts.size() && i < 8; i++) {
      const eng::CCContact& ct = contacts[i];
      printf("CCDBG   hit '%s'%s push=(%.2f,%.2f,%.2f) depth=%.3f",
             ct.collider && ct.collider->gameObject ? ct.collider->gameObject->name.c_str() : "(?)",
             ct.collider && ct.collider->isTrigger ? " [trigger]" : "",
             ct.push.x, ct.push.y, ct.push.z, ct.depth);
      // TEMP: world OBB of the hit so multiple same-name panels are distinguishable
      if (ct.collider && ct.collider->transform) {
        const bb::Transform* tr = ct.collider->transform;
        const bb::Vec3 s = tr->scale, r = tr->position;
        if (const bb::BoxCollider* bxc = dynamic_cast<const bb::BoxCollider*>(ct.collider))
          printf(" ctr=(%.2f,%.2f,%.2f) half=(%.2f,%.2f,%.2f)",
                 r.x, r.y, r.z, bxc->size.x * 0.5f * fabsf(s.x),
                 bxc->size.y * 0.5f * fabsf(s.y), bxc->size.z * 0.5f * fabsf(s.z));
        else if (const bb::MeshCollider* mc = dynamic_cast<const bb::MeshCollider*>(ct.collider))
          printf(" ctr=(%.2f,%.2f,%.2f) surf=%d halfUV=(%.2f,%.2f)x(%.2f,%.2f)",
                 r.x, r.y, r.z, (int)mc->surface,
                 mc->halfU * fabsf(s.x), mc->halfV * fabsf(s.y), s.x, s.y);
      }
      printf("\n");
    }
    fflush(stdout);
  }
}
Vec3 CharacterController::velocity() const {
  eng::State& g = eng::state();
  std::unordered_map<CharacterController*, eng::MoveSim>::const_iterator it = g.moveSim.find(const_cast<CharacterController*>(this));
  if (it == g.moveSim.end()) return Vec3::zero();
  const float dt = it->second.lastDt > 0.0f ? it->second.lastDt : 1.0f;
  return it->second.lastMotion / dt;
}

// ---- Renderer visibility (approx: layer bit + far clip vs active cameras) -----------
bool Renderer::isVisible() const {
  if (!gameObject || !transform) return false;
  eng::State& g = eng::state();
  const int bit = 1 << gameObject->layer;
  for (size_t i = 0; i < g.cameras.size(); i++) {
    Camera* cam = g.cameras[i];
    if (!cam->gameObject || !cam->gameObject->activeSelf) continue;
    if ((cam->cullingMask & bit) == 0) continue;
    if (dist(cam->transform->position, transform->position) <= cam->farClip * 1.05f) return true;
  }
  return false;
}

// ---- NavMeshAgent (straight-line integration; grid A* lands Phase 4) -----------------
Vec3 NavMeshAgent::velocity() const {
  eng::State& g = eng::state();
  std::unordered_map<NavMeshAgent*, eng::AgentSim>::const_iterator it = g.agentSim.find(const_cast<NavMeshAgent*>(this));
  return it != g.agentSim.end() ? it->second.lastStepVel : Vec3::zero();
}
void NavMeshAgent::SetDestination(const Vec3& target) {
  eng::AgentSim& a = eng::state().agentSim[this];
  a.dest = target;
  a.hasDest = true;
}
Vec3 NavMeshAgent::destination() const {
  eng::State& g = eng::state();
  std::unordered_map<NavMeshAgent*, eng::AgentSim>::const_iterator it = g.agentSim.find(const_cast<NavMeshAgent*>(this));
  return it != g.agentSim.end() ? it->second.dest : Vec3::zero();
}
Vec3 NavMeshAgent::steeringTarget() const {   // the next corner of the navmesh path (1st Prize steers by it)
  eng::State& g = eng::state();
  std::unordered_map<NavMeshAgent*, eng::AgentSim>::const_iterator it = g.agentSim.find(const_cast<NavMeshAgent*>(this));
  if (it != g.agentSim.end() && it->second.corner < it->second.corners.size()) return it->second.corners[it->second.corner];
  return destination();
}
// Unity Warp puts the agent ON the navmesh (nearest point to `position`) and its transform at
// surface + baseOffset * scale.y, exactly where a walking agent stands. Setting the raw position
// left the Principal at Principal_DetentionWarpPoint's y 0 -- 2 units into the floor -- and the
// next moving step "jumped" him back up (a 100 u/s velocity spike for one step).
void NavMeshAgent::Warp(const Vec3& position) {
  eng::AgentSim& a = eng::state().agentSim[this];
  if (transform) {
    Vec3 p = position;
    float nx, nz;
    const int np = eng::state().navPolys.empty() ? -1 : eng::navNearest(position.x, position.z, nx, nz);
    if (np >= 0) p = Vec3(nx, eng::navHeight(np) + baseOffset * fabsf(transform->scale.y), nz);
    transform->position = p;
  }
  a.hasDest = false;
  a.planned = false;
  a.corners.clear();
  a.pendingVel = Vec3::zero();
  a.hasPendingVel = false;
  a.lastStepVel = Vec3::zero();
}
void NavMeshAgent::setVelocity(const Vec3& v) {
  eng::AgentSim& a = eng::state().agentSim[this];
  a.pendingVel = v;
  a.hasPendingVel = true; // drives the fixed steps until the next Update sets (or stops setting) it
  a.pendingFrame = Time::frameCount;
}

// ---- PCM decode + pull mixer -----------------------------------------------------------
// Decode: all 127 shipped clips are RIFF/PCM16 mono or stereo at 8k/22.05k/44.1k/48k (python
// scan of the decompile's Assets\AudioClip). One loader handles exactly that; an unexpected
// tag decodes as silence so a future format surprise degrades, never crashes.
void AudioClip::loadPcm() {
  if (pcmLoaded) return;
  pcmLoaded = true; // one attempt even on failure (missing asset stays the documented silent path)
  eng::State& g = eng::state();
  std::unordered_map<bb::AudioClip*, std::string>::const_iterator p = g.clipPath.find(this);
  if (p == g.clipPath.end()) return;
  const std::string full = g.assetRoot + p->second;
  FILE* f = fopenSafe(full.c_str(), "rb");
  if (!f) return;
  unsigned char riff[12];
  if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0) { fclose(f); return; }
  int rate = 0, channels = 0;
  long long dataLen = -1;
  for (;;) {
    unsigned char chunk[8];
    if (fread(chunk, 1, 8, f) != 8) break;
    const unsigned int len = (unsigned int)chunk[4] | ((unsigned int)chunk[5] << 8) |
                             ((unsigned int)chunk[6] << 16) | ((unsigned int)chunk[7] << 24);
    if (memcmp(chunk, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      const unsigned int got = (unsigned int)fread(fmt, 1, sizeof(fmt), f);
      if (len > got && fseek(f, (long)(len - got), SEEK_CUR) != 0) break;
      if (got < 16) break;
      int tag = fmt[0] | (fmt[1] << 8);
      channels = fmt[2] | (fmt[3] << 8);
      rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
      int bits = fmt[14] | (fmt[15] << 8);
      if (tag != 1 || bits != 16 || (channels != 1 && channels != 2)) { fclose(f); return; }
    } else if (memcmp(chunk, "data", 4) == 0) {
      dataLen = len;    // read below; the tail beyond EOF is clamped by fread
      break;
    } else {
      if (fseek(f, (long)len, SEEK_CUR) != 0) break;
    }
  }
  if (dataLen < 0 || rate <= 0) { fclose(f); return; }
  const size_t want = (size_t)(dataLen & ~1LL); // PCM16 frames
  std::vector<unsigned char> raw(want);
  const size_t got = fread(&raw[0], 1, raw.size(), f);
  fclose(f);
  raw.resize(got & ~size_t(1));
  pcm.resize(raw.size() / 2);
  for (size_t i = 0; i < pcm.size(); i++) { // little-endian -> host
    pcm[i] = (int16_t)((unsigned short)raw[i * 2] | ((unsigned short)raw[i * 2 + 1] << 8));
  }
  this->rate = rate;
  this->channels = channels;
  if (getenv("BB_AUDIODBG")) {
    printf("AUDIODBG decode %s: rate=%d ch=%d frames=%d pcm[0..11]:", p->second.c_str(), rate, channels, (int)pcm.size() / channels);
    for (int k = 0; k < 12 && k < (int)pcm.size(); k++) printf(" %d", (int)pcm[k]);
    printf("\n");
  }
}

static const int kMixRate = 44100;

// One voice = one clip segment summed into the accumulation buffer. This shell's semantics:
// the voice started at mix position `start` (output samples), plays at `pitch` speed, position
// advances (clipRate/44100 * pitch) clip samples per output sample, loop-wraps or ends at the
// clip's length. Linear-interp resample is intentional (not bit-perfect Unity), fine for testbed.
static void mixVoice(float* acc, int frames, const bb::AudioClip* clip, double start,
                     double now, float gain, float pitch, bool loop) {
  if (!clip || !clip->pcmLoaded || clip->pcm.empty() || clip->rate <= 0 || clip->channels <= 0) return;
  if (gain <= 0.0f) return;
  const int n = (int)clip->pcm.size();
  const int srcLen = n / clip->channels;      // frames in the clip
  const double step = (double)clip->rate / (double)kMixRate * (pitch > 0.0f ? (double)pitch : 1.0);
  double pos = (now - start) * step;
  if (pos < 0.0f) return;                     // future cue (never happens: stamps use now)
  if (pos >= (double)srcLen) {
    if (!loop) return;
    pos = fmod(pos, (double)srcLen);          // loop voices wrap, Unity-style
  }
  for (int f = 0; f < frames; f++) {
    if (pos >= (double)srcLen) {
      if (!loop) break;
      pos -= (double)srcLen;
    }
    const int i0 = (int)pos;                  // floor, pos >= 0 here
    const int i1 = (i0 + 1 < srcLen) ? i0 + 1 : (loop ? 0 : srcLen - 1);
    const double frac = pos - (double)i0;
    const int c = clip->channels;
    for (int ch = 0; ch < 2; ch++) {          // channels 0/1; mono feeds both
      const double s0 = clip->pcm[(size_t)i0 * c + (c == 2 ? ch : 0)];
      const double s1 = clip->pcm[(size_t)i1 * c + (c == 2 ? ch : 0)];
      acc[(size_t)f * 2 + ch] += (float)((s0 + (s1 - s0) * frac) * gain);
    }
    pos += step;
  }
}

// The output mixer proper: called ONLY from the shell's waveOut feed thread. Advances the mix
// clock by `frames` and emits the LAST `frames` samples of every live voice. Headless/capture
// runs never call this, so they stay silent and behavior-identical to before.
// Mixes the next `frames` stereo frames into `acc` (RAW int16 units, unclamped) and advances the
// mix clock. The two output stages below differ only in how they hand the sum on.
// ONE COPY OF A CLIP PER SOURCE. While a one-shot of a clip is playing on a source, further
// one-shots of the SAME clip on the SAME source are not heard: only the earliest copy sounds.
// Evidence: the user's listening test against the genuine game (2026-10-27). Its game over calls
// audioDevice.PlayOneShot(aud_buzz) every frame for a second, yet the real jumpscare sounds exactly
// like BAL_Screech played ONCE -- the impossible third ThinkPad question plays that same clip once
// and the user hears them as identical. Every stacked variant (newest 1/2/4/8/32, oldest 32)
// sounded different; the port's stack of up to 32 copies was the "faster", harsher jumpscare. The
// Unity engine code behind this was not located (UnityPlayer.dll) -- the rule is matched by ear.
// Different clips, and the same clip on different sources, still overlap as before.
static bool shotIsDuplicate(const eng::AudioSim& s, size_t j) {
  const bb::AudioClip* cl = s.shots[j].clip;
  const double st = s.shots[j].startSamples;
  for (size_t k = 0; k < s.shots.size(); k++) {
    if (k == j || s.shots[k].clip != cl) continue;
    if (s.shots[k].startSamples < st || (s.shots[k].startSamples == st && k < j)) return true;
  }
  return false;
}

static void mixFrames(std::vector<float>& acc, int frames) {
  eng::State& g = eng::state();
  std::lock_guard<std::recursive_mutex> lock(g.audioMutex);   // see State::audioMutex
  const double now0 = (double)g.mixClock.load(std::memory_order_relaxed);
  static const bool dbg = getenv("BB_AUDIODBG") != 0;
  // REAL VOICE LIMIT. ProjectSettings/AudioManager.asset: m_RealVoiceCount 32 -- Unity mixes at
  // most 32 voices and virtualises (silences) the rest. The game-over calls
  // audioDevice.PlayOneShot(aud_buzz) EVERY frame (GameControllerScript.Update), and BAL_Screech
  // is 2.1 s, so ~126 screeches were summed here instead of 32: far past full scale, the harsh
  // "high-pitched" jumpscare. Voices of equal rank (this game never sets AudioSource.priority):
  // the NEWEST 32 are real. Measured on the user's recording of the GENUINE game-over: every 50 ms
  // stretch of it best matches BAL_Screech's first 0-90 ms (the most recent copies), all through
  // the second -- while the old "earliest 32" rule drifted through the clip (0.02 -> 0.21 -> 0.39 s
  // in the port's recording) and stopped re-attacking: the "faster" jumpscare.
  static const size_t kRealVoices = 32;
  std::vector<std::pair<double, std::pair<const bb::AudioSource*, int> > > voices;   // (start, (src, shot or -1))
  for (size_t i = 0; i < g.audioSources.size(); i++) {
    std::unordered_map<bb::AudioSource*, eng::AudioSim>::const_iterator it = g.audioSim.find(g.audioSources[i]);
    if (it == g.audioSim.end() || it->second.paused) continue;
    const eng::AudioSim& s = it->second;
    if (s.playing && g.audioSources[i]->clip && s.mainStart >= 0.0)
      voices.push_back(std::make_pair(s.mainStart, std::make_pair((const bb::AudioSource*)g.audioSources[i], -1)));
    for (size_t j = 0; j < s.shots.size(); j++)
      if (!shotIsDuplicate(s, j))   // a silent duplicate takes no real voice
        voices.push_back(std::make_pair(s.shots[j].startSamples, std::make_pair((const bb::AudioSource*)g.audioSources[i], (int)j)));
  }
  std::set<std::pair<const bb::AudioSource*, int> > realVoices;
  const bool capped = voices.size() > kRealVoices;
  if (capped) {
    std::stable_sort(voices.begin(), voices.end(),
                     [](const std::pair<double, std::pair<const bb::AudioSource*, int> >& a,
                        const std::pair<double, std::pair<const bb::AudioSource*, int> >& b) { return a.first < b.first; });
    for (size_t v = voices.size() - kRealVoices; v < voices.size(); v++) realVoices.insert(voices[v].second);
  }
  for (size_t i = 0; i < g.audioSources.size(); i++) {
    bb::AudioSource* src = g.audioSources[i];
    std::unordered_map<bb::AudioSource*, eng::AudioSim>::const_iterator it = g.audioSim.find(src);
    if (it == g.audioSim.end()) continue;
    const eng::AudioSim& s = it->second;
    if (s.paused || (!s.playing && s.shots.empty())) continue;
    // 3D roll-off: Unity attenuates by distance-to-listener when Spatial Blend says 3D. The
    // cached copy is set by eng::refreshAudioSpatial (main thread, end of the LateUpdate phase),
    // so this thread only reads already-built floats; between-frame staleness is benign. When the
    // first frame has not produced a camera cache yet, the voice lets through at full gain (Unity
    // runs its own AudioListener set up before any Play; ours momentarily has none).
    float att = 1.0f;
    if (src->spatial > 0.0f) {
      // No cache yet = Unity-without-AudioListener = silent (loadScene seeds the cache, so this
      // only guards the pathological pre-load pulls from the live waveOut thread).
      att = g.listenerValid.load(std::memory_order_acquire) != 0
                ? eng::audioRolloffAtten(*src, dist(src->rollPos, g.listenerPos))
                : 0.0f;
    }
    const float base = src->volume * (src->mute ? 0.0f : 1.0f) * att;
    if (dbg) printf("AUDIODBG now=%.0f src=%d vol=%.2f att=%.3f sp=%.1f starts=%.0f shots=%d clip=%s pos=(%.0f,%.0f,%.0f)\n",
                    now0, (int)i, src->volume, att, src->spatial, s.mainStart,
                    (int)s.shots.size(), src->clip ? src->clip->name.c_str() : "-",
                    src->rollPos.x, src->rollPos.y, src->rollPos.z);
    if (s.playing && src->clip && s.mainStart >= 0.0 && (!capped || realVoices.count(std::make_pair((const bb::AudioSource*)src, -1)))) {
      if (!src->clip->pcmLoaded) src->clip->loadPcm();
      mixVoice(&acc[0], frames, src->clip, s.mainStart, now0, base, src->pitch, src->loop);
    }
    for (size_t j = 0; j < s.shots.size(); j++) {
      bb::AudioClip* cl = s.shots[j].clip;
      if (!cl) continue;
      if (capped && !realVoices.count(std::make_pair((const bb::AudioSource*)src, (int)j))) continue;   // virtual
      if (shotIsDuplicate(s, j)) continue;   // same clip already sounding on this source
      if (!cl->pcmLoaded) cl->loadPcm();
      mixVoice(&acc[0], frames, cl, s.shots[j].startSamples, now0,
               base * s.shots[j].gain, src->pitch, false);
    }
  }
  if (dbg) {
    double mx = 0.0, sm = 0.0;
    for (size_t f = 0; f < acc.size(); f++) { mx = mx > acc[f] ? mx : acc[f]; sm += acc[f] > 0.0f ? acc[f] : -acc[f]; }
    printf("AUDIODBG accpeak=%.5f accmean=%.5f voices~=%d\n", mx, sm / acc.size(), (int)acc.size() / frames);
  }
  // mixVoice accumulates in RAW int16 units (sum of voice amplitudes), so the converter clamps
  // directly, without a second 32768 scale -- the first version multiplied the already-scaled
  // sum again and every capture came back pinned DC at +/- full scale.
  g.mixClock.store(now0 + (double)frames, std::memory_order_relaxed);
}

// int16 output (iOS RemoteIO, deterministic --wav-out captures): the sum is clamped here.
void audioPull(int16_t* dst, int frames) {
  eng::State& g = eng::state();
  if (frames <= 0) return;
  if (g.audioHold.load(std::memory_order_relaxed)) {   // scene swap in progress (State::audioHold)
    memset(dst, 0, (size_t)frames * 2 * sizeof(int16_t));
    return;
  }
  std::vector<float> acc((size_t)frames * 2, 0.0f);
  mixFrames(acc, frames);
  for (int f = 0; f < frames * 2; f++) {
    float v = acc[f];
    if (v > 32767.0f) v = 32767.0f;
    else if (v < -32768.0f) v = -32768.0f;
    dst[f] = (int16_t)v;
  }
}

// float output, NOT clamped (Windows): what Unity's FMOD hands the OS. Windows applies the user's
// volume to the float stream before anything is clipped, so a loud sum (the game-over's stack of
// per-frame BAL_Screech copies) comes out clean at a normal volume. Clamping to int16 first -- what
// the shell did -- hard-clipped 72% of the jumpscare into a harsh, shrill square wave, where the
// user's genuine recording peaks at 0.27 with no clipping at all.
void audioPullFloat(float* dst, int frames) {
  eng::State& g = eng::state();
  if (frames <= 0) return;
  if (g.audioHold.load(std::memory_order_relaxed)) {
    memset(dst, 0, (size_t)frames * 2 * sizeof(float));
    return;
  }
  std::vector<float> acc((size_t)frames * 2, 0.0f);
  mixFrames(acc, frames);
  for (int f = 0; f < frames * 2; f++) dst[f] = acc[f] * (1.0f / 32768.0f);
}

// ---- AudioSource playback simulation -------------------------------------------------
bool AudioSource::isPlaying() const {
  eng::State& g = eng::state();
  std::unordered_map<AudioSource*, eng::AudioSim>::const_iterator it = g.audioSim.find(const_cast<AudioSource*>(this));
  if (it == g.audioSim.end()) return false;
  const eng::AudioSim& s = it->second;
  if (!s.shots.empty()) return true;   // PlayOneShot keeps the source auditioning
  return s.playing && clip != 0;       // Unity: paused sources still report isPlaying true
}
void AudioSource::OnEnable() {
  if (playOnAwake && clip) Play();
}
void AudioSource::OnDisable() {
  Stop();                              // Unity stops a source whose object is deactivated
}
void AudioSource::Play() {
  eng::AudioSim& s = eng::state().audioSim[this];
  s.playing = true;
  s.paused = false;
  s.elapsed = 0.0f;
  // Output-mix stamp: the voice begins at the mixer's CURRENT timeline position. The mixer clock
  // only advances in eng::audioPull (the shell's waveOut feed); in headless/capture runs it stays
  // 0, so every stamp is 0 and nothing audible happens -- exactly right for --shot/--frames.
  s.mainStart = (double)eng::state().mixClock.load(std::memory_order_relaxed);
}
void AudioSource::Stop() {
  eng::AudioSim& s = eng::state().audioSim[this];
  s.playing = false;
  s.paused = false;
  s.elapsed = 0.0f;
  s.mainStart = -1.0;
  s.shots.clear();
}
void AudioSource::Pause() {
  eng::AudioSim& s = eng::state().audioSim[this];
  if (s.paused) return;                // already paused: keep the original pause stamp
  s.paused = true;
  s.pauseStamp = (double)eng::state().mixClock.load(std::memory_order_relaxed);
}
void AudioSource::UnPause() {
  eng::AudioSim& s = eng::state().audioSim[this];
  // Unity: UnPause on a source that is NOT paused does nothing. TapePlayerScript.Update calls it
  // EVERY frame while Baldi's antiHearingTime runs -- i.e. all through the payphone's tape -- and
  // the shift below (now - a stale pauseStamp) pushed the tape's start further into the future
  // each frame, so the phone played silence whenever Baldi was awake.
  if (!s.paused) return;
  // Shift the timeline markers by the pause duration so playback resumes where it stopped
  // (the mixer timeline itself never stops -- it is wall time through the device).
  const double now = (double)eng::state().mixClock.load(std::memory_order_relaxed);
  const double shift = now - s.pauseStamp;
  if (s.mainStart >= 0.0) s.mainStart += shift;
  for (size_t j = 0; j < s.shots.size(); j++) s.shots[j].startSamples += shift;
  s.paused = false;
}
void AudioSource::PlayOneShot(AudioClip* clip_, float volumeScale) {
  if (!clip_) return;
  eng::AudioSim& s = eng::state().audioSim[this];
  eng::AudioVoice v(clip_, 0.0f);
  v.startSamples = (double)eng::state().mixClock.load(std::memory_order_relaxed);
  v.gain = volumeScale;
  s.shots.push_back(v);
}

// ---- misc render settings / debug -----------------------------------------------------
void Debug::Log(const char* msg) {
  if (!msg) msg = "";
  printf("%s\n", msg);
  fflush(stdout);
}
void setAmbientLight(const Color& c) { eng::state().ambientLight = c; }

// ---- -debug cheat menu (the bottom-left panel; engine renders, game supplies rows) -----------
// The engine-side machinery (pointer store, hit tests, quad emission) is eng::debugUiFrameImpl +
// eng::buildDebugUiOverlay above; these thin wrappers are the game-facing API.
bool debugUiAvailable() { return (eng::debugFlags() & eng::kDebugUi) != 0; }
void debugUiFrame(DebugUiSink* sink, bool open) { eng::debugUiFrameImpl(sink, open); }
bool cheatNpcIgnore() { return eng::state().cheatNpcIgnore; }
void setCheatNpcIgnore(bool on) { eng::state().cheatNpcIgnore = on; }
void debugUiEsp(bool on, const char* const* gameObjectNames, int count) {
  // "ESP through walls": the billboard pass redraws sprites whose GameObject (root or any
  // ancestor) carries one of these names with depth testing off. Names are copied once per
  // toggle; toggling off clears the list, which makes the pass a complete no-op.
  eng::State& g = eng::state();
  g.debugEspOn = on;
  g.debugEspNames.clear();
  if (on && gameObjectNames)
    for (int i = 0; i < count; i++)
      if (gameObjectNames[i]) g.debugEspNames.push_back(std::string(gameObjectNames[i]));
}
void setSkybox(Material* m) { eng::state().skybox = m; }

// ---- raycast surface (providers in eng-core's eng block) --------------------------------
bool Physics::Raycast(const Ray& ray, RaycastHit* hit) {
  return eng::raycastImpl(ray.origin, ray.direction, hit, 0.0f, -1, QueryTriggerInteraction::UseGlobal);
}
bool Physics::Raycast(const Vec3& origin, const Vec3& direction, RaycastHit* hit,
                      float maxDistance, int layerMask) {
  return eng::raycastImpl(origin, direction, hit, maxDistance, layerMask, QueryTriggerInteraction::UseGlobal);
}
bool Physics::Raycast(const Vec3& origin, const Vec3& direction, RaycastHit* hit,
                      float maxDistance, int layerMask, QueryTriggerInteraction query) {
  return eng::raycastImpl(origin, direction, hit, maxDistance, layerMask, query);
}

} // namespace bb
