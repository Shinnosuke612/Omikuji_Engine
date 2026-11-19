// 役割: プレイの強化状態と三択sessionを所有し、出所を検証した決定だけを一回適用する。
#include "SceneRunUpgradeSystem.h"
#include "../combat/SceneStatSystem.h"
#include "../../../../../engine/scene/SceneEntityQuery.h"
#include "../../../../../engine/scene/SceneValidator.h"
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace {
	// Healthの既存参照規則と同じく、明示IDの不整合と重複名はfallbackしない。
	const SceneEntity* ResolveHealthTarget(const SceneDocument& document, const SceneEntity& owner, const SceneComponent& health) {
		if (health.healthStatsEntityId) return document.FindEntity(health.healthStatsEntityId);
		if (health.healthStatsEntityName.empty()) return &owner;
		const SceneEntity* result = nullptr;
		for (const auto& entity : document.GetEntities()) if (entity.name == health.healthStatsEntityName) {
			if (result) return nullptr;
			result = &entity;
		}
		return result;
	}
}

void SceneRunUpgradeSystem::BeginRun(uint64_t instance, uint64_t generation, uint64_t directorId, uint64_t playerId) {
	Clear();
	if (!instance || !generation || !directorId || !playerId) return;
	instance_ = instance; generation_ = generation; director_ = directorId; modifiers_.playerEntityId = playerId;
	random_.seed(std::random_device{}());
}
void SceneRunUpgradeSystem::Clear() {
	EndSelection(); lastSelectionSerial_ = 0; random_ = {};
	instance_ = generation_ = director_ = definitionComponent_ = playerComponent_ = 0;
	bound_ = false; modifiers_ = {}; definitions_.clear(); acquired_.clear();
}
void SceneRunUpgradeSystem::EndSelection() {
	token_ = {}; menuSource_ = {}; selectionPanel_ = selectionComponent_ = confirmItem_ = 0;
	slots_ = {}; choices_ = {}; pool_.clear(); selected_ = -1; committed_ = false;
}
std::string SceneRunUpgradeSystem::BeginSelection(const SceneDocument& document, const SceneUpgradeToken& token,
	uint64_t panelId, const std::vector<SceneUpgradePoolEntry>& pool) {
	Sync(document, token.instance);
	if (!bound_ || !token.selectionSerial || token.instance != instance_ || token.generation != generation_ || token.directorId != director_) return "UpgradeSelection:Changed";
	if (selectionPanel_) return token == token_ && panelId == selectionPanel_ && pool == pool_ && ValidateSelection(document, token)
		? std::string{} : "UpgradeSelection:Changed";
	if (token.selectionSerial <= lastSelectionSerial_) return "UpgradeSelection:Changed";
	const auto issue = SceneValidator::UpgradeSelectionIssue(document, panelId, &pool);
	if (!issue.empty()) return issue;
	const auto* settings = SceneEntityQuery::FindEnabledComponent(*document.FindEntity(panelId), "UpgradeSelection");
	if (settings->upgradePlayerEntityId != modifiers_.playerEntityId) return "UpgradeSelection:Player";
	std::vector<SceneUpgradePoolEntry> remaining;
	for (const auto& entry : pool) if (entry.weight > 0) remaining.push_back(entry);
	std::array<SceneRunUpgradeDefinition, 3> choices;
	// 除外しながら抽選するため重複しない。最大重みで正規化して和のoverflowを防ぐ。
	for (auto& choice : choices) {
		const float largest = std::max_element(remaining.begin(), remaining.end(), [](const auto& a, const auto& b) { return a.weight < b.weight; })->weight;
		std::vector<double> weights;
		for (const auto& entry : remaining) weights.push_back(double(entry.weight) / largest);
		const size_t index = std::discrete_distribution<size_t>(weights.begin(), weights.end())(random_);
		choice = *std::find_if(definitions_.begin(), definitions_.end(), [&](const auto& entry) { return entry.id == remaining[index].id; });
		remaining.erase(remaining.begin() + index);
	}
	token_ = token; pool_ = pool; slots_ = settings->upgradeSlots; confirmItem_ = settings->upgradeConfirmItemEntityId;
	choices_ = std::move(choices); selectionPanel_ = panelId; selectionComponent_ = settings->localId;
	lastSelectionSerial_ = token.selectionSerial; selected_ = -1; committed_ = false; return {};
}
bool SceneRunUpgradeSystem::ValidateSelection(const SceneDocument& document, const SceneUpgradeToken& token) {
	// tokenはFlow所有値を借りる。Syncによる自己session破棄後も呼出側tokenを失効照合できる。
	Sync(document, token.instance);
	if (!bound_ || !selectionPanel_ || token != token_ || !SceneValidator::UpgradeSelectionIssue(document, selectionPanel_, &pool_).empty()) return false;
	const auto* settings = SceneEntityQuery::FindEnabledComponent(*document.FindEntity(selectionPanel_), "UpgradeSelection");
	return settings->localId == selectionComponent_ && settings->upgradePlayerEntityId == modifiers_.playerEntityId &&
		settings->upgradeConfirmItemEntityId == confirmItem_ && settings->upgradeSlots == slots_;
}
bool SceneRunUpgradeSystem::BindMenuSource(const SceneMenuSource& source) {
	if (!selectionPanel_ || source.panelId != selectionPanel_ || source.instanceId != instance_ || source.generation != generation_ || !source.openSerial) return false;
	if (menuSource_.openSerial && menuSource_.openSerial != source.openSerial) return false;
	menuSource_ = source; return true;
}
bool SceneRunUpgradeSystem::SourceMatches(const SceneDocument& document, const SceneMenuSource& source) const {
	return source.instanceId == instance_ && source.generation == generation_ && source.revision == document.GetRevision() &&
		source.panelId == selectionPanel_ && source.openSerial && source.openSerial == menuSource_.openSerial;
}
bool SceneRunUpgradeSystem::Select(const SceneDocument& document, const SceneMenuSource& source, const SceneUpgradeToken& token, uint64_t itemId) {
	if (!ValidateSelection(document, token) || !SourceMatches(document, source) || committed_) return false;
	for (size_t i = 0; i < slots_.size(); ++i) if (slots_[i].itemEntityId == itemId) { selected_ = static_cast<int>(i); return true; }
	return false;
}
bool SceneRunUpgradeSystem::TryConfirm(const SceneDocument& document, SceneStatSystem& stats, const SceneMenuSource& source, const SceneUpgradeToken& token, uint64_t itemId) {
	if (!ValidateSelection(document, token) || !SourceMatches(document, source) || committed_ || itemId != confirmItem_ || selected_ < 0) return false;
	// commit以後は成功フラグだけを変更。表示/閉鎖/Flow再開はRuntimeSceneの次段が担当する。
	if (!ApplyDefinition(document, stats, choices_[selected_])) return false;
	committed_ = true; return true;
}
SceneUpgradePresentation SceneRunUpgradeSystem::GetPresentation(uint64_t hoveredItemId) const {
	SceneUpgradePresentation out;
	if (!selectionPanel_) return out;
	for (size_t i = 0; i < slots_.size(); ++i) {
		const auto& slot = slots_[i]; const auto& choice = choices_[i];
		out.texts[slot.summaryTextEntityId] = choice.name; out.texts[slot.titleTextEntityId] = choice.name; out.texts[slot.descriptionTextEntityId] = choice.description;
		out.hiddenRoots.push_back(hoveredItemId == slot.itemEntityId ? slot.summaryTextEntityId : slot.detailRootEntityId);
		if (selected_ != static_cast<int>(i)) out.hiddenRoots.push_back(slot.selectedMarkerEntityId);
		if (committed_) out.disabledItems.push_back(slot.itemEntityId);
	}
	if (selected_ < 0 || committed_) out.disabledItems.push_back(confirmItem_);
	return out;
}
void SceneRunUpgradeSystem::ResetEntity(uint64_t id) {
	if (id == modifiers_.playerEntityId || id == director_) Clear();
}
void SceneRunUpgradeSystem::Sync(const SceneDocument& document, uint64_t instance) {
	if (!modifiers_.playerEntityId) return;
	using namespace SceneEntityQuery;
	const auto* owner = document.FindEntity(modifiers_.playerEntityId);
	const auto* director = document.FindEntity(director_);
	const auto* player = owner ? FindEnabledComponent(*owner, "PlayerBehavior") : nullptr;
	const auto* settings = owner ? FindEnabledComponent(*owner, "RunUpgrade") : nullptr;
	if (instance != instance_ || generation_ != document.GetContentGeneration() || !owner ||
		!player || !settings || !director || !FindEnabledComponent(*director, "GameFlowDirector") ||
		!IsEntityActiveInHierarchy(document, *owner) || !IsEntityActiveInHierarchy(document, *director)) { Clear(); return; }
	std::unordered_set<std::string> ids;
	for (const auto& entry : settings->runUpgradeEntries)
		if (*entry.ValidationIssue() || !ids.insert(entry.id).second) { Clear(); return; }
	if (!bound_) {
		definitions_ = settings->runUpgradeEntries;
		definitionComponent_ = settings->localId; playerComponent_ = player->localId; bound_ = true;
	} else if (definitionComponent_ != settings->localId || playerComponent_ != player->localId ||
		!std::equal(definitions_.begin(), definitions_.end(), settings->runUpgradeEntries.begin(), settings->runUpgradeEntries.end(),
			[](const auto& a, const auto& b) { return a.id == b.id && a.name == b.name && a.description == b.description && a.effect == b.effect && a.value == b.value; })) Clear();
}
bool SceneRunUpgradeSystem::ApplyDefinition(const SceneDocument& document, SceneStatSystem& stats, const SceneRunUpgradeDefinition& definition) {
	Sync(document, instance_);
	if (!bound_ || *definition.ValidationIssue()) return false;
	const auto found = std::find_if(definitions_.begin(), definitions_.end(), [&](const auto& entry) {
		return entry.id == definition.id && entry.effect == definition.effect && entry.value == definition.value;
	});
	if (found == definitions_.end()) return false;
	const auto* owner = document.FindEntity(modifiers_.playerEntityId);
	const auto* health = owner ? SceneEntityQuery::FindEnabledComponent(*owner, "Health") : nullptr;
	if (!health || std::count_if(owner->components.begin(), owner->components.end(),
		[](const auto& c) { return c.enabled && c.type == "Health"; }) != 1) return false;
	const auto* target = ResolveHealthTarget(document, *owner, *health);
	float hp = 0, minimum = 0, maximum = 0;
	if (!target || !SceneEntityQuery::IsEntityActiveInHierarchy(document, *target) ||
		!SceneEntityQuery::FindEnabledComponent(*target, "StatSet") ||
		!stats.TryGet(target->id, health->healthStatId, hp, &minimum, &maximum) ||
		minimum != 0 || hp <= 0 || !std::isfinite(hp) || !std::isfinite(maximum) || maximum <= 0 ||
		stats.IsManagedDead(target->id, health->healthStatId)) return false;
	// 他のHealthとHP参照を共有する不正設定は回復/倍率のどちらも受理しない。
	size_t healthOwners = 0;
	for (const auto& entity : document.GetEntities()) for (const auto& candidate : entity.components) {
		if (!candidate.enabled || candidate.type != "Health" || candidate.healthStatId != health->healthStatId) continue;
		const auto* candidateTarget = ResolveHealthTarget(document, entity, candidate);
		if (candidateTarget && candidateTarget->id == target->id) ++healthOwners;
	}
	if (healthOwners != 1) return false;
	auto next = modifiers_;
	float* multiplier = definition.effect == "Damage" ? &next.values.damage : definition.effect == "MoveSpeed" ? &next.values.moveSpeed :
		definition.effect == "AttackRange" ? &next.values.attackRange : definition.effect == "Knockback" ? &next.values.knockback :
		definition.effect == "Recovery" ? &next.values.recovery : nullptr;
	if (multiplier) {
		const double value = double(*multiplier) * definition.value;
		if (!std::isfinite(value) || value > (std::numeric_limits<float>::max)() || value <= 0) return false;
		*multiplier = static_cast<float>(value);
		if (!next.values.IsValid()) return false;
	}
	auto history = acquired_;
	auto& count = history[definition.id];
	if (count == (std::numeric_limits<uint64_t>::max)()) return false;
	++count;
	if (definition.effect == "Heal") {
		// StatSystemの既存上限clampを使う。加算前overflowも避け、満タン時は0加算で取得可能。
		const double room = (std::max)(0.0, double(maximum) - hp);
		const float amount = static_cast<float>((std::min)(double(definition.value), room));
		if (!stats.Modify(target->id, health->healthStatId, "Add", amount)) return false;
	}
	modifiers_ = next; acquired_.swap(history); return true;
}
