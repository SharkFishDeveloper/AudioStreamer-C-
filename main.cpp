#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <io.h>
#include <deque>
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

#define FRAMES_PER_BUFFER 512
#define TARGET_RATE 16000
#define PROCESS_INTERVAL_MS 500
#define SEND_CHUNK_FRAMES 1600
#define DRIFT_LOG_INTERVAL 25
#define MAX_DRIFT_PPM 500.0

#ifndef paWASAPIUseOutputDeviceForInput
#define paWASAPIUseOutputDeviceForInput (1 << 4)
#endif

void nodeError(const string& msg){
    cerr << "ERR:" << msg << endl;
}

struct SharedAudio{
    PaUtilRingBuffer ringBuffer;
    float* dataBuffer=nullptr;

    void init(int channels,double sampleRate){
        size_t numFrames=1;
        while(numFrames<(size_t)(sampleRate*4)) numFrames<<=1;
        size_t bytesPerFrame=sizeof(float)*channels;
        dataBuffer=(float*)malloc(numFrames*bytesPerFrame);
        PaUtil_InitializeRingBuffer(&ringBuffer,bytesPerFrame,numFrames,dataBuffer);
    }

    void cleanup(){
        if(dataBuffer){free(dataBuffer);dataBuffer=nullptr;}
    }
};

SharedAudio micAudio,spkAudio;

struct OutputQueue{
    mutex mtx;
    condition_variable cv;
    deque<vector<float>> chunks;
};

OutputQueue outQueue;

atomic<uint32_t> micOverruns{0};
atomic<uint32_t> spkOverruns{0};

atomic<uint64_t> micTotalFrames{0};
atomic<uint64_t> spkTotalFrames{0};

atomic<bool> recording{true};
atomic<bool> processingDone{false};

// Which sources are active
atomic<bool> hasMic{false};
atomic<bool> hasSpk{false};

mutex wakeMtx;
condition_variable wakeCV;

int micChannels,spkChannels;
double micRate,spkRate;
steady_clock::time_point recordStart;

const float MIC_BOOST=2.5f;
const float SPK_ATTENUATION=0.2f;

static int micCallback(const void* in,void*,unsigned long n,
const PaStreamCallbackTimeInfo*,PaStreamCallbackFlags,void*)
{
    if(!in) return paContinue;

    micTotalFrames.fetch_add(n,memory_order_relaxed);

    ring_buffer_size_t written=
    PaUtil_WriteRingBuffer(&micAudio.ringBuffer,in,(ring_buffer_size_t)n);

    if(written<(ring_buffer_size_t)n)
        micOverruns.fetch_add(1,memory_order_relaxed);

    return paContinue;
}

static int spkCallback(const void* in,void*,unsigned long n,
const PaStreamCallbackTimeInfo*,PaStreamCallbackFlags,void*)
{
    if(!in) return paContinue;

    spkTotalFrames.fetch_add(n,memory_order_relaxed);

    ring_buffer_size_t written=
    PaUtil_WriteRingBuffer(&spkAudio.ringBuffer,in,(ring_buffer_size_t)n);

    if(written<(ring_buffer_size_t)n)
        spkOverruns.fetch_add(1,memory_order_relaxed);

    return paContinue;
}

struct DriftState{
    double effectiveMicRate=0;
    double effectiveSpkRate=0;
    uint64_t prevMicFrames=0;
    uint64_t prevSpkFrames=0;
    steady_clock::time_point lastUpdate;
    bool initialised=false;
    uint64_t lastDriftLogAt=0;
};

void updateDrift(DriftState& ds,double nomMic,double nomSpk,
double& outMic,double& outSpk)
{
    auto now=steady_clock::now();

    if(!ds.initialised){
        ds.effectiveMicRate=nomMic;
        ds.effectiveSpkRate=nomSpk;
        ds.prevMicFrames=micTotalFrames.load();
        ds.prevSpkFrames=spkTotalFrames.load();
        ds.lastUpdate=now;
        ds.initialised=true;
        outMic=nomMic;
        outSpk=nomSpk;
        return;
    }

    double elapsed=duration<double>(now-ds.lastUpdate).count();
    if(elapsed<1.0){
        outMic=ds.effectiveMicRate;
        outSpk=ds.effectiveSpkRate;
        return;
    }

    uint64_t curMic=micTotalFrames.load();
    uint64_t curSpk=spkTotalFrames.load();

    double mMic=(curMic-ds.prevMicFrames)/elapsed;
    double mSpk=(curSpk-ds.prevSpkFrames)/elapsed;

    auto plausible=[](double m,double n){
        double r=m/n;
        return r>0.9&&r<1.1;
    };

    if(plausible(mMic,nomMic))
        ds.effectiveMicRate=0.8*ds.effectiveMicRate+0.2*mMic;

    if(plausible(mSpk,nomSpk))
        ds.effectiveSpkRate=0.8*ds.effectiveSpkRate+0.2*mSpk;

    ds.prevMicFrames=curMic;
    ds.prevSpkFrames=curSpk;
    ds.lastUpdate=now;

    outMic=ds.effectiveMicRate;
    outSpk=ds.effectiveSpkRate;
}

struct Resampler{

    ma_data_converter conv;
    bool initialised=false;
    int inChannels=0;
    uint32_t currentRate = 0;
    double lastAppliedRatio = 0.0;// Added this to store state

    void init(int inCh,uint32_t inRate){

        ma_data_converter_config cfg=
        ma_data_converter_config_init(
        ma_format_f32,ma_format_f32,
        (ma_uint32)inCh,1,inRate,TARGET_RATE);

        cfg.resampling.algorithm=ma_resample_algorithm_linear;

        if(ma_data_converter_init(&cfg,NULL,&conv)!=MA_SUCCESS){
            nodeError("resampler init failed");
            exit(1);
        }

        inChannels=inCh;
        currentRate=inRate;
        lastAppliedRatio = (double)inRate / (double)TARGET_RATE;
        initialised=true;
    }

    vector<float> drain(SharedAudio& src,double inRate){
        if (!initialised) return {};
        double newRatio = inRate / (double)TARGET_RATE;
        if (std::abs(newRatio - lastAppliedRatio) > 0.0001) {
            ma_data_converter_set_rate_ratio(&conv, newRatio);
            lastAppliedRatio = newRatio;
        }

        ring_buffer_size_t available=
        PaUtil_GetRingBufferReadAvailable(&src.ringBuffer);

        if(available<=0) return {};

        vector<float> input(available*inChannels);

        PaUtil_ReadRingBuffer(&src.ringBuffer,input.data(),available);

        ma_uint64 framesOut=0;

        ma_data_converter_get_expected_output_frame_count(
        &conv,available,&framesOut);
        

        vector<float> out(framesOut+64);

        ma_uint64 actualIn=available;
        ma_uint64 actualOut=framesOut+64;

        ma_data_converter_process_pcm_frames(
        &conv,input.data(),&actualIn,out.data(),&actualOut);

        out.resize(actualOut);

        return out;
    }
};

void processingThread(){

    DriftState drift;

    // Only construct resamplers for active sources
    Resampler micRes,spkRes;

    if(hasMic.load()) micRes.init(micChannels,(uint32_t)micRate);
    if(hasSpk.load()) spkRes.init(spkChannels,(uint32_t)spkRate);

    // Nominal rates for drift tracking — use 16000 as fallback for inactive side
    double nomMic = hasMic.load() ? micRate : TARGET_RATE;
    double nomSpk = hasSpk.load() ? spkRate : TARGET_RATE;

    auto drainAndQueue=[&](){

        double effMic,effSpk;

        updateDrift(drift,nomMic,nomSpk,effMic,effSpk);

        vector<float> micMono,spkMono;

        if(hasMic.load()) micMono=micRes.drain(micAudio,effMic);
        if(hasSpk.load()) spkMono=spkRes.drain(spkAudio,effSpk);

        if(micMono.empty()&&spkMono.empty()) return;

        size_t frames=max(micMono.size(),spkMono.size());

        vector<float> mixed(frames);

        for(size_t i=0;i<frames;i++){

            float m=(i<micMono.size())?micMono[i]*MIC_BOOST:0;
            float s=(i<spkMono.size())?spkMono[i]*SPK_ATTENUATION:0;

            float combined=m+s;

            mixed[i]=max(-1.0f,min(1.0f,combined));
        }

        {
            lock_guard<mutex> lk(outQueue.mtx);
            outQueue.chunks.push_back(move(mixed));
            outQueue.cv.notify_one();
        }
    };

    while(recording.load()){

        unique_lock<mutex> lk(wakeMtx);

        wakeCV.wait_for(lk,milliseconds(PROCESS_INTERVAL_MS));

        drainAndQueue();
    }

    drainAndQueue();

    processingDone.store(true);

    outQueue.cv.notify_all();
}

void senderThread(){

    vector<float> carry;

    while(true){

        vector<float> incoming;

        {
            unique_lock<mutex> lk(outQueue.mtx);

            outQueue.cv.wait(lk,[]{
                return !outQueue.chunks.empty() ||
                (processingDone.load() && outQueue.chunks.empty());
            });

            if(outQueue.chunks.empty()&&processingDone.load())
                break;

            incoming=move(outQueue.chunks.front());
            outQueue.chunks.pop_front();
        }

        if(!carry.empty()){
            carry.insert(carry.end(),incoming.begin(),incoming.end());
            incoming=move(carry);
            carry.clear();
        }

        size_t offset=0;

        while(offset+SEND_CHUNK_FRAMES<=incoming.size()){

            const float* src=incoming.data()+offset;

            int16_t pcm[SEND_CHUNK_FRAMES];

            for(int i=0;i<SEND_CHUNK_FRAMES;i++)
                pcm[i]=(int16_t)(
                max(-1.0f,min(1.0f,src[i]))*32767.0f);

            fwrite(pcm,sizeof(int16_t),SEND_CHUNK_FRAMES,stdout);

            offset+=SEND_CHUNK_FRAMES;
        }

        if(offset<incoming.size())
            carry.assign(incoming.begin()+offset,incoming.end());
    }
}

void listDevices(){

    int num=Pa_GetDeviceCount();

    cout<<"[";

    for(int i=0;i<num;i++){

        const PaDeviceInfo* d=Pa_GetDeviceInfo(i);

        cout<<"{";
        cout<<"\"id\":"<<i<<",";
        cout<<"\"name\":\""<<d->name<<"\",";
        cout<<"\"input\":"<<d->maxInputChannels<<",";
        cout<<"\"output\":"<<d->maxOutputChannels<<",";
        cout<<"\"rate\":"<<d->defaultSampleRate;
        cout<<"}";

        if(i<num-1) cout<<",";
    }

    cout<<"]";
}

int main(int argc,char* argv[]){

    _setmode(_fileno(stdout),_O_BINARY);
    setvbuf(stdout,NULL,_IONBF,0);

    Pa_Initialize();

    int micDev=-1;
    int spkDev=-1;
    bool listMode=false;

    for(int i=1;i<argc;i++){

        string arg=argv[i];

        if(arg=="--list")
            listMode=true;

        else if(arg=="--mic" && i+1<argc)
            micDev=stoi(argv[++i]);

        else if(arg=="--speaker" && i+1<argc)
            spkDev=stoi(argv[++i]);
    }

    if(listMode){
        listDevices();
        Pa_Terminate();
        return 0;
    }

    if(micDev<0 && spkDev<0){
        nodeError("No devices specified. Use --mic, --speaker, or both.");
        Pa_Terminate();
        return 1;
    }

    int numDevices=Pa_GetDeviceCount();

    if(micDev>=0 && micDev>=numDevices){
        nodeError("invalid mic device id");
        Pa_Terminate();
        return 1;
    }

    if(spkDev>=0 && spkDev>=numDevices){
        nodeError("invalid speaker device id");
        Pa_Terminate();
        return 1;
    }

    const PaDeviceInfo* micInfo = (micDev>=0) ? Pa_GetDeviceInfo(micDev) : nullptr;
    const PaDeviceInfo* spkInfo = (spkDev>=0) ? Pa_GetDeviceInfo(spkDev) : nullptr;

    if(micInfo){
        micChannels = micInfo->maxInputChannels;
        micRate     = micInfo->defaultSampleRate;
        micAudio.init(micChannels, micRate);
        hasMic.store(true);
    }

    if(spkInfo){
        spkChannels = spkInfo->maxOutputChannels==0
            ? spkInfo->maxInputChannels
            : spkInfo->maxOutputChannels;
        spkRate = spkInfo->defaultSampleRate;
        spkAudio.init(spkChannels, spkRate);
        hasSpk.store(true);
    }

    PaStream *micStream=nullptr, *spkStream=nullptr;
    PaError err;

    if(micInfo){
        PaStreamParameters micParams{};
        micParams.device           = micDev;
        micParams.channelCount     = micChannels;
        micParams.sampleFormat     = paFloat32;
        micParams.suggestedLatency = micInfo->defaultLowInputLatency;

        err=Pa_OpenStream(&micStream,&micParams,NULL,micRate,
        FRAMES_PER_BUFFER,paClipOff,micCallback,NULL);

        if(err!=paNoError){
            nodeError("cannot open mic stream");
            Pa_Terminate();
            return 1;
        }
    }

    if(spkInfo){
        PaStreamParameters spkParams{};
        spkParams.device           = spkDev;
        spkParams.channelCount     = spkChannels;
        spkParams.sampleFormat     = paFloat32;
        spkParams.suggestedLatency = spkInfo->defaultLowInputLatency;

        err=Pa_OpenStream(&spkStream,&spkParams,NULL,spkRate,
        FRAMES_PER_BUFFER,paClipOff,spkCallback,NULL);

        if(err!=paNoError){
            nodeError("cannot open speaker stream");
            Pa_Terminate();
            return 1;
        }
    }

    recordStart=steady_clock::now();

    thread proc(processingThread);
    thread send(senderThread);

    if(micStream)  Pa_StartStream(micStream);
    if(spkStream)  Pa_StartStream(spkStream);

    while(true)
        this_thread::sleep_for(seconds(1));

    return 0;
}