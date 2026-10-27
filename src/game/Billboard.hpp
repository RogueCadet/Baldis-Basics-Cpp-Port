#pragma once
#include "../eng.h"

// 1:1 translation of Assets\Scripts\Billboard.cs.
namespace bb {

struct Billboard : public Component {
  virtual void Start() override {
    m_Camera = Camera::main();
  }

  virtual void LateUpdate() override {
    // Unity: transform.LookAt(position + cam.rot * Vector3.forward)
    transform->LookAt(transform->position + m_Camera->transform->rotation.rotate(Vec3(0, 0, 1)));
  }

  Camera* m_Camera = 0; // non-serialized in source (private, Start-cached)
};

} // namespace bb
