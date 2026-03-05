#include <iostream>
#include <vector>
#include <deque>
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
#define FRAMES_PER_BUFFER       512
#define TARGET_RATE             16000
#define TARGET_CHANNELS         1

// Internal processing: stable 500ms chunks — large enough that callbacks
// always have data ready and the resampler LPF is never starved.
#define PROCESS_INTERVAL_MS     500

// Output chunk: 100ms worth of frames at 16kHz = 1600 frames.
// This is what you feed to AWS Transcribe.
#define SEND_CHUNK_FRAMES       1600    // 100ms @ 16kHz

#define DRIFT_LOG_INTERVAL      25
#define MAX_DRIFT_PPM           500.0

#ifndef paWASAPIUseOutputDeviceForInput
    #define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

// ─── Shared audio input buffers (callback → processing thread) ───────────────
struct SharedAudio {
    mutex         mtx;
    vector<float> samples;
};
SharedAudio micAudio, spkAudio;

// ─── Output queue (processing thread → sender thread) ────────────────────────
// Processing thread pushes fully mixed mono frames here.
// Sender thread pops SEND_CHUNK_FRAMES at a time.
struct OutputQueue {
    mutex          mtx;
    condition_variable cv;
    deque<float>   frames;   // mono float32 @ TARGET_RATE
};
OutputQueue outQueue;

// ─── Global state ─────────────────────────────────────────────────────────────
atomic<uint64_t>   micTotalFrames{0};
atomic<uint64_t>   spkTotalFrames{0};
atomic<bool>       recording{true};
atomic<bool>       processingDone{false};
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

void updateDrift(DriftState& ds, double nomMic, double nomSpk,
                 double& outMic, double& outSpk)
{
    auto now = steady_clock::now();
    if (!ds.initialised) {
        ds.effectiveMicRate = nomMic; ds.effectiveSpkRate = nomSpk;
        ds.prevMicFrames = micTotalFrames.load();
        ds.prevSpkFrames = spkTotalFrames.load();
        ds.lastUpdate = now; ds.initialised = true;
        outMic = nomMic; outSpk = nomSpk; return;
    }
    double elapsed = duration<double>(now - ds.lastUpdate).count();
    if (elapsed < 1.0) { outMic = ds.effectiveMicRate; outSpk = ds.effectiveSpkRate; return; }

    uint64_t curMic = micTotalFrames.load(memory_order_relaxed);
    uint64_t curSpk = spkTotalFrames.load(memory_order_relaxed);
    double mMic = (curMic - ds.prevMicFrames) / elapsed;
    double mSpk = (curSpk - ds.prevSpkFrames) / elapsed;

    auto plausible = [](double m, double n){ double r=m/n; return r>0.9&&r<1.1; };
    if (plausible(mMic, nomMic)) ds.effectiveMicRate = 0.8*ds.effectiveMicRate + 0.2*mMic;
    if (plausible(mSpk, nomSpk)) ds.effectiveSpkRate = 0.8*ds.effectiveSpkRate + 0.2*mSpk;

    uint64_t wallSec = (uint64_t)duration<double>(now - recordStart).count();
    if (wallSec - ds.lastDriftLogAt >= (uint64_t)DRIFT_LOG_INTERVAL) {
        ds.lastDriftLogAt = wallSec;
        double micPpm = 1e6*(ds.effectiveMicRate-nomMic)/nomMic;
        double spkPpm = 1e6*(ds.effectiveSpkRate-nomSpk)/nomSpk;
        double relPpm = micPpm - spkPpm;
        cout << "[Drift @ " << wallSec << "s]"
             << "  mic=" << (int)micPpm << " ppm"
             << "  spk=" << (int)spkPpm << " ppm"
             << "  relative=" << (int)relPpm << " ppm";
        if (fabs(relPpm) > MAX_DRIFT_PPM) cout << "  *** HIGH — correction active ***";
        cout << "\n";
    }
    ds.prevMicFrames = curMic; ds.prevSpkFrames = curSpk;
    ds.lastUpdate = now;
    outMic = ds.effectiveMicRate; outSpk = ds.effectiveSpkRate;
}

// ─── Persistent resampler → mono ─────────────────────────────────────────────
struct Resampler {
    ma_data_converter conv;
    bool     initialised = false;
    int      inChannels  = 0;
    uint32_t currentRate = 0;

    void init(int inCh, uint32_t inRate) {
        if (initialised) ma_data_converter_uninit(&conv, NULL);
        ma_data_converter_config cfg = ma_data_converter_config_init(
            ma_format_f32, ma_format_f32,
            (ma_uint32)inCh, 1, inRate, TARGET_RATE
        );
        cfg.resampling.algorithm       = ma_resample_algorithm_linear;
        cfg.resampling.linear.lpfOrder = 4;   // order 4: lower latency, still clean at 16kHz
        if (ma_data_converter_init(&cfg, NULL, &conv) != MA_SUCCESS) {
            cerr << "Resampler init failed\n"; exit(1);
        }
        inChannels = inCh; currentRate = inRate; initialised = true;
    }
    ~Resampler() { if (initialised) ma_data_converter_uninit(&conv, NULL); }

    vector<float> drain(SharedAudio& src, double inRate) {
        uint32_t rateInt = (uint32_t)round(inRate);
        // if (!initialised || rateInt != currentRate) init(inChannels, rateInt);
        if (!initialised) init(inChannels, rateInt);

        vector<float> input;
        { lock_guard<mutex> lk(src.mtx); input.swap(src.samples); }
        if (input.empty()) return {};

        ma_uint64 framesIn  = input.size() / inChannels;
        ma_uint64 framesOut = 0;
        ma_data_converter_get_expected_output_frame_count(&conv, framesIn, &framesOut);

        vector<float> out(framesOut + 64);
        ma_uint64 actualIn = framesIn, actualOut = framesOut + 64;
        ma_data_converter_process_pcm_frames(&conv, input.data(), &actualIn, out.data(), &actualOut);
        out.resize(actualOut);
        return out;
    }
};

// ─── WAV helpers ──────────────────────────────────────────────────────────────
void writeWavHeader(ofstream& f, uint32_t totalFrames) {
    uint32_t dataBytes = totalFrames * sizeof(int16_t);
    uint32_t byteRate  = TARGET_RATE * sizeof(int16_t);

    auto w16 = [&](uint16_t v){ f.write((char*)&v,2); };
    auto w32 = [&](uint32_t v){ f.write((char*)&v,4); };

    f.write("RIFF",4);
    w32(36 + dataBytes);
    f.write("WAVE",4);

    f.write("fmt ",4);
    w32(16);

    w16(1);              // PCM format
    w16(1);              // mono
    w32(TARGET_RATE);
    w32(byteRate);
    w16(sizeof(int16_t)); // block align
    w16(16);              // bits per sample

    f.write("data",4);
    w32(dataBytes);
}

void patchWavHeader(ofstream& f, uint32_t totalFrames) {
    uint32_t dataBytes = totalFrames * sizeof(int16_t);
    f.seekp(4);  uint32_t cs = 36+dataBytes; f.write((char*)&cs,4);
    f.seekp(40); f.write((char*)&dataBytes,4);
    f.seekp(0, ios::end);
}

// ─── Processing thread ────────────────────────────────────────────────────────
// Runs at PROCESS_INTERVAL_MS (500ms). Drains mic+spk, resamples, mixes,
// then pushes all resulting frames into outQueue for the sender to consume.
void processingThread()
{
    DriftState drift;
    Resampler  micRes, spkRes;
    micRes.init(micChannels, (uint32_t)micRate);
    spkRes.init(spkChannels, (uint32_t)spkRate);

    auto drainAndQueue = [&]() {
        double effMic, effSpk;
        updateDrift(drift, micRate, spkRate, effMic, effSpk);

        vector<float> micMono = micRes.drain(micAudio, effMic);
        vector<float> spkMono = spkRes.drain(spkAudio, effSpk);
        if (micMono.empty() && spkMono.empty()) return;

        size_t frames = max(micMono.size(), spkMono.size());
        {
            lock_guard<mutex> lk(outQueue.mtx);
            for (size_t i = 0; i < frames; i++) {
                float m = (i < micMono.size()) ? micMono[i] : 0.0f;
                float s = (i < spkMono.size()) ? spkMono[i] : 0.0f;

                // Smart mix: only average when BOTH streams are active.
                // If one is silent (tail), pass the other at full level.
                float mixed;
                bool hasMic = (i < micMono.size());
                bool hasSpk = (i < spkMono.size());
                if (hasMic && hasSpk)
                    mixed = (m + s) * 0.5f;
                else
                    mixed = hasMic ? m : s;

                outQueue.frames.push_back(max(-1.0f, min(1.0f, mixed)));
            }
            outQueue.cv.notify_one();  // wake sender
        }

        // RAM check on input side
        size_t pending;
        {
            lock_guard<mutex> lm(micAudio.mtx);
            lock_guard<mutex> ls(spkAudio.mtx);
            pending = (micAudio.samples.size() + spkAudio.samples.size()) * sizeof(float);
        }
        if (pending > 50*1024*1024)
            cout << "[WARNING] " << pending/(1024*1024) << " MB input pending\n";
    };

    while (true) {
        {
            unique_lock<mutex> lk(wakeMtx);
            wakeCV.wait_for(lk, milliseconds(PROCESS_INTERVAL_MS),
                            []{ return !recording.load(); });
        }
        drainAndQueue();
        if (!recording.load()) {
            drainAndQueue();  // final boundary drain
            break;
        }
    }

    processingDone.store(true);
    outQueue.cv.notify_all();  // wake sender so it can exit cleanly
}

// ─── Sender thread ────────────────────────────────────────────────────────────
// Wakes whenever outQueue has data. Pops exactly SEND_CHUNK_FRAMES at a time
// (100ms @ 16kHz) and writes to WAV file AND can be adapted to stream to AWS.
// Partial final chunk is flushed as-is when recording ends.
void senderThread(const string& outPath)
{
    ofstream out(outPath, ios::binary);
    if (!out) { cerr << "Cannot open output file\n"; return; }
    writeWavHeader(out, 0);
    uint64_t totalFramesWritten = 0;

    while (true) {
        vector<float> chunk;
        {
            unique_lock<mutex> lk(outQueue.mtx);
            // Wait until we have a full chunk OR processing is done
            outQueue.cv.wait(lk, []{
                return outQueue.frames.size() >= SEND_CHUNK_FRAMES
                    || (processingDone.load() && !outQueue.frames.empty())
                    || (processingDone.load() && outQueue.frames.empty());
            });

            if (outQueue.frames.empty() && processingDone.load()) break;

            // Take exactly SEND_CHUNK_FRAMES, or whatever remains on final flush
            size_t take = min((size_t)SEND_CHUNK_FRAMES, outQueue.frames.size());
            chunk.assign(outQueue.frames.begin(), outQueue.frames.begin() + take);
            outQueue.frames.erase(outQueue.frames.begin(), outQueue.frames.begin() + take);
        }

        if (chunk.empty()) continue;

        // ── Write to WAV ──────────────────────────────────────────────────────
        // out.write((char*)chunk.data(), chunk.size() * sizeof(float));
        vector<int16_t> pcm(chunk.size());

        for(size_t i=0;i<chunk.size();i++)
        {
        float s = max(-1.0f, min(1.0f, chunk[i]));
        pcm[i] = (int16_t)(s * 32767.0f);
        }

        out.write((char*)pcm.data(), pcm.size() * sizeof(int16_t));
        totalFramesWritten += chunk.size();

        // ── HERE: send chunk to AWS Transcribe ───────────────────────────────
        // chunk is exactly 100ms of mono float32 @ 16kHz.
        // Convert to int16 if AWS SDK requires PCM16:
        //
        //   vector<int16_t> pcm16(chunk.size());
        //   for (size_t i = 0; i < chunk.size(); i++)
        //       pcm16[i] = (int16_t)(chunk[i] * 32767.0f);
        //   aws_transcribe_send(pcm16.data(), pcm16.size() * sizeof(int16_t));
        // ─────────────────────────────────────────────────────────────────────
    }

    patchWavHeader(out, (uint32_t)totalFramesWritten);
    out.close();

    double secs = (double)totalFramesWritten / TARGET_RATE;
    cout << "\nOutput: " << outPath << "\n";
    cout << "Duration: " << fixed << secs << " s\n";
    cout << "Format: Mono PCM16 WAV @ 16kHz\n";
    cout << "Chunk size used: " << SEND_CHUNK_FRAMES
         << " frames (" << (SEND_CHUNK_FRAMES*1000/TARGET_RATE) << " ms)\n";
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
    spkChannels = spkInfo->maxOutputChannels == 0
                ? spkInfo->maxInputChannels
                : spkInfo->maxOutputChannels;
    micRate = micInfo->defaultSampleRate;
    spkRate = spkInfo->defaultSampleRate;

    cout << "\n--- Quality Check ---\n";
    cout << "Mic:     " << micChannels << "ch @ " << micRate << "Hz\n";
    cout << "Speaker: " << spkChannels << "ch @ " << spkRate << "Hz\n";
    cout << "Process: every " << PROCESS_INTERVAL_MS << " ms\n";
    cout << "Send:    every " << (SEND_CHUNK_FRAMES*1000/TARGET_RATE) << " ms ("
         << SEND_CHUNK_FRAMES << " frames)\n";
    cout << "Output:  Mono float32 WAV @ 16kHz\n";
    cout << "---------------------\n\n";

    // Pre-reserve: 2x the processing interval worth of samples
    {
        lock_guard<mutex> lm(micAudio.mtx);
        lock_guard<mutex> ls(spkAudio.mtx);
        micAudio.samples.reserve((size_t)(micRate * micChannels * (PROCESS_INTERVAL_MS/1000.0) * 2));
        spkAudio.samples.reserve((size_t)(spkRate * spkChannels * (PROCESS_INTERVAL_MS/1000.0) * 2));
    }

    PaStreamParameters micParams{}, spkParams{};
    micParams.device = micDev; micParams.channelCount = micChannels;
    micParams.sampleFormat = paFloat32; micParams.suggestedLatency = micInfo->defaultLowInputLatency;
    spkParams.device = spkDev; spkParams.channelCount = spkChannels;
    spkParams.sampleFormat = paFloat32; spkParams.suggestedLatency = spkInfo->defaultLowInputLatency;

    PaStream *micStream, *spkStream;
    PaError err;
    err = Pa_OpenStream(&micStream, &micParams, NULL, micRate, FRAMES_PER_BUFFER, paClipOff, micCallback, NULL);
    if (err != paNoError) { cout << "ERROR mic: " << Pa_GetErrorText(err) << "\n"; return 1; }
    err = Pa_OpenStream(&spkStream, &spkParams, NULL, spkRate, FRAMES_PER_BUFFER, paClipOff, spkCallback, NULL);
    if (err != paNoError) { cout << "ERROR spk: " << Pa_GetErrorText(err) << "\n"; return 1; }

    recordStart = steady_clock::now();

    thread proc(processingThread);
    thread send(senderThread, "audio.wav");

    Pa_StartStream(micStream);
    Pa_StartStream(spkStream);

    cout << "Recording... Press ENTER to stop\n";
    cin.get();

    Pa_StopStream(micStream);  Pa_StopStream(spkStream);
    Pa_CloseStream(micStream); Pa_CloseStream(spkStream);
    Pa_Terminate();

    {
        lock_guard<mutex> lk(wakeMtx);
        recording.store(false);
    }
    wakeCV.notify_one();

    proc.join();
    send.join();
    return 0;
}