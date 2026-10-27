// eng-impl.h  -  inline template bodies for the engine surface.
// Included at the bottom of eng.h (inside namespace bb) once all full types exist.
// These need RTTI (dynamic_cast) over the runtime component list  -  C++11-safe, MSVC /GR on.
#pragma once
#include <vector>
// Minimal forward declarations (eng-internal.h holds the full set): this header sits inside the
// eng.h include chain, so the full internal header must be pulled from the ENTRY header only.
namespace eng {
void noteComponent(bb::Component* c);
std::vector<bb::GameObject*>& liveGameObjects();
} // namespace eng

namespace bb {

template <class T> inline T* GetComponent_impl(GameObject* go) {
  if (!go) return 0;
  for (Component* c : go->components) {
    if (T* t = dynamic_cast<T*>(c)) return t;
  }
  return 0;
}

// Specialization: Transform is a standalone struct (not Component-derived), attached directly.
template <> inline Transform* GetComponent_impl<Transform>(GameObject* go) {
  return go ? go->transform : 0;
}

template <class T> inline T* GameObject::AddComponent() {
  T* c = new T();
  c->gameObject = this;
  c->transform = transform;
  components.push_back(c);
  eng::noteComponent(c);
  // Unity: Awake fires IMMEDIATELY when a component is added to an active object (Start stays
  // deferred to the next frame's first-Update window -- State::pendingStarts). This is what
  // Singleton<T>::CreateInstance's self-created DDoL objects rely on for their Awake-time
  // DontDestroyOnLoad + Load() (InputManager). Inactive objects defer it to activation.
  if (activeSelf) c->Awake();
  return c;
}

// Unity FindObjectOfType: first ACTIVE object carrying T (scene creation order).
template <class T> inline T* FindObjectOfType() {
  for (GameObject* go : eng::liveGameObjects()) {
    if (!go->activeSelf) continue;
    for (Component* c : go->components) {
      if (T* t = dynamic_cast<T*>(c)) return t;
    }
  }
  return 0;
}

} // namespace bb
