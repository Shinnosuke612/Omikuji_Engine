// 役割: SceneEntityに対応するModel/Spriteの実行時オブジェクトを所有・同期する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../SceneRuntimeObjectBinding.h"
#include "../../SceneSpawnPresentation.h"
#include "../../../../engine/collision/OBBCollider.h"
#include "../../../../engine/collision/SphereCollider.h"
#include "../../../../engine/math/Vector4.h"
#include "../../../../engine/math/Matrix4x4.h"
#include "../../../../engine/2d/SpriteCommon.h"
#include "../../../../engine/physics/PhysicsBody.h"

class SceneReplayHistorySystem;
struct SceneReplayFrame;
struct SceneReplayFramingBasis;
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
		uint64_t replayIncarnation = 0;
		bool replayActive = false;
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
	// 変更対象と子のlocal/worldだけ同期。Animation/Asset/Physics/Pool世代は進めない。
	void RefreshTransforms(const SceneDocument& document, const std::vector<uint64_t>& changedTargets);
	void BuildBindings(
		SceneDocument& document,
		std::vector<SceneRuntimeObjectBinding>& bindings
	);

	// Updateごとに観測し、採取周期の間の再有効化/Pool再利用も別対象として扱う。
	void ObserveReplaySubjects(const SceneDocument& document);
	void ResetReplayEntity(uint64_t id);
	bool EstimateReplayFrameBytes(const SceneDocument& document, uint64_t& out, uint64_t originOwner = 0) const;
	bool CaptureReplaySubjects(const SceneDocument& document, SceneReplayHistorySystem& history, SceneReplayFrame& frame) const;
    // 現在の表示姿勢の対象+モデル子だけ。CPU boundsを返し、Asset読込/Animation更新なし。
    bool TryGetReplayFraming(const SceneDocument&, uint64_t owner, SceneReplayFramingBasis& out) const;
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
	// 現在再生中の姿勢を継続。攻撃対象だけLoopをruntime上で解除する。
	void BeginDeathContinuation(const std::unordered_set<uint64_t>& participants,
		const std::unordered_set<uint64_t>& attackTargets);
	void ClearDeathContinuation();
	void ResetDeathContinuationEntity(uint64_t entityId);
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

	uint64_t nextReplayIncarnation_ = 1;
	std::unordered_map<uint64_t, ModelRuntime> models_;
	std::unordered_map<uint64_t, SpriteRuntime> sprites_;
	SceneSpawnFrame spawnFrame_;
	bool deathContinuation_ = false;
	std::unordered_set<uint64_t> deathParticipants_, deathAttackTargets_;
	// SyncModelsから次の同期/Finalizeまで借用。Frame自体にはpointerを格納しない。
	const SceneDocument* modelDocument_ = nullptr;
};
