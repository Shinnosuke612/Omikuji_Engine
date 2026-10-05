// 役割: BoneAttachment EntityをAnimation後のJoint World Matrixへ接続する。
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"

class SceneDocument;
class SceneObjectSystem;

class SceneAttachmentSystem {
public:
	void Update(
		SceneDocument& document,
		SceneObjectSystem& objectSystem,
		const std::vector<SceneRuntimeObjectBinding>& bindings
	);
	void Clear(SceneObjectSystem* objectSystem = nullptr);

private:
	std::unordered_set<uint64_t> attachedEntityIds_;
};
