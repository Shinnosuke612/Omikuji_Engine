// 役割: 個体設定とチームoverrideの優先順位を実装する。
#include "AgentSettingsResolver.h"

#include "../scene/SceneDocument.h"

namespace AgentSettingsResolver {
	SceneComponent ResolveAgentBehaviorSettings(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& behavior
	) {
		SceneComponent resolved = behavior;
		const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
		if (
			!team ||
			!team->agentBehaviorOverride ||
			resolved.agentTeamSettingsOverride
		) {
			return resolved;
		}

		resolved.agentGroupName = team->agentGroupName.empty()
			? team->name
			: team->agentGroupName;
		resolved.agentMinSpeed = team->agentMinSpeed;
		resolved.agentMaxSpeed = team->agentMaxSpeed;
		resolved.agentTurnSpeed = team->agentTurnSpeed;
		resolved.agentWanderStrength = team->agentWanderStrength;
		resolved.agentWanderChangeInterval = team->agentWanderChangeInterval;
		resolved.agentWanderDirectionRange = team->agentWanderDirectionRange;
		resolved.agentWanderVerticalRange = team->agentWanderVerticalRange;
		resolved.agentRandomizeSeedOnPlay = team->agentRandomizeSeedOnPlay;
		resolved.agentRandomSeed = team->agentRandomSeed;
		resolved.agentFlockDecisionInterval = team->agentFlockDecisionInterval;
		resolved.agentFlockAcceleration = team->agentFlockAcceleration;
		resolved.agentFlockTurnRate = team->agentFlockTurnRate;
		resolved.agentMemberCenterFollow = team->agentMemberCenterFollow;
		resolved.agentMemberJitterStrength = team->agentMemberJitterStrength;
		resolved.agentMemberJitterFrequency = team->agentMemberJitterFrequency;
		resolved.agentMemberJitterUpdateInterval =
			team->agentMemberJitterUpdateInterval;
		resolved.agentMemberJitterFollowSpeed =
			team->agentMemberJitterFollowSpeed;
		resolved.agentMemberSpeedVariation = team->agentMemberSpeedVariation;
		resolved.agentMemberLeashDistance = team->agentMemberLeashDistance;
		resolved.agentMemberLeashStrength = team->agentMemberLeashStrength;
		resolved.agentMemberCatchupSpeed = team->agentMemberCatchupSpeed;
		resolved.agentMemberSeparationUpdateInterval =
			team->agentMemberSeparationUpdateInterval;
		resolved.agentMemberSeparationBlend = team->agentMemberSeparationBlend;
		resolved.agentMemberMinimumDistance =
			team->agentMemberMinimumDistance;
		resolved.agentUseTeamHeading = team->agentUseTeamHeading;
		resolved.agentTeamHeadingFromAverage =
			team->agentTeamHeadingFromAverage;
		resolved.agentTeamHeadingDirection = team->agentTeamHeadingDirection;
		resolved.agentTeamHeadingWeight = team->agentTeamHeadingWeight;
		resolved.agentTeamHeadingFollowSpeed =
			team->agentTeamHeadingFollowSpeed;
		resolved.agentUseTeamRotation = team->agentUseTeamRotation;
		resolved.agentTeamRotationWeight = team->agentTeamRotationWeight;
		resolved.agentTeamRotationFollowSpeed =
			team->agentTeamRotationFollowSpeed;
		resolved.agentAlignForwardToVelocity =
			team->agentAlignForwardToVelocity;
		resolved.agentForwardAxis = team->agentForwardAxis;
		resolved.agentRotateAxisX = team->agentRotateAxisX;
		resolved.agentRotateAxisY = team->agentRotateAxisY;
		resolved.agentRotateAxisZ = team->agentRotateAxisZ;
		resolved.agentRotationFollowSpeed = team->agentRotationFollowSpeed;
		resolved.agentPitchFromVerticalVelocity =
			team->agentPitchFromVerticalVelocity;
		resolved.agentBankingStrength = team->agentBankingStrength;
		resolved.agentSchooling = team->agentSchooling;
		resolved.agentSchoolingUpdateInterval =
			team->agentSchoolingUpdateInterval;
		resolved.agentSchoolingUpdateJitter =
			team->agentSchoolingUpdateJitter;
		resolved.agentNeighborLimit = team->agentNeighborLimit;
		resolved.agentSchoolingBlend = team->agentSchoolingBlend;
		resolved.agentSeparationRadius = team->agentSeparationRadius;
		resolved.agentAlignmentRadius = team->agentAlignmentRadius;
		resolved.agentCohesionRadius = team->agentCohesionRadius;
		resolved.agentSeparationWeight = team->agentSeparationWeight;
		resolved.agentAlignmentWeight = team->agentAlignmentWeight;
		resolved.agentCohesionWeight = team->agentCohesionWeight;
		resolved.agentVisualColor = team->agentVisualColor;
		resolved.agentEnableLighting = team->agentEnableLighting;
		return resolved;
	}

	SceneComponent ResolveTeamLeaderSettings(
		const SceneDocument& document,
		const SceneEntity& entity,
		const SceneComponent& behavior
	) {
		SceneComponent resolved = behavior;
		const SceneTeamSettings* team = document.ResolveEntityTeam(entity);
		if (!team) {
			return resolved;
		}
		resolved.agentMinSpeed = team->agentMinSpeed;
		resolved.agentMaxSpeed = team->agentMaxSpeed;
		resolved.agentWanderStrength = team->agentWanderStrength;
		resolved.agentWanderChangeInterval = team->agentWanderChangeInterval;
		resolved.agentWanderDirectionRange = team->agentWanderDirectionRange;
		resolved.agentWanderVerticalRange = team->agentWanderVerticalRange;
		resolved.agentRandomizeSeedOnPlay = team->agentRandomizeSeedOnPlay;
		resolved.agentRandomSeed = team->agentRandomSeed;
		resolved.agentFlockDecisionInterval = team->agentFlockDecisionInterval;
		resolved.agentFlockAcceleration = team->agentFlockAcceleration;
		resolved.agentFlockTurnRate = team->agentFlockTurnRate;
		resolved.agentUseTeamHeading = team->agentUseTeamHeading;
		resolved.agentTeamHeadingDirection = team->agentTeamHeadingDirection;
		resolved.agentTeamHeadingWeight = team->agentTeamHeadingWeight;
		resolved.agentForwardAxis = team->agentForwardAxis;
		return resolved;
	}
}
