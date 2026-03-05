#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <portaudio.h>
#include <pa_win_wasapi.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using namespace std;

#define FRAMES_PER_BUFFER 512
#define TARGET_RATE 48000
#define TARGET_CHANNELS 2

#ifndef paWASAPIUseOutputDeviceForInput
    #define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

vector<float> micSamples;
vector<float> spkSamples;

int micChannels;
int spkChannels;
double micRate;
double spkRate;

static int micCallback(const void* inputBuffer, void*, unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    if (!inputBuffer) return paContinue;
    const float* in = (const float*)inputBuffer;
    for (unsigned int i = 0; i < framesPerBuffer * micChannels; i++)
        micSamples.push_back(in[i]);
    return paContinue;
}

static int spkCallback(const void* inputBuffer, void*, unsigned long framesPerBuffer,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    if (!inputBuffer) return paContinue;
    const float* in = (const float*)inputBuffer;
    for (unsigned int i = 0; i < framesPerBuffer * spkChannels; i++)
        spkSamples.push_back(in[i]);
    return paContinue;
}

vector<float> resampleWithMiniaudio(vector<float>& input, int inChannels, double inRate) {
    if (input.empty()) return {};

    ma_data_converter_config config = ma_data_converter_config_init(
        ma_format_f32, ma_format_f32,
        (ma_uint32)inChannels, TARGET_CHANNELS,
        (ma_uint32)inRate, TARGET_RATE
    );
    config.resampling.algorithm      = ma_resample_algorithm_linear;
    config.resampling.linear.lpfOrder = 8;

    ma_data_converter converter;
    if (ma_data_converter_init(&config, NULL, &converter) != MA_SUCCESS) {
        cout << "Resample init failed\n";
        exit(1);
    }

    ma_uint64 framesIn  = input.size() / inChannels;
    ma_uint64 framesOut = 0;
    ma_data_converter_get_expected_output_frame_count(&converter, framesIn, &framesOut);

    vector<float> output(framesOut * TARGET_CHANNELS);
    ma_uint64 actualIn  = framesIn;
    ma_uint64 actualOut = framesOut;

    ma_data_converter_process_pcm_frames(&converter, input.data(), &actualIn, output.data(), &actualOut);
    ma_data_converter_uninit(&converter, NULL);

    output.resize(actualOut * TARGET_CHANNELS);
    return output;
}

int main() {

    Pa_Initialize();

    int numDevices = Pa_GetDeviceCount();
    cout << "Available devices:\n\n";
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* dev = Pa_GetDeviceInfo(i);
        cout << "ID: " << i
             << " | " << dev->name
             << " | In: "  << dev->maxInputChannels
             << " | Out: " << dev->maxOutputChannels
             << " | Rate: " << dev->defaultSampleRate << "\n";
    }

    int micDev, spkDev;
    cout << "\nEnter MIC ID: ";
    cin >> micDev;
    cout << "Enter SPEAKER DEVICE ID (real output, NOT [Loopback]): ";
    cin >> spkDev;
    cin.ignore();

    // -------------- START

    const PaDeviceInfo* micInfo = Pa_GetDeviceInfo(micDev);
    const PaDeviceInfo* spkInfo = Pa_GetDeviceInfo(spkDev);

    micChannels = micInfo->maxInputChannels;
    spkChannels = spkInfo->maxOutputChannels;
    micRate     = micInfo->defaultSampleRate;
    spkRate     = spkInfo->defaultSampleRate;

    cout << "\n--- Quality Check ---\n";
    cout << "Mic:     " << micChannels << "ch @ " << micRate << "Hz\n";
    cout << "Speaker: " << spkChannels << "ch @ " << spkRate << "Hz\n";
    if (micRate < 44100) cout << "WARNING: Mic rate is low (" << micRate << "Hz)\n";
    if (spkRate < 44100) cout << "WARNING: Speaker rate is low (" << spkRate << "Hz)\n";
    cout << "---------------------\n\n";

    PaStreamParameters micParams;
    micParams.device                    = micDev;
    micParams.channelCount              = micChannels;
    micParams.sampleFormat              = paFloat32;
    micParams.suggestedLatency          = micInfo->defaultLowInputLatency;
    micParams.hostApiSpecificStreamInfo = NULL;

    PaStreamParameters spkParams;
    spkParams.device                    = spkDev;
    spkParams.channelCount              = spkChannels;
    spkParams.sampleFormat              = paFloat32;
    spkParams.suggestedLatency          = spkInfo->defaultLowInputLatency;
    spkParams.hostApiSpecificStreamInfo = NULL;

    PaStream* micStream;
    PaStream* spkStream;
    PaError err;

    err = Pa_OpenStream(&micStream, &micParams, NULL, micRate, FRAMES_PER_BUFFER, paClipOff, micCallback, NULL);
    if (err != paNoError) { cout << "ERROR mic: " << Pa_GetErrorText(err) << "\n"; return 1; }

    err = Pa_OpenStream(&spkStream, &spkParams, NULL, spkRate, FRAMES_PER_BUFFER, paClipOff, spkCallback, NULL);
    if (err != paNoError) { cout << "ERROR spk: " << Pa_GetErrorText(err) << "\n"; return 1; }

    Pa_StartStream(micStream);
    Pa_StartStream(spkStream);

    cout << "Recording... Press ENTER to stop\n";
    cin.get();

    Pa_StopStream(micStream);
    Pa_StopStream(spkStream);
    Pa_CloseStream(micStream);
    Pa_CloseStream(spkStream);
    Pa_Terminate();

    cout << "\nCaptured " << micSamples.size() / micChannels << " mic frames\n";
    cout << "Captured " << spkSamples.size() / spkChannels << " speaker frames\n";

    if (spkSamples.empty()) { cout << "ERROR: No speaker audio captured!\n"; return 1; }


    // -------------- RESAMPLING

    cout << "\nResampling to 48kHz Stereo...\n";
    vector<float> mic48 = resampleWithMiniaudio(micSamples, micChannels, micRate);
    vector<float> spk48 = resampleWithMiniaudio(spkSamples, spkChannels, spkRate);

    cout << "Mixing...\n";
    size_t maxSize = max(mic48.size(), spk48.size());
    vector<float> mixed(maxSize, 0.0f);

    for (size_t i = 0; i < mic48.size(); i++) mixed[i] += mic48[i];
    for (size_t i = 0; i < spk48.size(); i++) mixed[i] += spk48[i];

    // Just clamp — no attenuation, no filtering, no touching the signal
    for (size_t i = 0; i < maxSize; i++)
        mixed[i] = max(-1.0f, min(1.0f, mixed[i]));

    ofstream out("audio.raw", ios::binary);
    out.write((char*)mixed.data(), mixed.size() * sizeof(float));
    out.close();

    cout << "\nSuccess! Convert using:\n";
    cout << "ffmpeg -f f32le -ar 48000 -ac 2 -i audio.raw audio.wav\n";

    return 0;
}