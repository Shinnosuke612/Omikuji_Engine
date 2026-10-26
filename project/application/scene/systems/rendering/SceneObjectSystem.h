// 役割: SceneEntityに対応するModel/Spriteの実行時オブジェクトを所有・同期する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"
#include "../../SceneSpawnPresentation.h"
#include "../../../../engine/collision/OBBCollider.h"
#include "../../../../engine/collision/SphereCollider.h"
#include "../../../../engine/math/Vector4.h"
#include "../../../../engine/math/Matrix4x4.h"
#include "../../../../engine/2d/SpriteCommon.h"
#include "../../../../engine/physics/PhysicsBody.h"

class Camera;
class Object3d;
class SceneDocument;
class ScenePhysicsSystem;
class Sprite;

// Scene由来のObject3dとSpriteを一意に所有する。
// BuildBindingsが返すポインタは次のSyncModelsまたはFinalizeまでだけ有効。
class SceneObjectSystem {
public:
	struct ModelRuntime {
		std::unique_ptr<Object3d> object;
		std::string modelPath;
		std::string materialOverrideSignature;
		bool hasRenderer = false;
		bool animatorInitialized = false;
		bool hasAnimator = false;
		bool animatorAutoPlayAllowed = false;
		bool animatorPlayOnStart = true;
		bool animatorLoop = true;
		float animatorSpeed = 1.0f;
		int animatorDefaultClip = 0;
		float animatorTransitionDuration = 0.2f;
		std::string animatorBlendCurve = "SmoothStep";
		OBBCollider boxCollider;
		SphereCollider sphereCollider;
		Collider* collider = nullptr;
		bool hasCollider = false;
		Vector4 colliderDebugColor = { 0.2f, 0.95f, 0.7f, 1.0f };
		bool colliderDebugVisible = true;
		std::string colliderDebugDrawMode = "Wireframe";
		uint32_t colliderDebugSegments = 16;
		PhysicsBody physicsBody;
		bool hasPhysicsBody = false;
		uint64_t spawnActivationSerial = 0;
	};

	SceneObjectSystem();
	~SceneObjectSystem();

	void SyncModels(
		SceneDocument* document,
		ScenePhysicsSystem& physicsSystem,
		float deltaTime,
		bool allowAnimatorAutoPlay,
		bool editing
	);
	void SyncSprites(const SceneDocument* document);
	// Runtime姿勢/再生状態を保持し、初回モデルだけ時刻0を準備する。Animation時計は進めない。
	void SyncModelsForTransition(SceneDocument* document, ScenePhysicsSystem& physicsSystem);
	// Agent・Physics・Environmentへ渡す非所有参照を、Object同期直後に再構築する。
	void BuildBindings(
		SceneDocument& document,
		std::vector<SceneRuntimeObjectBinding>& bindings
	);

	void ApplyRenderCamera(Camera* camera);
	// 値を保持し、各viewの描画定数を作る直前にID/世代から再解決する。
	void SetSpawnPresentationFrame(SceneSpawnFrame frame);
	void PrepareModelDraw() const;
	void DrawModels(
		const SceneDocument& document,
		uint64_t skipEntityId,
		bool hidePlayerModel
	) const;
	void DrawSprites(
		const SceneDocument& document,
		uint64_t skipEntityId
	) const;
	void CollectShadowCasters(
		const SceneDocument& document,
		bool hidePlayerModel,
		std::vector<Object3d*>& shadowCasters
	) const;
	// SyncSpritesの資源を借用して新UIを一要素描く。所有/Authoring値は変更しない。
	// editorViewはUI編集RTへの要求だけtrue。textureを複製せず描画定数の寿命を分ける。
	void DrawUISprite(uint64_t entityId, const Matrix4x4& unitQuadToClip,
		const Vector4& color, SpriteCommon::OutputTarget target, bool editorView = false) const;

	Object3d* FindObject(uint64_t entityId) const;
	Object3d* FindObjectByName(
		const SceneDocument* document,
		const char* name
	) const;
	ModelRuntime* FindModelRuntime(uint64_t entityId);
	const ModelRuntime* FindModelRuntime(uint64_t entityId) const;
	void Finalize();

private:
	void SyncModelsInternal(
		SceneDocument* document,
		ScenePhysicsSystem& physicsSystem,
		float deltaTime,
		bool allowAnimatorAutoPlay,
		bool editing,
		bool advanceAnimation
	);
	struct SpriteRuntime {
		std::unique_ptr<Sprite> sprite;
		std::string texturePath;
	};

	void ClearModels();
	void ApplySpawnPresentation();
	void ClearSprites();

	std::unordered_map<uint64_t, ModelRuntime> models_;
	std::unordered_map<uint64_t, SpriteRuntime> sprites_;
	SceneSpawnFrame spawnFrame_;
	// SyncModelsから次の同期/Finalizeまで借用。Frame自体にはpointerを格納しない。
	const SceneDocument* modelDocument_ = nullptr;
};
