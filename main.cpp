#include <iostream>
#include <vector>
#include <fstream>
#include <portaudio.h>

using namespace std;

#define SAMPLE_RATE 48000
#define FRAMES_PER_BUFFER 512

vector<float> recordedSamples;

static int audioCallback(
    const void *inputBuffer,
    void *,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags,
    void *)
{
    if (!inputBuffer) return paContinue;

    const float* in = (const float*)inputBuffer;

    // 2 channels (stereo)
    for (unsigned int i = 0; i < framesPerBuffer * 2; i++) {
        recordedSamples.push_back(in[i]);
    }

    return paContinue;
}

int main() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        cout << "PortAudio init error\n";
        return 1;
    }

    int numDevices = Pa_GetDeviceCount();
    int loopbackDevice = -1;

    // Find Realtek Loopback device automatically
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);

        if (string(info->name).find("[Loopback]") != string::npos &&
            string(info->name).find("Realtek") != string::npos) {
            loopbackDevice = i;
            break;
        }
    }

    if (loopbackDevice == -1) {
        cout << "Realtek Loopback device not found.\n";
        return 1;
    }

    cout << "Using device ID: " << loopbackDevice << endl;

    PaStreamParameters params;
    params.device = loopbackDevice;
    params.channelCount = 2;
    params.sampleFormat = paFloat32;
    params.suggestedLatency =
        Pa_GetDeviceInfo(loopbackDevice)->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = NULL;

    PaStream* stream;

    err = Pa_OpenStream(
        &stream,
        &params,
        NULL,
        SAMPLE_RATE,
        FRAMES_PER_BUFFER,
        paClipOff,
        audioCallback,
        NULL);

    if (err != paNoError) {
        cout << "Open error: " << Pa_GetErrorText(err) << endl;
        return 1;
    }

    Pa_StartStream(stream);

    cout << ">>> PLAY AUDIO NOW. Press Enter to stop recording.\n";
    cin.get();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);

    cout << "Captured samples: " << recordedSamples.size() << endl;

    if (!recordedSamples.empty()) {
        ofstream file("realtek_loopback.raw", ios::binary);
        file.write((char*)recordedSamples.data(),
                   recordedSamples.size() * sizeof(float));
        file.close();
        cout << "Saved realtek_loopback.raw\n";
    } else {
        cout << "No audio captured.\n";
    }

    Pa_Terminate();
    return 0;
}