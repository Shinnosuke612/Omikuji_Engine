// 役割: Playerの攻撃/被弾の参照解決と共通Inspectorを提供し、変更fieldを検証する。
// Runtimeの時計・物理・入力やAsset保存には依存しない。
#include "PlayerCombatEditor.h"
#include "../../externals/imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace PlayerCombatEditor {
namespace {
const SceneComponent* Component(const SceneEntity* entity, const std::string& type) {
	if (!entity) return nullptr;
	const SceneComponent* result = nullptr;
	for (const auto& c : entity->components) if (c.type == type) {
		if (result) return nullptr;
		result = &c;
	}
	return result;
}
const SceneEntity* ResolveEntity(const SceneDocument& doc, uint64_t id, const std::string& name, bool& ambiguous) {
	if (const auto* entity = doc.FindEntity(id)) return entity;
	const SceneEntity* result = nullptr;
	if (!name.empty()) for (const auto& entity : doc.GetEntities()) if (entity.name == name) {
		if (result) { ambiguous = true; return nullptr; }
		result = &entity;
	}
	return result;
}
const SceneStateParameter* Parameter(const SceneStateDefinition& state, const char* name) {
	for (const auto& p : state.parameters) if (p.name == name) return &p;
	return nullptr;
}
AttackValues Values(const SceneAttackDefinition& a) {
	return { a.windup, a.activeTime, a.recovery, a.forwardDistance, a.sideDistance,
		a.facingRotateAngle, a.motionEasing, a.facingMode, a.facingTargetEntityName, a.facingTargetEntityId };
}
bool Contains(const char* const* values, size_t count, const std::string& value) {
	for (size_t i = 0; i < count; ++i) if (value == values[i]) return true;
	return false;
}
constexpr const char* easingNames[] = { "Linear", "EaseIn", "EaseOut", "EaseInOut", "SmoothStep" };
constexpr const char* facingNames[] = { "FixedAtStart", "InputDirection", "TargetDirection", "RotateByAngle" };
constexpr float degreesPerRadian = 57.29577951308232f;
const char* Text(EditorLanguage l, const char* ja, const char* en) { return SelectEditorText(l, ja, en); }
void Heading(const char* label) {
	ImGui::Spacing();
	const auto position = ImGui::GetCursorScreenPos();
	const float width = (std::max)(1.0f, ImGui::GetContentRegionAvail().x);
	const float height = ImGui::CalcTextSize(label, nullptr, false, width).y;
	ImGui::GetWindowDrawList()->AddRectFilled(position, ImVec2(position.x + width, position.y + height),
		ImGui::GetColorU32(ImVec4(0.25f, 0.43f, 0.53f, 0.3f)));
	// 通常itemで領域を確保し、狭幅でも見出しが折り返すようにする。
	ImGui::TextWrapped("%s", label);
	ImGui::Separator();
}
void Notice(const Issue& issue, EditorLanguage l) {
	if (issue.code == IssueCode::None) return;
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.76f, 0.38f, 1.0f));
	ImGui::TextWrapped("%s", DescribeIssue(issue, l).c_str());
	ImGui::PopStyleColor();
}
bool Number(const char* label, const char* id, float& value, float speed = 0.01f) {
	ImGui::TextWrapped("%s", label);
	ImGui::SetNextItemWidth(-1);
	return ImGui::DragFloat(id, &value, speed, 0, 0, "%.3f");
}
bool StringField(const char* label, const char* id, std::string& value) {
	ImGui::TextWrapped("%s", label);
	std::vector<char> buffer((std::max)(value.size() + 256, size_t(256)), 0);
	std::memcpy(buffer.data(), value.data(), value.size());
	ImGui::SetNextItemWidth(-1);
	if (!ImGui::InputText(id, buffer.data(), buffer.size())) return false;
	value = buffer.data(); return true;
}
bool Choice(const char* label, const char* id, std::string& value,
	const char* const* keys, const char* const* labels, size_t count) {
	ImGui::TextWrapped("%s", label);
	const char* preview = value.c_str();
	for (size_t i = 0; i < count; ++i) if (value == keys[i]) preview = labels[i];
	ImGui::SetNextItemWidth(-1);
	bool changed = false;
	if (ImGui::BeginCombo(id, preview)) {
		for (size_t i = 0; i < count; ++i) {
			ImGui::PushID(keys[i]);
			if (ImGui::Selectable(labels[i], value == keys[i])) { value = keys[i]; changed = true; }
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	return changed;
}
void Link(std::vector<Request>& requests, const char* label, uint64_t actor, uint64_t entity,
	const char* type, Operation operation = Operation::Inspect) {
	if (ImGui::Button(label, ImVec2(-1, 0))) {
		Request r; r.operation = operation; r.subject = { actor, entity, 0, type, {} };
		requests.push_back(std::move(r));
	}
}
bool Descendant(const SceneDocument& doc, const SceneEntity& entity, uint64_t root) {
	const SceneEntity* current = &entity;
	std::unordered_set<uint64_t> seen;
	while (current && seen.insert(current->id).second) {
		if (current->id == root) return true;
		current = doc.FindEntity(current->parentId);
	}
	return false;
}
}

std::string DescribeIssue(const Issue& issue, EditorLanguage l) {
	const char* message = "";
	switch (issue.code) {
	case IssueCode::None: break;
	case IssueCode::Stale: message = Text(l, "設定先が変更されたため適用できません。現在の対象を確認してもう一度操作してください。", "The source changed. Check the current target and retry the operation."); break;
	case IssueCode::Locked: message = Text(l, "対象がロック中、またはFolderです。設定先のロックを解除し、Entityを選んでください。", "The target is locked or a folder. Unlock the source and select an entity."); break;
	case IssueCode::Playing: message = Text(l, "Play中は制作設定の確認のみです。編集するにはPlayを停止してください。", "Authoring settings are read-only during Play. Stop Play to edit."); break;
	case IssueCode::Missing: message = Text(l, "設定先が見つかりません。参照元のEntity・Component・攻撃名を確認してください。", "Source not found. Check the entity, component and attack name in the referring state."); break;
	case IssueCode::Ambiguous: message = Text(l, "同じ名前または型が複数あり編集先を特定できません。元のInspectorで重複を解消してください。", "Duplicate names or components make the source ambiguous. Resolve duplicates in the original Inspector."); break;
	case IssueCode::Competing: message = Text(l, "同じ画面更新中に別の変更があり、この操作は適用していません。もう一度操作してください。", "Another edit was requested in this frame. This operation was not applied; retry it."); break;
	case IssueCode::Legacy: message = Text(l, "AttackSetを使用できないため旧設定の経路です。発動・コンボからStateの設定を開いて確認してください。", "The AttackSet is unavailable; this uses the legacy path. Open the state under Activation / Combo."); break;
	case IssueCode::UnknownAction: message = Text(l, "このActionの攻撃元はここでは解決できません。発動・コンボからStateを確認してください。", "This action's attack source cannot be resolved here. Inspect its state under Activation / Combo."); break;
	case IssueCode::Disabled: message = Text(l, "参照元または設定のComponentが無効です。元のInspectorで有効状態を確認してください。", "The source or settings component is disabled. Check its enabled state in the original Inspector."); break;
	case IssueCode::ReceiverElsewhere: message = Text(l, "被弾の受け手が別Entityです。HurtBoxと受け手を確認し、そのHitReactionを編集してください。", "Hits resolve to another entity. Inspect the HurtBox and edit the receiver's HitReaction."); break;
	case IssueCode::ReceiverMissing: message = Text(l, "被弾の受け手が未接続です。吹き飛び設定は先に追加できますが、接触にはHurtBoxのStatSet参照も必要です。", "The hit receiver is not connected. You may add knockback first, then connect the HurtBox to its StatSet."); break;
	case IssueCode::BodyMissing: message = Text(l, "有効なDynamic PhysicsBodyが必要です。PhysicsBodyの種類と有効状態を確認してください。", "An enabled Dynamic PhysicsBody is required. Check its type and enabled state."); break;
	case IssueCode::FrozenAxes: message = Text(l, "位置が固定された軸は吹き飛びません。PhysicsBodyの軸固定を確認してください。", "Frozen position axes cannot move. Check the PhysicsBody axis locks."); break;
	case IssueCode::HealthMissing: message = Text(l, "有効なHealthがありません。共通の無敵時間を使う場合はHealthの設定を確認してください。", "No enabled Health component. Check Health settings to use shared invulnerability."); break;
	case IssueCode::InvalidTime: message = Text(l, "時間を適用できません。構え・動作・後隙は0以上の有限値にし、合計も有限になるようにしてください。", "Timing was not applied. Windup, action and recovery must be finite, nonnegative, with a finite total."); break;
	case IssueCode::InvalidDistance: message = Text(l, "移動を適用できません。距離と回転角度には有限の数値を指定してください。", "Motion was not applied. Enter finite distances and rotation angles."); break;
	case IssueCode::InvalidReaction: message = Text(l, "被弾設定を適用できません。倍率・時間・怯み値は0以上の有限値にし、耐性の識別名を確認してください。", "Reaction was not applied. Multiplier, times and poise damage must be finite and nonnegative; check the poise identifier."); break;
	case IssueCode::InvalidState: message = Text(l, "その状態は存在しないか名前が重複しています。既存の一意なState、または「切り替えない」を選んでください。", "The state is missing or duplicated. Select a unique existing state or No state change."); break;
	case IssueCode::InvalidTarget: message = Text(l, "向く対象が見つかりません。同じDocumentのEntityを選び直してください。", "Facing target not found. Select an entity in the same document."); break;
	case IssueCode::InvalidChoice: message = Text(l, "選択値を適用できません。現在のプルダウンから選び直してください。", "The selected option cannot be applied. Choose an option from the dropdown."); break;
	case IssueCode::AddFailed: message = Text(l, "吹き飛び設定を追加できません。対象のComponent構成を確認して再操作してください。", "Knockback could not be added. Check the target's components and retry."); break;
	}
	return issue.detail.empty() ? message : std::string(message) + "\n" + issue.detail;
}

AttackModel ResolveAttack(const SceneDocument& doc, uint64_t id, const std::string& name) {
	AttackModel m; m.subject = { 0, id, 0, "AttackSet", name };
	const auto* entity = doc.FindEntity(id);
	const auto* set = Component(entity, "AttackSet");
	if (!entity || !set) { m.issue = { IssueCode::Missing, name }; return m; }
	m.ownerName = entity->name; m.sourcePrefab = entity->prefabSourcePath;
	m.subject.componentId = set->localId;
	const SceneAttackDefinition* found = nullptr;
	for (const auto& a : set->attackDefinitions) if (a.name == name) {
		if (found) { m.issue = { IssueCode::Ambiguous, name }; return m; }
		found = &a;
	}
	if (!found) { m.issue = { IssueCode::Missing, name }; return m; }
	m.values = Values(*found); m.animation = found->animation;
	m.editable = !entity->locked && !entity->folder;
	if (!set->enabled) m.issue = { IssueCode::Disabled, entity->name };
	if (!m.editable) m.issue = { IssueCode::Locked, entity->name };
	for (const auto& candidate : doc.GetEntities()) if (!candidate.folder)
		m.targets.push_back({ candidate.id, candidate.name });
	return m;
}

ReactionModel ResolveReaction(const SceneDocument& doc, uint64_t id) {
	ReactionModel m; m.subject = { 0, id, 0, "HitReaction", {} };
	const auto* entity = doc.FindEntity(id);
	if (!entity) { m.issues.push_back({ IssueCode::Missing, {} }); return m; }
	const auto* c = Component(entity, "HitReaction");
	m.editable = !entity->locked && !entity->folder;
	m.canAdd = m.editable && std::none_of(entity->components.begin(), entity->components.end(),
		[](const auto& value) { return value.type == "HitReaction"; });
	m.exists = c != nullptr;
	if (!m.editable) m.issues.push_back({ IssueCode::Locked, entity->name });
	if (!c && !m.canAdd && m.editable) m.issues.push_back({ IssueCode::Ambiguous, "HitReaction" });
	if (c) {
		m.subject.componentId = c->localId;
		m.values = { c->enabled, c->hitReactionKnockbackMultiplier, c->hitReactionStateDuration,
			c->hitReactionMinimumPoiseDamage, c->hitReactionPoiseRecoveryDelay,
			c->hitReactionTriggerMode, c->hitReactionPoiseStatId, c->hitReactionStateName };
	}
	if (const auto* states = Component(entity, "StateMachine")) {
		for (const auto& state : states->stateMachineStates) m.states.push_back(state.name);
		if (!states->enabled) m.issues.push_back({ IssueCode::Disabled, "StateMachine" });
	}
	const auto* body = Component(entity, "PhysicsBody");
	if (!body || !body->enabled || body->physicsBodyType != "Dynamic") m.issues.push_back({ IssueCode::BodyMissing, {} });
	else if (body->physicsFreezePositionX || body->physicsFreezePositionY || body->physicsFreezePositionZ)
		m.issues.push_back({ IssueCode::FrozenAxes, std::string(body->physicsFreezePositionX ? "X " : "") +
			(body->physicsFreezePositionY ? "Y " : "") + (body->physicsFreezePositionZ ? "Z" : "") });
	const auto* health = Component(entity, "Health");
	if (!health || !health->enabled) m.issues.push_back({ IssueCode::HealthMissing, {} });
	return m;
}

ActorModel ResolveActor(const SceneDocument& doc, uint64_t id) {
	ActorModel m; m.id = id;
	const auto* actor = doc.FindEntity(id);
	if (!actor || !Component(actor, "PlayerBehavior")) return m;
	m.name = actor->name; m.reaction = ResolveReaction(doc, id); m.reaction.subject.actorId = id;
	if (!Component(actor, "PlayerBehavior")->enabled) m.issues.push_back({ IssueCode::Disabled, "PlayerBehavior" });
	const auto* machine = Component(actor, "StateMachine");
	if (machine) {
		if (!machine->enabled) m.issues.push_back({ IssueCode::Disabled, "StateMachine" });
		for (const auto& state : machine->stateMachineStates) {
			const bool melee = state.actionId == "Builtin.MeleeComboAttack";
			const bool spinStart = state.actionId == "Builtin.SpinStartAttack";
			const bool spinLoop = state.actionId == "Builtin.SpinLoopAttack";
			if (!melee && !spinStart && !spinLoop) {
				if (state.actionId != "Builtin.Idle" && state.actionId != "Builtin.Move")
					m.issues.push_back({ IssueCode::UnknownAction, state.name + " / " + state.actionId });
				continue;
			}
			const auto* target = Parameter(state, "AnimationTarget");
			bool ambiguous = false;
			const auto* owner = target ? ResolveEntity(doc, target->entityId, target->entityName, ambiguous) : nullptr;
			// RuntimeのGetStringParameterと同じく、空文字と未登録を区別する。
			const auto* attackParameter = Parameter(state, "AttackName");
			const auto* animation = Parameter(state, "Animation");
			const std::string name = attackParameter ? attackParameter->stringValue :
				melee ? (animation ? animation->stringValue : "") : spinStart ? "SpinStart" : "SpinLoop";
			if (ambiguous) { m.issues.push_back({ IssueCode::Ambiguous, state.name }); continue; }
			const auto* set = Component(owner, "AttackSet");
			if (!owner || !set || !set->enabled) {
				m.issues.push_back({ melee ? IssueCode::Legacy : IssueCode::Missing, state.name + " / " + name }); continue;
			}
			auto found = std::find_if(m.attacks.begin(), m.attacks.end(), [&](const auto& a) {
				return a.subject.entityId == owner->id && a.subject.attackName == name;
			});
			if (found != m.attacks.end()) { found->sourceStates.push_back(state.name); continue; }
			auto a = ResolveAttack(doc, owner->id, name); a.subject.actorId = id; a.sourceStates.push_back(state.name);
			if (melee && a.issue.code == IssueCode::Missing) a.issue.code = IssueCode::Legacy;
			m.attacks.push_back(std::move(a));
		}
	} else m.issues.push_back({ IssueCode::Missing, "StateMachine" });
	bool unresolved = false;
	for (const auto& entity : doc.GetEntities()) {
		const auto* hurt = Component(&entity, "HurtBox");
		if (!hurt || !Descendant(doc, entity, id)) continue;
		m.hurtBoxes.push_back({ entity.id, entity.name });
		bool ambiguous = false;
		const SceneEntity* receiver = ResolveEntity(doc, hurt->hurtBoxStatsEntityId, hurt->hurtBoxStatsEntityName, ambiguous);
		if (ambiguous) { unresolved = true; m.reaction.issues.push_back({ IssueCode::Ambiguous, entity.name }); continue; }
		if (!receiver) {
			receiver = &entity;
			std::unordered_set<uint64_t> seen;
			while (receiver && seen.insert(receiver->id).second) {
				const auto* stats = Component(receiver, "StatSet");
				if (stats && stats->enabled) break;
				receiver = doc.FindEntity(receiver->parentId);
			}
			const auto* stats = Component(receiver, "StatSet");
			if (!stats || !stats->enabled) receiver = nullptr;
		}
		if (!receiver) { unresolved = true; continue; }
		if (std::none_of(m.receivers.begin(), m.receivers.end(), [&](const auto& r) { return r.id == receiver->id; }))
			m.receivers.push_back({ receiver->id, receiver->name });
		if (!hurt->enabled) m.reaction.issues.push_back({ IssueCode::Disabled, entity.name + " / HurtBox" });
	}
	const bool elsewhere = std::any_of(m.receivers.begin(), m.receivers.end(), [id](const auto& r) { return r.id != id; });
	if (elsewhere) {
		m.reaction.canAdd = false; m.reaction.editable = false;
		m.reaction.issues.push_back({ IssueCode::ReceiverElsewhere, {} });
	} else if (unresolved || m.receivers.empty()) m.reaction.issues.push_back({ IssueCode::ReceiverMissing, {} });
	return m;
}

Request DrawAttackFields(const AttackModel& m, EditorLanguage l, bool editable) {
	Request r; r.operation = Operation::EditAttack; r.subject = m.subject; r.attack = m.values;
	ImGui::PushID(m.subject.attackName.c_str());
	Notice(m.issue, l);
	ImGui::BeginDisabled(!editable || !m.editable);
	Heading(Text(l, "時間", "Timing"));
	if (Number(Text(l, "構え（秒）", "Windup (s)"), "##windup", r.attack.windup)) r.fields |= Windup;
	if (Number(Text(l, "動作（秒）", "Action (s)"), "##active", r.attack.activeTime)) r.fields |= ActiveTime;
	if (Number(Text(l, "後隙（秒）", "Recovery (s)"), "##recovery", r.attack.recovery)) r.fields |= Recovery;
	ImGui::TextWrapped(Text(l, "合計 %.3f 秒", "Total %.3f s"), m.values.windup + m.values.activeTime + m.values.recovery);
	Heading(Text(l, "攻撃中の移動", "Attack movement"));
	if (Number(Text(l, "前後距離（＋前／－後ろ）", "Forward distance (+ forward / - backward)"), "##forward", r.attack.forward)) r.fields |= Forward;
	if (Number(Text(l, "左右距離（＋右／－左）", "Side distance (+ right / - left)"), "##side", r.attack.side)) r.fields |= Side;
	const char* easingLabels[] = { Text(l,"一定","Linear"), Text(l,"加速","Ease in"), Text(l,"減速","Ease out"),
		Text(l,"加速して減速","Ease in / out"), Text(l,"なめらかに加減速","Smooth step") };
	if (Choice(Text(l,"加減速","Easing"),"##easing",r.attack.easing,easingNames,easingLabels,5)) r.fields |= Easing;
	const char* facingLabels[] = { Text(l,"開始時の向き","Start facing"), Text(l,"移動入力の向き","Movement input"),
		Text(l,"指定対象の向き","Face target"), Text(l,"指定角度だけ回転","Rotate by angle") };
	if (Choice(Text(l,"向き","Facing"),"##facing",r.attack.facing,facingNames,facingLabels,4)) r.fields |= Facing;
	if (r.attack.facing == "TargetDirection") {
		ImGui::TextWrapped("%s", Text(l,"向く対象","Facing target"));
		std::string label = Text(l,"なし（開始時の向き）","None (start facing)");
		bool found = false;
		for (const auto& e : m.targets) if (e.id == r.attack.targetId) { label = e.name; found = true; break; }
		if (!found && (r.attack.targetId || !r.attack.targetName.empty())) label = r.attack.targetName + " (#" + std::to_string(r.attack.targetId) + ")";
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("##target", label.c_str())) {
			if (ImGui::Selectable(Text(l,"なし（開始時の向き）","None (start facing)"), !r.attack.targetId && r.attack.targetName.empty())) {
				r.attack.targetId = 0; r.attack.targetName.clear(); r.fields |= FacingTarget;
			}
			for (const auto& e : m.targets) {
				const std::string option = e.name + " (#" + std::to_string(e.id) + ")";
				if (ImGui::Selectable(option.c_str(), e.id == r.attack.targetId)) {
					r.attack.targetId = e.id; r.attack.targetName = e.name; r.fields |= FacingTarget;
				}
			}
			ImGui::EndCombo();
		}
	} else if (r.attack.facing == "RotateByAngle") {
		float degrees = m.values.angle * degreesPerRadian;
		if (Number(Text(l,"回転角度（度）","Rotation (degrees)"),"##angle",degrees,0.1f)) {
			r.attack.angle = degrees / degreesPerRadian; r.fields |= Angle;
		}
	}
	ImGui::TextWrapped(Text(l,"移動区間 %.3f → %.3f 秒（構え終了から動作終了）","Movement %.3f → %.3f s (end of windup to end of action)"),
		m.values.windup, m.values.windup + m.values.activeTime);
	if (m.values.activeTime == 0 && (m.values.forward != 0 || m.values.side != 0))
		ImGui::TextWrapped("%s",Text(l,"動作時間が0なので、移動が急激になる場合があります。","Zero action time can produce abrupt movement."));
	ImGui::EndDisabled(); ImGui::PopID();
	if (!r.fields) r.operation = Operation::None;
	return r;
}

Request DrawReactionFields(const ReactionModel& m, EditorLanguage l, bool editable, bool showEnabled) {
	Request r; r.subject = m.subject; r.reaction = m.values; r.operation = Operation::EditReaction;
	for (const auto& issue : m.issues) Notice(issue,l);
	ImGui::BeginDisabled(!editable || !m.editable);
	if (!m.exists) {
		ImGui::TextWrapped("%s",Text(l,"吹き飛びの設定：未追加","Knockback: not configured"));
		ImGui::BeginDisabled(!m.canAdd);
		if (ImGui::Button(Text(l,"吹き飛びを追加###addReaction","Add knockback###addReaction"),ImVec2(-1,0))) r.operation = Operation::AddReaction;
		ImGui::EndDisabled();
		ImGui::TextWrapped("%s",Text(l,"追加内容：倍率1 / 継続0.2秒 / 状態切替なし","Adds: multiplier 1 / duration 0.2 s / no state change"));
	} else {
		if (showEnabled && ImGui::Checkbox(Text(l,"有効###reactionEnabled","Enabled###reactionEnabled"), &r.reaction.enabled)) r.fields |= Enabled;
		if (Number(Text(l,"受ける吹き飛ばし倍率","Received knockback multiplier"),"##multiplier",r.reaction.multiplier)) r.fields |= Multiplier;
		if (Number(Text(l,"水平の吹き飛び時間（秒）","Horizontal knockback duration (s)"),"##duration",r.reaction.duration)) r.fields |= Duration;
		ImGui::TextWrapped(Text(l,"状態切替：%s","State change: %s"),m.values.state.empty() ? Text(l,"なし","None") : m.values.state.c_str());
		const auto stateCount = std::count(m.states.begin(),m.states.end(),m.values.state);
		if (!m.values.state.empty() && stateCount != 1) Notice({IssueCode::InvalidState,m.values.state},l);
		if (ImGui::TreeNode(Text(l,"状態切替の詳細###reactionDetails","State change details###reactionDetails"))) {
			std::string stateLabel = r.reaction.state.empty() ? Text(l,"切り替えない","No state change") : r.reaction.state;
			ImGui::TextWrapped("%s",Text(l,"被弾時の状態","State on hit")); ImGui::SetNextItemWidth(-1);
			if (ImGui::BeginCombo("##state",stateLabel.c_str())) {
				if (ImGui::Selectable(Text(l,"切り替えない","No state change"),r.reaction.state.empty())) { r.reaction.state.clear(); r.fields |= State; }
				std::unordered_set<std::string> shown;
				for (const auto& name : m.states) if (!name.empty() && shown.insert(name).second) {
					if (ImGui::Selectable(name.c_str(), name == r.reaction.state)) { r.reaction.state = name; r.fields |= State; }
				}
				ImGui::EndCombo();
			}
			const char* keys[] = {"MinimumDamage","PoiseBreak"};
			const char* labels[] = {Text(l,"怯み値の閾値","Poise damage threshold"),Text(l,"耐性を使い切る","Poise break")};
			if (Choice(Text(l,"状態切替条件","State change condition"),"##trigger",r.reaction.trigger,keys,labels,2)) r.fields |= Trigger;
			if (r.reaction.trigger == "PoiseBreak") {
				if (StringField(Text(l,"耐性の識別名","Poise stat identifier"),"##poiseStat",r.reaction.poiseStat)) r.fields |= PoiseStat;
				if (Number(Text(l,"耐性の回復待機（秒）","Poise recovery delay (s)"),"##poiseDelay",r.reaction.poiseDelay)) r.fields |= PoiseDelay;
			} else if (r.reaction.trigger == "MinimumDamage") {
				if (Number(Text(l,"必要な怯み値（HPダメージとは別）","Required poise damage (separate from HP)"),"##minPoise",r.reaction.minimumPoise)) r.fields |= MinimumPoise;
			}
			ImGui::TreePop();
		}
	}
	ImGui::EndDisabled();
	if (!r.fields && r.operation != Operation::AddReaction) r.operation = Operation::None;
	return r;
}

std::vector<Request> DrawActor(const ActorModel& m, Selection& selected, EditorLanguage l, bool editable) {
	std::vector<Request> requests;
	ImGui::TextWrapped(Text(l,"対象：%s","Actor: %s"),m.name.c_str());
	if (!editable) Notice({IssueCode::Playing,{}},l);
	if (!ImGui::BeginTabBar("PlayerCombatTabs")) return requests;
	if (ImGui::BeginTabItem(Text(l,"攻撃###attackTab","Attack###attackTab"))) {
		if (!selected.initialized) {
			selected.initialized = true;
			if (!m.attacks.empty()) { selected.attackOwner = m.attacks.front().subject.entityId; selected.attackName = m.attacks.front().subject.attackName; }
		}
		const AttackModel* attack = nullptr;
		for (const auto& a : m.attacks) if (a.subject.entityId == selected.attackOwner && a.subject.attackName == selected.attackName) attack = &a;
		if (!attack) { selected.attackOwner = 0; selected.attackName.clear(); }
		ImGui::TextWrapped("%s",Text(l,"攻撃","Attack")); ImGui::SetNextItemWidth(-1);
		const std::string preview = attack ? attack->subject.attackName + " / " + attack->ownerName : Text(l,"選択してください","Select an attack");
		if (ImGui::BeginCombo("##attackChoice",preview.c_str())) {
			for (const auto& a : m.attacks) {
				const std::string key = a.subject.attackName + " / " + a.ownerName + "##" + std::to_string(a.subject.entityId);
				if (ImGui::Selectable(key.c_str(), &a == attack)) { selected.attackOwner = a.subject.entityId; selected.attackName = a.subject.attackName; }
			}
			ImGui::EndCombo();
		}
		if (attack) {
			ImGui::TextWrapped(Text(l,"設定元：%s","Source: %s"),attack->ownerName.c_str());
			for (const auto& name : attack->sourceStates) ImGui::TextWrapped(Text(l,"発動元：%s","Activated by: %s"),name.c_str());
			Link(requests,Text(l,"設定元へ###inspectSource","Inspect source###inspectSource"),m.id,attack->subject.entityId,"AttackSet");
			if (!attack->sourcePrefab.empty()) {
				ImGui::BeginDisabled(!editable);
				Link(requests,Text(l,"元Prefabを開く###openSourcePrefab","Open source Prefab###openSourcePrefab"),m.id,attack->subject.entityId,"AttackSet",Operation::OpenPrefab);
				ImGui::EndDisabled();
			}
			ImGui::PushID(std::to_string(attack->subject.entityId).c_str());
			ImGui::PushID(std::to_string(attack->subject.componentId).c_str());
			auto r = DrawAttackFields(*attack,l,editable); if (r.operation != Operation::None) requests.push_back(std::move(r));
			ImGui::PopID(); ImGui::PopID();
			if (ImGui::TreeNode(Text(l,"姿勢・当たり判定###pose","Pose / hit windows###pose"))) {
				ImGui::TextWrapped(Text(l,"姿勢のClip：%s","Pose clip: %s"),attack->animation.c_str());
				Link(requests,Text(l,"攻撃の詳細を開く###windows","Inspect attack details###windows"),m.id,attack->subject.entityId,"AttackSet");
				Link(requests,Text(l,"姿勢の設定を開く###animator","Inspect pose settings###animator"),m.id,attack->subject.entityId,"PrefabAnimator");
				ImGui::TreePop();
			}
		}
		for (const auto& issue : m.issues) Notice(issue,l);
		if (ImGui::TreeNode(Text(l,"発動・コンボ / 詳細・旧設定###states","Activation / Combo / Legacy###states"))) {
			Link(requests,Text(l,"StateMachineを開く###stateMachine","Inspect StateMachine###stateMachine"),m.id,m.id,"StateMachine");
			ImGui::TreePop();
		}
		ImGui::EndTabItem();
	}
	if (ImGui::BeginTabItem(Text(l,"被弾###hitTab","On hit###hitTab"))) {
		for (const auto& receiver : m.receivers) {
			ImGui::PushID(std::to_string(receiver.id).c_str());
			ImGui::TextWrapped(Text(l,"受け手：%s","Receiver: %s"),receiver.name.c_str());
			if (receiver.id != m.id) Link(requests,Text(l,"受け手を開く###receiver","Inspect receiver###receiver"),m.id,receiver.id,"HitReaction");
			ImGui::PopID();
		}
		ImGui::PushID("ReactionProxy");
		ImGui::PushID(std::to_string(m.reaction.subject.componentId).c_str());
		auto r = DrawReactionFields(m.reaction,l,editable); if (r.operation != Operation::None) requests.push_back(std::move(r));
		ImGui::PopID(); ImGui::PopID();
		if (ImGui::TreeNode(Text(l,"受け手の接続を確認###receiverSetup","Receiver connections###receiverSetup"))) {
			for (const auto& hurt : m.hurtBoxes) {
				ImGui::PushID(std::to_string(hurt.id).c_str());
				Link(requests,hurt.name.c_str(),m.id,hurt.id,"HurtBox"); ImGui::PopID();
			}
			Link(requests,Text(l,"PhysicsBodyを開く###body","Inspect PhysicsBody###body"),m.id,m.id,"PhysicsBody");
			Link(requests,Text(l,"Healthを開く###health","Inspect Health###health"),m.id,m.id,"Health");
			ImGui::TreePop();
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
	return requests;
}

void DrawReference(EditorLanguage l) {
	Heading(Text(l,"Playerから編集する","Edit from the Player"));
	ImGui::TextWrapped("%s",Text(l,"Playerを選択し「プレイヤーの動作」を開きます。簡易Inspectorでもこの入口は表示されます。攻撃は使用元のStateとAttack名から選びます。","Select the Player and open Player actions, also available in the Simple Inspector. Attacks are resolved from their states and attack names."));
	Heading(Text(l,"保存先と移動","Source and movement"));
	ImGui::TextWrapped("%s",Text(l,"画面上部の保存先を確認してください。Scene内の変更はSceneへ、Prefab内の変更は開いているPrefabへ保存します。元Prefabを開くだけでは同期しません。移動は構え終了から動作終了まで。前後は＋前／－後ろ、左右は＋右／－左です。壁で移動が止まる場合があります。","Check the save destination above. Scene edits save to the scene; Prefab edits save to the open Prefab. Opening a source Prefab does not synchronize it. Movement spans the action phase: + forward / - backward and + right / - left. Walls can limit movement."));
	Heading(Text(l,"吹き飛びを追加する","Add knockback"));
	ImGui::TextWrapped("%s",Text(l,"被弾タブの追加ボタンはHitReactionだけを追加します（倍率1、水平0.2秒、状態切替なし）。攻撃側の水平・上向き量に受け手の倍率が掛かります。時間は水平速度の継続・減衰用で、滞空やStateの硬直時間ではありません。追加と数値変更は既存のUndoで戻せます。","The On hit button adds only HitReaction (multiplier 1, horizontal duration 0.2 s, no state change). It multiplies the attack's horizontal and vertical knockback. Duration controls horizontal decay, not airtime or state stun. Use the existing Undo for additions and field edits."));
	Heading(Text(l,"受け手と確認","Receiver and verification"));
	ImGui::TextWrapped("%s",Text(l,"実際の受け手はHurtBoxのStatSet参照で決まります。Playerと別ならその受け手のHitReactionを編集してください。Dynamic Bodyと軸固定も確認します。状態切替条件の怯み値はHPダメージと別で、条件が未成立でも吹き飛びは予約されます。保存後、Playで攻撃移動・被弾・壁との接触を確認してください。全身プレビューの統一は後続工程です。","The HurtBox's StatSet reference determines the receiver. If it is not the Player, edit that receiver's HitReaction. Check Dynamic Body and frozen axes. The poise threshold is separate from HP damage; knockback is scheduled before the state-change condition. Save and verify attacks, hits and walls in Play. Unified full-body preview is a later step."));
}

bool Matches(const DocumentToken& a, const DocumentToken& b) {
	return a.prefab == b.prefab && a.key == b.key && a.epoch == b.epoch &&
		a.generation == b.generation && a.revision == b.revision;
}

bool ValidateAndApplyValueEdit(SceneDocument& doc, const DocumentToken& current, const Request& r,
	bool editingAllowed, bool& changed, Issue& issue) {
	changed = false; issue = {};
	auto fail = [&](IssueCode code) { issue = { code, r.subject.attackName }; return false; };
	if (!editingAllowed) return fail(IssueCode::Playing);
	if (!Matches(current,r.token) || current.generation != doc.GetContentGeneration() || current.revision != doc.GetRevision()) return fail(IssueCode::Stale);
	auto* entity = doc.FindEntity(r.subject.entityId);
	if (!entity) return fail(IssueCode::Missing);
	if (entity->locked || entity->folder) return fail(IssueCode::Locked);
	const auto* original = Component(entity,r.subject.componentType);
	if (!original || original->localId != r.subject.componentId) return fail(IssueCode::Stale);
	// 一意なComponentを再取得。検証中には配列やDocumentを置き換えない。
	auto* c = &entity->components[static_cast<size_t>(original - entity->components.data())];
	auto nonnegative = [](float value) { return std::isfinite(value) && value >= 0; };
	auto assign = [&changed](auto& destination, const auto& value) { if (destination != value) { destination = value; changed = true; } };
	if (r.operation == Operation::EditAttack && c->type == "AttackSet") {
		SceneAttackDefinition* a = nullptr;
		for (auto& candidate : c->attackDefinitions) if (candidate.name == r.subject.attackName) {
			if (a) return fail(IssueCode::Ambiguous);
			a = &candidate;
		}
		if (!a) return fail(IssueCode::Missing);
		const auto& v = r.attack;
		const float windup = r.fields & Windup ? v.windup : a->windup;
		const float active = r.fields & ActiveTime ? v.activeTime : a->activeTime;
		const float recovery = r.fields & Recovery ? v.recovery : a->recovery;
		if ((r.fields & (Windup | ActiveTime | Recovery)) &&
			(!nonnegative(windup) || !nonnegative(active) || !nonnegative(recovery) || !std::isfinite(windup + active + recovery))) return fail(IssueCode::InvalidTime);
		if (((r.fields & Forward) && !std::isfinite(v.forward)) || ((r.fields & Side) && !std::isfinite(v.side)) ||
			((r.fields & Angle) && !std::isfinite(v.angle))) return fail(IssueCode::InvalidDistance);
		if ((r.fields & Easing) && !Contains(easingNames,5,v.easing)) return fail(IssueCode::InvalidChoice);
		if ((r.fields & Facing) && !Contains(facingNames,4,v.facing)) return fail(IssueCode::InvalidChoice);
		if (r.fields & FacingTarget) {
			const auto* target = doc.FindEntity(v.targetId);
			if ((v.targetId || !v.targetName.empty()) && (!target || target->folder || target->name != v.targetName)) return fail(IssueCode::InvalidTarget);
		}
		if (r.fields & Windup) assign(a->windup,v.windup);
		if (r.fields & ActiveTime) assign(a->activeTime,v.activeTime);
		if (r.fields & Recovery) assign(a->recovery,v.recovery);
		if (r.fields & Forward) assign(a->forwardDistance,v.forward);
		if (r.fields & Side) assign(a->sideDistance,v.side);
		if (r.fields & Easing) assign(a->motionEasing,v.easing);
		if (r.fields & Facing) assign(a->facingMode,v.facing);
		if (r.fields & FacingTarget) { assign(a->facingTargetEntityId,v.targetId); assign(a->facingTargetEntityName,v.targetName); }
		if (r.fields & Angle) assign(a->facingRotateAngle,v.angle);
		return true;
	}
	if (r.operation == Operation::EditReaction && c->type == "HitReaction") {
		const auto& v = r.reaction;
		if (((r.fields & Multiplier) && !nonnegative(v.multiplier)) || ((r.fields & Duration) && !nonnegative(v.duration)) ||
			((r.fields & MinimumPoise) && !nonnegative(v.minimumPoise)) || ((r.fields & PoiseDelay) && !nonnegative(v.poiseDelay)) ||
			((r.fields & PoiseStat) && v.poiseStat.empty())) return fail(IssueCode::InvalidReaction);
		if ((r.fields & Trigger) && v.trigger != "MinimumDamage" && v.trigger != "PoiseBreak") return fail(IssueCode::InvalidChoice);
		if ((r.fields & State) && !v.state.empty()) {
			const auto* machine = Component(entity,"StateMachine");
			if (!machine || std::count_if(machine->stateMachineStates.begin(),machine->stateMachineStates.end(),
				[&](const auto& state) { return state.name == v.state; }) != 1) return fail(IssueCode::InvalidState);
		}
		if (r.fields & Enabled) assign(c->enabled,v.enabled);
		if (r.fields & Multiplier) assign(c->hitReactionKnockbackMultiplier,v.multiplier);
		if (r.fields & Duration) assign(c->hitReactionStateDuration,v.duration);
		if (r.fields & Trigger) assign(c->hitReactionTriggerMode,v.trigger);
		if (r.fields & MinimumPoise) assign(c->hitReactionMinimumPoiseDamage,v.minimumPoise);
		if (r.fields & PoiseStat) assign(c->hitReactionPoiseStatId,v.poiseStat);
		if (r.fields & PoiseDelay) assign(c->hitReactionPoiseRecoveryDelay,v.poiseDelay);
		if (r.fields & State) assign(c->hitReactionStateName,v.state);
		return true;
	}
	return fail(IssueCode::Missing);
}
}
