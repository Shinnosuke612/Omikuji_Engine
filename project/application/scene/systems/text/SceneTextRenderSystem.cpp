// 役割: TextRendererをRuntime bitmapへ同期し、ScreenOverlayとScene2Dを描画する。
#include "SceneTextRenderSystem.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <memory>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../../engine/2d/TextSprite.h"
#include "../../../../engine/2d/TextureManager.h"
#include "../../../../engine/base/DirectXCommon.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneTransformResolver.h"
#include "../../../../engine/scene/SceneUILayout.h"
#include "../../../../engine/text/TextRasterizer.h"

namespace {
	using SceneEntityQuery::FindEnabledComponent;
	using SceneEntityQuery::IsEntityActiveInHierarchy;
	using SceneTransformResolver::ResolveScene2DTransform;

	TextRasterizer::Settings ToRasterizerSettings(
		const SceneComponent& component,
		const std::string& text
	) {
		TextRasterizer::Settings settings{};
		settings.text = text;
		settings.fontFamily = component.textFontFamily;
		settings.fontSize = component.textFontSize;
		settings.bold = component.textFontWeight == "Bold";
		settings.italic = component.textFontStyle == "Italic";
		settings.color = component.textColor;
		settings.opacity = component.textOpacity;
		settings.horizontalAlignment = component.textHorizontalAlignment;
		settings.verticalAlignment = component.textVerticalAlignment;
		settings.wordWrap = component.textWrapMode == "Word";
		settings.overflowMode = component.textOverflowMode;
		settings.layoutSize = component.textLayoutSize;
		settings.characterSpacing = component.textCharacterSpacing;
		settings.lineSpacing = component.textLineSpacing;
		settings.outlineEnabled = component.textOutlineEnabled;
		settings.outlineColor = component.textOutlineColor;
		settings.outlineWidth = component.textOutlineWidth;
		settings.shadowEnabled = component.textShadowEnabled;
		settings.shadowColor = component.textShadowColor;
		settings.shadowOffset = component.textShadowOffset;
		return settings;
	}

	const Text2DPlacement* GetPlacement(const SceneComponent& component) {
		if (!component.textHasPlacementProfiles) {
			return nullptr;
		}
		return component.textRenderSpace == "Scene2D"
			? &component.textScene2DPlacement
			: &component.textOverlayPlacement;
	}

	std::string BuildContentSignature(
		const SceneComponent& component,
		const std::string& text, const Vector4& color
	) {
		std::ostringstream stream;
		stream << std::setprecision(std::numeric_limits<float>::max_digits10);
		stream << text << '\n' << component.textFontFamily << '|'
			<< component.textFontSize << '|' << component.textFontWeight << '|'
			<< component.textFontStyle << '|' << color.x << ','
			<< color.y << ',' << color.z << ','
			<< color.w << '|' << component.textOpacity << '|'
			<< component.textHorizontalAlignment << '|' << component.textVerticalAlignment << '|'
			<< component.textWrapMode << '|' << component.textOverflowMode << '|'
			<< component.textLayoutSize.x << ',' << component.textLayoutSize.y << '|'
			<< component.textCharacterSpacing << '|' << component.textLineSpacing << '|'
			<< component.textOutlineEnabled << '|' << component.textOutlineColor.x << ','
			<< component.textOutlineColor.y << ',' << component.textOutlineColor.z << ','
			<< component.textOutlineColor.w << '|' << component.textOutlineWidth << '|'
			<< component.textShadowEnabled << '|' << component.textShadowColor.x << ','
			<< component.textShadowColor.y << ',' << component.textShadowColor.z << ','
			<< component.textShadowColor.w << '|' << component.textShadowOffset.x << ','
			<< component.textShadowOffset.y;
		return stream.str();
	}

	template <class RuntimeMap>
	std::vector<const SceneEntity*> CollectOrderedEntities(
		const SceneDocument& document,
		const RuntimeMap& texts,
		const char* renderSpace
	) {
		std::vector<const SceneEntity*> result;
		for (const SceneEntity& entity : document.GetEntities()) {
			const SceneComponent* text = FindEnabledComponent(entity, "TextRenderer");
			if (
				text && text->textRenderSpace == renderSpace && !SceneUILayout::IsUIEntity(document, entity) &&
				texts.contains(entity.id) &&
				IsEntityActiveInHierarchy(document, entity)
			) {
				result.push_back(&entity);
			}
		}
		std::stable_sort(result.begin(), result.end(), [](const SceneEntity* left, const SceneEntity* right) {
			const SceneComponent* leftText = FindEnabledComponent(*left, "TextRenderer");
			const SceneComponent* rightText = FindEnabledComponent(*right, "TextRenderer");
			const Text2DPlacement* leftPlacement = GetPlacement(*leftText);
			const Text2DPlacement* rightPlacement = GetPlacement(*rightText);
			const int leftOrder = leftPlacement ? leftPlacement->sortingOrder : leftText->textSortingOrder;
			const int rightOrder = rightPlacement ? rightPlacement->sortingOrder : rightText->textSortingOrder;
			return leftOrder < rightOrder;
		});
		return result;
	}
}

SceneTextRenderSystem::~SceneTextRenderSystem() = default;

void SceneTextRenderSystem::Initialize(DirectXCommon* dxCommon, std::string runtimeKey) {
	dxCommon_ = dxCommon;
	runtimeKey_ = std::move(runtimeKey);
}

void SceneTextRenderSystem::SetTextOverride(uint64_t entityId, std::string text) {
	if (entityId != 0) {
		textOverrides_[entityId] = std::move(text);
	}
}

void SceneTextRenderSystem::ClearTextOverrides() {
	textOverrides_.clear();
}

void SceneTextRenderSystem::SetPresentationOverride(
	uint64_t entityId,
	const Vector2& positionOffset,
	float rotationOffset,
	const Vector2& scaleMultiplier,
	float opacityMultiplier
) {
	if (entityId == 0) {
		return;
	}
	presentationOverrides_[entityId] = {
		positionOffset,
		rotationOffset,
		scaleMultiplier,
		std::clamp(opacityMultiplier, 0.0f, 1.0f)
	};
}

void SceneTextRenderSystem::ClearPresentationOverrides() {
	presentationOverrides_.clear();
}

const SceneTextRenderSystem::PresentationOverride*
SceneTextRenderSystem::FindPresentationOverride(uint64_t entityId) const {
	const auto found = presentationOverrides_.find(entityId);
	return found == presentationOverrides_.end() ? nullptr : &found->second;
}

void SceneTextRenderSystem::Sync(SceneDocument* document) {
	if (!document || !dxCommon_) {
		Finalize();
		return;
	}
	std::unordered_set<uint64_t> requiredIds;
	TextRasterizer rasterizer;
	for (const SceneEntity& entity : document->GetEntities()) {
		const SceneComponent* component = FindEnabledComponent(entity, "TextRenderer");
		if (!component) {
			continue;
		}
		const bool isUI = SceneUILayout::IsUIEntity(*document, entity);
		SceneUILayout::Layout layout;
		if (isUI && (!SceneUILayout::Resolve(*document, entity.id, layout) ||
			!SceneEntityQuery::FindComponent(entity, "UITransform") ||
			layout.size.x > 4096.0f || layout.size.y > 4096.0f)) {
			continue;
		}
		requiredIds.insert(entity.id);
		RuntimeText& runtime = texts_[entity.id];
		if (runtime.textureKey.empty()) {
			runtime.textureKey = "__runtime_text_" + runtimeKey_ + "_" +
				std::to_string(entity.id);
			runtime.sprite = std::make_unique<TextSprite>();
		}
		const auto override = textOverrides_.find(entity.id);
		const std::string& text = override != textOverrides_.end()
			? override->second
			: component->textValue;
		// Rasterizerとsignatureは同じ実効色を使用し、選択中の毎Frame再生成を避ける。
		auto settings = ToRasterizerSettings(*component, text);
		if (const auto found = menuColors_.find(entity.id); found != menuColors_.end()) settings.color = found->second;
		Vector2 staticOffset{};
		if (isUI) {
			settings.layoutSize = layout.size;
			if (const auto* item = SceneEntityQuery::FindComponent(entity, "MenuItem");
				item && (!item->menuLabelEntityId || item->menuLabelEntityId == entity.id)) {
				settings.layoutSize = {layout.size.x - 2 * item->menuTextPadding.x, layout.size.y - 2 * item->menuTextPadding.y};
				staticOffset = {item->menuTextPadding.x + item->menuTextOffset.x, item->menuTextPadding.y + item->menuTextOffset.y};
			}
		}
		std::ostringstream layoutSignature;
		layoutSignature << std::setprecision(std::numeric_limits<float>::max_digits10)
			<< '|' << isUI << '|' << settings.layoutSize.x << ',' << settings.layoutSize.y;
		const std::string signature = BuildContentSignature(*component, text, settings.color) + layoutSignature.str();
		if (runtime.contentSignature == signature && runtime.bitmapSize.x > 0.0f) {
			// offsetだけの変更ではbitmapを再生成せず、描画cacheだけを更新する。
			runtime.uiTextOffset = staticOffset;
			runtime.uiLayoutSize = isUI ? layout.size : Vector2{};
			continue;
		}
		TextRasterizer::Bitmap bitmap{};
		if (!rasterizer.Rasterize(
			settings, bitmap
		)) {
			runtime.bitmapSize = {};
			continue;
		}
		if (!TextureManager::GetInstance()->UpdateTextureFromPixels(
			runtime.textureKey,
			bitmap.bgraPixels.data(),
			bitmap.width,
			bitmap.height
		)) {
			continue;
		}
		if (!runtime.sprite) {
			runtime.sprite = std::make_unique<TextSprite>();
		}
		if (!runtime.spriteInitialized) {
			runtime.sprite->Initialize(dxCommon_, runtime.textureKey);
			runtime.spriteInitialized = true;
		} else {
			runtime.sprite->SetTextureKey(runtime.textureKey);
		}
		runtime.contentSignature = signature;
		runtime.isUI = isUI;
		runtime.uiLayoutSize = isUI ? layout.size : Vector2{};
		runtime.uiTextOffset = staticOffset;
		runtime.layoutOrigin = bitmap.layoutOrigin;
		runtime.bitmapSize = {
			static_cast<float>(bitmap.width),
			static_cast<float>(bitmap.height)
		};
	}
	for (auto iterator = texts_.begin(); iterator != texts_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			if (!iterator->second.textureKey.empty()) {
				TextureManager::GetInstance()->ReleaseTexture(iterator->second.textureKey);
			}
			iterator = texts_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneTextRenderSystem::DrawScene2D(
	const SceneDocument& document,
	uint32_t width,
	uint32_t height,
	bool editorView
) const {
	for (const SceneEntity* entity : CollectOrderedEntities(document, texts_, "Scene2D")) {
		const SceneComponent* component = FindEnabledComponent(*entity, "TextRenderer");
		const auto found = texts_.find(entity->id);
		if (!component || found == texts_.end() || !found->second.sprite ||
			found->second.bitmapSize.x <= 0.0f) {
			continue;
		}
		const Transform transform = ResolveScene2DTransform(document, *entity);
		const Text2DPlacement* placement = GetPlacement(*component);
		const PresentationOverride* motion = FindPresentationOverride(entity->id);
		const Vector2 positionOffset = motion ? motion->positionOffset : Vector2{};
		const float rotationOffset = motion ? motion->rotationOffset : 0.0f;
		const Vector2 scaleMultiplier = motion
			? motion->scaleMultiplier : Vector2{ 1.0f, 1.0f };
		const float opacityMultiplier = motion ? motion->opacityMultiplier : 1.0f;
		const Vector2 basePosition = placement
			? placement->position : Vector2{ transform.translate.x, transform.translate.y };
		const float baseRotation = placement ? placement->rotation : transform.rotate.z;
		const Vector2 baseScale = placement
			? placement->scale : Vector2{ transform.scale.x, transform.scale.y };
		found->second.sprite->Update(
			{ basePosition.x + positionOffset.x, basePosition.y + positionOffset.y },
			baseRotation + rotationOffset,
			{ found->second.bitmapSize.x * baseScale.x * scaleMultiplier.x,
				found->second.bitmapSize.y * baseScale.y * scaleMultiplier.y },
			placement ? placement->pivot : component->textPivot,
			{ 1.0f, 1.0f, 1.0f, 1.0f * opacityMultiplier },
			width,
			height
		);
		found->second.sprite->Draw(TextSprite::OutputTarget::SceneHdr, editorView);
	}
}

void SceneTextRenderSystem::DrawScreenOverlay(
	const SceneDocument& document,
	uint32_t width,
	uint32_t height,
	bool editorView
) const {
	for (const SceneEntity* entity : CollectOrderedEntities(document, texts_, "ScreenOverlay")) {
		const SceneComponent* component = FindEnabledComponent(*entity, "TextRenderer");
		const auto found = texts_.find(entity->id);
		if (!component || found == texts_.end() || !found->second.sprite ||
			found->second.bitmapSize.x <= 0.0f) {
			continue;
		}
		const Transform transform = ResolveScene2DTransform(document, *entity);
		const Text2DPlacement* placement = GetPlacement(*component);
		const PresentationOverride* motion = FindPresentationOverride(entity->id);
		const Vector2 positionOffset = motion ? motion->positionOffset : Vector2{};
		const float rotationOffset = motion ? motion->rotationOffset : 0.0f;
		const Vector2 scaleMultiplier = motion
			? motion->scaleMultiplier : Vector2{ 1.0f, 1.0f };
		const float opacityMultiplier = motion ? motion->opacityMultiplier : 1.0f;
		const Vector2 anchor = placement ? placement->viewportAnchor : component->textViewportAnchor;
		const Vector2 basePosition = placement
			? placement->position : Vector2{ transform.translate.x, transform.translate.y };
		const float baseRotation = placement ? placement->rotation : transform.rotate.z;
		const Vector2 baseScale = placement
			? placement->scale : Vector2{ transform.scale.x, transform.scale.y };
		const Vector2 position{
			anchor.x * static_cast<float>(width) + basePosition.x + positionOffset.x,
			anchor.y * static_cast<float>(height) + basePosition.y + positionOffset.y
		};
		found->second.sprite->Update(
			position,
			baseRotation + rotationOffset,
			{ found->second.bitmapSize.x * baseScale.x * scaleMultiplier.x,
				found->second.bitmapSize.y * baseScale.y * scaleMultiplier.y },
			placement ? placement->pivot : component->textPivot,
			{ 1.0f, 1.0f, 1.0f, 1.0f * opacityMultiplier },
			width,
			height
		);
		found->second.sprite->Draw(TextSprite::OutputTarget::Display, editorView);
	}
}

void SceneTextRenderSystem::DrawUIText(uint64_t entityId, const Matrix4x4& layoutToClip,
	const Vector2& size, const Vector2& pivot, TextSprite::OutputTarget target, bool editorView) const {
	const auto found = texts_.find(entityId);
	if (found == texts_.end()) return;
	const RuntimeText& runtime = found->second;
	if (!runtime.isUI || !runtime.sprite || !runtime.spriteInitialized || runtime.bitmapSize.x <= 0.0f ||
		runtime.uiLayoutSize.x != size.x || runtime.uiLayoutSize.y != size.y) return;
	const PresentationOverride* motion = FindPresentationOverride(entityId);
	const Vector2 offset = motion ? motion->positionOffset : Vector2{};
	const Vector2 scale = motion ? motion->scaleMultiplier : Vector2{ 1.0f, 1.0f };
	const Vector2 pivotPosition{ pivot.x * size.x, pivot.y * size.y };
	// bitmap余白を枠へ戻し、枠の基点で文字演出を合成してから祖先とCanvasへ写す。
	const Matrix4x4 bitmapToLayout = Multiply(
		MakeScaleMatrix({ runtime.bitmapSize.x, runtime.bitmapSize.y, 1.0f }),
			MakeTranslateMatrix({ -runtime.layoutOrigin.x - pivotPosition.x + runtime.uiTextOffset.x,
				-runtime.layoutOrigin.y - pivotPosition.y + runtime.uiTextOffset.y, 0.0f }));
	const Matrix4x4 presentation = MakeAffineMatrix({ scale.x, scale.y, 1.0f },
		Vector3{ 0.0f, 0.0f, motion ? motion->rotationOffset : 0.0f },
		{ pivotPosition.x + offset.x, pivotPosition.y + offset.y, 0.0f });
	const Matrix4x4 matrix = Multiply(Multiply(bitmapToLayout, presentation), layoutToClip);
	for (const auto& row : matrix.m) for (float value : row) if (!std::isfinite(value)) return;
	runtime.sprite->UpdateUI(matrix, { 1.0f, 1.0f, 1.0f, motion ? motion->opacityMultiplier : 1.0f });
	runtime.sprite->Draw(target, editorView);
}

bool SceneTextRenderSystem::HasScreenOverlay(const SceneDocument& document) const {
	for (const SceneEntity* entity : CollectOrderedEntities(document, texts_, "ScreenOverlay")) {
		if (entity) {
			return true;
		}
	}
	return false;
}

void SceneTextRenderSystem::Finalize() {
	menuColors_.clear();
	for (const auto& entry : texts_) {
		if (!entry.second.textureKey.empty()) {
			TextureManager::GetInstance()->ReleaseTexture(entry.second.textureKey);
		}
	}
	texts_.clear();
	textOverrides_.clear();
	presentationOverrides_.clear();
}
