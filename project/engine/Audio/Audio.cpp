// 役割: Audio ClipのDecode／cacheとXAudio2 Source Voiceの生成・停止・破棄を実装する。
#include "Audio.h"
#include "AudioDirector.h"
#include "AudioDecoder.h"
#include "AudioStreamDecoder.h"
#include "MediaFoundationRuntime.h"
#include "../utility/EditableResourcePath.h"
#include "../utility/Logger.h"
#include "../utility/StringUtility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <utility>

#include <ks.h>
#include <ksmedia.h>

namespace {
	std::string ToLower(std::string value) {
		std::transform(
			value.begin(),
			value.end(),
			value.begin(),
			[](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			}
		);
		return value;
	}

	bool IsSupportedAudioExtension(const std::filesystem::path& path) {
		const std::string extension = ToLower(path.extension().string());
		return extension == ".wav" || extension == ".mp3" ||
			extension == ".aac" || extension == ".m4a";
	}

	bool IsPointDownmixFormatSupported(const AudioClip& audioClip) {
		const WAVEFORMATEX* waveFormat = audioClip.GetWaveFormat();
		if (
			!waveFormat || waveFormat->nChannels != 2 ||
			waveFormat->nBlockAlign == 0 || audioClip.pcmData.empty() ||
			audioClip.pcmData.size() % waveFormat->nBlockAlign != 0
		) {
			return false;
		}
		const bool isPcm = waveFormat->wFormatTag == WAVE_FORMAT_PCM;
		const bool isExtensiblePcm = waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
			audioClip.waveFormatSize >= sizeof(WAVEFORMATEXTENSIBLE) &&
			IsEqualGUID(audioClip.waveFormat.SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
			(
				audioClip.waveFormat.Samples.wValidBitsPerSample == 0 ||
				audioClip.waveFormat.Samples.wValidBitsPerSample == waveFormat->wBitsPerSample
			);
		return (isPcm || isExtensiblePcm) &&
			(waveFormat->wBitsPerSample == 8 || waveFormat->wBitsPerSample == 16 ||
				waveFormat->wBitsPerSample == 24 || waveFormat->wBitsPerSample == 32) &&
			waveFormat->nBlockAlign == waveFormat->nChannels * waveFormat->wBitsPerSample / 8;
	}

	std::string MakePointDownmixCacheKey(const AudioClip& audioClip) {
		const WAVEFORMATEX* waveFormat = audioClip.GetWaveFormat();
		const uint16_t validBits = waveFormat && waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
			? audioClip.waveFormat.Samples.wValidBitsPerSample
			: 0;
		return ToLower(audioClip.resourcePath) + "|point-downmix-v1|" +
			std::to_string(waveFormat ? waveFormat->wFormatTag : 0) + "|" +
			std::to_string(waveFormat ? waveFormat->nChannels : 0) + "|" +
			std::to_string(waveFormat ? waveFormat->nSamplesPerSec : 0) + "|" +
			std::to_string(waveFormat ? waveFormat->wBitsPerSample : 0) + "|" +
			std::to_string(validBits) + "|" +
			std::to_string(waveFormat ? waveFormat->nBlockAlign : 0) + "|" +
			std::to_string(audioClip.pcmData.size());
	}

	int64_t ReadPcmSample(const BYTE* data, uint16_t bitsPerSample) {
		switch (bitsPerSample) {
		case 8:
			return static_cast<int64_t>(*data) - 128;
		case 16: {
			int16_t value = 0;
			std::memcpy(&value, data, sizeof(value));
			return value;
		}
		case 24: {
			int32_t value = static_cast<int32_t>(data[0]) |
				(static_cast<int32_t>(data[1]) << 8) |
				(static_cast<int32_t>(data[2]) << 16);
			return (value & 0x00800000) != 0 ? value | ~0x00ffffff : value;
		}
		case 32: {
			int32_t value = 0;
			std::memcpy(&value, data, sizeof(value));
			return value;
		}
		default:
			return 0;
		}
	}

	void WritePcmSample(BYTE* data, uint16_t bitsPerSample, int64_t value) {
		const int64_t minimum = bitsPerSample == 32
			? static_cast<int64_t>((std::numeric_limits<int32_t>::min)())
			: -(int64_t{ 1 } << (bitsPerSample - 1));
		const int64_t maximum = bitsPerSample == 32
			? static_cast<int64_t>((std::numeric_limits<int32_t>::max)())
			: (int64_t{ 1 } << (bitsPerSample - 1)) - 1;
		value = (std::clamp)(value, minimum, maximum);
		switch (bitsPerSample) {
		case 8:
			data[0] = static_cast<BYTE>(value + 128);
			break;
		case 16: {
			const int16_t sample = static_cast<int16_t>(value);
			std::memcpy(data, &sample, sizeof(sample));
			break;
		}
		case 24: {
			const uint32_t sample = static_cast<uint32_t>(static_cast<int32_t>(value));
			data[0] = static_cast<BYTE>(sample & 0xff);
			data[1] = static_cast<BYTE>((sample >> 8) & 0xff);
			data[2] = static_cast<BYTE>((sample >> 16) & 0xff);
			break;
		}
		case 32: {
			const int32_t sample = static_cast<int32_t>(value);
			std::memcpy(data, &sample, sizeof(sample));
			break;
		}
		}
	}
}

Audio* Audio::instance = nullptr;

Audio::Audio() : voiceCallback_(this), audioDirector_(std::make_unique<AudioDirector>(*this)) {}
Audio::~Audio() = default;

Audio* Audio::GetInstance() {
	return instance;
}

void Audio::Initialize(const MediaFoundationRuntime* mediaFoundationRuntime){
	Finalize();
	mediaFoundationRuntime_ = mediaFoundationRuntime;
	HRESULT result = XAudio2Create(
		&xAudio2_,
		0,
		XAUDIO2_DEFAULT_PROCESSOR
	);
	if (FAILED(result)) {
		return;
	}

	result = xAudio2_->CreateMasteringVoice(&masterVoice_);
	if (FAILED(result)) {
		xAudio2_.Reset();
		return;
	}
	DWORD speakerChannelMask = 0;
	XAUDIO2_VOICE_DETAILS voiceDetails{};
	if (SUCCEEDED(masterVoice_->GetChannelMask(&speakerChannelMask))) {
		masterVoice_->GetVoiceDetails(&voiceDetails);
		if (speakerChannelMask != 0 && voiceDetails.InputChannels != 0) {
			if (SUCCEEDED(X3DAudioInitialize(
				speakerChannelMask,
				X3DAUDIO_SPEED_OF_SOUND,
				x3dAudio_
			))) {
				masteringChannelCount_ = voiceDetails.InputChannels;
				x3dAudioInitialized_ = true;
			}
		}
	}
	instance = this;
}

void Audio::Finalize(){
	gamePaused_ = gamePauseBgm_ = false;
	if (audioDirector_) {
		audioDirector_->Reset();
	}
	std::vector<uint64_t> playbackIds;
	playbackIds.reserve(playbacks_.size());
	for (const auto& [playbackId, state] : playbacks_) {
		(void)state;
		playbackIds.push_back(playbackId);
	}
	for (uint64_t playbackId : playbackIds) {
		DestroyPlayback(playbackId);
	}
	CollectRetiredPlaybacks(true);
	{
		std::scoped_lock lock(callbackEventMutex_);
		callbackEvents_.clear();
	}
	naturallyCompletedPlaybackIds_.clear();
	if (masterVoice_) {
		masterVoice_->DestroyVoice();
		masterVoice_ = nullptr;
	}
	x3dAudioInitialized_ = false;
	masteringChannelCount_ = 0;
	xAudio2_.Reset();
	clipCache_.clear();
	pointDownmixClipCache_.clear();
	metadataCache_.clear();
	mediaFoundationRuntime_ = nullptr;
	if (instance == this) {
		instance = nullptr;
	}
}

void Audio::Update(float realDeltaTime) {
	ProcessCallbackEvents();
	UpdateStreamingPlaybacks();
	CollectRetiredPlaybacks(false);
	if (audioDirector_) {
		// BGMも止める設定ではcrossfadeの時計も保持する。callback/stream回収は継続する。
		audioDirector_->Update(gamePaused_ && gamePauseBgm_ ? 0.0f : realDeltaTime);
	}
	for (auto iterator = clipCache_.begin(); iterator != clipCache_.end();) {
		if (iterator->second.expired()) {
			iterator = clipCache_.erase(iterator);
		} else {
			++iterator;
		}
	}
	for (auto iterator = pointDownmixClipCache_.begin(); iterator != pointDownmixClipCache_.end();) {
		if (iterator->second.expired()) {
			iterator = pointDownmixClipCache_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

AudioClipPtr Audio::LoadAudioFile(const char* filename, std::string* error){
	if (error) {
		error->clear();
	}
	if (!filename || filename[0] == '\0') {
		if (error) {
			*error = "Audio path is empty.";
		}
		return {};
	}
	if (!mediaFoundationRuntime_ || !mediaFoundationRuntime_->IsInitialized()) {
		if (error) {
			*error = mediaFoundationRuntime_ &&
				!mediaFoundationRuntime_->GetLastError().empty()
				? mediaFoundationRuntime_->GetLastError()
				: "Media Foundation is not initialized.";
		}
		return {};
	}

	const std::filesystem::path requestedPath = StringUtility::ToPath(filename);
	if (!IsSupportedAudioExtension(requestedPath)) {
		if (error) {
			*error = "Unsupported audio extension. Use WAV, MP3, AAC, or M4A.";
		}
		return {};
	}

	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(requestedPath).lexically_normal();
	const std::string cacheKey = ToLower(StringUtility::ToUtf8(resolvedPath));
	if (const auto found = clipCache_.find(cacheKey); found != clipCache_.end()) {
		if (AudioClipPtr cachedClip = found->second.lock()) {
			return cachedClip;
		}
		clipCache_.erase(found);
	}

	const std::string resourcePath = StringUtility::ToUtf8(
		EditableResourcePath::ToProjectRelative(resolvedPath)
	);
	AudioDecodeResult decoded = AudioDecoder::DecodeFile(
		resolvedPath,
		resourcePath
	);
	if (!decoded.clip) {
		if (error) {
			*error = resourcePath + ": " + decoded.error;
		}
		return {};
	}

	AudioClipPtr audioClip = std::move(decoded.clip);
	clipCache_[cacheKey] = audioClip;
	return audioClip;
}

bool Audio::CanProbeAudioFileMetadata() const {
	return mediaFoundationRuntime_ && mediaFoundationRuntime_->IsInitialized();
}

bool Audio::TryGetAudioFileMetadata(
	const char* filename,
	AudioFileMetadata& metadata,
	std::string* error
) {
	metadata = {};
	if (error) {
		error->clear();
	}
	if (!filename || filename[0] == '\0') {
		if (error) {
			*error = "Audio path is empty.";
		}
		return false;
	}
	if (!CanProbeAudioFileMetadata()) {
		if (error) {
			*error = "Media Foundation is not initialized.";
		}
		return false;
	}

	const std::filesystem::path requestedPath = StringUtility::ToPath(filename);
	if (!IsSupportedAudioExtension(requestedPath)) {
		if (error) {
			*error = "Unsupported audio extension. Use WAV, MP3, AAC, or M4A.";
		}
		return false;
	}
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(requestedPath).lexically_normal();
	std::error_code filesystemError;
	if (!std::filesystem::is_regular_file(resolvedPath, filesystemError)) {
		if (error) {
			*error = "Audio file is missing or inaccessible.";
		}
		return false;
	}
	const uintmax_t fileSize = std::filesystem::file_size(resolvedPath, filesystemError);
	if (filesystemError) {
		if (error) {
			*error = "Audio file size could not be read.";
		}
		return false;
	}
	const std::filesystem::file_time_type lastWriteTime =
		std::filesystem::last_write_time(resolvedPath, filesystemError);
	if (filesystemError) {
		if (error) {
			*error = "Audio file timestamp could not be read.";
		}
		return false;
	}

	const std::string cacheKey = ToLower(StringUtility::ToUtf8(resolvedPath));
	if (const auto found = metadataCache_.find(cacheKey); found != metadataCache_.end() &&
		found->second.fileSize == fileSize &&
		found->second.lastWriteTime == lastWriteTime) {
		metadata = found->second.metadata;
		if (error) {
			*error = found->second.error;
		}
		return found->second.succeeded;
	}

	AudioMetadataProbeResult probe = AudioDecoder::ProbeFile(resolvedPath);
	AudioMetadataCacheEntry entry{};
	entry.fileSize = fileSize;
	entry.lastWriteTime = lastWriteTime;
	entry.succeeded = probe.error.empty();
	entry.metadata = probe.metadata;
	entry.error = probe.error;
	metadataCache_[cacheKey] = entry;
	metadata = entry.metadata;
	if (error) {
		*error = entry.error;
	}
	return entry.succeeded;
}

AudioClipPtr Audio::GetOrCreatePointDownmixClip(
	const AudioClipPtr& audioClip,
	std::string* error
) {
	if (error) {
		error->clear();
	}
	if (!audioClip || !IsPointDownmixFormatSupported(*audioClip)) {
		if (error) {
			*error = "ThreeD Point Downmix requires stereo integer PCM with 8, 16, 24, or 32 valid bits.";
		}
		return {};
	}
	const std::string cacheKey = MakePointDownmixCacheKey(*audioClip);
	if (const auto found = pointDownmixClipCache_.find(cacheKey); found != pointDownmixClipCache_.end()) {
		if (AudioClipPtr cachedClip = found->second.lock()) {
			return cachedClip;
		}
		pointDownmixClipCache_.erase(found);
	}

	const WAVEFORMATEX* sourceFormat = audioClip->GetWaveFormat();
	const uint32_t bytesPerSample = sourceFormat->wBitsPerSample / 8;
	const size_t frameCount = audioClip->pcmData.size() / sourceFormat->nBlockAlign;
	auto derivedClip = std::make_shared<AudioClip>();
	derivedClip->resourcePath = audioClip->resourcePath + "#point-downmix-v1";
	derivedClip->container = audioClip->container;
	derivedClip->waveFormat = audioClip->waveFormat;
	derivedClip->waveFormatSize = audioClip->waveFormatSize;
	derivedClip->waveFormat.Format.nChannels = 1;
	derivedClip->waveFormat.Format.nBlockAlign = static_cast<WORD>(bytesPerSample);
	derivedClip->waveFormat.Format.nAvgBytesPerSec =
		derivedClip->waveFormat.Format.nSamplesPerSec * derivedClip->waveFormat.Format.nBlockAlign;
	if (derivedClip->waveFormat.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		derivedClip->waveFormat.dwChannelMask = SPEAKER_FRONT_CENTER;
	}
	derivedClip->pcmData.resize(frameCount * bytesPerSample);
	for (size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
		const BYTE* inputFrame = audioClip->pcmData.data() + frameIndex * sourceFormat->nBlockAlign;
		const int64_t left = ReadPcmSample(inputFrame, sourceFormat->wBitsPerSample);
		const int64_t right = ReadPcmSample(inputFrame + bytesPerSample, sourceFormat->wBitsPerSample);
		WritePcmSample(
			derivedClip->pcmData.data() + frameIndex * bytesPerSample,
			sourceFormat->wBitsPerSample,
			(left + right) / 2
		);
	}
	derivedClip->durationSeconds = audioClip->durationSeconds;
	derivedClip->channelCount = 1;
	derivedClip->sampleRate = audioClip->sampleRate;
	derivedClip->bitsPerSample = audioClip->bitsPerSample;

	AudioClipPtr result = std::move(derivedClip);
	pointDownmixClipCache_[cacheKey] = result;
	return result;
}

Audio::PlaybackHandle Audio::PlayAudioClip(
	const AudioClipPtr& audioClip,
	const AudioPlaybackOptions& options
){
	PlaybackHandle handle{};
	const WAVEFORMATEX* waveFormat = audioClip
		? audioClip->GetWaveFormat()
		: nullptr;
	if (!xAudio2_ || !audioClip || !waveFormat || audioClip->pcmData.empty()) {
		return handle;
	}

	const uint64_t playbackId = nextPlaybackId_++;
	IXAudio2SourceVoice* sourceVoice = nullptr;
	HRESULT result = xAudio2_->CreateSourceVoice(
		&sourceVoice,
		waveFormat,
		0,
		XAUDIO2_DEFAULT_FREQ_RATIO,
		&voiceCallback_
	);
	if (FAILED(result)) {
		return handle;
	}

	XAUDIO2_BUFFER buffer{};
	buffer.pAudioData = audioClip->pcmData.data();
	buffer.AudioBytes = static_cast<UINT32>(audioClip->pcmData.size());
	buffer.Flags = XAUDIO2_END_OF_STREAM;
	if (options.loop) {
		buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
	}
	auto playback = std::make_unique<PlaybackState>();
	playback->voice = sourceVoice;
	playback->audioClip = audioClip;
	playback->bus = options.bus;
	playback->sourceVolume = (std::max)(options.volume, 0.0f);
	playback->pitch = (std::clamp)(options.pitch, 0.01f, XAUDIO2_MAX_FREQ_RATIO);
	playback->loop = options.loop;
	playback->trackNaturalCompletion = options.trackNaturalCompletion;
	playback->clipContext = std::make_unique<BufferCallbackContext>(
		BufferCallbackContext{ playbackId, 0, false, true }
	);
	buffer.pContext = playback->clipContext.get();
	result = sourceVoice->SubmitSourceBuffer(&buffer);
	if (FAILED(result)) {
		sourceVoice->DestroyVoice();
		return handle;
	}

	PlaybackState& playbackState = *playback;
	playbacks_.emplace(playbackId, std::move(playback));
	ApplyVolume(playbackState);
	sourceVoice->SetFrequencyRatio(playbackState.pitch);
	// 停止中の新規Voiceを一瞬鳴らさない。解除時は手動理由とのORを再確認する。
	const bool startNow = !IsGamePauseTarget(playbackState);
	result = startNow ? sourceVoice->Start() : S_OK;
	if (FAILED(result)) {
		DestroyPlayback(playbackId);
		return handle;
	}
	handle.id_ = playbackId;
	playbackState.voiceStarted = startNow;
	return handle;
}

Audio::PlaybackHandle Audio::PlayAudioStream(
	const char* filename,
	const AudioPlaybackOptions& options,
	std::string* error
) {
	PlaybackHandle handle{};
	if (error) {
		error->clear();
	}
	if (!xAudio2_ || !filename || filename[0] == '\0') {
		if (error) {
			*error = "Audio streaming path is empty or Audio is not initialized.";
		}
		return handle;
	}
	if (!mediaFoundationRuntime_ || !mediaFoundationRuntime_->IsInitialized()) {
		if (error) {
			*error = "Media Foundation is not initialized.";
		}
		return handle;
	}
	const std::filesystem::path requestedPath = StringUtility::ToPath(filename);
	if (!IsSupportedAudioExtension(requestedPath)) {
		if (error) {
			*error = "Unsupported audio extension. Use WAV, MP3, AAC, or M4A.";
		}
		return handle;
	}
	const std::filesystem::path resolvedPath =
		EditableResourcePath::ResolveResource(requestedPath).lexically_normal();
	auto decoder = std::make_unique<AudioStreamDecoder>();
	if (!decoder->Start(resolvedPath, options.loop)) {
		if (error) {
			*error = "Audio streaming worker could not be started.";
		}
		return handle;
	}

	const uint64_t playbackId = nextPlaybackId_++;
	auto playback = std::make_unique<PlaybackState>();
	playback->streamDecoder = std::move(decoder);
	playback->bus = options.bus;
	playback->sourceVolume = (std::max)(options.volume, 0.0f);
	playback->pitch = (std::clamp)(options.pitch, 0.01f, XAUDIO2_MAX_FREQ_RATIO);
	playback->loop = options.loop;
	playback->trackNaturalCompletion = options.trackNaturalCompletion;
	playback->streaming = true;
	for (uint32_t index = 0; index < AudioStreamDecoder::kSlotCount; ++index) {
		playback->streamContexts[index] = std::make_unique<BufferCallbackContext>(
			BufferCallbackContext{ playbackId, index, true, false }
		);
	}
	playbacks_.emplace(playbackId, std::move(playback));
	handle.id_ = playbackId;
	return handle;
}

void Audio::SetBusVolume(AudioBus bus, float volume) {
	busVolumes_[static_cast<size_t>(bus)] = (std::max)(volume, 0.0f);
	for (auto& [id, playback] : playbacks_) {
		if (bus == AudioBus::Master || playback->bus == bus) {
			ApplyVolume(*playback);
		}
	}
}

bool Audio::IsGamePauseTarget(const PlaybackState& playback) const {
	return gamePaused_ && (playback.bus == AudioBus::SFX || playback.bus == AudioBus::Ambience ||
		(gamePauseBgm_ && playback.bus == AudioBus::BGM));
}
void Audio::SetGamePause(bool paused, bool pauseBgm) {
	if (gamePaused_ == paused && gamePauseBgm_ == pauseBgm) return;
	const bool oldPaused = gamePaused_, oldBgm = gamePauseBgm_;
	gamePaused_ = paused; gamePauseBgm_ = pauseBgm;
	for (auto& [id, state] : playbacks_) {
		auto& p = *state;
		const bool wasPaused = oldPaused && (p.bus == AudioBus::SFX || p.bus == AudioBus::Ambience ||
			(oldBgm && p.bus == AudioBus::BGM));
		const bool nowPaused = IsGamePauseTarget(p);
		if (!p.voice || wasPaused == nowPaused) continue;
		if (nowPaused) p.voice->Stop();
		else if (!p.paused && (!p.streaming || p.submittedStreamBuffers > 0)) {
			if (SUCCEEDED(p.voice->Start())) p.voiceStarted = true;
		}
	}
}
void Audio::Pause(PlaybackHandle& handle) {
	const auto found = playbacks_.find(handle.id_);
	if (found == playbacks_.end()) {
		return;
	}
	found->second->paused = true;
	if (found->second->voice) {
		found->second->voice->Stop();
	}
}

void Audio::Resume(PlaybackHandle& handle) {
	const auto found = playbacks_.find(handle.id_);
	if (found == playbacks_.end()) {
		return;
	}
	found->second->paused = false;
	if (found->second->voice && !IsGamePauseTarget(*found->second) &&
		(!found->second->streaming || found->second->submittedStreamBuffers > 0)) {
		if (SUCCEEDED(found->second->voice->Start())) {
			found->second->voiceStarted = true;
		}
	}
}

void Audio::SetPlaybackFadeGain(const PlaybackHandle& handle, float gain) {
	const auto found = playbacks_.find(handle.id_);
	if (found == playbacks_.end()) {
		return;
	}
	found->second->fadeGain = (std::clamp)(gain, 0.0f, 1.0f);
	ApplyVolume(*found->second);
}

void Audio::SetPlaybackProperties(
	const PlaybackHandle& handle,
	float volume,
	float pitch,
	bool loop
) {
	const auto found = playbacks_.find(handle.id_);
	if (found == playbacks_.end()) {
		return;
	}
	found->second->sourceVolume = (std::max)(volume, 0.0f);
	found->second->pitch = (std::clamp)(pitch, 0.01f, XAUDIO2_MAX_FREQ_RATIO);
	found->second->loop = loop;
	if (found->second->streamDecoder) {
		found->second->streamDecoder->SetLoop(loop);
	}
	ApplyVolume(*found->second);
	if (found->second->voice) {
		found->second->voice->SetFrequencyRatio(found->second->pitch);
	}
}

bool Audio::IsPlaybackStarted(const PlaybackHandle& handle) const {
	const auto found = playbacks_.find(handle.id_);
	return found != playbacks_.end() && found->second->voiceStarted;
}

bool Audio::ConsumeNaturalCompletion(const PlaybackHandle& handle) {
	if (!handle.IsValid()) {
		return false;
	}
	return naturallyCompletedPlaybackIds_.erase(handle.id_) != 0;
}

bool Audio::ConsumePersistentBgmCompletion(
	uint64_t sceneOwnerId,
	uint64_t componentKey
) {
	return audioDirector_ && audioDirector_->ConsumeFinished(
		sceneOwnerId,
		componentKey
	);
}

bool Audio::SetSpatialParameters(
	const PlaybackHandle& handle,
	const AudioSpatialParameters& parameters
) {
	const auto found = playbacks_.find(handle.id_);
	if (
		!x3dAudioInitialized_ || !masterVoice_ ||
		masteringChannelCount_ == 0 ||
		found == playbacks_.end() || !found->second->voice ||
		!found->second->audioClip ||
		!found->second->audioClip->GetWaveFormat() ||
		parameters.minimumDistance < 0.0f ||
		parameters.minimumDistance >= parameters.maximumDistance
	) {
		return false;
	}
	const UINT32 sourceChannelCount = found->second->audioClip->GetWaveFormat()->nChannels;
	if (
		(parameters.layout == AudioSpatialLayout::Point && sourceChannelCount != 1) ||
		(parameters.layout == AudioSpatialLayout::StereoArea &&
			(sourceChannelCount != 2 || !std::isfinite(parameters.stereoAreaWidth) ||
				parameters.stereoAreaWidth <= 0.0f))
	) {
		return false;
	}

	const float minimumNormalized = parameters.minimumDistance /
		parameters.maximumDistance;
	X3DAUDIO_DISTANCE_CURVE_POINT volumePoints[] = {
		{ 0.0f, 1.0f },
		{ minimumNormalized, 1.0f },
		{ 1.0f, 0.0f }
	};
	X3DAUDIO_DISTANCE_CURVE volumeCurve{
		volumePoints,
		static_cast<UINT32>(std::size(volumePoints))
	};
	X3DAUDIO_LISTENER listener{};
	listener.Position = {
		parameters.listenerPosition.x,
		parameters.listenerPosition.y,
		parameters.listenerPosition.z
	};
	listener.OrientFront = {
		parameters.listenerFront.x,
		parameters.listenerFront.y,
		parameters.listenerFront.z
	};
	listener.OrientTop = {
		parameters.listenerTop.x,
		parameters.listenerTop.y,
		parameters.listenerTop.z
	};
	X3DAUDIO_EMITTER emitter{};
	emitter.ChannelCount = sourceChannelCount;
	emitter.CurveDistanceScaler = parameters.maximumDistance;
	emitter.pVolumeCurve = &volumeCurve;
	emitter.Position = {
		parameters.emitterPosition.x,
		parameters.emitterPosition.y,
		parameters.emitterPosition.z
	};
	emitter.OrientFront = {
		parameters.emitterFront.x,
		parameters.emitterFront.y,
		parameters.emitterFront.z
	};
	emitter.OrientTop = {
		parameters.emitterTop.x,
		parameters.emitterTop.y,
		parameters.emitterTop.z
	};
	float channelAzimuths[2] = {};
	if (parameters.layout == AudioSpatialLayout::StereoArea) {
		channelAzimuths[0] = 3.0f * X3DAUDIO_PI / 2.0f;
		channelAzimuths[1] = X3DAUDIO_PI / 2.0f;
		emitter.ChannelRadius = parameters.stereoAreaWidth / 2.0f;
		emitter.pChannelAzimuths = channelAzimuths;
	}
	std::vector<float> matrix(sourceChannelCount * masteringChannelCount_);
	X3DAUDIO_DSP_SETTINGS settings{};
	settings.SrcChannelCount = sourceChannelCount;
	settings.DstChannelCount = masteringChannelCount_;
	settings.pMatrixCoefficients = matrix.data();
	// Dopplerは速度とTeleport規約が未設計のため、このPackageでは計算しない。
	X3DAudioCalculate(
		x3dAudio_,
		&listener,
		&emitter,
		X3DAUDIO_CALCULATE_MATRIX,
		&settings
	);
	return SUCCEEDED(found->second->voice->SetOutputMatrix(
		masterVoice_,
		sourceChannelCount,
		masteringChannelCount_,
		matrix.data()
	));
}

Audio::SoundDataPtr Audio::SoundLoadWave(const char* filename) {
	return LoadAudioFile(filename);
}

Audio::PlaybackHandle Audio::SoundPlayWave(const SoundDataPtr& soundData) {
	return PlayAudioClip(soundData);
}

void Audio::SoundStop(PlaybackHandle& handle) {
	if (!handle.IsValid()) {
		return;
	}
	naturallyCompletedPlaybackIds_.erase(handle.id_);
	DestroyPlayback(handle.id_);
	handle.id_ = 0;
}

bool Audio::IsPlaying(const PlaybackHandle& handle) const {
	return handle.IsValid() && playbacks_.contains(handle.id_);
}

bool Audio::ResolvePersistentBgm(const PersistentBgmRequest& request) {
	if (audioDirector_) {
		return audioDirector_->Resolve(request);
	}
	return false;
}

void Audio::ResolveNoPersistentBgm(uint64_t sceneOwnerId) {
	if (audioDirector_) {
		audioDirector_->ResolveNone(sceneOwnerId);
	}
}

void Audio::BeginPersistentBgmSceneTransition(uint64_t sceneOwnerId) {
	if (audioDirector_) {
		audioDirector_->BeginSceneTransition(sceneOwnerId);
	}
}

void Audio::ReleasePersistentBgmOwner(uint64_t sceneOwnerId) {
	if (audioDirector_) {
		audioDirector_->ReleaseOwner(sceneOwnerId);
	}
}

void Audio::StopPersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey) {
	if (audioDirector_) {
		audioDirector_->Stop(sceneOwnerId, componentKey);
	}
}

void Audio::PausePersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey) {
	if (audioDirector_) {
		audioDirector_->Pause(sceneOwnerId, componentKey);
	}
}

void Audio::ResumePersistentBgm(uint64_t sceneOwnerId, uint64_t componentKey) {
	if (audioDirector_) {
		audioDirector_->Resume(sceneOwnerId, componentKey);
	}
}

void Audio::VoiceCallback::OnBufferEnd(void* bufferContext) {
	if (!owner_ || !bufferContext) {
		return;
	}
	owner_->QueueCallbackEvent(
		*static_cast<const BufferCallbackContext*>(bufferContext),
		false
	);
}

void Audio::VoiceCallback::OnVoiceError(
	void* bufferContext,
	HRESULT
) {
	if (!owner_ || !bufferContext) {
		return;
	}
	owner_->QueueCallbackEvent(
		*static_cast<const BufferCallbackContext*>(bufferContext),
		true
	);
}

void Audio::QueueCallbackEvent(
	const BufferCallbackContext& context,
	bool error
) {
	if (context.playbackId == 0) {
		return;
	}
	std::scoped_lock lock(callbackEventMutex_);
	callbackEvents_.push_back({ context, error });
}

void Audio::ProcessCallbackEvents() {
	std::vector<CallbackEvent> events;
	{
		std::scoped_lock lock(callbackEventMutex_);
		events.swap(callbackEvents_);
	}
	std::unordered_set<uint64_t> failedPlaybackIds;
	for (const CallbackEvent& event : events) {
		if (event.error) {
			failedPlaybackIds.insert(event.context.playbackId);
		}
	}
	for (const CallbackEvent& event : events) {
		const auto found = playbacks_.find(event.context.playbackId);
		if (found == playbacks_.end()) {
			continue;
		}
		PlaybackState& playback = *found->second;
		if (event.error || failedPlaybackIds.contains(event.context.playbackId)) {
			naturallyCompletedPlaybackIds_.erase(event.context.playbackId);
			DestroyPlayback(event.context.playbackId);
			continue;
		}
		if (!event.context.streaming) {
			if (playback.trackNaturalCompletion && !playback.loop) {
				naturallyCompletedPlaybackIds_.insert(event.context.playbackId);
			}
			DestroyPlayback(event.context.playbackId);
			continue;
		}
		if (playback.streamDecoder) {
			playback.streamDecoder->ReleaseSubmittedSlot(event.context.slotIndex);
		}
		if (playback.submittedStreamBuffers > 0) {
			--playback.submittedStreamBuffers;
		}
		if (event.context.endOfStream) {
			if (playback.trackNaturalCompletion && !playback.loop) {
				naturallyCompletedPlaybackIds_.insert(event.context.playbackId);
			}
			DestroyPlayback(event.context.playbackId);
		}
	}
}

void Audio::UpdateStreamingPlaybacks() {
	std::vector<uint64_t> playbackIds;
	for (const auto& [playbackId, playback] : playbacks_) {
		if (playback->streaming) {
			playbackIds.push_back(playbackId);
		}
	}
	for (uint64_t playbackId : playbackIds) {
		const auto found = playbacks_.find(playbackId);
		if (found == playbacks_.end()) {
			continue;
		}
		PlaybackState& playback = *found->second;
		if (!playback.streamDecoder) {
			DestroyPlayback(playbackId);
			continue;
		}
		if (playback.streamDecoder->HasFailed()) {
			Logger::Log("Audio streaming failed: " + playback.streamDecoder->GetError() + "\n");
			DestroyPlayback(playbackId);
			continue;
		}

		if (!playback.voice) {
			WAVEFORMATEXTENSIBLE waveFormat{};
			uint32_t waveFormatSize = 0;
			const bool formatReady = playback.streamDecoder->TryGetWaveFormat(
				waveFormat,
				waveFormatSize
			);
			const uint32_t readyCount = playback.streamDecoder->GetReadySlotCount();
			if (
				!formatReady ||
				(readyCount < 2 && !playback.streamDecoder->IsWorkerFinished()) ||
				readyCount == 0
			) {
				continue;
			}
			(void)waveFormatSize;
			HRESULT result = xAudio2_->CreateSourceVoice(
				&playback.voice,
				&waveFormat.Format,
				0,
				XAUDIO2_DEFAULT_FREQ_RATIO,
				&voiceCallback_
			);
			if (FAILED(result)) {
				DestroyPlayback(playbackId);
				continue;
			}
			playback.voice->SetFrequencyRatio(playback.pitch);
			ApplyVolume(playback);
		}

		bool submitFailed = false;
		AudioStreamDecoder::SlotView slot{};
		while (playback.streamDecoder->TryAcquireReadySlot(slot)) {
			BufferCallbackContext& context = *playback.streamContexts[slot.index];
			context.endOfStream = slot.endOfStream;
			XAUDIO2_BUFFER buffer{};
			buffer.pAudioData = slot.data;
			buffer.AudioBytes = slot.size;
			buffer.Flags = slot.endOfStream ? XAUDIO2_END_OF_STREAM : 0;
			buffer.pContext = &context;
			const HRESULT result = playback.voice->SubmitSourceBuffer(&buffer);
			if (FAILED(result)) {
				playback.streamDecoder->ReleaseSubmittedSlot(slot.index);
				submitFailed = true;
				break;
			}
			++playback.submittedStreamBuffers;
		}
		if (submitFailed) {
			DestroyPlayback(playbackId);
			continue;
		}
		if (!playback.voiceStarted && !playback.paused && !IsGamePauseTarget(playback) && playback.submittedStreamBuffers > 0) {
			if (FAILED(playback.voice->Start())) {
				DestroyPlayback(playbackId);
				continue;
			}
			playback.voiceStarted = true;
		}
	}
}

void Audio::DestroyPlaybackVoice(PlaybackState& playback) {
	if (!playback.voice) {
		return;
	}
	playback.voice->Stop();
	playback.voice->FlushSourceBuffers();
	playback.voice->DestroyVoice();
	playback.voice = nullptr;
}

void Audio::DestroyPlayback(uint64_t playbackId) {
	const auto found = playbacks_.find(playbackId);
	if (found == playbacks_.end()) {
		return;
	}
	std::unique_ptr<PlaybackState> playback = std::move(found->second);
	playbacks_.erase(found);
	if (playback->streamDecoder) {
		playback->streamDecoder->Cancel();
	}
	DestroyPlaybackVoice(*playback);
	if (playback->streamDecoder) {
		retiringPlaybacks_.push_back(std::move(playback));
	}
}

void Audio::CollectRetiredPlaybacks(bool waitForAll) {
	for (auto iterator = retiringPlaybacks_.begin(); iterator != retiringPlaybacks_.end();) {
		PlaybackState& playback = **iterator;
		if (!playback.streamDecoder) {
			iterator = retiringPlaybacks_.erase(iterator);
			continue;
		}
		if (waitForAll) {
			playback.streamDecoder->Cancel();
			playback.streamDecoder->Join();
			iterator = retiringPlaybacks_.erase(iterator);
			continue;
		}
		if (playback.streamDecoder->IsWorkerFinished()) {
			playback.streamDecoder->Join();
			iterator = retiringPlaybacks_.erase(iterator);
		} else {
			++iterator;
		}
	}
}

float Audio::GetEffectiveVolume(const PlaybackState& playback) const {
	return playback.sourceVolume * playback.fadeGain *
		busVolumes_[static_cast<size_t>(AudioBus::Master)] *
		busVolumes_[static_cast<size_t>(playback.bus)];
}

void Audio::ApplyVolume(PlaybackState& playback) {
	if (playback.voice) {
		playback.voice->SetVolume(GetEffectiveVolume(playback));
	}
}
