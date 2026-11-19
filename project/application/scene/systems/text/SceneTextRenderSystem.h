// 役割: TextRenderer ComponentのRuntime texture cacheと2D描画空間の振り分けを所有する。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <optional>

#include "../../../../engine/2d/TextSprite.h"
#include "../../../../engine/scene/ScenePreparation.h"
#include "../../../../engine/text/TextRasterizer.h"
#include "../../../../engine/math/Vector2.h"
#include "../../../../engine/math/Vector4.h"

class DirectXCommon;
class SceneDocument;
struct SceneEntity;
struct SceneComponent;
class SceneStatSystem;

class SceneTextRenderSystem {
public:
	~SceneTextRenderSystem();
	void Initialize(DirectXCommon* dxCommon, std::string runtimeKey);
	void SetTextOverride(uint64_t entityId, std::string text);
	void ClearTextOverrides();
	// 三択専用の短命値。空mapで解除してもFlow/Statの表示を消さない。
	void SetUpgradeTextOverrides(const std::unordered_map<uint64_t, std::string>& values) { upgradeTexts_ = values; }
	// Menuの実効文字色だけを置換する。保存値/content/Pose overrideは変更しない。
	void SetMenuColorOverrides(const std::unordered_map<uint64_t, Vector4>& colors) { menuColors_ = colors; }
	void SetPresentationOverride(
		uint64_t entityId,
		const Vector2& positionOffset,
		float rotationOffset,
		const Vector2& scaleMultiplier,
		float opacityMultiplier
	);
	void ClearPresentationOverrides();
	// statsは呼出し中だけ借用。nullは編集用initial/max、非nullはRuntime読取のみ。
	void Sync(SceneDocument* document, const SceneStatSystem* stats = nullptr);
	RuntimePrepareStep StepPreparedText(const SceneDocument&, const SceneRuntimePrepareBudget&, std::string&);
	bool TryTakePreparedImage(PreparedSceneTextImage&);
	bool AcceptPublishedImage(uint64_t, const std::string&, std::string&);
	void DrawScene2D(const SceneDocument& document, uint32_t width, uint32_t height, bool editorView = false) const;
	void DrawScreenOverlay(const SceneDocument& document, uint32_t width, uint32_t height, bool editorView = false) const;
	bool HasScreenOverlay(const SceneDocument& document) const;
	// Sync済みUI文字を借用描画する。行列は文章枠の制作座標→clipで、画面倍率を含めない。
	// editorViewはUI編集RTへの描画時だけtrue。通常Viewとは描画Bufferを分ける。
	void DrawUIText(uint64_t entityId, const Matrix4x4& layoutToClip,
		const Vector2& size, const Vector2& pivot, TextSprite::OutputTarget target, bool editorView = false) const;
	void Finalize();

private:
	struct RuntimeText {
		std::unique_ptr<TextSprite> sprite;
		std::string textureKey;
		std::string contentSignature;
		Vector2 bitmapSize{};
		Vector2 layoutOrigin{};
		Vector2 uiLayoutSize{};
		Vector2 uiTextOffset{}; // 外側枠は変更せず、文字だけを内側へ配置する。
		bool isUI = false;
		bool spriteInitialized = false;
	};
	struct PresentationOverride {
		Vector2 positionOffset{};
		float rotationOffset = 0.0f;
		Vector2 scaleMultiplier = { 1.0f, 1.0f };
		float opacityMultiplier = 1.0f;
	};
	void ResolveRasterSettings(const SceneDocument*, const SceneEntity&, const SceneComponent*, const SceneStatSystem*,
		bool, Vector2, TextRasterizer::Settings&, Vector2&, std::string&) const;
	std::optional<PreparedSceneTextImage> preparedImage_;
	uint64_t pendingPreparedEntity_ = 0;
	bool imagePublished_ = false;
	size_t preparedTextCursor_ = 0;

	const PresentationOverride* FindPresentationOverride(
		uint64_t entityId
	) const;

	DirectXCommon* dxCommon_ = nullptr;
	std::string runtimeKey_;
	std::unordered_map<uint64_t, RuntimeText> texts_;
	std::unordered_map<uint64_t, std::string> textOverrides_;
	std::unordered_map<uint64_t, std::string> upgradeTexts_;
	std::unordered_map<uint64_t, Vector4> menuColors_;
	std::unordered_map<uint64_t, PresentationOverride> presentationOverrides_;
};
