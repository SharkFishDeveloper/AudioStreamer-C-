#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <io.h>
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
#include <pa_ringbuffer.h>
#include <pa_win_wasapi.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using namespace std;
using namespace chrono;

// ─── Config ───────────────────────────────────────────────────────────────────
#define FRAMES_PER_BUFFER       512
#define TARGET_RATE             16000
#define TARGET_CHANNELS         1
#define PROCESS_INTERVAL_MS     500
#define SEND_CHUNK_FRAMES       1600    // 100ms @ 16kHz
#define DRIFT_LOG_INTERVAL      25
#define MAX_DRIFT_PPM           500.0

#ifndef paWASAPIUseOutputDeviceForInput
    #define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

// ─── Ring buffer input (lock-free, callback-safe) ─────────────────────────────
struct SharedAudio {
    PaUtilRingBuffer ringBuffer;
    float*           dataBuffer = nullptr;

    void init(int channels, double sampleRate) {
        size_t numFrames = 1;
        // 4 seconds of capacity, rounded up to next power of two (required by PA ring buffer)
        while (numFrames < (size_t)(sampleRate * 4)) numFrames <<= 1;
        size_t bytesPerFrame = sizeof(float) * channels;
        dataBuffer = (float*)malloc(numFrames * bytesPerFrame);
        PaUtil_InitializeRingBuffer(&ringBuffer, bytesPerFrame, numFrames, dataBuffer);
    }

    void cleanup() {
        if (dataBuffer) { free(dataBuffer); dataBuffer = nullptr; }
    }
};

SharedAudio micAudio, spkAudio;

// ─── Output queue: processing → sender (whole chunk vectors) ─────────────────
// Using deque<vector<float>> so pop_front is a single pointer swap, not O(n).
struct OutputQueue {
    mutex                    mtx;
    condition_variable       cv;
    deque<vector<float>>     chunks;   // each element = one mixed chunk
    size_t                   totalFrames = 0;
};
OutputQueue outQueue;

// ─── Overrun counters (set by callbacks, read by processing thread) ───────────
atomic<uint32_t> micOverruns{0};
atomic<uint32_t> spkOverruns{0};

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

// ─── Gain Constants ──────────────────────────────────────────────────────────
const float MIC_BOOST = 2.5f;     // Increase this (e.g., 3.0f, 4.0f) to hear yourself more
const float SPK_ATTENUATION = 0.2f;

// ─── Callbacks (lock-free) ────────────────────────────────────────────────────
static int micCallback(const void* in, void*, unsigned long n,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*)
{
    if (!in) return paContinue;
    micTotalFrames.fetch_add(n, memory_order_relaxed);
    ring_buffer_size_t written = PaUtil_WriteRingBuffer(&micAudio.ringBuffer, in, (ring_buffer_size_t)n);
    if (written < (ring_buffer_size_t)n)
        micOverruns.fetch_add(1, memory_order_relaxed);
    return paContinue;
}

static int spkCallback(const void* in, void*, unsigned long n,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*)
{
    if (!in) return paContinue;
    spkTotalFrames.fetch_add(n, memory_order_relaxed);
    ring_buffer_size_t written = PaUtil_WriteRingBuffer(&spkAudio.ringBuffer, in, (ring_buffer_size_t)n);
    if (written < (ring_buffer_size_t)n)
        spkOverruns.fetch_add(1, memory_order_relaxed);
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

    auto plausible = [](double m, double n){ double r = m/n; return r > 0.9 && r < 1.1; };
    if (plausible(mMic, nomMic)) ds.effectiveMicRate = 0.8*ds.effectiveMicRate + 0.2*mMic;
    if (plausible(mSpk, nomSpk)) ds.effectiveSpkRate = 0.8*ds.effectiveSpkRate + 0.2*mSpk;

    uint64_t wallSec = (uint64_t)duration<double>(now - recordStart).count();
    if (wallSec - ds.lastDriftLogAt >= (uint64_t)DRIFT_LOG_INTERVAL) {
        ds.lastDriftLogAt = wallSec;
        double micPpm = 1e6*(ds.effectiveMicRate - nomMic) / nomMic;
        double spkPpm = 1e6*(ds.effectiveSpkRate - nomSpk) / nomSpk;
        double relPpm = micPpm - spkPpm;
        cout << "[Drift @ " << wallSec << "s]"
             << "  mic=" << (int)micPpm << " ppm"
             << "  spk=" << (int)spkPpm << " ppm"
             << "  relative=" << (int)relPpm << " ppm";
        if (fabs(relPpm) > MAX_DRIFT_PPM) cout << "  *** HIGH — correction active ***";

        // Report overruns if any have occurred
        uint32_t mo = micOverruns.load(), so = spkOverruns.load();
        if (mo || so) cout << "  [OVERRUN mic=" << mo << " spk=" << so << "]";
        cout << "\n";
    }

    ds.prevMicFrames = curMic; ds.prevSpkFrames = curSpk;
    ds.lastUpdate = now;
    outMic = ds.effectiveMicRate; outSpk = ds.effectiveSpkRate;
}

// ─── Persistent resampler → mono ─────────────────────────────────────────────
// FIX 1: currentRate is now tracked and drift-corrected rate changes trigger reinit.
struct Resampler {
    ma_data_converter conv;
    bool     initialised = false;
    int      inChannels  = 0;
    uint32_t currentRate = 0;       // tracks last rate used — reinit if drift changes it

    void init(int inCh, uint32_t inRate) {
        if (initialised) ma_data_converter_uninit(&conv, NULL);
        ma_data_converter_config cfg = ma_data_converter_config_init(
            ma_format_f32, ma_format_f32,
            (ma_uint32)inCh, 1, inRate, TARGET_RATE
        );
        cfg.resampling.algorithm       = ma_resample_algorithm_linear;
        cfg.resampling.linear.lpfOrder = 4;
        if (ma_data_converter_init(&cfg, NULL, &conv) != MA_SUCCESS) {
            cerr << "Resampler init failed\n"; exit(1);
        }
        inChannels  = inCh;
        currentRate = inRate;       // store so drain() can detect rate changes
        initialised = true;
    }

    ~Resampler() { if (initialised) ma_data_converter_uninit(&conv, NULL); }

    void updateRate(uint32_t newRate) {
        if (!initialised) {
            init(inChannels, newRate);
        } else if (newRate != currentRate) {
            ma_data_converter_set_rate(&conv, newRate, TARGET_RATE);
            currentRate = newRate;
        }
    }


    vector<float> drain(SharedAudio& src, double inRate) {
        uint32_t rateInt = (uint32_t)round(inRate);
        // Reinit when drift correction changes integer rate — preserves correction accuracy
        updateRate(rateInt);

        ring_buffer_size_t available = PaUtil_GetRingBufferReadAvailable(&src.ringBuffer);
        if (available <= 0) return {};

        vector<float> input(available * inChannels);
        PaUtil_ReadRingBuffer(&src.ringBuffer, input.data(), available);

        ma_uint64 framesOut = 0;
        ma_data_converter_get_expected_output_frame_count(&conv, available, &framesOut);

        vector<float> out(framesOut + 64);  // +64 headroom for resampler overshoot
        ma_uint64 actualIn = available, actualOut = framesOut + 64;
        ma_data_converter_process_pcm_frames(&conv, input.data(), &actualIn, out.data(), &actualOut);
        out.resize(actualOut);
        return out;
    }
};

// ─── WAV helpers (PCM int16 mono) ─────────────────────────────────────────────
void writeWavHeader(ofstream& f, uint32_t totalFrames) {
    uint32_t dataBytes = totalFrames * sizeof(int16_t);
    uint32_t byteRate  = TARGET_RATE * sizeof(int16_t);
    auto w16 = [&](uint16_t v){ f.write((char*)&v, 2); };
    auto w32 = [&](uint32_t v){ f.write((char*)&v, 4); };
    f.write("RIFF", 4); w32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(1);
    w32(TARGET_RATE); w32(byteRate); w16(sizeof(int16_t)); w16(16);
    f.write("data", 4); w32(dataBytes);
}

void patchWavHeader(ofstream& f, uint32_t totalFrames) {
    uint32_t dataBytes = totalFrames * sizeof(int16_t);
    f.seekp(4);  uint32_t cs = 36 + dataBytes; f.write((char*)&cs, 4);
    f.seekp(40); f.write((char*)&dataBytes, 4);
    f.seekp(0, ios::end);
}

// ─── Processing thread (500ms) ────────────────────────────────────────────────
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
        vector<float> mixed(frames);
        for (size_t i = 0; i < frames; i++) {
            // Apply boost to mic and optional reduction to speakers
            float m = (i < micMono.size()) ? (micMono[i] * MIC_BOOST) : 0.0f;
            float s = (i < spkMono.size()) ? (spkMono[i] * SPK_ATTENUATION) : 0.0f;
            
            bool hasMic = (i < micMono.size());
            bool hasSpk = (i < spkMono.size());
            
            // Use a "Summing" mix instead of a "Dividing" mix to keep mic volume high
            // We clip to 1.0f anyway to prevent distortion
            float combined = m + s;
            mixed[i] = max(-1.0f, min(1.0f, combined));
        }

        // FIX 4: push whole vector — pop_front on deque<vector> is O(1)
        {
            lock_guard<mutex> lk(outQueue.mtx);
            outQueue.totalFrames += frames;
            outQueue.chunks.push_back(move(mixed));
            outQueue.cv.notify_one();
        }
    };

    while (recording.load()) {
        {
            unique_lock<mutex> lk(wakeMtx);
            wakeCV.wait_for(lk, milliseconds(PROCESS_INTERVAL_MS),
                            []{ return !recording.load(); });
        }
        drainAndQueue();
    }
    drainAndQueue();  // final drain after streams stop
    processingDone.store(true);
    outQueue.cv.notify_all();
}

// ─── Sender thread (100ms chunks) ────────────────────────────────────────────
void senderThread(const string& outPath)
{
    ofstream out(outPath, ios::binary);
    if (!out) { cerr << "Cannot open output file\n"; return; }
    writeWavHeader(out, 0);

    uint64_t totalFramesWritten = 0;
    vector<float> carry;   // leftover frames from previous processing chunk

    while (true) {
        // Pull next processing chunk (500ms) from queue
        vector<float> incoming;
        {
            unique_lock<mutex> lk(outQueue.mtx);
            outQueue.cv.wait(lk, []{
                return !outQueue.chunks.empty()
                    || (processingDone.load() && outQueue.chunks.empty());
            });
            if (outQueue.chunks.empty() && processingDone.load()) break;
            incoming = move(outQueue.chunks.front());
            outQueue.chunks.pop_front();   // FIX 4: O(1) pointer swap
        }

        // Prepend any leftover frames from the previous iteration
        if (!carry.empty()) {
            carry.insert(carry.end(), incoming.begin(), incoming.end());
            incoming = move(carry);
            carry.clear();
        }

        // Slice into exact SEND_CHUNK_FRAMES pieces
        size_t offset = 0;
        while (offset + SEND_CHUNK_FRAMES <= incoming.size()) {
            const float* src = incoming.data() + offset;

            // Convert float32 → int16 for WAV and AWS Transcribe
            vector<int16_t> pcm(SEND_CHUNK_FRAMES);
            for (int i = 0; i < SEND_CHUNK_FRAMES; i++)
                pcm[i] = (int16_t)(max(-1.0f, min(1.0f, src[i])) * 32767.0f);

            out.write((char*)pcm.data(), pcm.size() * sizeof(int16_t));
            totalFramesWritten += SEND_CHUNK_FRAMES;

            // ── Send to AWS Transcribe here ───────────────────────────────────
            // pcm.data() = int16 PCM, SEND_CHUNK_FRAMES samples, 100ms @ 16kHz
            // aws_transcribe_send(pcm.data(), pcm.size() * sizeof(int16_t));
            // ─────────────────────────────────────────────────────────────────

            offset += SEND_CHUNK_FRAMES;
        }

        // Keep any sub-chunk remainder for the next iteration
        if (offset < incoming.size())
            carry.assign(incoming.begin() + offset, incoming.end());
    }

    // Flush any remaining frames that didn't fill a complete 100ms chunk
    if (!carry.empty()) {
        vector<int16_t> pcm(carry.size());
        for (size_t i = 0; i < carry.size(); i++)
            pcm[i] = (int16_t)(max(-1.0f, min(1.0f, carry[i])) * 32767.0f);
        out.write((char*)pcm.data(), pcm.size() * sizeof(int16_t));
        totalFramesWritten += carry.size();
    }

    patchWavHeader(out, (uint32_t)totalFramesWritten);
    out.close();

    double secs = (double)totalFramesWritten / TARGET_RATE;
    cout << "\nOutput: " << outPath << "\n";
    cout << "Duration: " << fixed << secs << " s\n";
    cout << "Format: Mono PCM int16 WAV @ 16kHz\n";
    cout << "Chunk size: " << SEND_CHUNK_FRAMES
         << " frames (" << (SEND_CHUNK_FRAMES * 1000 / TARGET_RATE) << " ms)\n";
    uint32_t mo = micOverruns.load(), so = spkOverruns.load();
    if (mo || so)
        cout << "[WARNING] Ring buffer overruns — mic: " << mo << "  spk: " << so << "\n";
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

    cout << "\n--- Config ---\n";
    cout << "Mic:     " << micChannels << "ch @ " << micRate << "Hz\n";
    cout << "Speaker: " << spkChannels << "ch @ " << spkRate << "Hz\n";
    cout << "Process: every " << PROCESS_INTERVAL_MS << " ms\n";
    cout << "Send:    every " << (SEND_CHUNK_FRAMES * 1000 / TARGET_RATE) << " ms\n";
    cout << "Output:  Mono PCM int16 WAV @ 16kHz\n";
    cout << "--------------\n\n";

    micAudio.init(micChannels, micRate);
    spkAudio.init(spkChannels, spkRate);

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
    thread proc(processingThread);
    thread send(senderThread, "audio.wav");

    Pa_StartStream(micStream);
    Pa_StartStream(spkStream);

    cout << "Recording... Press ENTER to stop\n";
    cin.get();

    Pa_StopStream(micStream);
    Pa_StopStream(spkStream);
    // FIX 2: CloseStream before Terminate — prevents undefined behavior on some WASAPI drivers
    Pa_CloseStream(micStream);
    Pa_CloseStream(spkStream);

    {
        lock_guard<mutex> lk(wakeMtx);
        recording.store(false);
    }
    wakeCV.notify_one();

    proc.join();
    send.join();

    micAudio.cleanup();
    spkAudio.cleanup();
    Pa_Terminate();
    return 0;
}