// 役割: 新規V2作成と旧creation journalの再開をgeneratorから正式生成まで接続する。
#include "ProjectCreationService.h"
#include "../utility/StringUtility.h"
#include "ProjectCompatibilityProbe.h"
#include "ProjectSolutionGenerationService.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>
#include <vector>

namespace {
	bool ReadUtf8File(const std::filesystem::path& path, std::string& output) {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) return false;
		output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		return !input.bad();
	}

	bool ContainsExactlyOnce(const std::string& text, const std::string& marker) {
		const size_t first = text.find(marker);
		return first != std::string::npos && text.find(marker, first + marker.size()) == std::string::npos;
	}

	std::filesystem::path GetEnvironmentPath(const wchar_t* variable) {
		const DWORD required = GetEnvironmentVariableW(variable, nullptr, 0);
		if (required == 0) {
			return {};
		}
		std::vector<wchar_t> buffer(required);
		if (GetEnvironmentVariableW(variable, buffer.data(), required) == 0) {
			return {};
		}
		return std::filesystem::path(buffer.data());
	}

	bool IsRegularFile(const std::filesystem::path& path) {
		std::error_code error;
		return std::filesystem::is_regular_file(path, error) && !error;
	}

	bool IsDirectory(const std::filesystem::path& path) {
		std::error_code error;
		return std::filesystem::is_directory(path, error) && !error;
	}

	uint64_t FileTimeToUint64(const FILETIME& fileTime) {
		ULARGE_INTEGER value{};
		value.LowPart = fileTime.dwLowDateTime;
		value.HighPart = fileTime.dwHighDateTime;
		return value.QuadPart;
	}

	bool IsReparsePoint(const std::filesystem::path& path) {
		const DWORD attributes = GetFileAttributesW(path.c_str());
		return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	}
}

ProjectCreationService::ProjectCreationService(ProjectRegistry& registry)
	: registry_(registry) {
}

bool ProjectCreationService::Start(const ProjectCreationRequest& request, std::string& errorMessage) {
	if (generatorProcess_.IsRunning()) {
		errorMessage = "A Project generator operation is already running.";
		return false;
	}
	ProjectCreationOperation operation{};
	if (!ValidateRequest(request, operation, errorMessage)) {
		return false;
	}
	operation.operationId = MakeOperationId();
	operation.state = ProjectCreationOperationState::InProgress;
	operation.outputLayout = "ProjectFilesV2";
	operation.logPath = registry_.GetRegistryPath().parent_path() / L"operations" / StringUtility::ToPath(operation.operationId + ".log");
	if (!PersistOperation(operation, errorMessage)) {
		return false;
	}

	const std::filesystem::path powerShell = FindPowerShell();
	if (powerShell.empty()) {
		operation.state = ProjectCreationOperationState::Failed;
		PersistOperation(operation, errorMessage);
		errorMessage = "PowerShell executable could not be resolved.";
		SetSnapshot(ProjectCreationServiceState::Failed, operation, errorMessage);
		return false;
	}
	WindowsProcessRequest processRequest{};
	processRequest.applicationPath = powerShell;
	processRequest.arguments = {
		L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-File",
		operation.generatorPath.native(),
		StringUtility::ToPath(operation.projectId).native(),
		StringUtility::ToPath(operation.displayName).native(),
		operation.destinationRoot.native(),
		StringUtility::ToPath(operation.startSceneId).native(),
		L"-OutputLayout", StringUtility::ToPath(operation.outputLayout).native()
	};
	processRequest.workingDirectory = operation.templateSourceRoot / L"project";
	processRequest.outputLogPath = operation.logPath;
	if (!generatorProcess_.Start(processRequest, errorMessage)) {
		operation.state = ProjectCreationOperationState::Failed;
		PersistOperation(operation, errorMessage);
		SetSnapshot(ProjectCreationServiceState::Failed, operation, errorMessage);
		return false;
	}
	const WindowsProcessSnapshot process = generatorProcess_.Poll();
	operation.processId = process.processId;
	operation.processStartTimeFileTime = GetProcessStartTimeFileTime(operation.processId);
	activeOperation_ = operation;
	if (!PersistOperation(*activeOperation_, errorMessage)) {
		SetSnapshot(ProjectCreationServiceState::Failed, *activeOperation_, errorMessage);
		return false;
	}
	SetSnapshot(ProjectCreationServiceState::GeneratorRunning, *activeOperation_, "Project generator started.");
	return true;
}

bool ProjectCreationService::Update(std::string& errorMessage) {
	if (!activeOperation_.has_value()) {
		return true;
	}
	const WindowsProcessSnapshot process = generatorProcess_.Poll();
	if (process.state == WindowsProcessState::Running) {
		SetSnapshot(ProjectCreationServiceState::GeneratorRunning, *activeOperation_, "Project generator is running.");
		return true;
	}
	activeOperation_->exitCode = process.state == WindowsProcessState::Exited ? static_cast<int32_t>(process.exitCode) : -1;
	if (process.state != WindowsProcessState::Exited || process.exitCode != 0) {
		activeOperation_->state = ProjectCreationOperationState::Failed;
		if (!PersistOperation(*activeOperation_, errorMessage)) {
			return false;
		}
		SetSnapshot(ProjectCreationServiceState::Failed, *activeOperation_, "Project generator failed.");
		return true;
	}
	return Finalize(*activeOperation_, errorMessage);
}

bool ProjectCreationService::Reconcile(std::string& errorMessage) {
	for (const ProjectCreationOperation operation : registry_.GetCreationOperations()) {
		if (operation.state != ProjectCreationOperationState::InProgress) {
			continue;
		}
		if (IsProcessIdentityRunning(operation.processId, operation.processStartTimeFileTime)) {
			SetSnapshot(ProjectCreationServiceState::GeneratorRunning, operation, "Project generator is still running.");
			continue;
		}
		ProjectCreationOperation recovered = operation;
		std::string validationError;
		if (ValidateFinalTree(recovered, validationError)) {
			if (!Finalize(recovered, errorMessage)) {
				return false;
			}
			continue;
		}
		recovered.state = ProjectCreationOperationState::Failed;
		recovered.exitCode = -1;
		if (!PersistOperation(recovered, errorMessage)) {
			return false;
		}
		SetSnapshot(ProjectCreationServiceState::Failed, recovered, "Project generator is no longer running and no final Project was found.");
	}
	return true;
}

bool ProjectCreationService::RetryFinalize(const std::string& operationId, std::string& errorMessage) {
	const ProjectCreationOperation* saved = registry_.FindCreationOperation(operationId);
	if (saved == nullptr || saved->state == ProjectCreationOperationState::InProgress || saved->state == ProjectCreationOperationState::Complete) {
		errorMessage = "Creation operation is not eligible for finalize retry.";
		return false;
	}
	ProjectCreationOperation operation = *saved;
	return Finalize(operation, errorMessage);
}

std::vector<std::filesystem::path> ProjectCreationService::FindOrphanedStagingCandidates(
	const std::string& operationId,
	std::string& errorMessage
) const {
	std::vector<std::filesystem::path> candidates;
	const ProjectCreationOperation* operation = registry_.FindCreationOperation(operationId);
	if (operation == nullptr) {
		errorMessage = "Creation operation was not found.";
		return candidates;
	}
	if (IsProcessIdentityRunning(operation->processId, operation->processStartTimeFileTime)) {
		return candidates;
	}
	const std::regex namePattern(
		"^\\.creating-" + operation->projectId + "-[0-9a-fA-F]{32}$"
	);
	std::error_code error;
	std::filesystem::directory_iterator iterator(operation->destinationRoot, error);
	if (error) {
		errorMessage = "Destination root could not be inspected for staging candidates.";
		return {};
	}
	for (const std::filesystem::directory_entry& entry : iterator) {
		const std::filesystem::path candidate = entry.path().lexically_normal();
		if (!entry.is_directory(error) || error || !std::regex_match(candidate.filename().string(), namePattern) ||
			HasReparsePointInExistingPath(candidate) || !AreSamePath(candidate.parent_path(), operation->destinationRoot) ||
			AreSamePath(candidate, operation->finalProjectRoot)) {
			error.clear();
			continue;
		}
		candidates.push_back(candidate);
	}
	return candidates;
}

bool ProjectCreationService::ValidateRequest(
	const ProjectCreationRequest& request,
	ProjectCreationOperation& operation,
	std::string& errorMessage
) {
	if (!request.destinationRoot.is_absolute() || !request.templateSourceRoot.is_absolute() ||
		!IsDirectory(request.destinationRoot) || HasReparsePointInExistingPath(request.destinationRoot) ||
		HasReparsePointInExistingPath(request.templateSourceRoot)) {
		errorMessage = "Destination root or Template Source root is invalid.";
		return false;
	}
	operation.projectId = request.projectId;
	operation.displayName = request.displayName;
	operation.startSceneId = request.startSceneId;
	operation.destinationRoot = std::filesystem::absolute(request.destinationRoot).lexically_normal();
	operation.finalProjectRoot = (operation.destinationRoot / StringUtility::ToPath(request.projectId)).lexically_normal();
	operation.templateSourceRoot = std::filesystem::absolute(request.templateSourceRoot).lexically_normal();
	operation.generatorPath = operation.templateSourceRoot / L"project" / L"tools" / L"New-GameProject.ps1";
	if (!AreSamePath(operation.finalProjectRoot.parent_path(), operation.destinationRoot) ||
		std::filesystem::exists(operation.finalProjectRoot) || !IsRegularFile(operation.generatorPath) ||
		!IsRegularFile(operation.templateSourceRoot / L"project" / L"templates" / L"empty_game" / L"template-manifest.json")) {
		errorMessage = "Final Project path or Template Source generator is invalid.";
		return false;
	}
	ProjectDescriptor descriptor{};
	if (!ConfigureDescriptor(operation, descriptor, errorMessage, true)) {
		return false;
	}
	return true;
}

bool ProjectCreationService::ValidateFinalTree(
	const ProjectCreationOperation& operation,
	std::string& errorMessage
) {
	if (!AreSamePath(operation.finalProjectRoot.parent_path(), operation.destinationRoot) ||
		!IsDirectory(operation.finalProjectRoot) || HasReparsePointInExistingPath(operation.finalProjectRoot)) {
		errorMessage = "Generated final Project root is invalid.";
		return false;
	}
	const std::filesystem::path projectRoot = operation.finalProjectRoot / L"project";
	const std::filesystem::path projectId = StringUtility::ToPath(operation.projectId);
	const std::filesystem::path sceneCatalog = projectRoot / L"resources" / L"scenes" / L"scenes.json";
	if (!IsRegularFile(sceneCatalog) || HasReparsePointInExistingPath(sceneCatalog)) {
		errorMessage = "Generated Project scene catalog is missing or unsafe.";
		return false;
	}
	if (const SolutionGenerationOperation* generation = registry_.FindSolutionGenerationOperation(operation.operationId)) {
		const std::filesystem::path expectedManifest = operation.outputLayout == "ProjectFilesV2"
			? std::filesystem::path(L"intermediate/project-files/solution-generation.json")
			: std::filesystem::path(L"project/build/generated/solution-generation.json");
		if ((!operation.outputLayout.empty() && operation.outputLayout != "ProjectFilesV2" && operation.outputLayout != "GroupedV1") ||
			generation->projectId != operation.projectId || !AreSamePath(generation->projectRoot, operation.finalProjectRoot) ||
			std::count_if(generation->files.begin(), generation->files.end(), [&expectedManifest](const SolutionGenerationOperationFile& file) {
				return file.kind == SolutionGenerationOperationFileKind::Manifest && file.nextExists && file.relativePath == expectedManifest;
			}) != 1 ||
			std::count_if(generation->files.begin(), generation->files.end(), [](const SolutionGenerationOperationFile& file) {
				return file.kind == SolutionGenerationOperationFileKind::Descriptor && file.relativePath == std::filesystem::path(L"game.project.json") && !file.previousExists && file.nextExists;
			}) != 1) {
			errorMessage = "Creation generation journal does not match the Project or initial layout.";
			return false;
		}
		// IDE退役後の中断もFinalizeへ渡す。hash/部分commitの検証と復旧は生成Service/Transactionが所有する。
		return true;
	}
	for (const std::filesystem::path& required : {
		projectRoot / (projectId.native() + L".sln"),
		projectRoot / (projectId.native() + L".vcxproj"),
		projectRoot / (projectId.native() + L".vcxproj.filters")
	}) {
		if (!IsRegularFile(required)) {
			errorMessage = "Generated Project is missing a required file.";
			return false;
		}
	}
	return true;
}

bool ProjectCreationService::ConfigureDescriptor(
	const ProjectCreationOperation& operation,
	ProjectDescriptor& descriptor,
	std::string& errorMessage,
	bool groupedLayout
) {
	descriptor.SetDescriptorPath(operation.finalProjectRoot / L"game.project.json");
	descriptor.SetProjectId(operation.projectId);
	descriptor.SetDisplayName(operation.displayName);
	descriptor.SetPreferredVisualStudioMajor(18);
	descriptor.SetEngineMode(ProjectEngineMode::Snapshot);
	descriptor.SetEngineProvenance({});
	descriptor.SetDependencies({ "dependencies/manifest.json", "dependencies/lock.json" });
	const std::string generatedBase = operation.outputLayout == "ProjectFilesV2" ? "intermediate/project-files/" + operation.projectId : groupedLayout
		? "project/build/generated/" + operation.projectId + "/" + operation.projectId
		: "project/" + operation.projectId;
	descriptor.SetPaths({
		generatedBase + ".sln",
		generatedBase + ".vcxproj",
		"project/resources/scenes/scenes.json",
		"generated/outputs/" + operation.projectId + "/Development/" + operation.projectId + ".exe"
	});
	descriptor.SetTemplate({ "empty-game", 1, "snapshot" });
	return descriptor.Validate(errorMessage);
}

std::filesystem::path ProjectCreationService::FindPowerShell() {
	const DWORD needed = SearchPathW(nullptr, L"pwsh.exe", nullptr, 0, nullptr, nullptr);
	if (needed > 0) {
		std::vector<wchar_t> buffer(needed + 1);
		if (SearchPathW(nullptr, L"pwsh.exe", nullptr, static_cast<DWORD>(buffer.size()), buffer.data(), nullptr) > 0) {
			return std::filesystem::path(buffer.data());
		}
	}
	std::filesystem::path windowsRoot = GetEnvironmentPath(L"WINDIR");
	if (windowsRoot.empty()) {
		windowsRoot = L"C:\\Windows";
	}
	const std::filesystem::path fallback = windowsRoot / L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
	return IsRegularFile(fallback) ? fallback : std::filesystem::path{};
}

uint64_t ProjectCreationService::GetProcessStartTimeFileTime(uint32_t processId) {
	if (processId == 0) {
		return 0;
	}
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == nullptr) {
		return 0;
	}
	FILETIME creation{};
	FILETIME exit{};
	FILETIME kernel{};
	FILETIME user{};
	const BOOL result = GetProcessTimes(process, &creation, &exit, &kernel, &user);
	CloseHandle(process);
	return result ? FileTimeToUint64(creation) : 0;
}

bool ProjectCreationService::IsProcessIdentityRunning(uint32_t processId, uint64_t startTimeFileTime) {
	if (processId == 0 || startTimeFileTime == 0) {
		return false;
	}
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
	if (process == nullptr) {
		return false;
	}
	DWORD exitCode = STILL_ACTIVE;
	FILETIME creation{};
	FILETIME exit{};
	FILETIME kernel{};
	FILETIME user{};
	const bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE &&
		GetProcessTimes(process, &creation, &exit, &kernel, &user) && FileTimeToUint64(creation) == startTimeFileTime;
	CloseHandle(process);
	return running;
}

bool ProjectCreationService::HasReparsePointInExistingPath(const std::filesystem::path& path) {
	std::filesystem::path current = path.root_path();
	if (!current.empty() && IsReparsePoint(current)) {
		return true;
	}
	for (const std::filesystem::path& part : path.relative_path()) {
		current /= part;
		std::error_code error;
		if (!std::filesystem::exists(current, error) || error) {
			break;
		}
		if (IsReparsePoint(current)) {
			return true;
		}
	}
	return false;
}

bool ProjectCreationService::AreSamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
	const std::wstring normalizedLeft = std::filesystem::absolute(left).lexically_normal().native();
	const std::wstring normalizedRight = std::filesystem::absolute(right).lexically_normal().native();
	return CompareStringOrdinal(normalizedLeft.c_str(), -1, normalizedRight.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::string ProjectCreationService::MakeOperationId() {
	std::random_device randomDevice;
	std::mt19937 generator(randomDevice());
	std::uniform_int_distribution<unsigned int> distribution(0, 255);
	std::ostringstream output;
	output << std::hex << std::setfill('0');
	for (size_t index = 0; index < 16; ++index) {
		output << std::setw(2) << distribution(generator);
	}
	return output.str();
}

bool ProjectCreationService::PersistOperation(ProjectCreationOperation& operation, std::string& errorMessage) {
	if (!registry_.UpsertCreationOperation(operation, errorMessage)) {
		return false;
	}
	return registry_.Save(errorMessage);
}

bool ProjectCreationService::Finalize(ProjectCreationOperation& operation, std::string& errorMessage) {
	if (!ValidateFinalTree(operation, errorMessage)) {
		operation.state = ProjectCreationOperationState::Failed;
		PersistOperation(operation, errorMessage);
		SetSnapshot(ProjectCreationServiceState::Failed, operation, errorMessage);
		return false;
	}
	const std::filesystem::path descriptorPath = operation.finalProjectRoot / L"game.project.json";
	const auto RegistrationFailure = [&]() {
		operation.state = ProjectCreationOperationState::RegistrationIncomplete;
		PersistOperation(operation, errorMessage);
		SetSnapshot(ProjectCreationServiceState::RegistrationIncomplete, operation, errorMessage);
		return false;
	};
	const std::filesystem::path headerPath = operation.finalProjectRoot / L"project" / L"engine" / L"utility" / L"EditableResourcePath.h";
	std::string headerContent;
	if (!ReadUtf8File(headerPath, headerContent)) {
		errorMessage = "Generated Project root marker could not be read.";
		return RegistrationFailure();
	}
	const std::string legacyMarker = "path / \"" + operation.projectId + ".vcxproj\"";
	const std::string groupedMarker = "path / \"build/generated/" + operation.projectId + "/" + operation.projectId + ".vcxproj\"";
	const bool groupedPreparation = ContainsExactlyOnce(headerContent, groupedMarker) && headerContent.find(legacyMarker) == std::string::npos;
	const bool legacyPreparation = ContainsExactlyOnce(headerContent, legacyMarker) && headerContent.find(groupedMarker) == std::string::npos;
	const bool projectFilesPreparation = operation.outputLayout == "ProjectFilesV2";
	if ((!operation.outputLayout.empty() && operation.outputLayout != "ProjectFilesV2" && operation.outputLayout != "GroupedV1") ||
		(projectFilesPreparation && !legacyPreparation) || (operation.outputLayout == "GroupedV1" && !groupedPreparation)) { errorMessage = "Creation journal layout disagrees with generated Source."; return RegistrationFailure(); }
	if (groupedPreparation == legacyPreparation) {
		errorMessage = "Generated Project layout marker is missing or ambiguous.";
		return RegistrationFailure();
	}

	ProjectDescriptor descriptor{};
	if (groupedPreparation || projectFilesPreparation) {
		if (!ConfigureDescriptor(operation, descriptor, errorMessage, true)) return RegistrationFailure();
		if (std::filesystem::exists(descriptorPath)) {
			ProjectDescriptor existing{};
			if (!existing.Load(descriptorPath, errorMessage) || existing.GetProjectId() != operation.projectId ||
				!AreSamePath(existing.GetProjectRoot(), operation.finalProjectRoot) ||
				existing.GetPaths().solution != descriptor.GetPaths().solution || existing.GetPaths().msbuildProject != descriptor.GetPaths().msbuildProject) {
				errorMessage = "Existing Grouped V1 descriptor does not match the creation operation.";
				return RegistrationFailure();
			}
		}
		ProjectSolutionGenerationService generation(registry_);
		const std::vector<std::filesystem::path> retiredArtifacts = {
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".sln"),
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".vcxproj"),
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".vcxproj.filters")
		};
		if (!generation.CreateInitialGroupedGeneration(descriptor, operation.operationId, retiredArtifacts, errorMessage, projectFilesPreparation ? SolutionGenerationLayout::ProjectFilesV2 : SolutionGenerationLayout::GroupedV1)) return RegistrationFailure();
	} else {
		if (!ConfigureDescriptor(operation, descriptor, errorMessage, false)) return RegistrationFailure();
		if (std::filesystem::exists(descriptorPath)) {
			if (!descriptor.Load(descriptorPath, errorMessage)) return RegistrationFailure();
			if (descriptor.GetProjectId() != operation.projectId || !AreSamePath(descriptor.GetProjectRoot(), operation.finalProjectRoot)) {
				errorMessage = "Existing descriptor does not match the creation operation.";
				return RegistrationFailure();
			}
		} else if (!descriptor.Save(errorMessage)) {
			return RegistrationFailure();
		}
	}
	ProjectDescriptor verifiedDescriptor{};
	if (!verifiedDescriptor.Load(descriptorPath, errorMessage)) {
		return RegistrationFailure();
	}
	if (groupedPreparation || projectFilesPreparation) {
		ProjectSolutionPreviewSnapshot inspection{};
		ProjectSolutionGenerationService generation(registry_);
		if (projectFilesPreparation && (!generation.InspectProject(descriptorPath, inspection, errorMessage) || inspection.state != ProjectSolutionPreviewState::Ready ||
			inspection.layoutMigrationRequired || inspection.modifiedOwnedArtifactCount != 0)) {
			if (errorMessage.empty()) errorMessage = "Final Grouped V1 generation could not be verified.";
			return RegistrationFailure();
		}
		for (const std::filesystem::path& relativePath : {
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".sln"),
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".vcxproj"),
			std::filesystem::path("project") / StringUtility::ToPath(operation.projectId + ".vcxproj.filters")
		}) {
			if (std::filesystem::exists(operation.finalProjectRoot / relativePath)) {
				errorMessage = "Legacy Root artifact remains after Grouped V1 generation.";
				return RegistrationFailure();
			}
		}
		const ProjectCompatibilityReport compatibility = ProjectCompatibilityProbe{}.Probe(verifiedDescriptor);
		if (!compatibility.IsDescriptorCompatible()) {
			errorMessage = compatibility.errors.empty() ? "Grouped V1 Project compatibility check failed." : compatibility.errors.front();
			return RegistrationFailure();
		}
	}
	ProjectRegistryEntry entry{};
	entry.descriptorPath = descriptorPath;
	if (!registry_.RegisterProject(entry, errorMessage)) {
		operation.state = ProjectCreationOperationState::RegistrationIncomplete;
		PersistOperation(operation, errorMessage);
		SetSnapshot(ProjectCreationServiceState::RegistrationIncomplete, operation, errorMessage);
		return false;
	}
	operation.state = ProjectCreationOperationState::Complete;
	operation.exitCode = 0;
	if (!PersistOperation(operation, errorMessage)) {
		SetSnapshot(ProjectCreationServiceState::RegistrationIncomplete, operation, errorMessage);
		return false;
	}
	SetSnapshot(ProjectCreationServiceState::Complete, operation, "Project created and registered.");
	return true;
}

void ProjectCreationService::SetSnapshot(
	ProjectCreationServiceState state,
	const ProjectCreationOperation& operation,
	const std::string& message
) {
	snapshot_.state = state;
	snapshot_.operation = operation;
	snapshot_.statusMessage = message;
}
