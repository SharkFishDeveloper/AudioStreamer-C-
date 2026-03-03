#include <iostream>
#include <vector>
#include <fstream>
#include <portaudio.h>

using namespace std;

#define SAMPLE_RATE 16000
#define FRAMES_PER_BUFFER 512

vector<float> recordedSamples;

static int micCallback(
    const void *inputBuffer,
    void *,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags,
    void *)
{
    if (!inputBuffer) return paContinue;

    const float* in = (const float*)inputBuffer;

    // Mono mic
    for (unsigned int i = 0; i < framesPerBuffer; i++) {
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
    int jibMicDevice = -1;

    // Find JIB mic (NOT loopback)
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        string name = info->name;

        if (name.find("Headset (JIB TRUE 2)") != string::npos) {
            jibMicDevice = i;
            break;
        }
    }

    if (jibMicDevice == -1) {
        cout << "JIB mic not found.\n";
        return 1;
    }

    cout << "Using JIB mic ID: " << jibMicDevice << endl;

    PaStreamParameters params;
    params.device = jibMicDevice;
    params.channelCount = 1;   // mono
    params.sampleFormat = paFloat32;
    params.suggestedLatency =
        Pa_GetDeviceInfo(jibMicDevice)->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = NULL;

    PaStream* stream;

    err = Pa_OpenStream(
        &stream,
        &params,
        NULL,
        SAMPLE_RATE,
        FRAMES_PER_BUFFER,
        paClipOff,
        micCallback,
        NULL);

    if (err != paNoError) {
        cout << "Open error: " << Pa_GetErrorText(err) << endl;
        return 1;
    }

    Pa_StartStream(stream);

    cout << ">>> Speak into JIB mic. Press Enter to stop.\n";
    cin.get();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);

    cout << "Captured samples: " << recordedSamples.size() << endl;

    if (!recordedSamples.empty()) {
        ofstream file("jib_mic.raw", ios::binary);
        file.write((char*)recordedSamples.data(),
                   recordedSamples.size() * sizeof(float));
        file.close();
        cout << "Saved jib_mic.raw\n";
    } else {
        cout << "No audio captured.\n";
    }

    Pa_Terminate();
    return 0;
}