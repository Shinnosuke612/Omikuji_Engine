// 役割: TextRendererをRuntime bitmapへ同期し、ScreenOverlayとScene2Dを描画する。
#include "SceneTextRenderSystem.h"

#include <algorithm>
#include <cstdint>
#include <memory>
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
		const std::string& text
	) {
		std::ostringstream stream;
		stream << text << '\n' << component.textFontFamily << '|'
			<< component.textFontSize << '|' << component.textFontWeight << '|'
			<< component.textFontStyle << '|' << component.textColor.x << ','
			<< component.textColor.y << ',' << component.textColor.z << ','
			<< component.textColor.w << '|' << component.textOpacity << '|'
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
				text && text->textRenderSpace == renderSpace &&
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
		const std::string signature = BuildContentSignature(*component, text);
		if (runtime.contentSignature == signature && runtime.bitmapSize.x > 0.0f) {
			continue;
		}
		TextRasterizer::Bitmap bitmap{};
		if (!rasterizer.Rasterize(
			ToRasterizerSettings(*component, text), bitmap
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
			runtime.bitmapSize = {};
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
		runtime.bitmapSize = {
			static_cast<float>(bitmap.width),
			static_cast<float>(bitmap.height)
		};
	}
	for (auto iterator = texts_.begin(); iterator != texts_.end();) {
		if (!requiredIds.contains(iterator->first)) {
			iterator = texts_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

void SceneTextRenderSystem::DrawScene2D(
	const SceneDocument& document,
	uint32_t width,
	uint32_t height
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
		found->second.sprite->Draw(TextSprite::OutputTarget::SceneHdr);
	}
}

void SceneTextRenderSystem::DrawScreenOverlay(
	const SceneDocument& document,
	uint32_t width,
	uint32_t height
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
		found->second.sprite->Draw(TextSprite::OutputTarget::Display);
	}
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
	texts_.clear();
	textOverrides_.clear();
	presentationOverrides_.clear();
}
