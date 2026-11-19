// 役割: Menu参照・選択・描画packetと、開いているPause rootの停止理由を検証する。
#include "SceneMenuSystem.h"
#include "../../../../engine/scene/SceneDocument.h"
#include "../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../engine/scene/SceneUILayout.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace {
	using namespace SceneEntityQuery;
	uint64_t Nearest(const SceneDocument& doc, uint64_t id, const char* type) {
		auto* e = doc.FindEntity(id);
		for (size_t depth = 0; e && depth <= doc.GetEntities().size(); ++depth) {
			if (FindComponent(*e, type)) return e->id;
			e = doc.FindEntity(e->parentId);
		}
		return 0;
	}
	bool Within(const SceneDocument& doc, uint64_t id, uint64_t root) {
		auto* e = doc.FindEntity(id);
		for (size_t depth = 0; e && depth <= doc.GetEntities().size(); ++depth) {
			if (e->id == root) return true;
			e = doc.FindEntity(e->parentId);
		}
		return false;
	}
	bool Visible(const SceneDocument& doc, uint64_t id) {
		SceneUILayout::Layout layout;
		return SceneUILayout::Resolve(doc, id, layout) && layout.visible && layout.renderSpace == "ScreenOverlay";
	}
	bool ColorValid(const Vector4& c) {
		return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(c.w) &&
			c.x >= 0 && c.x <= 1 && c.y >= 0 && c.y <= 1 && c.z >= 0 && c.z <= 1 && c.w >= 0 && c.w <= 1;
	}
}
void SceneMenuSystem::Clear() {
	hiddenVisualRoots_.clear(); disabledItemIds_.clear();
	ResetPointer();
	frame_ = {}; order_.clear(); eligible_.clear(); initialRejected_ = false;
	stack_.clear(); pulses_.clear(); diagnostic_.clear(); initialized_ = false;
}
bool SceneMenuSystem::Matches(const SceneDocument& doc, uint64_t instanceId) const {
	return frame_.instanceId == instanceId && frame_.generation == doc.GetContentGeneration() && frame_.revision == doc.GetRevision();
}
bool SceneMenuSystem::RefreshPanel(const SceneDocument& doc, uint64_t instanceId, uint64_t panelId, bool opening) {
	const auto* owner = doc.FindEntity(panelId);
	if (!owner || !FindEnabledComponent(*owner, "MenuPanel") || !Visible(doc, panelId) || Nearest(doc, owner->parentId, "MenuPanel")) return false;
	const bool newPanel = opening;
	const auto oldOrder = order_;
	const uint64_t oldSelected = frame_.selectedItemId;
	frame_.instanceId = instanceId; frame_.generation = doc.GetContentGeneration(); frame_.revision = doc.GetRevision();
	frame_.panelId = panelId; frame_.textColors.clear(); frame_.hiddenMarkerRoots.clear();
	order_.clear(); eligible_.clear();
	const auto& panel = *FindEnabledComponent(*doc.FindEntity(panelId), "MenuPanel");
	// 重複登録は保存一覧を補正せず、操作候補から除外する。
	std::unordered_map<uint64_t, size_t> registrations, markers;
	for (const auto& e : doc.GetEntities()) {
		if (const auto* p = FindComponent(e, "MenuPanel")) for (auto id : p->menuItemEntityIds) ++registrations[id];
		if (const auto* item = FindComponent(e, "MenuItem")) if (item->menuSelectionMarkerEntityId) ++markers[item->menuSelectionMarkerEntityId];
	}
	for (auto id : panel.menuItemEntityIds) {
		const auto* e = doc.FindEntity(id);
		if (!e || registrations[id] != 1 || Nearest(doc, e->parentId, "MenuPanel") != panelId) continue;
		const auto* item = FindComponent(*e, "MenuItem");
		if (!item) continue;
		order_.push_back(id);
		if (item->enabled && item->menuInteractable && Visible(doc, id)) eligible_.push_back(id);
	}
	auto eligible = [&](uint64_t id) { return std::find(eligible_.begin(), eligible_.end(), id) != eligible_.end(); };
	if (panel.menuInputMode == "Pointer") {
		initialRejected_ = false;
		frame_.selectedItemId = eligible(hoveredItemId_) ? hoveredItemId_ : 0;
	} else if (newPanel) {
		initialRejected_ = panel.menuInitialItemEntityId && !eligible(panel.menuInitialItemEntityId);
		frame_.selectedItemId = initialRejected_ ? 0 : panel.menuInitialItemEntityId ? panel.menuInitialItemEntityId : eligible_.empty() ? 0 : eligible_.front();
	} else if (!eligible(oldSelected)) {
		frame_.selectedItemId = 0;
		// 削除済み項目でも以前の保存位置から次の生存候補を探す。
		const auto& searchOrder = std::find(order_.begin(), order_.end(), oldSelected) != order_.end() ? order_ : oldOrder;
		const auto found = std::find(searchOrder.begin(), searchOrder.end(), oldSelected);
		if (found != searchOrder.end()) {
			size_t pos = static_cast<size_t>(found - searchOrder.begin());
			for (size_t step = 1; step <= searchOrder.size(); ++step) if (eligible(searchOrder[(pos + step) % searchOrder.size()])) {
				frame_.selectedItemId = searchOrder[(pos + step) % searchOrder.size()]; break;
			}
		}
		if (!frame_.selectedItemId && !eligible_.empty()) frame_.selectedItemId = eligible_.front();
	}
	for (auto id : order_) {
		const auto& item = *FindComponent(*doc.FindEntity(id), "MenuItem");
		const uint64_t labelId = item.menuLabelEntityId ? item.menuLabelEntityId : id;
		const auto* label = doc.FindEntity(labelId);
		if (id == frame_.selectedItemId && item.menuSelectedTextColorEnabled && ColorValid(item.menuSelectedTextColor) &&
			label && Within(doc, labelId, id) && Nearest(doc, labelId, "MenuItem") == id && Nearest(doc, labelId, "MenuPanel") == panelId &&
			FindEnabledComponent(*label, "TextRenderer") && Visible(doc, labelId)) frame_.textColors[labelId] = item.menuSelectedTextColor;
		const uint64_t markerId = item.menuSelectionMarkerEntityId;
		const auto* marker = doc.FindEntity(markerId);
		if (!marker || markerId == id || markers[markerId] != 1 || !Within(doc, markerId, id) ||
			Nearest(doc, markerId, "MenuItem") != id || Nearest(doc, markerId, "MenuPanel") != panelId ||
			Within(doc, labelId, markerId) ||
			(!FindComponent(*marker, "SpriteRenderer") && !FindComponent(*marker, "TextRenderer"))) continue;
		bool containsItemOrLabel = false;
		for (const auto& other : doc.GetEntities()) {
			if (const auto* otherItem = FindComponent(other, "MenuItem")) {
				const uint64_t otherLabel = otherItem->menuLabelEntityId ? otherItem->menuLabelEntityId : other.id;
				if (Within(doc, otherLabel, markerId)) { containsItemOrLabel = true; break; }
			}
			if (!Within(doc, other.id, markerId)) continue;
			if (FindComponent(other, "MenuItem") || FindComponent(other, "MenuPanel")) { containsItemOrLabel = true; break; }
		}
		if (!containsItemOrLabel && id != frame_.selectedItemId) frame_.hiddenMarkerRoots.push_back(markerId);
	}
	if (initialRejected_) return false;
	if (panel.menuCancelMode != "Close" && panel.menuCancelMode != "None" && panel.menuCancelMode != "InvokeItem") return false;
	if (panel.menuCancelMode == "InvokeItem" && !eligible(panel.menuCancelItemEntityId)) return false;
	return !eligible_.empty() || panel.menuCancelMode == "Close";
}
void SceneMenuSystem::Apply(const SceneDocument& doc, const SceneMenuMove& move) {
	if (!Matches(doc, move.instanceId) || move.generation != frame_.generation || move.revision != frame_.revision ||
		move.panelId != frame_.panelId || move.previous == move.next || initialRejected_ || eligible_.empty()) return;
	Apply(doc, SceneMenuCommand{GetSource(), move.next ? SceneMenuCommandType::Next : SceneMenuCommandType::Previous, move.panelId});
}

SceneMenuSource SceneMenuSystem::GetSource() const {
	return {frame_.instanceId, frame_.generation, frame_.revision, frame_.panelId, frame_.openSerial};
}
SceneMenuSource SceneMenuSystem::GetOpenSource(uint64_t panelId) const {
	for (const auto& panel : stack_) if (panel.id == panelId)
		return {frame_.instanceId, frame_.generation, frame_.revision, panel.id, panel.serial};
	return {};
}
void SceneMenuSystem::RemoveBranch(const SceneDocument& doc, uint64_t panelId) {
	const auto found = std::find_if(stack_.begin(), stack_.end(), [&](const auto& p) { return p.id == panelId; });
	if (found != stack_.end()) { stack_.erase(found, stack_.end()); Rebuild(doc); }
}
bool SceneMenuSystem::IsSourceValid(const SceneDocument& doc, const SceneMenuSource& source) const {
	if (!Matches(doc, source.instanceId) || source.generation != frame_.generation || source.revision != frame_.revision) return false;
	if (!source.panelId) return source.openSerial == 0;
	return std::any_of(stack_.begin(), stack_.end(), [&](const auto& panel) { return panel.id == source.panelId && panel.serial == source.openSerial; });
}
bool SceneMenuSystem::Selectable(const SceneDocument& doc, uint64_t panelId, uint64_t itemId) const {
	if (std::find(disabledItemIds_.begin(), disabledItemIds_.end(), itemId) != disabledItemIds_.end()) return false;
	const auto* e = doc.FindEntity(itemId);
	const auto* item = e ? FindEnabledComponent(*e, "MenuItem") : nullptr;
	const auto* owner = doc.FindEntity(panelId);
	const auto* panel = owner ? FindEnabledComponent(*owner, "MenuPanel") : nullptr;
	return item && item->menuInteractable && Visible(doc, itemId) && panel && Nearest(doc, e->parentId, "MenuPanel") == panelId &&
		std::find(panel->menuItemEntityIds.begin(), panel->menuItemEntityIds.end(), itemId) != panel->menuItemEntityIds.end();
}
bool SceneMenuSystem::Open(const SceneDocument& doc, uint64_t panelId) {
	if (std::any_of(stack_.begin(), stack_.end(), [&](const auto& p) { return p.id == panelId; })) return true;
	// 設定は複数配置可能だが、同時に開くPause rootは一つ。stack変更前に拒否する。
	std::string pauseError;
	const auto* pause = SceneGamePauseSystem::FindSettings(doc, panelId, pauseError);
	if (!pauseError.empty()) { diagnostic_ = pauseError; return false; }
	if (pause) for (const auto& open : stack_) {
		if (SceneGamePauseSystem::FindSettings(doc, open.id, pauseError)) {
			diagnostic_ = "MenuRuntime:PauseMultiple"; return false;
		}
		if (!pauseError.empty()) { diagnostic_ = pauseError; return false; }
	}
	order_.clear(); frame_.selectedItemId = 0; initialRejected_ = false;
	if (!RefreshPanel(doc, frame_.instanceId, panelId, true)) {
		diagnostic_ = "MenuRuntime:InvalidPanel"; Rebuild(doc); return false;
	}
	stack_.push_back({panelId, nextOpenSerial_++, frame_.selectedItemId, order_});
	Rebuild(doc);
	return true;
}
void SceneMenuSystem::Rebuild(const SceneDocument& doc) {
	SceneMenuFrame combined;
	combined.instanceId = frame_.instanceId; combined.generation = doc.GetContentGeneration(); combined.revision = doc.GetRevision();
	for (size_t i = 0; i < stack_.size(); ++i) {
		auto& entry = stack_[i];
		order_ = entry.order; frame_.selectedItemId = entry.selected; initialRejected_ = false;
		if (!RefreshPanel(doc, combined.instanceId, entry.id, false)) {
			stack_.erase(stack_.begin() + i, stack_.end()); diagnostic_ = "MenuRuntime:InvalidPanel"; break;
		}
		entry.selected = frame_.selectedItemId; entry.order = order_;
		combined.panelId = entry.id; combined.selectedItemId = entry.selected; combined.openSerial = entry.serial;
		combined.visiblePanelIds.push_back(entry.id);
		combined.hiddenMarkerRoots.insert(combined.hiddenMarkerRoots.end(), frame_.hiddenMarkerRoots.begin(), frame_.hiddenMarkerRoots.end());
		combined.textColors.insert(frame_.textColors.begin(), frame_.textColors.end());
	}
	// 無効な上層を除去したFrameも、移動候補は復帰したtopと一致させる。
	if (!stack_.empty()) {
		order_ = stack_.back().order; frame_.selectedItemId = stack_.back().selected; initialRejected_ = false;
		RefreshPanel(doc, combined.instanceId, stack_.back().id, false);
	} else { order_.clear(); eligible_.clear(); initialRejected_ = false; }
	combined.hoveredItemId = combined.selectedItemId == hoveredItemId_ ? hoveredItemId_ : 0;
	combined.hiddenVisualRoots = hiddenVisualRoots_; combined.disabledItemIds = disabledItemIds_;
	for (uint64_t id : disabledItemIds_) {
		const auto* entity = doc.FindEntity(id);
		const auto* item = entity ? FindEnabledComponent(*entity, "MenuItem") : nullptr;
		const auto* label = item ? doc.FindEntity(item->menuLabelEntityId ? item->menuLabelEntityId : id) : nullptr;
		if (const auto* text = label ? FindEnabledComponent(*label, "TextRenderer") : nullptr) {
			auto color = text->textColor; color.w *= 0.35f; combined.textColors[label->id] = color;
		}
	}
	frame_ = std::move(combined);
}
void SceneMenuSystem::SetPresentation(const SceneDocument& document, const std::vector<uint64_t>& hiddenRoots,
	const std::vector<uint64_t>& disabledItems) {
	hiddenVisualRoots_ = hiddenRoots; disabledItemIds_ = disabledItems; Rebuild(document);
}
void SceneMenuSystem::Sync(const SceneDocument& doc, uint64_t instanceId) {
	if (frame_.instanceId != instanceId || frame_.generation != doc.GetContentGeneration()) Clear();
	frame_.instanceId = instanceId; frame_.generation = doc.GetContentGeneration(); frame_.revision = doc.GetRevision();
	if (!initialized_) {
		initialized_ = true;
		uint64_t initial = 0; size_t count = 0;
		for (const auto& e : doc.GetEntities()) {
			const auto* p = FindEnabledComponent(e, "MenuPanel");
			if (p && p->menuInitiallyOpen && Visible(doc, e.id)) { initial = e.id; ++count; }
		}
		if (count == 1) Open(doc, initial);
		else if (count > 1) diagnostic_ = "MenuRuntime:MultipleInitialPanels";
	}
	Rebuild(doc);
}
bool SceneMenuSystem::Apply(const SceneDocument& doc, const SceneMenuCommand& command) {
	if (!IsSourceValid(doc, command.source)) return false;
	if (command.type == SceneMenuCommandType::Open) return Open(doc, command.panelId);
	if (stack_.empty() || command.panelId != frame_.panelId) { diagnostic_ = "MenuRuntime:InvalidPanel"; return false; }
	if (command.type == SceneMenuCommandType::Close) { stack_.pop_back(); Rebuild(doc); return true; }
	const auto source = GetSource();
	auto emit = [&](SceneMenuPulseType type, uint64_t itemId, bool close = false) {
		const bool duplicate = std::any_of(pulses_.begin(), pulses_.end(), [&](const auto& p) {
			return p.type == type && p.itemId == itemId && p.source.panelId == source.panelId && p.source.openSerial == source.openSerial;
		});
		if (!duplicate) pulses_.push_back({source, type, itemId, close});
	};
	if (command.type == SceneMenuCommandType::Cancel) {
		const auto& panel = *FindEnabledComponent(*doc.FindEntity(frame_.panelId), "MenuPanel");
		emit(SceneMenuPulseType::Cancelled, 0, panel.menuCancelMode == "Close");
		if (panel.menuCancelMode == "InvokeItem" && Selectable(doc, source.panelId, panel.menuCancelItemEntityId))
			emit(SceneMenuPulseType::ItemConfirmed, panel.menuCancelItemEntityId);
		return true;
	}
	if (!Selectable(doc, frame_.panelId, frame_.selectedItemId)) return false;
	if (command.type == SceneMenuCommandType::Confirm) { emit(SceneMenuPulseType::ItemConfirmed, frame_.selectedItemId); return true; }
	const auto found = std::find(eligible_.begin(), eligible_.end(), frame_.selectedItemId);
	if (found == eligible_.end()) return false;
	const size_t pos = static_cast<size_t>(found - eligible_.begin());
	stack_.back().selected = eligible_[(pos + (command.type == SceneMenuCommandType::Next ? 1 : eligible_.size() - 1)) % eligible_.size()];
	Rebuild(doc); return true;
}
void SceneMenuSystem::ResetPointer() {
	pointerSource_ = {}; pointerViewportSerial_ = 0;
	hoveredItemId_ = pressedItemId_ = 0;
	pointerArmed_ = pointerWasDown_ = false;
	frame_.hoveredItemId = 0;
}
void SceneMenuSystem::ApplyPointer(const SceneDocument& doc, const SceneMenuPointerFrame& pointer) {
	const auto* owner = doc.FindEntity(frame_.panelId);
	const auto* panel = owner ? FindEnabledComponent(*owner, "MenuPanel") : nullptr;
	if (!pointer.allowed || !panel || panel->menuInputMode != "Pointer" || !Matches(doc, frame_.instanceId)) {
		const bool hadHover = hoveredItemId_ != 0;
		ResetPointer();
		if (hadHover) Rebuild(doc);
		return;
	}
	const auto source = GetSource();
	if (pointerSource_.instanceId != source.instanceId || pointerSource_.generation != source.generation ||
		pointerSource_.revision != source.revision || pointerSource_.panelId != source.panelId ||
		pointerSource_.openSerial != source.openSerial || pointerViewportSerial_ != pointer.viewportSerial) {
		ResetPointer();
		pointerSource_ = source; pointerViewportSerial_ = pointer.viewportSerial;
	}
	hoveredItemId_ = 0;
	if (pointer.inside) {
		const auto groups = SceneUILayout::CollectPaintOrder(doc, "ScreenOverlay", 0, nullptr, source.panelId, &frame_, true);
		for (auto group = groups.rbegin(); group != groups.rend() && !hoveredItemId_; ++group) {
			for (auto node = group->items.rbegin(); node != group->items.rend(); ++node) {
				const uint64_t itemId = Nearest(doc, node->entityId, "MenuItem");
				if (std::find(eligible_.begin(), eligible_.end(), itemId) == eligible_.end() || !Selectable(doc, source.panelId, itemId)) continue;
				const auto* item = doc.FindEntity(itemId);
				if (!item || !FindEnabledComponent(*item, "UITransform")) continue;
				SceneUILayout::Layout bounds;
				// 文字/Markerの絵が大きくても、クリック範囲はMenuItem自身の枠に限定する。
				if (SceneUILayout::Resolve(doc, itemId, bounds) && SceneUILayout::ContainsNormalizedPoint(bounds, pointer.normalizedPosition)) {
					hoveredItemId_ = itemId; break;
				}
			}
		}
	}
	Rebuild(doc);
	if (!pointerArmed_) {
		// 開いた時/復帰時に押されていたボタンは、解放後の新しい押下だけを受け付ける。
		pointerArmed_ = !pointer.leftDown;
	} else if (pointer.leftDown && !pointerWasDown_) {
		pressedItemId_ = hoveredItemId_;
	} else if (!pointer.leftDown && pointerWasDown_) {
		const uint64_t pressed = std::exchange(pressedItemId_, 0);
		if (pressed && pressed == hoveredItemId_ && Selectable(doc, source.panelId, pressed))
			Apply(doc, SceneMenuCommand{source, SceneMenuCommandType::Confirm, source.panelId});
	}
	// 枠外へのdragは、戻ってから離しても新たな押し直しが必要。
	if (pointer.leftDown && pressedItemId_ != hoveredItemId_) pressedItemId_ = 0;
	pointerWasDown_ = pointer.leftDown;
}
std::vector<SceneMenuPulse> SceneMenuSystem::ConsumePulses() { return std::exchange(pulses_, {}); }
void SceneMenuSystem::CompleteCancel(const SceneDocument& doc, const SceneMenuPulse& pulse) {
	if (pulse.closeAfterNotification && IsSourceValid(doc, pulse.source) && frame_.panelId == pulse.source.panelId && frame_.openSerial == pulse.source.openSerial)
		Apply(doc, SceneMenuCommand{pulse.source, SceneMenuCommandType::Close, pulse.source.panelId});
}
std::string SceneMenuSystem::ConsumeDiagnostic() { return std::exchange(diagnostic_, {}); }

const SceneComponent* SceneGamePauseSystem::FindSettings(const SceneDocument& doc, uint64_t panelId, std::string& error) {
	error.clear();
	const SceneComponent* found = nullptr;
	for (const auto& entity : doc.GetEntities()) {
		if (!IsEntityActiveInHierarchy(doc, entity)) continue;
		for (const auto& setting : entity.components) {
			if (setting.type != "GamePause" || !setting.enabled) continue;
			if (setting.gamePausePanelEntityId == panelId) {
				if (found) { error = "MenuRuntime:PauseAmbiguous"; return nullptr; }
				found = &setting;
			}
		}
	}
	if (!found) return nullptr;
	const auto* entity = doc.FindEntity(panelId);
	const auto* panel = entity ? FindEnabledComponent(*entity, "MenuPanel") : nullptr;
	if (!panel || !IsEntityActiveInHierarchy(doc, *entity)) error = "MenuRuntime:PauseInvalidPanel";
	else if (panel->menuInitiallyOpen) error = "MenuRuntime:PauseInitiallyOpen";
	return error.empty() ? found : nullptr;
}
uint64_t SceneGamePauseSystem::Sync(const SceneDocument& doc, const SceneMenuSystem& menus, bool supported, std::string& error) {
	Clear(); error.clear();
	for (const auto id : menus.GetFrame().visiblePanelIds) {
		const auto* setting = FindSettings(doc, id, error);
		if (!error.empty()) return id;
		if (!setting) continue;
		if (!supported) { error = "MenuRuntime:PauseUnsupportedScene"; return id; }
		if (root_.panelId) { error = "MenuRuntime:PauseMultiple"; return id; }
		root_ = menus.GetOpenSource(id); pauseBgm_ = setting->gamePauseBgm;
	}
	return 0;
}
