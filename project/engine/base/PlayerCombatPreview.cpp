// 役割: 原本の参照を検証し、絶対時刻から非累積の戦闘プレビューを生成する。
#include "../scene/SceneAttackHitGeometryEvaluator.h"
#include "PlayerCombatPreview.h"
#include "../scene/ScenePrefabAnimationEvaluator.h"
#include "../scene/SceneTransformResolver.h"
#include "../math/Math.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {
bool AttackGeometryTarget(const SceneDocument& document, const SceneEntity* entity, const SceneAttackHitWindow& window) {
	if (!entity) return false;
	if (!document.FindEntity(window.hitBoxEntityId)) {
		int matches = 0;
		for (const auto& candidate : document.GetEntities()) if (candidate.name == window.hitBoxEntityName) ++matches;
		if (matches != 1) return false;
	}
	int hitBoxes = 0, colliders = 0;
	for (const auto& c : entity->components) {
		if (c.type == "HitBox") { if (!c.enabled) return false; ++hitBoxes; }
		if (c.type == "OBBCollider") { if (!c.enabled || !c.colliderIsTrigger) return false; ++colliders; }
		if (c.enabled && (c.type == "HurtBox" || c.type == "PhysicsBody")) return false;
	}
	if (hitBoxes != 1 || colliders != 1) return false;
	for (const auto& owner : document.GetEntities()) for (const auto& c : owner.components) {
		if (!c.enabled || c.type != "EnemyBehavior") continue;
		const auto* target = document.FindEntity(c.enemyAttackHitBoxEntityId);
		if (!target && !c.enemyAttackHitBoxEntityName.empty()) target = document.FindEntityByName(c.enemyAttackHitBoxEntityName);
		if (target && target->id == entity->id) return false;
	}
	return true;
}
const SceneComponent* Component(const SceneEntity* e, const char* type) {
	if (!e) return nullptr;
	const SceneComponent* found = nullptr;
	for (const auto& c : e->components) if (c.type == type) {
		if (found) return nullptr;
		found = &c;
	}
	return found && found->enabled ? found : nullptr;
}
bool Finite(const Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool InRoot(const SceneDocument& d, uint64_t id, uint64_t root) {
	std::unordered_set<uint64_t> visited;
	const auto* e = d.FindEntity(id);
	while (e && visited.insert(e->id).second) {
		if (e->id == root) return true;
		e = d.FindEntity(e->parentId);
	}
	return false;
}
bool ValidHierarchy(const SceneDocument& d, const SceneEntity& e) {
	std::unordered_set<uint64_t> visited;
	const auto* current = &e;
	while (current) {
		if (!visited.insert(current->id).second || !Finite(current->transform.translate) || !Finite(current->transform.scale) ||
			!std::isfinite(current->transform.rotate.x) || !std::isfinite(current->transform.rotate.y) ||
			!std::isfinite(current->transform.rotate.z) || !std::isfinite(current->transform.rotate.w)) return false;
		if (!current->parentId) return true;
		current = d.FindEntity(current->parentId);
	}
	return false;
}
// ID優先。名前が複数なら先頭を採用せず、呼出元へ曖昧さを返す。
const SceneEntity* Resolve(const SceneDocument& d, uint64_t id, const std::string& name, bool& ambiguous) {
	if (id) if (const auto* e = d.FindEntity(id)) return e;
	const SceneEntity* found = nullptr;
	if (!name.empty()) for (const auto& e : d.GetEntities()) if (e.name == name) {
		if (found) { ambiguous = true; return nullptr; }
		found = &e;
	}
	return found;
}
float Progress(float time, const SceneAttackDefinition& a) {
	const float p = std::clamp((time - a.windup) / (std::max)(a.activeTime,0.0001f),0.0f,1.0f);
	if (a.motionEasing == "Linear") return p;
	if (a.motionEasing == "EaseIn") return p*p*p;
	if (a.motionEasing == "EaseOut") return Math::EaseOutCubic(p);
	if (a.motionEasing == "EaseInOut") return p < 0.5f ? 4.0f*p*p*p : 1.0f-std::pow(-2.0f*p+2.0f,3.0f)*0.5f;
	return Math::SmoothStep(p);
}
}

void PlayerCombatPreview::Clear() {
	display_.reset(); issue_ = {}; motionIssue_ = {}; windows_.clear();
	attack_ = {}; clipName_.clear(); rootId_ = 0; duration_ = 0;
	hasActor_ = false; animationFallback_ = false;
}

bool PlayerCombatPreview::RebuildAndSample(const SceneDocument& source, const PlayerCombatEditor::Subject& subject, float time) {
	Clear();
	auto fail = [&](const char* ja, const char* en, const std::string& detail = std::string{}) {
		issue_ = {ja,en,detail}; display_.reset(); return false;
	};
	const auto* owner = source.FindEntity(subject.entityId);
	const auto* set = Component(owner,"AttackSet");
	if (!set || (subject.componentId && set->localId != subject.componentId))
		return fail("AttackSetが変更されています。対象を確認して再開始してください。","AttackSet changed. Check the source and restart.");
	const SceneAttackDefinition* attack = nullptr;
	for (const auto& a : set->attackDefinitions) if (a.name == subject.attackName) {
		if (attack) return fail("攻撃名が重複しています。元Inspectorで重複を解消してください。","Duplicate attack name. Resolve it in the source Inspector.");
		attack = &a;
	}
	if (!attack || subject.attackName.empty()) return fail("攻撃がありません。攻撃を選び直してください。","Attack is missing. Select it again.");
	attack_ = *attack;
	if (!std::isfinite(time) || !std::isfinite(attack->windup) || !std::isfinite(attack->activeTime) || !std::isfinite(attack->recovery) ||
		attack->windup < 0 || attack->activeTime < 0 || attack->recovery < 0 ||
		!std::isfinite(attack->windup+attack->activeTime+attack->recovery) ||
		!std::isfinite(attack->forwardDistance) || !std::isfinite(attack->sideDistance))
		return fail("時間と距離が不正です。元Inspectorで有限の値と0以上の時間を指定してください。","Invalid timing or distance. Use finite values and nonnegative times in the source Inspector.");
	duration_ = attack->windup+attack->activeTime+attack->recovery;
	time = std::clamp(time,0.0f,duration_);
	rootId_ = owner->id;
	if (subject.actorId) {
		const auto model = PlayerCombatEditor::ResolveActor(source,subject.actorId);
		if (!Component(source.FindEntity(subject.actorId),"PlayerBehavior") ||
			!std::any_of(model.attacks.begin(),model.attacks.end(),[&](const auto& a) {
				return a.editable && a.subject.entityId == subject.entityId && a.subject.attackName == subject.attackName;
			}) || !InRoot(source,owner->id,subject.actorId))
			return fail("Playerと攻撃の接続が変わりました。発動元を確認して再開始してください。","Player attack connection changed. Check the source state and restart.");
		rootId_ = subject.actorId; hasActor_ = true;
	}
	bool ambiguous = false;
	const auto* animatorOwner = Resolve(source,attack->animationTargetEntityId,attack->animationTargetEntityName,ambiguous);
	if (ambiguous) return fail("姿勢の参照名が重複しています。参照を修正してください。","Animation target name is ambiguous. Correct the reference.");
	if (!animatorOwner) { animatorOwner = owner; animationFallback_ = true; }
	const auto* animator = Component(animatorOwner,"PrefabAnimator");
	const ScenePrefabAnimationClip* clip = nullptr;
	if (animator) for (const auto& c : animator->prefabAnimationClips) if (c.name == attack->animation) {
		if (clip) return fail("Clip名が重複しています。元Inspectorで重複を解消してください。","Duplicate Clip name. Resolve it in the source Inspector.");
		clip = &c;
	}
	if (!clip || attack->animation.empty() || !std::isfinite(clip->duration) || clip->duration < 0 || !InRoot(source,animatorOwner->id,rootId_))
		return fail("有効なClipと表示階層内のAnimatorが必要です。攻撃の姿勢参照を確認してください。","An enabled Animator and valid Clip inside the display hierarchy are required. Check the attack animation reference.");
	clipName_ = clip->name;
	const float clipDuration = (std::max)(clip->duration,0.001f);
	const float clipTime = clip->loop ? std::fmod(time,clipDuration) : (std::min)(time,clipDuration);
	bool actorTrack = false;
	std::unordered_map<uint64_t,unsigned> trackMasks;
	std::unordered_map<uint64_t,size_t> activeTrackIndices;
	for (size_t trackIndex = 0; trackIndex < clip->tracks.size(); ++trackIndex) {
		const auto& track = clip->tracks[trackIndex];
		const auto* target = Resolve(source,track.targetEntityId,track.targetEntityName,ambiguous);
		if (!target && !track.targetEntityId && track.targetEntityName.empty()) target = animatorOwner;
		const unsigned mask = track.property == "LocalPosition" ? 1u : track.property == "LocalRotation" ? 2u : track.property == "LocalScale" ? 4u : track.property == "Active" ? 8u : 0u;
		if (ambiguous || !target || !mask || !InRoot(source,target->id,rootId_) || (trackMasks[target->id] & mask))
			return fail("Track対象・property・重複を確認してください。表示階層外の対象は使用できません。","Check Track targets, properties and duplicates. Targets outside the display hierarchy are unsupported.");
		trackMasks[target->id] |= mask;
		// 空TrackはApplyClipも書き込まない。ID/名前/Selfは上で検証した同じ対象を使う。
		if (mask == 8u && !track.keyframes.empty()) activeTrackIndices.emplace(target->id, trackIndex);
		actorTrack |= target->id == subject.actorId && mask != 8u && !track.keyframes.empty();
		float previous = -1;
		for (const auto& k : track.keyframes) {
			if (!std::isfinite(k.time) || k.time < 0 || k.time > clip->duration || k.time <= previous || !Finite(k.value) || !Finite(k.positionBulge))
				return fail("キーの時刻・値が不正です。元Inspectorで時刻順と有限値を確認してください。","Invalid key time or value. Check finite values and time ordering in the source Inspector.");
			previous = k.time;
		}
		if (mask == 2u) {
			Vector3 euler{};
			if ((track.rotationInterpolation != "QuaternionSlerp" && track.rotationInterpolation != "EulerContinuous") ||
				(track.rotationInterpolation == "EulerContinuous" && !track.keyframes.empty() &&
				!ScenePrefabAnimationEvaluator::TrySampleContinuousEuler(track,clipTime,euler)))
				return fail("回転補間またはキーが不正です。回転方式とキーを確認してください。","Invalid rotation interpolation or keys. Check the rotation mode and keys.");
		}
	}
	std::vector<uint64_t> windowIds;
	for (const auto& w : attack->hitWindows) {
		const auto* target = Resolve(source,w.hitBoxEntityId,w.hitBoxEntityName,ambiguous);
		const auto* collider = Component(target,"OBBCollider");
		if (ambiguous || !target || !InRoot(source,target->id,rootId_) || !Component(target,"HitBox") || !Component(target,"OBBCollider") ||
			!std::isfinite(w.startTime) || !std::isfinite(w.endTime) || w.endTime < w.startTime ||
			(w.payloadSource != "HitBox" && w.overrideHitBoxHalfSize && !Finite(w.hitBoxHalfSize)) ||
			(collider && (!Finite(collider->colliderOffset) || !Finite(collider->colliderSizeMultiplier) || !std::isfinite(collider->colliderSphereRadius))))
			return fail("命中区間・HitBox・Colliderを確認してください。参照は表示階層内で一意にしてください。","Check hit windows, HitBox and Collider. References must be unique and inside the display hierarchy.");
		const auto activeTrack = activeTrackIndices.find(target->id);
		if (activeTrack != activeTrackIndices.end()) {
			// RuntimeはHit Windowの後にActiveを評価する。ここで上書きすると誤った判定表示になる。
			// 表示だけで勝者を選ばず、ClipとWindowの二重制御を解消してから再開始してもらう。
			return fail(
				"攻撃プレビューを開始・更新できません。同じHitBoxをHit WindowとActiveトラックが操作しています。元InspectorでこのHitBoxの有効区間をHit Windowへまとめ、競合するActiveトラックを外してから再開始してください。",
				"Cannot start or update attack preview: a Hit Window and an Active track control the same HitBox. In the source Inspector, use Hit Windows for this HitBox's active intervals, remove the conflicting Active track, then restart.",
				"Clip: " + clip->name + "\nHitBox: " + target->name + " (#" + std::to_string(target->id) +
				")\nTrack: #" + std::to_string(activeTrack->second + 1) + " / Active\nHit Window: #" + std::to_string(windowIds.size() + 1));
		}
		windowIds.push_back(target->id);
	}
	for (const auto& e : source.GetEntities()) if (InRoot(source,e.id,rootId_) && !ValidHierarchy(source,e))
		return fail("階層または姿勢が不正です。親参照とTransformを確認してください。","Invalid hierarchy or pose. Check parent references and Transforms.");

	// 毎回基準から作り直し、逆スクラブで移動/Collider変更を累積させない。
	display_ = std::make_unique<SceneDocument>(source);
	ScenePrefabAnimationEvaluator::ApplyClip(*display_,animatorOwner->id,*clip,clipTime);
	std::unordered_set<uint64_t> activeBoxes;
	for (uint64_t id : windowIds) display_->FindEntity(id)->active = false;
	for (size_t i = 0; i < windowIds.size(); ++i) {
		const auto& w = attack->hitWindows[i];
		auto* box = display_->FindEntity(windowIds[i]);
		bool active = time < duration_ && time >= w.startTime && time < w.endTime && activeBoxes.insert(box->id).second;
		SceneAttackHitGeometryEvaluator::Issue geometryIssue;
		SceneAttackHitGeometryEvaluator::Sample geometry;
		if (!w.geometryTrack.keys.empty()) {
			const auto* collider = Component(box,"OBBCollider");
			if (!AttackGeometryTarget(source,source.FindEntity(box->id),w)) geometryIssue.code = "Target";
			else if (SceneAttackHitGeometryEvaluator::Validate(w,duration_,collider->colliderShape,geometryIssue))
				SceneAttackHitGeometryEvaluator::SampleTrack(w.geometryTrack,time,geometry,geometryIssue);
			if (!geometryIssue.code.empty()) active = false;
			else if (active) for (auto& c : box->components) if (c.type == "OBBCollider" && c.enabled) {
				c.colliderOffset = geometry.offset;
				if (geometry.shape == "Box") c.colliderSizeMultiplier = geometry.halfSize;
				else c.colliderSphereRadius = geometry.radius;
			}
		}
		if (active) {
			box->active = true;
			if (w.payloadSource != "HitBox" && w.overrideHitBoxHalfSize) for (auto& c : box->components)
				if (c.type == "OBBCollider" && c.enabled && c.colliderShape == "Box")
					c.colliderSizeMultiplier = {(std::max)(w.hitBoxHalfSize.x,0.001f),(std::max)(w.hitBoxHalfSize.y,0.001f),(std::max)(w.hitBoxHalfSize.z,0.001f)};
		}
		windows_.push_back({box->id,box->name,w.startTime,w.endTime,active,false,w.startTime < 0 || w.endTime > duration_});
		if (!geometryIssue.code.empty()) windows_.back().geometryIssue = {
			"判定キーが不正なため、この区間は無効です。キーの時刻・値・形状・参照を修正してください。",
			"Invalid hit geometry keys. This window is disabled. Correct key times, values, shape or target.",geometryIssue.code};
	}
	// 親Transformを保持しつつ、薄く描かれる範囲外Colliderも確実に除く。
	for (auto& e : display_->GetEntities()) if (!InRoot(source,e.id,rootId_)) for (auto& c : e.components)
		if (c.type == "MeshRenderer" || c.type == "SpriteRenderer" || c.type == "TextRenderer" || c.type == "UICanvas" ||
			c.type == "OBBCollider" || c.type == "HitBox" || c.type == "HurtBox") c.enabled = false;
	for (auto& w : windows_) {
		const auto* e = display_->FindEntity(w.entityId);
		w.visible = Component(e,"OBBCollider")->colliderActive;
		while (e) { w.visible &= e->active; e = display_->FindEntity(e->parentId); }
	}
	const auto* root = source.FindEntity(rootId_);
	if (!hasActor_) motionIssue_ = {"移動は未再現：Player未指定（武器単体）。","Movement not reproduced: no Player (weapon only).",{}};
	else if (root->parentId) motionIssue_ = {"移動は未再現：親付きPlayer。","Movement not reproduced: parented Player.",{}};
	else if (attack->facingMode != "FixedAtStart") motionIssue_ = {"移動は未再現：開始時の向きを保つ攻撃だけに対応。","Movement not reproduced: only start-facing attacks are supported.",{}};
	else if (actorTrack) motionIssue_ = {"移動は未再現：Player自身の姿勢Trackと競合。","Movement not reproduced: Player Transform Track conflict.",{}};
	else {
		const auto world = SceneTransformResolver::ResolveSceneWorldMatrix(source,*root);
		Vector3 forward{world.m[2][0],0,world.m[2][2]};
		const float length = std::sqrt(forward.x*forward.x+forward.z*forward.z);
		forward = length <= 0.0001f ? Vector3{0,0,1} : Vector3{forward.x/length,0,forward.z/length};
		const float p = Progress(time,*attack);
		auto* pose = display_->FindEntity(rootId_);
		pose->transform.translate.x += (forward.x*attack->forwardDistance+forward.z*attack->sideDistance)*p;
		pose->transform.translate.z += (forward.z*attack->forwardDistance-forward.x*attack->sideDistance)*p;
	}
	for (const auto& e : display_->GetEntities()) if (InRoot(source,e.id,rootId_) && !ValidHierarchy(*display_,e))
		return fail("評価結果が有限値ではありません。姿勢キー・拡縮・移動量を確認してください。","Nonfinite evaluated pose. Check keys, scales and movement distances.");
	return true;
}

bool PlayerCombatPreview::GetEditVolume(size_t index, float time, EditVolume& volume) const {
	volume={};
	if(!display_ || index>=attack_.hitWindows.size() || index>=windows_.size()) return false;
	const auto& window=attack_.hitWindows[index];
	const auto* entity=display_->FindEntity(windows_[index].entityId);
	const auto* collider=Component(entity,"OBBCollider");
	if(!collider || !windows_[index].geometryIssue.japanese.empty()) return false;
	volume.shape=collider->colliderShape;
	volume.offset=collider->colliderOffset;
	volume.halfSize=collider->colliderSizeMultiplier;
	volume.radius=collider->colliderSphereRadius;
	if(!window.geometryTrack.keys.empty()) {
		SceneAttackHitGeometryEvaluator::Issue issue;
		SceneAttackHitGeometryEvaluator::Sample sample;
		if(!SceneAttackHitGeometryEvaluator::Validate(window,duration_,volume.shape,issue) || !SceneAttackHitGeometryEvaluator::SampleTrack(window.geometryTrack,time,sample,issue)) return false;
		volume.offset=sample.offset;volume.halfSize=sample.halfSize;volume.radius=sample.radius;
	}
	volume.world=SceneTransformResolver::ResolveSceneWorldMatrix(*display_,*entity);
	// 編集枠も実判定と同様に有限のworld範囲だけをRendererへ渡す。
	const auto& w=volume.world;
	double radius=0;
	for(int i=0;i<3;++i){
		for(int j=0;j<4;++j)if(!std::isfinite(w.m[i][j]))return false;
		const double length=std::sqrt(double(w.m[i][0])*w.m[i][0]+double(w.m[i][1])*w.m[i][1]+double(w.m[i][2])*w.m[i][2]);
		const float h[3]={volume.halfSize.x,volume.halfSize.y,volume.halfSize.z};
		if(volume.shape=="Sphere")radius=(std::max)(radius,length*volume.radius);
		else radius+=length*std::abs(h[i]);
	}
	const float p[3]={volume.offset.x,volume.offset.y,volume.offset.z};
	for(int j=0;j<3;++j){
		double center=w.m[3][j];for(int i=0;i<3;++i)center+=double(p[i])*w.m[i][j];
		if(!std::isfinite(center)||!std::isfinite(radius)||std::abs(center)+radius>(std::numeric_limits<float>::max)())return false;
	}
	volume.active=windows_[index].active;
	volume.valid=true;
	return true;
}
