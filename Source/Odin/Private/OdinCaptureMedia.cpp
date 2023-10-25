/* Copyright (c) 2022-2023 4Players GmbH. All rights reserved. */

#include "OdinCaptureMedia.h"

#include "Odin.h"
#include "OdinCore/include/odin.h"
#include "OdinFunctionLibrary.h"

UOdinCaptureMedia::UOdinCaptureMedia(const class FObjectInitializer& PCIP)
    : Super(PCIP)
{
}

void UOdinCaptureMedia::SetRoom(UOdinRoom* connected_room)
{
    this->connected_room_ = connected_room;
}

void UOdinCaptureMedia::RemoveRoom()
{
    this->connected_room_ = nullptr;
}

void UOdinCaptureMedia::SetAudioCapture(UAudioCapture* audio_capture)
{
    if (!audio_capture) {
        UE_LOG(Odin, Error,
               TEXT("UOdinCaptureMedia::SetAudioCapture - audio capture is null, microphone will "
                    "not work."));
    }

    this->audio_capture_ = audio_capture;

    if (this->stream_handle_) {
        odin_media_stream_destroy(this->stream_handle_);
        this->SetMediaHandle(0);
    }

    stream_sample_rate_  = 48000;
    stream_num_channels_ = 1;

    if (audio_capture) {
        stream_sample_rate_  = audio_capture->GetSampleRate();
        stream_num_channels_ = audio_capture->GetNumChannels();
    }

    UE_LOG(Odin, Log,
           TEXT("Initializing Audio Capture stream with Sample Rate: %d and Channels: %d"),
           stream_sample_rate_, stream_num_channels_);
    this->stream_handle_ = odin_audio_stream_create(
        OdinAudioStreamConfig{(uint32_t)stream_sample_rate_, (uint8_t)stream_num_channels_});

    if (audio_capture && audio_capture->IsValidLowLevel()) {
        TFunction<void(const float* InAudio, int32 NumSamples)> fp = [this](const float* InAudio,
                                                                            int32 NumSamples) {
            if (bIsBeingReset)
                return;

            if (nullptr != audio_capture_
                && (stream_sample_rate_ != audio_capture_->GetSampleRate()
                    || stream_num_channels_ != audio_capture_->GetNumChannels())) {
                UE_LOG(
                    Odin, Display,
                    TEXT("Incompatible sample rate, stream: %d, capture: %d. Restarting stream."),
                    stream_sample_rate_, audio_capture_->GetSampleRate());

                HandleInputDeviceChanges();
                return;
            }

            if (this->stream_handle_) {
                odin_audio_push_data(this->stream_handle_, (float*)InAudio, NumSamples);
                // UE_LOG(Odin, Log, TEXT("Pushing data, Num Samples: %d"), NumSamples);
            }
        };
        this->audio_generator_handle_ = audio_capture->AddGeneratorDelegate(fp);
    }
}

void UOdinCaptureMedia::Reset()
{
    if (nullptr != audio_capture_) {
        audio_capture_                = nullptr;
        this->audio_generator_handle_ = {};
    }

    if (this->stream_handle_) {
        odin_media_stream_destroy(this->stream_handle_);
        this->stream_handle_ = 0;
    }
}

OdinReturnCode UOdinCaptureMedia::ResetOdinStream()
{
    FScopeLock lock(&this->capture_generator_delegate_);
    if (nullptr != audio_capture_)
        this->audio_capture_->RemoveGeneratorDelegate(this->audio_generator_handle_);

    this->audio_generator_handle_ = {};

    if (this->stream_handle_) {
        auto result          = odin_media_stream_destroy(this->stream_handle_);
        this->stream_handle_ = 0;
        return result;
    }
    bIsBeingReset = false;

    return 0;
}

void UOdinCaptureMedia::BeginDestroy()
{
    Reset();
    Super::BeginDestroy();
}

void UOdinCaptureMedia::HandleInputDeviceChanges()
{
    bIsBeingReset = true;

    AsyncTask(ENamedThreads::GameThread, [this]() {
        if (!connected_room_.IsValid()) {
            UE_LOG(Odin, Error,
                   TEXT("Missing connected Room on capture stream when trying to reconnect due to "
                        "Input Device change."));
            return;
        }
        if (!audio_capture_) {
            UE_LOG(Odin, Error,
                   TEXT("Missing connected audio capture object on capture stream when trying to "
                        "reconnect due to Input Device change."));
            return;
        }

        auto       capturePointer = audio_capture_;
        const auto roomPointer    = connected_room_.Get();
        roomPointer->UnbindCaptureMedia(this);
        this->ResetOdinStream();
        UOdinCaptureMedia* NewCaptureMedia = UOdinFunctionLibrary::Odin_CreateMedia(capturePointer);
        const OdinRoomHandle        room_handle = roomPointer ? roomPointer->RoomHandle() : 0;
        const OdinMediaStreamHandle media_handle =
            NewCaptureMedia ? NewCaptureMedia->GetMediaHandle() : 0;
        const OdinReturnCode result = odin_room_add_media(room_handle, media_handle);
        if (odin_is_error(result)) {
            const FString FormattedError = UOdinFunctionLibrary::FormatError(result, true);
            UE_LOG(Odin, Error,
                   TEXT("Error during media stream reset due to input device changes: %s"),
                   *FormattedError);
        } else {
            roomPointer->BindCaptureMedia(NewCaptureMedia);
            UE_LOG(Odin, Verbose, TEXT("Binding to New Capture Media."));
        }
    });
}
