// 役割: SceneEntityとRuntime側の描画・衝突・物理オブジェクトを接続する。
#pragma once

#include <cstdint>

class Collider;
class Object3d;
struct PhysicsBody;
struct SceneEntity;

struct SceneRuntimeObjectBinding {
	// Document再配置後もruntime Objectを識別するための安定ID。
	uint64_t entityId = 0;
	SceneEntity* entity = nullptr;
	Object3d* object = nullptr;
	Collider* collider = nullptr;
	PhysicsBody* body = nullptr;
};
