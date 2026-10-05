// 役割: Projectileの速度、追尾、重力、寿命とRuntime破棄を管理する。
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../../SceneRuntimeObjectBinding.h"
#include "../../../../../engine/math/Vector3.h"

class SceneDocument;

class SceneProjectileSystem {
public:
	void Update(
		SceneDocument& document,
		const std::vector<SceneRuntimeObjectBinding>& bindings,
		float deltaTime
	);
	void FlushRemovals(SceneDocument& document);
	void Clear();

private:
	struct ProjectileRuntime {
		Vector3 velocity{};
		float age = 0.0f;
		bool initialized = false;
	};

	std::unordered_map<uint64_t, ProjectileRuntime> runtimes_;
	std::unordered_set<uint64_t> pendingRemovals_;
};
