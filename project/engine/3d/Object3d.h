// 役割: モデルインスタンスのTransform、材質、Animation状態、描画を管理する。
#pragma once
#include "../scene/SceneRuntimePreparation.h"
#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <cassert>
#include <memory>
#include <vector>
#include "../math/Vector2.h"
#include "../math/Vector3.h"
#include "../math/Vector4.h"
#include "../math/Matrix4x4.h"
#include "../math/Transform.h"
#include "Object3dCommon.h"
#include "Skeleton.h"
#include "SkinCluster.h"
#include <d3d12.h> 

class Model;
class Camera;

class Object3d{
private://インナークラス

	//座標変換行列データ
	struct TransformationMatrix{
		Matrix4x4 WVP;
		Matrix4x4 World;
	};

	struct CameraForGPU {
		Vector3 worldPosition;
		float environmentCoefficient;
	};

	struct ShadowTransformationMatrix {
		Matrix4x4 WVP;
	};

	struct Material {
		Vector4 color;
		int32_t enableLighting;
		float emissiveIntensity;
		float padding[2];
		Matrix4x4 uvTransform;
		Vector4 emissiveColor;
		float shininess;
		float dissolveAmount;
		float dissolveEdgeWidth;
		float dissolveNoiseScale;
	};

	struct MaterialSlotResource {
		ID3D12Resource* resource = nullptr;
		Material* data = nullptr;
		Vector4 modelColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool colorOverrideEnabled = false;
		Vector4 colorOverride = { 1.0f, 1.0f, 1.0f, 1.0f };
		D3D12_GPU_DESCRIPTOR_HANDLE textureOverride{};
		std::string textureOverrideKey;
	};

public: //公開メンバ関数
	struct MaterialOverride {
		std::string materialName;
		bool enabled = false;
		bool colorOverrideEnabled = false;
		Vector4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
		std::string texturePath;
	};

	// GPU address/元Objectへの参照を含まない確定表示値。資源cacheは採取から適用まで不変が前提。
	struct ReplayMaterial {
		Vector4 color{}, emissiveColor{};
		Matrix4x4 uvTransform{};
		int32_t enableLighting = 0;
		float emissiveIntensity = 0, shininess = 0;
		float dissolveAmount = 0, dissolveEdgeWidth = 0, dissolveNoiseScale = 0;
		std::string textureKey;
	};
	struct ReplayJoint {
		std::string name;
		int32_t parent = -1;
		QuaternionTransform transform{};
	};
	struct ReplayPose {
		std::string modelKey, environmentKey;
		// root animation/親/出現補正をすべて含む。適用時に再合成しない。
		Matrix4x4 renderWorld{};
		std::vector<ReplayJoint> joints;
		std::vector<ReplayMaterial> materials;
		float environmentCoefficient = 0;
		Object3dCommon::CullMode cullMode = Object3dCommon::CullMode::kBack;
	};
    struct ReplayBounds { Vector3 min{}, max{}; bool valid = false; };
    // GPU readbackなし。cache内の頂点/共通weightからworld姿勢のboundsを計算する。
    static bool ComputeReplayBounds(const ReplayPose& pose, ReplayBounds& out);
	// Update後の確定姿勢だけを値コピー。assetKeyは現在modelのcache key。
	// 未確定Animation/由来不明texture/非有限値はfalse、out不変。時計/GPU dispatchなし。
	// 採取前の保守的なCPU見積り。配列を確保せずoverflow/未解決modelを拒否する。
	bool EstimateReplayPoseBytes(const std::string& assetKey, uint64_t& out) const;
	bool CaptureReplayPose(const std::string& assetKey, ReplayPose& out) const;
	// 同じmodelを設定済みの表示専用Objectで使用。全検証後に自分の描画資源だけへ適用。
	// 適用後の材質/Transform setterと論理World/Joint getterは使わず、Camera/描画だけを使う。
	// 成功後Update/UpdateForCameraは投影のみ更新し、UpdateAnimationは時計を進めない。
	// GPU前frame完了後・当該frameの最初の影描画前に呼ぶ。SetModelで専用状態を解除。
	// texture同keyの差替え検出/資源保持は延期。拒否時は既存表示を保持する。
	bool ApplyReplayPose(const ReplayPose& pose);

	// 新規の表示専用instanceだけ。失敗したinstanceは描画せず破棄する。cacheをLoadしない。
	bool TryInitializeReplay(Object3dCommon* common, const ReplayPose& pose);

	//初期化
	// main専用。公開済み資源を借用し、Readyまで描画しない。
	bool BeginPreparedInitialize(Object3dCommon& common, Camera& candidateCamera, Model* preparedModel, const std::string& publishedEnvironmentKey,
		std::string& diagnostic);
	RuntimePrepareStep StepPreparedInitialize(SceneRuntimePrepareBudget& remaining, std::string& diagnostic);
	bool TrySetPreparedEnvironmentMap(const std::string& publishedKey, float coefficient);
	bool TrySetPreparedMaterialOverrides(const std::vector<MaterialOverride>& resolvedOverrides);
	void Initialize(Object3dCommon* object3dCommon);
	//更新
	void Update();
	void UpdateAnimation(float deltaTime);
	void UpdateForCamera(Camera* camera);
	//描画
	void Draw();
	// この描画だけに適用する画素被覆率。材質と影の状態には保存しない。
	void DrawWithCoverage(float coverage);
	// Editorのstatic mesh虚像専用。通常alpha/Depth test、Depthへの書込みなし。
	void DrawEditorGhost();
	void DrawShadow(const Matrix4x4& lightViewProjection);
	void DrawSkeletonDebug(
		bool drawJointNames = false,
		bool drawJointAxes = true,
		float jointRadius = 0.025f,
		float axisLength = 0.08f
	) const;
	//setter
	void SetModel(Model* model);
	// setter
	void SetScale(const Vector3& scale){ transform.scale = scale; }
	void SetRotate(const Vector3& rotate){
		transform.rotate = rotate;
		transform.useQuaternionRotation = false;
	}
	void SetRotateQuaternion(const Quaternion& rotate);
	void ClearRotateQuaternion() { transform.useQuaternionRotation = false; }
	void SetTranslate(const Vector3& translate){transform.translate = translate;}
	void SetModel(const std::string& filePath);
	void SetCamera(Camera* camera){ this->camera = camera; }
	void SetParent(const Object3d* parent) { parent_ = parent; }
	// ワールド空間の描画補正。論理World/骨参照/親子Transformには適用しない。
	void SetRenderWorldDelta(const Matrix4x4& matrix) { renderWorldDelta_ = matrix; }
	void SetParentMatrixOverride(const Matrix4x4& matrix) {
		parentMatrixOverride_ = matrix;
		hasParentMatrixOverride_ = true;
	}
	void ClearParentMatrixOverride() { hasParentMatrixOverride_ = false; }
	void SetCullMode(Object3dCommon::CullMode cullMode) {
		cullMode_ = cullMode;
	}
	void SetEnvironmentMap(const std::string& textureFilePath, float coefficient);
	void SetEnvironmentCoefficient(float coefficient);
	void SetAnimationEnabled(bool enabled) {
		animationPlayer_.SetEnabled(enabled);
		animationPoseDirty_ = true;
	}
	void SetAnimationPlaying(bool isPlaying) {
		animationPlayer_.SetPlaying(isPlaying);
	}
	void SetAnimationLoop(bool isLooping) { animationPlayer_.SetLooping(isLooping); }
	void SetAnimationSpeed(float speed) { animationPlayer_.SetSpeed(speed); }
	void SetAnimationTime(float time) {
		animationPlayer_.SetTime(time);
		animationPoseDirty_ = true;
	}
	void SetAnimationBlendCurve(AnimationBlendCurve curve) {
		animationPlayer_.SetBlendCurve(curve);
		animationPoseDirty_ = true;
	}
	bool PlayAnimation(size_t clipIndex, float transitionDuration = 0.0f, bool restart = true) {
		const bool played = animationPlayer_.Play(
			clipIndex,
			transitionDuration,
			restart
		);
		animationPoseDirty_ |= played;
		return played;
	}
	void SetColor(const Vector4& color);
	void SetEnableLighting(bool enableLighting);
	void SetEmissive(float intensity, const Vector4& color = { 1.0f, 1.0f, 1.0f, 1.0f });
	void SetDissolve(
		float amount,
		float edgeWidth = 0.08f,
		float noiseScale = 6.0f
	);
	void SetMaterialOverrides(const std::vector<MaterialOverride>& overrides);
	void SetTextureOverride(D3D12_GPU_DESCRIPTOR_HANDLE handle);
	void ClearTextureOverride();
	uint64_t GetTextureOverridePtr() const {
		return textureOverrideHandle_.ptr;
	}

	// getter（参照返しが軽くて安全）
	const Vector3& GetScale() const{return transform.scale;}
	const Vector3& GetRotate() const{return transform.rotate;}
	const Vector3& GetTranslate() const{return transform.translate;}
	const Quaternion& GetRotateQuaternion() const {
		return transform.quaternionRotate;
	}
	bool UsesQuaternionRotation() const {
		return transform.useQuaternionRotation;
	}
	const Transform& GetTransform() const { return transform; }
	Transform& GetTransform() { return transform; }
	const Matrix4x4& GetWorldMatrix() const { return objectWorldMatrix_; }
	bool HasAnimation() const;
	bool IsAnimationEnabled() const { return animationPlayer_.IsEnabled(); }
	bool IsAnimationPlaying() const { return animationPlayer_.IsPlaying(); }
	bool IsAnimationLooping() const { return animationPlayer_.IsLooping(); }
	bool IsAnimationTransitioning() const { return animationPlayer_.IsTransitioning(); }
	float GetAnimationSpeed() const { return animationPlayer_.GetSpeed(); }
	float GetAnimationTime() const { return animationPlayer_.GetTime(); }
	float GetAnimationBlendWeight() const { return animationPlayer_.GetBlendWeight(); }
	float GetAnimationDuration() const;
	size_t GetAnimationClipCount() const;
	size_t GetAnimationClipIndex() const {
		return animationPlayer_.GetCurrentClipIndex();
	}
	const std::string& GetAnimationClipName(size_t clipIndex) const;
	AnimationBlendCurve GetAnimationBlendCurve() const {
		return animationPlayer_.GetBlendCurve();
	}
	const Skeleton* GetSkeleton() const {
		return skeleton_.IsValid() ? &skeleton_ : nullptr;
	}
	bool TryGetJointWorldMatrix(
		const std::string& jointName,
		Matrix4x4& worldMatrix
	) const;
private: //非公開メンバ関数
	void DrawInternal(bool editorGhost, float coverage);

	//座標変換行列用リソース作成関数
	void CreateTransformationMatrixResource();

	void CreateCameraResource();
	void CreateShadowTransformationMatrixResource();
	void CreateMaterialResources();
	void UpdateMaterialResources();
	void UpdateMaterialResource(size_t index);
	void UpdateAnimationPose();
	void DispatchSkinningIfNeeded();
	void UpdateInternal();
private://メンバ変数
	// 通常Objectの既存所有契約には触れず、Replayのraw aliasだけを所有する。
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> replayResources_;
	bool replayPoseActive_ = false;
	int preparedStage_ = 0;
	std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> preparedResources_;
	std::size_t preparedMaterialIndex_ = 0;
	Transform transform;
	Matrix4x4 objectWorldMatrix_ = MakeIdentity4x4();
	Matrix4x4 renderWorldDelta_ = MakeIdentity4x4();
	const Object3d* parent_ = nullptr;
	Matrix4x4 parentMatrixOverride_ = MakeIdentity4x4();
	bool hasParentMatrixOverride_ = false;

	Object3dCommon* object3dCommon = nullptr;


	//バッファリソース
	ID3D12Resource* transformationMatrixResource = nullptr;
	//バッファリソース内のデータおw指すポインタ
	TransformationMatrix* transformationMatrixData = nullptr;
	ID3D12Resource* shadowTransformationMatrixResource = nullptr;
	ShadowTransformationMatrix* shadowTransformationMatrixData = nullptr;

	//見た目用のモデル
	Model* model = nullptr;
	Skeleton skeleton_{};
	std::unique_ptr<SkinCluster> skinCluster_;
	//カメラ
	Camera* camera = nullptr;

	ID3D12Resource* cameraResource = nullptr;
	CameraForGPU* cameraData = nullptr;
	std::vector<MaterialSlotResource> materialSlots_;
	std::vector<ID3D12Resource*> materialResourcesForDraw_;
	std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> materialTexturesForDraw_;
	Vector4 materialColorMultiplier_ = { 1.0f, 1.0f, 1.0f, 1.0f };
	bool materialEnableLighting_ = true;
	float materialEmissiveIntensity_ = 0.0f;
	Vector4 materialEmissiveColor_ = { 1.0f, 1.0f, 1.0f, 1.0f };
	float materialDissolveAmount_ = 0.0f;
	float materialDissolveEdgeWidth_ = 0.08f;
	float materialDissolveNoiseScale_ = 6.0f;
	std::string environmentTextureFilePath_;
	AnimationPlayer animationPlayer_;
	bool animationPoseDirty_ = true;
	// Skinning出力は姿勢が変化した時だけ更新し、同一フレームの各描画パスで共有する。
	bool skinningDispatchPending_ = true;
	D3D12_GPU_DESCRIPTOR_HANDLE textureOverrideHandle_{};
	Object3dCommon::CullMode cullMode_ = Object3dCommon::CullMode::kBack;
};

