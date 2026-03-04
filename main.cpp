#include <iostream>
#include <vector>
#include <fstream>
#include <portaudio.h>

using namespace std;

#define FRAMES_PER_BUFFER 512

int g_numChannels = 2;
double g_sampleRate = 44100;

vector<float> recordedSamples;

static int audioCallback(
    const void* inputBuffer,
    void*,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags,
    void*)
{
    if (!inputBuffer) return paContinue;

    const float* in = (const float*)inputBuffer;

    for (unsigned int i = 0; i < framesPerBuffer * g_numChannels; i++)
        recordedSamples.push_back(in[i]);

    return paContinue;
}

int main()
{
    Pa_Initialize();

    int numDevices = Pa_GetDeviceCount();

    cout << "Available audio devices:\n\n";

    for (int i = 0; i < numDevices; i++)
    {
        const PaDeviceInfo* dev = Pa_GetDeviceInfo(i);
        const PaHostApiInfo* host = Pa_GetHostApiInfo(dev->hostApi);

        cout << "ID: " << i << endl;
        cout << "Name: " << dev->name << endl;
        cout << "Host API: " << host->name << endl;
        cout << "Max Input Channels: " << dev->maxInputChannels << endl;
        cout << "Max Output Channels: " << dev->maxOutputChannels << endl;
        cout << "Default Sample Rate: " << dev->defaultSampleRate << endl;
        cout << "---------------------------\n";
    }

    int selectedDevice;

    cout << "\nEnter device ID to capture: ";
    cin >> selectedDevice;
    cin.ignore();

    const PaDeviceInfo* dev = Pa_GetDeviceInfo(selectedDevice);


    g_numChannels = dev->maxInputChannels;
    g_sampleRate = dev->defaultSampleRate;

    cout << "\nUsing device settings:\n";
    cout << "Channels: " << g_numChannels << endl;
    cout << "Sample Rate: " << g_sampleRate << endl;

    PaStreamParameters params;
    params.device = selectedDevice;
    params.channelCount = g_numChannels;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = dev->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = NULL;

    PaStream* stream;

    PaError err = Pa_OpenStream(
        &stream,
        &params,
        NULL,
        g_sampleRate,
        FRAMES_PER_BUFFER,
        paClipOff,
        audioCallback,
        NULL
    );

    if (err != paNoError)
    {
        cout << "Stream error: " << Pa_GetErrorText(err) << endl;
        Pa_Terminate();
        return 1;
    }

    cout << "\nCapturing audio from: " << dev->name << endl;
    cout << "Press ENTER to stop\n";

    recordedSamples.reserve(g_sampleRate * 60 * g_numChannels);

    Pa_StartStream(stream);

    cin.get();

    Pa_StopStream(stream);
    Pa_CloseStream(stream);

    ofstream file("mic.raw", ios::binary);
    file.write((char*)recordedSamples.data(),recordedSamples.size() * sizeof(float));
    file.close();

    cout << "Saved to mic.raw\n";

    Pa_Terminate();
}