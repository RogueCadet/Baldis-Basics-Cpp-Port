#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Core\Singleton.cs (pristine decompile v5).
// C# -> C++ notes:
//  - Application.quitting registration became the engine's shutdown hook path
//    (isApplicationQuitting is cleared in Awake and set by the engine on shutdown).
//  - Instantiate/create flow preserved; TypeName() replaces typeof(T).Name
//    (each concrete component supplies a name tag).

namespace bb {

template <class T>
struct Singleton : public Component {
  static T* Instance() {
    if (isApplicationQuitting) return 0;
    if (!instance) {
      instance = FindObjectOfType<T>();
      if (!instance && isPlaying()) instance = CreateInstance();
    }
    return instance;
  }

  static bool TryGetInstance(T*& out) {
    if (isApplicationQuitting) { out = 0; return false; }
    out = Instance();
    return out != 0;
  }

  virtual void Awake() override {
    isApplicationQuitting = false;
    if (instance && instance != static_cast<T*>(this)) {
      Destroy(gameObject);
      return;
    }
    instance = static_cast<T*>(this);
    OnAwake();
    if (!transform->parent) DontDestroyOnLoad(gameObject);
  }

  virtual void OnDestroy() override { instance = 0; }

  virtual void OnAwake() {}

  static bool Exist() { return instance != 0; }

protected:
  static T* CreateInstance() {
    GameObject* obj = new GameObject();
    obj->name = T::TypeName();
    // Unity: every GameObject carries a Transform from construction. This engine's ctor mints
    // none (scene GOs get theirs as serialized loader pairs), so runtime creation wires one here.
    // AddComponent's GO-adoption (noteComponent -> noteGameObject) then notes BOTH the object and
    // its Transform into the lifetime registries. Found via the scene-swap sweep sim check: the
    // self-created DDoL singleton used to outlive swaps with transform == 0, breaking the
    // transforms == gos invariant and UB-ing Singleton::Awake's transform->parent read.
    bb::Transform* t = new bb::Transform();
    t->go = obj;
    obj->transform = t;
    DontDestroyOnLoad(obj);
    return obj->AddComponent<T>();
  }

  static T* instance;
  static bool isApplicationQuitting;
};

template <class T> T* Singleton<T>::instance = 0;
template <class T> bool Singleton<T>::isApplicationQuitting = false;

} // namespace bb
