#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <portaudio.h>
#include <pa_win_wasapi.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using namespace std;
using namespace chrono;

// ─── Config ───────────────────────────────────────────────────────────────────
#define FRAMES_PER_BUFFER   512
#define TARGET_RATE         48000
#define TARGET_CHANNELS     1       // mono output
#define CHUNK_INTERVAL_MS   300
#define DRIFT_LOG_INTERVAL  30
#define MAX_DRIFT_PPM       500.0

#ifndef paWASAPIUseOutputDeviceForInput
    #define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

// ─── Shared audio buffer ──────────────────────────────────────────────────────
struct SharedAudio {
    mutex         mtx;
    vector<float> samples;
};

SharedAudio micAudio, spkAudio;

// ─── Global state ─────────────────────────────────────────────────────────────
atomic<uint64_t>   micTotalFrames{0};
atomic<uint64_t>   spkTotalFrames{0};
atomic<bool>       recording{true};
mutex              wakeMtx;
condition_variable wakeCV;

int    micChannels, spkChannels;
double micRate,     spkRate;
steady_clock::time_point recordStart;

// ─── Callbacks ────────────────────────────────────────────────────────────────
static int micCallback(const void* in, void*, unsigned long n,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*)
{
    if (!in) return paContinue;
    const float* src = (const float*)in;
    micTotalFrames.fetch_add(n, memory_order_relaxed);
    lock_guard<mutex> lk(micAudio.mtx);
    micAudio.samples.insert(micAudio.samples.end(), src, src + n * micChannels);
    return paContinue;
}

static int spkCallback(const void* in, void*, unsigned long n,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*)
{
    if (!in) return paContinue;
    const float* src = (const float*)in;
    spkTotalFrames.fetch_add(n, memory_order_relaxed);
    lock_guard<mutex> lk(spkAudio.mtx);
    spkAudio.samples.insert(spkAudio.samples.end(), src, src + n * spkChannels);
    return paContinue;
}

// ─── Drift tracking ───────────────────────────────────────────────────────────
struct DriftState {
    double   effectiveMicRate = 0;
    double   effectiveSpkRate = 0;
    uint64_t prevMicFrames    = 0;
    uint64_t prevSpkFrames    = 0;
    steady_clock::time_point lastUpdate;
    bool     initialised      = false;
    uint64_t lastDriftLogAt   = 0;
};

void updateDrift(DriftState& ds,
                 double nomMic, double nomSpk,
                 double& outMic, double& outSpk)
{
    auto now = steady_clock::now();

    if (!ds.initialised) {
        ds.effectiveMicRate = nomMic;
        ds.effectiveSpkRate = nomSpk;
        ds.prevMicFrames    = micTotalFrames.load();
        ds.prevSpkFrames    = spkTotalFrames.load();
        ds.lastUpdate       = now;
        ds.initialised      = true;
        outMic = nomMic; outSpk = nomSpk;
        return;
    }

    double elapsed = duration<double>(now - ds.lastUpdate).count();
    if (elapsed < 1.0) { outMic = ds.effectiveMicRate; outSpk = ds.effectiveSpkRate; return; }

    uint64_t curMic = micTotalFrames.load(memory_order_relaxed);
    uint64_t curSpk = spkTotalFrames.load(memory_order_relaxed);
    double mMic = (curMic - ds.prevMicFrames) / elapsed;
    double mSpk = (curSpk - ds.prevSpkFrames) / elapsed;

    auto plausible = [](double m, double n) { double r = m/n; return r > 0.9 && r < 1.1; };
    if (plausible(mMic, nomMic)) ds.effectiveMicRate = 0.8*ds.effectiveMicRate + 0.2*mMic;
    if (plausible(mSpk, nomSpk)) ds.effectiveSpkRate = 0.8*ds.effectiveSpkRate + 0.2*mSpk;

    uint64_t wallSec = (uint64_t)duration<double>(now - recordStart).count();
    if (wallSec - ds.lastDriftLogAt >= (uint64_t)DRIFT_LOG_INTERVAL) {
        ds.lastDriftLogAt = wallSec;
        double micPpm = 1e6*(ds.effectiveMicRate - nomMic)/nomMic;
        double spkPpm = 1e6*(ds.effectiveSpkRate - nomSpk)/nomSpk;
        double relPpm = micPpm - spkPpm;
        cout << "[Drift @ " << wallSec << "s]"
             << "  mic=" << (int)micPpm << " ppm"
             << "  spk=" << (int)spkPpm << " ppm"
             << "  relative=" << (int)relPpm << " ppm";
        if (fabs(relPpm) > MAX_DRIFT_PPM) cout << "  *** HIGH — correction active ***";
        cout << "\n";
    }

    ds.prevMicFrames = curMic;
    ds.prevSpkFrames = curSpk;
    ds.lastUpdate    = now;
    outMic = ds.effectiveMicRate;
    outSpk = ds.effectiveSpkRate;
}

// ─── Persistent resampler → mono out ─────────────────────────────────────────
struct Resampler {
    ma_data_converter conv;
    bool     initialised = false;
    int      inChannels  = 0;
    uint32_t currentRate = 0;

    void init(int inCh, uint32_t inRate) {
        if (initialised) ma_data_converter_uninit(&conv, NULL);
        ma_data_converter_config cfg = ma_data_converter_config_init(
            ma_format_f32, ma_format_f32,
            (ma_uint32)inCh, 1,          // always resample to mono
            inRate, TARGET_RATE
        );
        cfg.resampling.algorithm       = ma_resample_algorithm_linear;
        cfg.resampling.linear.lpfOrder = 8;
        if (ma_data_converter_init(&cfg, NULL, &conv) != MA_SUCCESS) {
            cerr << "Resampler init failed\n"; exit(1);
        }
        inChannels  = inCh;
        currentRate = inRate;
        initialised = true;
    }

    ~Resampler() { if (initialised) ma_data_converter_uninit(&conv, NULL); }

    vector<float> drain(SharedAudio& src, double inRate) {
        uint32_t rateInt = (uint32_t)round(inRate);
        if (!initialised || rateInt != currentRate) init(inChannels, rateInt);

        vector<float> input;
        {
            lock_guard<mutex> lk(src.mtx);
            input.swap(src.samples);
        }
        if (input.empty()) return {};

        ma_uint64 framesIn  = input.size() / inChannels;
        ma_uint64 framesOut = 0;
        ma_data_converter_get_expected_output_frame_count(&conv, framesIn, &framesOut);

        vector<float> out(framesOut + 64);
        ma_uint64 actualIn  = framesIn;
        ma_uint64 actualOut = framesOut + 64;
        ma_data_converter_process_pcm_frames(&conv, input.data(), &actualIn, out.data(), &actualOut);
        out.resize(actualOut);
        return out;
    }
};

// ─── WAV helpers (mono float32) ───────────────────────────────────────────────
void writeWavHeader(ofstream& f, uint32_t totalFrames)
{
    uint32_t dataBytes = totalFrames * sizeof(float);   // mono: 1 sample per frame
    uint32_t byteRate  = TARGET_RATE * sizeof(float);
    auto w16 = [&](uint16_t v){ f.write((char*)&v, 2); };
    auto w32 = [&](uint32_t v){ f.write((char*)&v, 4); };
    f.write("RIFF", 4); w32(36 + dataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16);
    w16(3);              // IEEE float PCM
    w16(1);              // mono
    w32(TARGET_RATE);
    w32(byteRate);
    w16(sizeof(float));  // block align
    w16(32);             // bits per sample
    f.write("data", 4); w32(dataBytes);
}

void patchWavHeader(ofstream& f, uint32_t totalFrames)
{
    uint32_t dataBytes = totalFrames * sizeof(float);
    f.seekp(4);  uint32_t chunkSize = 36 + dataBytes; f.write((char*)&chunkSize, 4);
    f.seekp(40); f.write((char*)&dataBytes, 4);
    f.seekp(0, ios::end);
}

// ─── Processing thread ────────────────────────────────────────────────────────
void processingThread(const string& outPath)
{
    ofstream out(outPath, ios::binary);
    if (!out) { cerr << "Cannot open output file\n"; return; }
    writeWavHeader(out, 0);

    DriftState drift;
    Resampler  micRes, spkRes;
    micRes.init(micChannels, (uint32_t)micRate);
    spkRes.init(spkChannels, (uint32_t)spkRate);

    uint64_t totalFramesWritten = 0;

    auto drainAndWrite = [&]() {
        double effMic, effSpk;
        updateDrift(drift, micRate, spkRate, effMic, effSpk);

        vector<float> micMono = micRes.drain(micAudio, effMic);
        vector<float> spkMono = spkRes.drain(spkAudio, effSpk);
        if (micMono.empty() && spkMono.empty()) return;

        // Mix: average both streams into a single mono sample.
        // If one stream is shorter, the tail gets only the other stream at full level.
        size_t frames = max(micMono.size(), spkMono.size());
        vector<float> mono(frames);
        for (size_t i = 0; i < frames; i++) {
            float m = (i < micMono.size()) ? micMono[i] : 0.0f;
            float s = (i < spkMono.size()) ? spkMono[i] : 0.0f;
            // Average instead of sum — prevents clipping when both streams are loud
            mono[i] = max(-1.0f, min(1.0f, (m + s) * 0.5f));
        }

        out.write((char*)mono.data(), mono.size() * sizeof(float));
        totalFramesWritten += frames;

        size_t pending;
        {
            lock_guard<mutex> lm(micAudio.mtx);
            lock_guard<mutex> ls(spkAudio.mtx);
            pending = (micAudio.samples.size() + spkAudio.samples.size()) * sizeof(float);
        }
        if (pending > 50 * 1024 * 1024)
            cout << "[WARNING] " << pending/(1024*1024) << " MB pending — disk too slow?\n";
    };

    while (true) {
        {
            unique_lock<mutex> lk(wakeMtx);
            wakeCV.wait_for(lk, milliseconds(CHUNK_INTERVAL_MS),
                            []{ return !recording.load(); });
        }
        drainAndWrite();
        if (!recording.load()) {
            drainAndWrite();  // final boundary drain
            break;
        }
    }

    patchWavHeader(out, (uint32_t)totalFramesWritten);
    out.close();

    double secs = (double)totalFramesWritten / TARGET_RATE;
    cout << "\nOutput: " << outPath << "\n";
    cout << "Duration: " << fixed << secs << " s\n";
    cout << "Format: Mono float32 WAV @ 48kHz — ready to use directly\n";
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main()
{
    Pa_Initialize();

    int numDevices = Pa_GetDeviceCount();
    cout << "Available devices:\n\n";
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
        cout << "ID: " << i
             << " | " << d->name
             << " | In: "  << d->maxInputChannels
             << " | Out: " << d->maxOutputChannels
             << " | Rate: " << d->defaultSampleRate << "\n";
    }

    int micDev, spkDev;
    cout << "\nEnter MIC ID: ";           cin >> micDev;
    cout << "Enter SPEAKER DEVICE ID: "; cin >> spkDev;
    cin.ignore();

    const PaDeviceInfo* micInfo = Pa_GetDeviceInfo(micDev);
    const PaDeviceInfo* spkInfo = Pa_GetDeviceInfo(spkDev);

    micChannels = micInfo->maxInputChannels;
    spkChannels = spkInfo->maxOutputChannels == 0 ? spkInfo->maxInputChannels: spkInfo->maxOutputChannels;
    micRate = micInfo->defaultSampleRate;
    spkRate = spkInfo->defaultSampleRate;

    cout << "\n--- Quality Check ---\n";
    cout << "Mic:     " << micChannels << "ch @ " << micRate << "Hz\n";
    cout << "Speaker: " << spkChannels << "ch @ " << spkRate << "Hz\n";
    if (micRate < 44100) cout << "WARNING: Mic rate low (" << micRate << "Hz)\n";
    if (spkRate < 44100) cout << "WARNING: Speaker rate low (" << spkRate << "Hz)\n";
    cout << "Output:  Mono WAV (mic + speaker averaged)\n";
    cout << "---------------------\n\n";

    {
        lock_guard<mutex> lm(micAudio.mtx);
        lock_guard<mutex> ls(spkAudio.mtx);
        micAudio.samples.reserve((size_t)(micRate * micChannels * (CHUNK_INTERVAL_MS/1000.0) * 2));
        spkAudio.samples.reserve((size_t)(spkRate * spkChannels * (CHUNK_INTERVAL_MS/1000.0) * 2));
    }

    PaStreamParameters micParams{}, spkParams{};
    micParams.device           = micDev;
    micParams.channelCount     = micChannels;
    micParams.sampleFormat     = paFloat32;
    micParams.suggestedLatency = micInfo->defaultLowInputLatency;

    spkParams.device           = spkDev;
    spkParams.channelCount     = spkChannels;
    spkParams.sampleFormat     = paFloat32;
    spkParams.suggestedLatency = spkInfo->defaultLowInputLatency;

    PaStream *micStream, *spkStream;
    PaError err;

    err = Pa_OpenStream(&micStream, &micParams, NULL, micRate, FRAMES_PER_BUFFER, paClipOff, micCallback, NULL);
    if (err != paNoError) { cout << "ERROR mic: " << Pa_GetErrorText(err) << "\n"; return 1; }

    err = Pa_OpenStream(&spkStream, &spkParams, NULL, spkRate, FRAMES_PER_BUFFER, paClipOff, spkCallback, NULL);
    if (err != paNoError) { cout << "ERROR spk: " << Pa_GetErrorText(err) << "\n"; return 1; }

    recordStart = steady_clock::now();
    thread proc(processingThread, "audio.wav");

    Pa_StartStream(micStream);
    Pa_StartStream(spkStream);

    cout << "Recording... Press ENTER to stop\n";
    cin.get();

    Pa_StopStream(micStream);
    Pa_StopStream(spkStream);
    Pa_CloseStream(micStream);
    Pa_CloseStream(spkStream);
    Pa_Terminate();

    {
        lock_guard<mutex> lk(wakeMtx);
        recording.store(false);
    }
    wakeCV.notify_one();

    proc.join();
    return 0;
}