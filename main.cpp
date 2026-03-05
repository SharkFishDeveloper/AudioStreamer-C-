#include <iostream>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <mutex>
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
#define TARGET_CHANNELS     2
#define CHUNK_INTERVAL_SEC  10      // flush to disk every 10 s  (~3.7 MB/chunk)
#define DRIFT_LOG_INTERVAL  30      // print drift report every 30 s
#define MAX_DRIFT_PPM       500.0   // warn if drift exceeds 500 ppm (~2s/hour)

#ifndef paWASAPIUseOutputDeviceForInput
    #define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

// ─── Shared state ─────────────────────────────────────────────────────────────
struct SharedAudio {
    mutex           mtx;
    vector<float>   samples;      // drained by the processing thread
};

SharedAudio micAudio, spkAudio;

// Drift tracking: count raw frames received by each callback
atomic<uint64_t>  micTotalFrames{0};
atomic<uint64_t>  spkTotalFrames{0};
atomic<bool>      recording{true};

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
    {
        lock_guard<mutex> lk(micAudio.mtx);
        micAudio.samples.insert(micAudio.samples.end(), src, src + n * micChannels);
    }
    return paContinue;
}

static int spkCallback(const void* in, void*, unsigned long n,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*)
{
    if (!in) return paContinue;
    const float* src = (const float*)in;
    spkTotalFrames.fetch_add(n, memory_order_relaxed);
    {
        lock_guard<mutex> lk(spkAudio.mtx);
        spkAudio.samples.insert(spkAudio.samples.end(), src, src + n * spkChannels);
    }
    return paContinue;
}

// ─── Drift detection ──────────────────────────────────────────────────────────
struct DriftState {
    // Running effective rates, updated each chunk.
    // Start with nominal values; converge toward measured values.
    double effectiveMicRate = 0;
    double effectiveSpkRate = 0;
    uint64_t prevMicFrames  = 0;
    uint64_t prevSpkFrames  = 0;
    steady_clock::time_point lastUpdate;
    bool initialised = false;
    uint64_t lastDriftLogAt = 0;   // wall seconds of last drift log
};

// Returns drift-corrected effective rates via out params.
// Call this before resampling each chunk.
void updateDrift(DriftState& ds,
                 double nominalMicRate, double nominalSpkRate,
                 double& outMicRate,   double& outSpkRate)
{
    auto now = steady_clock::now();

    if (!ds.initialised) {
        ds.effectiveMicRate = nominalMicRate;
        ds.effectiveSpkRate = nominalSpkRate;
        ds.prevMicFrames    = micTotalFrames.load();
        ds.prevSpkFrames    = spkTotalFrames.load();
        ds.lastUpdate       = now;
        ds.initialised      = true;
        outMicRate = nominalMicRate;
        outSpkRate = nominalSpkRate;
        return;
    }

    double elapsed = duration<double>(now - ds.lastUpdate).count();
    if (elapsed < 1.0) {            // need at least 1 s to get a stable estimate
        outMicRate = ds.effectiveMicRate;
        outSpkRate = ds.effectiveSpkRate;
        return;
    }

    uint64_t curMic = micTotalFrames.load(memory_order_relaxed);
    uint64_t curSpk = spkTotalFrames.load(memory_order_relaxed);

    uint64_t deltaMic = curMic - ds.prevMicFrames;
    uint64_t deltaSpk = curSpk - ds.prevSpkFrames;

    double measuredMicRate = deltaMic / elapsed;
    double measuredSpkRate = deltaSpk / elapsed;

    // Sanity-gate: ignore wild outliers (scheduler hiccups)
    auto plausible = [](double measured, double nominal) {
        double ratio = measured / nominal;
        return ratio > 0.9 && ratio < 1.1;   // within 10 % of nominal
    };

    // Low-pass blend: 80 % old estimate, 20 % new measurement
    // → converges in ~5 chunks while rejecting transients
    if (plausible(measuredMicRate, nominalMicRate))
        ds.effectiveMicRate = 0.8 * ds.effectiveMicRate + 0.2 * measuredMicRate;
    if (plausible(measuredSpkRate, nominalSpkRate))
        ds.effectiveSpkRate = 0.8 * ds.effectiveSpkRate + 0.2 * measuredSpkRate;

    // Drift report
    uint64_t wallSec = (uint64_t)duration<double>(now - recordStart).count();
    if (wallSec - ds.lastDriftLogAt >= (uint64_t)DRIFT_LOG_INTERVAL) {
        ds.lastDriftLogAt = wallSec;
        double micPpm = 1e6 * (ds.effectiveMicRate - nominalMicRate) / nominalMicRate;
        double spkPpm = 1e6 * (ds.effectiveSpkRate - nominalSpkRate) / nominalSpkRate;
        double relPpm = micPpm - spkPpm;   // relative drift between the two clocks
        cout << "[Drift @ " << wallSec << "s]"
             << "  mic=" << fixed << (int)micPpm << " ppm"
             << "  spk=" << (int)spkPpm << " ppm"
             << "  relative=" << (int)relPpm << " ppm";
        if (fabs(relPpm) > MAX_DRIFT_PPM)
            cout << "  *** HIGH DRIFT — correction active ***";
        cout << "\n";
    }

    ds.prevMicFrames = curMic;
    ds.prevSpkFrames = curSpk;
    ds.lastUpdate    = now;

    outMicRate = ds.effectiveMicRate;
    outSpkRate = ds.effectiveSpkRate;
}

// ─── Resampler ────────────────────────────────────────────────────────────────
// Drain `src`, resample from (inChannels, inRate) → (TARGET_CHANNELS, TARGET_RATE).
// Returns resampled frames; leaves src empty.
vector<float> drainAndResample(SharedAudio& src, int inChannels, double inRate)
{
    vector<float> input;
    {
        lock_guard<mutex> lk(src.mtx);
        input.swap(src.samples);          // zero-copy drain
    }
    if (input.empty()) return {};

    ma_data_converter_config cfg = ma_data_converter_config_init(
        ma_format_f32, ma_format_f32,
        (ma_uint32)inChannels,  TARGET_CHANNELS,
        (ma_uint32)round(inRate), TARGET_RATE
    );
    cfg.resampling.algorithm       = ma_resample_algorithm_linear;
    cfg.resampling.linear.lpfOrder = 8;

    ma_data_converter conv;
    if (ma_data_converter_init(&cfg, NULL, &conv) != MA_SUCCESS) {
        cerr << "Resample init failed\n"; exit(1);
    }

    ma_uint64 framesIn  = input.size() / inChannels;
    ma_uint64 framesOut = 0;
    ma_data_converter_get_expected_output_frame_count(&conv, framesIn, &framesOut);

    vector<float> out(framesOut * TARGET_CHANNELS);
    ma_uint64 actualIn  = framesIn;
    ma_uint64 actualOut = framesOut;
    ma_data_converter_process_pcm_frames(&conv, input.data(), &actualIn, out.data(), &actualOut);
    ma_data_converter_uninit(&conv, NULL);

    out.resize(actualOut * TARGET_CHANNELS);
    return out;
}

// ─── WAV helpers ──────────────────────────────────────────────────────────────
void writeWavHeader(ofstream& f, uint32_t totalSamples)
{
    // totalSamples = number of float32 samples (frames * channels)
    uint32_t byteRate   = TARGET_RATE * TARGET_CHANNELS * sizeof(float);
    uint32_t dataBytes  = totalSamples * sizeof(float);
    uint32_t chunkSize  = 36 + dataBytes;

    auto w16 = [&](uint16_t v){ f.write((char*)&v, 2); };
    auto w32 = [&](uint32_t v){ f.write((char*)&v, 4); };

    f.write("RIFF", 4);  w32(chunkSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);  w32(16);
    w16(3);                          // IEEE float
    w16(TARGET_CHANNELS);
    w32(TARGET_RATE);
    w32(byteRate);
    w16(TARGET_CHANNELS * sizeof(float));
    w16(32);                         // bits per sample
    f.write("data", 4);  w32(dataBytes);
}

void patchWavHeader(ofstream& f, uint32_t totalSamples)
{
    uint32_t dataBytes = totalSamples * sizeof(float);
    f.seekp(4);  uint32_t chunkSize = 36 + dataBytes; f.write((char*)&chunkSize, 4);
    f.seekp(40); f.write((char*)&dataBytes, 4);
    f.seekp(0, ios::end);
}

// ─── Processing thread ────────────────────────────────────────────────────────
// Wakes every CHUNK_INTERVAL_SEC seconds, drains both buffers,
// applies drift-corrected resampling, mixes, and appends to file.
void processingThread(const string& outPath)
{
    ofstream out(outPath, ios::binary);
    if (!out) { cerr << "Cannot open output file\n"; return; }

    // Reserve space for WAV header; patch it at the end.
    writeWavHeader(out, 0);

    DriftState drift;
    uint64_t totalSamplesWritten = 0;

    while (recording.load() || !micAudio.samples.empty() || !spkAudio.samples.empty())
    {
        auto wakeAt = steady_clock::now() + seconds(CHUNK_INTERVAL_SEC);

        // ── Drain and resample ────────────────────────────────────────────────
        double effMic, effSpk;
        updateDrift(drift, micRate, spkRate, effMic, effSpk);

        vector<float> mic48 = drainAndResample(micAudio, micChannels, effMic);
        vector<float> spk48 = drainAndResample(spkAudio, spkChannels, effSpk);

        if (mic48.empty() && spk48.empty()) {
            if (recording.load())
                this_thread::sleep_until(wakeAt);
            continue;
        }

        // ── Mix ───────────────────────────────────────────────────────────────
        size_t maxSz = max(mic48.size(), spk48.size());
        vector<float> chunk(maxSz, 0.0f);
        for (size_t i = 0; i < mic48.size(); i++) chunk[i] += mic48[i];
        for (size_t i = 0; i < spk48.size(); i++) chunk[i] += spk48[i];
        for (float& s : chunk) s = max(-1.0f, min(1.0f, s));

        // ── Append to file ────────────────────────────────────────────────────
        out.write((char*)chunk.data(), chunk.size() * sizeof(float));
        totalSamplesWritten += chunk.size();

        // Print RAM pressure info
        {
            lock_guard<mutex> lm(micAudio.mtx);
            lock_guard<mutex> ls(spkAudio.mtx);
            size_t pending = (micAudio.samples.size() + spkAudio.samples.size()) * sizeof(float);
            if (pending > 50 * 1024 * 1024)  // warn if > 50 MB pending
                cout << "[WARNING] Pending buffer: " << pending / (1024*1024) << " MB — disk too slow?\n";
        }

        if (recording.load())
            this_thread::sleep_until(wakeAt);
    }

    // Patch the WAV header with the actual data size
    patchWavHeader(out, (uint32_t)totalSamplesWritten);
    out.close();

    cout << "\nOutput written: " << outPath << "\n";
    cout << "Total samples: " << totalSamplesWritten
         << " (" << totalSamplesWritten / TARGET_CHANNELS / TARGET_RATE << " s)\n";
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
    cout << "\nEnter MIC ID: ";             cin >> micDev;
    cout << "Enter SPEAKER DEVICE ID: ";   cin >> spkDev;
    cin.ignore();

    const PaDeviceInfo* micInfo = Pa_GetDeviceInfo(micDev);
    const PaDeviceInfo* spkInfo = Pa_GetDeviceInfo(spkDev);

    micChannels = micInfo->maxInputChannels;
    spkChannels = spkInfo->maxOutputChannels ==0 ? spkInfo->maxInputChannels :spkInfo->maxOutputChannels;
    micRate     = micInfo->defaultSampleRate;
    spkRate     = spkInfo->defaultSampleRate;

    if (spkInfo->maxInputChannels == 0) {
    cout << "Selected speaker device cannot provide input (no loopback).\n";
    return 1;
    }

    cout << "\n--- Quality Check ---\n";
    cout << "Mic:     " << micChannels << "ch @ " << micRate << "Hz\n";
    cout << "Speaker: " << spkChannels << "ch @ " << spkRate << "Hz\n";
    if (micRate < 44100) cout << "WARNING: Mic rate is low (" << micRate << "Hz)\n";
    if (spkRate < 44100) cout << "WARNING: Speaker rate is low (" << spkRate << "Hz)\n";
    cout << "---------------------\n\n";

    PaStreamParameters micParams{}, spkParams{};
    micParams.device           = micDev;
    micParams.channelCount     = micChannels;
    micParams.sampleFormat     = paFloat32;
    micParams.suggestedLatency = micInfo->defaultLowInputLatency;

    spkParams.device           = spkDev;
    spkParams.channelCount     = spkChannels;
    spkParams.sampleFormat     = paFloat32;
    spkParams.suggestedLatency = spkInfo->defaultLowInputLatency;
    

    PaWasapiStreamInfo wasapiInfo{};
    wasapiInfo.size = sizeof(PaWasapiStreamInfo);
    wasapiInfo.hostApiType = paWASAPI;
    wasapiInfo.version = 1;
    wasapiInfo.flags = paWASAPIUseOutputDeviceForInput;

    spkParams.hostApiSpecificStreamInfo = &wasapiInfo;

    PaStream *micStream, *spkStream;
    PaError err;

    err = Pa_OpenStream(&micStream, &micParams, NULL, micRate, FRAMES_PER_BUFFER, paClipOff, micCallback, NULL);
    if (err != paNoError) { cout << "ERROR mic: " << Pa_GetErrorText(err) << "\n"; return 1; }

    err = Pa_OpenStream(&spkStream, &spkParams, NULL, spkRate, FRAMES_PER_BUFFER, paClipOff, spkCallback, NULL);
    if (err != paNoError) { cout << "ERROR spk: " << Pa_GetErrorText(err) << "\n"; return 1; }

    recordStart = steady_clock::now();

    // Start background processing thread BEFORE the streams
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

    recording.store(false);
    proc.join();           // wait for final flush

    return 0;
}