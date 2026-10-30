"""Production PCM backend tests with a deterministic fake XAudio2 device.

Only the XAudio2 header/API is replaced. Conversion, worker, per-client queues,
callbacks, failure handling and Close execute from the actual audio_output.cpp.
No game, sound device or playback is used. Real SDK compilation is also checked.
"""
from pathlib import Path
import subprocess
import tempfile
from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "port/src/runtime"

FAKE = r'''
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <vector>
#pragma comment(lib,"ole32.lib")
std::atomic<unsigned> checks{0};
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct IXAudio2VoiceCallback {
 virtual void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32)=0;
 virtual void STDMETHODCALLTYPE OnVoiceProcessingPassEnd()=0;
 virtual void STDMETHODCALLTYPE OnStreamEnd()=0;
 virtual void STDMETHODCALLTYPE OnBufferStart(void*)=0;
 virtual void STDMETHODCALLTYPE OnBufferEnd(void*)=0;
 virtual void STDMETHODCALLTYPE OnLoopEnd(void*)=0;
 virtual void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT)=0;
};
struct XAUDIO2_BUFFER {
 UINT32 Flags=0,AudioBytes=0;const BYTE* pAudioData=nullptr;
 UINT32 PlayBegin=0,PlayLength=0,LoopBegin=0,LoopLength=0,LoopCount=0;void* pContext=nullptr;
};
constexpr float XAUDIO2_DEFAULT_FREQ_RATIO=2.0f;
struct IXAudio2MasteringVoice {};
std::mutex fakeMutex;std::condition_variable fakeCv;
int failureMode=0;
bool blockCompletion=false,completionEntered=false,releaseCompletion=false;
unsigned sourceCount=0,destroyedCount=0;
struct IXAudio2SourceVoice;
std::vector<IXAudio2SourceVoice*> sources;
struct IXAudio2SourceVoice {
 IXAudio2VoiceCallback* callback;
 std::deque<XAUDIO2_BUFFER> buffers;
 unsigned callbacks=0;bool destroyed=false,started=false;
 explicit IXAudio2SourceVoice(IXAudio2VoiceCallback* c):callback(c){}
 HRESULT Start(){if(failureMode==4)return E_FAIL;std::lock_guard l(fakeMutex);started=true;return S_OK;}
 HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER* b){
  CHECK(b->AudioBytes==6144&&b->PlayLength==256&&b->pContext);
  if(failureMode==5)return E_FAIL;
  std::lock_guard l(fakeMutex);CHECK(!destroyed);buffers.push_back(*b);return S_OK;
 }
 void DestroyVoice(){
  std::unique_lock l(fakeMutex);fakeCv.wait(l,[&]{return callbacks==0;});
  CHECK(!destroyed);
  // Device may read every pending buffer until this method finishes.
  for(auto& b:buffers){volatile float sample=*reinterpret_cast<const float*>(b.pAudioData);(void)sample;}
  buffers.clear();destroyed=true;++destroyedCount;
 }
 void CompleteOne(){
  XAUDIO2_BUFFER b;
  {std::unique_lock l(fakeMutex);CHECK(!destroyed&&started&&!buffers.empty());b=buffers.front();buffers.pop_front();++callbacks;
   if(blockCompletion){completionEntered=true;fakeCv.notify_all();fakeCv.wait(l,[]{return releaseCompletion;});}}
  callback->OnBufferEnd(b.pContext);
  {std::lock_guard l(fakeMutex);--callbacks;}fakeCv.notify_all();
 }
};
struct IXAudio2 {
 HRESULT CreateMasteringVoice(IXAudio2MasteringVoice** out){if(failureMode==2)return E_FAIL;static IXAudio2MasteringVoice master;*out=&master;return S_OK;}
 HRESULT CreateSourceVoice(IXAudio2SourceVoice** out,const WAVEFORMATEX* wf,UINT32,float,IXAudio2VoiceCallback* cb){
  CHECK(wf->wFormatTag==WAVE_FORMAT_EXTENSIBLE&&wf->nChannels==6&&wf->nSamplesPerSec==48000);
  CHECK(wf->wBitsPerSample==32&&wf->nBlockAlign==24&&wf->nAvgBytesPerSec==1152000);
  const auto* ext=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
  CHECK(ext->dwChannelMask==0x3F&&ext->Samples.wValidBitsPerSample==32&&ext->SubFormat.Data1==WAVE_FORMAT_IEEE_FLOAT);
  if(failureMode==3)return E_FAIL;
  std::lock_guard l(fakeMutex);*out=new IXAudio2SourceVoice(cb);sources.push_back(*out);++sourceCount;return S_OK;
 }
 void Release(){}
};
HRESULT XAudio2Create(IXAudio2** out){if(failureMode==1)return E_FAIL;static IXAudio2 engine;*out=&engine;return S_OK;}
'''

HARNESS = r'''
#include <algorithm>
#include <bit>
#include <cstring>
#include <future>
#include <string>
@PRODUCTION@
template<class F> void until(F f){for(int i=0;i<600&&!f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));CHECK(f());}
void put(uint8_t* p,float f){uint32_t v=std::bit_cast<uint32_t>(f);p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);}
void fill(std::array<uint8_t,AUDIO_PCM_BYTES>& bytes,float offset){for(size_t c=0;c<6;++c)for(size_t s=0;s<256;++s)put(bytes.data()+(c*256+s)*4,offset+float(c*256+s)/2048.0f);}
void checkBuffer(IXAudio2SourceVoice* voice,float offset){
 std::lock_guard l(fakeMutex);CHECK(!voice->buffers.empty());
 const float* data=reinterpret_cast<const float*>(voice->buffers.front().pAudioData);
 for(size_t c=0;c<6;++c)for(size_t s=0;s<256;++s)CHECK(data[s*6+c]==offset+float(c*256+s)/2048.0f);
}
int main(int argc,char** argv){
 failureMode=argc>1?std::atoi(argv[1]):0;
 std::array<uint8_t,AUDIO_PCM_BYTES> bytes;fill(bytes,0.125f);
 if(failureMode){
  AudioOutputClient a(0x41550000);for(size_t i=0;i<AUDIO_OUTPUT_PRIME_FRAMES;++i)a.Submit(bytes.data());
  until([&]{return a.Stats().failed;});auto st=a.Stats();
  CHECK(!st.ready&&st.queued==0&&st.received==AUDIO_OUTPUT_PRIME_FRAMES&&st.dropped==AUDIO_OUTPUT_PRIME_FRAMES);
  a.Submit(bytes.data());CHECK(a.Stats().dropped==AUDIO_OUTPUT_PRIME_FRAMES+1);
  CHECK(a.TryAcquireRenderCredit(100));CHECK(!a.TryAcquireRenderCredit(100+AUDIO_PCM_FRAME_NS-1));
  CHECK(a.TryAcquireRenderCredit(100+AUDIO_PCM_FRAME_NS));CHECK(!a.TryAcquireRenderCredit(100+AUDIO_PCM_FRAME_NS));
  a.Close();a.Close();CHECK(!a.TryAcquireRenderCredit(100000000000ull));
  std::printf("PASS device failure mode %d (%u checks)\n",failureMode,checks.load());return 0;
 }
 // Conversion checks all positions, unaligned input, no source mutation and
 // destination canaries; negative zero remains signed, NaN/Inf become silence.
 std::array<uint8_t,AUDIO_PCM_BYTES+2> unaligned;unaligned.fill(0xA5);
 std::memcpy(unaligned.data()+1,bytes.data(),bytes.size());auto original=unaligned;
 struct Guarded{uint64_t before=0xABCDEF0123456789;std::array<float,AUDIO_PCM_VALUES> pcm;uint64_t after=0x123456789ABCDEF0;} out;
 auto cvt=ConvertAudioPcm(unaligned.data()+1,out.pcm);CHECK(cvt.nonzero&&!cvt.nonfinite);
 for(size_t c=0;c<6;++c)for(size_t s=0;s<256;++s)CHECK(out.pcm[s*6+c]==0.125f+float(c*256+s)/2048.0f);
 CHECK(unaligned==original&&out.before==0xABCDEF0123456789&&out.after==0x123456789ABCDEF0);
 const uint32_t special[]={0x80000000,0x7FC00000,0x7F800000,0xFF800000,0xBF800000};
 bytes.fill(0);for(size_t i=0;i<5;++i)put(bytes.data()+i*4,std::bit_cast<float>(special[i]));
 cvt=ConvertAudioPcm(bytes.data(),out.pcm);CHECK(cvt.nonzero&&cvt.nonfinite==3);
 CHECK(std::bit_cast<uint32_t>(out.pcm[0])==0x80000000&&out.pcm[6]==0&&out.pcm[12]==0&&out.pcm[18]==0&&out.pcm[24]==-1);
 // Two voices copy shared guest scratch immediately; later writes cannot alter
 // frames queued to either device voice.
 AudioOutputClient a(0x41550000),b(0x41550001);
 fill(bytes,0.125f);a.Submit(bytes.data());fill(bytes,0.25f);b.Submit(bytes.data());bytes.fill(0xCC);
 until([&]{return a.Stats().submitted==1&&b.Stats().submitted==1;});
 IXAudio2SourceVoice* va;IXAudio2SourceVoice* vb;
 {std::lock_guard l(fakeMutex);CHECK(sources.size()==2);va=sources[0];vb=sources[1];}
 checkBuffer(va,0.125f);checkBuffer(vb,0.25f);
 CHECK(a.Stats().nonzero==1&&b.Stats().nonzero==1);
 // Backpressure is bounded, drops don't overwrite queued audio, and a completed
 // slot can be reused only after the device callback releases it.
 fill(bytes,0.5f);for(size_t i=1;i<AUDIO_OUTPUT_QUEUE_FRAMES;++i)a.Submit(bytes.data());
 until([&]{return a.Stats().submitted==AUDIO_OUTPUT_QUEUE_FRAMES&&a.Stats().started;});
 a.Submit(bytes.data());CHECK(a.Stats().dropped==1&&a.Stats().queued==AUDIO_OUTPUT_QUEUE_FRAMES);
 checkBuffer(va,0.125f);va->CompleteOne();CHECK(a.Stats().completed==1&&a.Stats().queued==AUDIO_OUTPUT_QUEUE_FRAMES-1);
 a.Submit(bytes.data());until([&]{return a.Stats().submitted==AUDIO_OUTPUT_QUEUE_FRAMES+1;});
 CHECK(a.Stats().queued==AUDIO_OUTPUT_QUEUE_FRAMES);
 b.Close();b.Close();CHECK(b.Stats().queued==0&&!b.Stats().ready&&b.Stats().dropped==1);
 CHECK(a.Stats().ready);checkBuffer(va,0.5f);
 // Close must wait through an in-flight device callback; storage stays alive.
 {std::lock_guard l(fakeMutex);blockCompletion=true;}
 std::thread completing([&]{va->CompleteOne();});
 {std::unique_lock l(fakeMutex);CHECK(fakeCv.wait_for(l,std::chrono::seconds(3),[]{return completionEntered;}));}
 auto closing=std::async(std::launch::async,[&]{a.Close();});
 CHECK(closing.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);
 {std::lock_guard l(fakeMutex);releaseCompletion=true;}fakeCv.notify_all();
 completing.join();closing.get();CHECK(a.Stats().queued==0&&a.Stats().completed==2);
 a.Submit(bytes.data());CHECK(a.Stats().received==AUDIO_OUTPUT_QUEUE_FRAMES+3);
 {std::lock_guard l(fakeMutex);CHECK(destroyedCount==2);blockCompletion=false;}
 // Asynchronous device failure stops only that output voice, not other clients.
 AudioOutputClient c(0x41550002),d(0x41550003);
 for(size_t i=0;i<AUDIO_OUTPUT_PRIME_FRAMES;++i){c.Submit(bytes.data());d.Submit(bytes.data());}
 until([&]{return c.Stats().started&&d.Stats().started;});
 IXAudio2SourceVoice* vc;IXAudio2SourceVoice* vd;
 {std::lock_guard l(fakeMutex);vc=sources[2];vd=sources[3];}
 vc->callback->OnVoiceError(nullptr,E_FAIL);until([&]{return c.Stats().failed;});
 CHECK(c.Stats().queued==0&&d.Stats().ready);vd->CompleteOne();CHECK(d.Stats().completed==1);
 c.Close();d.Close();
 // Initial credits prefill before Start; completed device buffers alone grant
 // new credits. Arbitrarily advancing wall time cannot overrun a healthy queue.
 AudioOutputClient paced(0x41550004);
 for(size_t i=0;i<AUDIO_OUTPUT_PRIME_FRAMES-1;++i){CHECK(paced.TryAcquireRenderCredit(0));paced.Submit(bytes.data());}
 until([&]{return paced.Stats().submitted==AUDIO_OUTPUT_PRIME_FRAMES-1;});
 CHECK(!paced.Stats().started);CHECK(paced.TryAcquireRenderCredit(0));paced.Submit(bytes.data());
 CHECK(!paced.TryAcquireRenderCredit(1000000000000ull));until([&]{return paced.Stats().started;});
 IXAudio2SourceVoice* vp;{std::lock_guard l(fakeMutex);vp=sources.back();}
 uint64_t rendered=AUDIO_OUTPUT_PRIME_FRAMES;
 for(unsigned tick=0;tick<200;++tick){
  const unsigned batch=1+tick%3;const auto token=AudioOutputWakeVersion();
  for(unsigned i=0;i<batch;++i)vp->CompleteOne();
  CHECK(AudioOutputWakeVersion()!=token);
  WaitAudioOutputWake(token,1000000000); // completion before waiting must not be lost
  for(unsigned i=0;i<batch;++i){CHECK(paced.TryAcquireRenderCredit(uint64_t(tick)*7000000));paced.Submit(bytes.data());++rendered;}
  CHECK(!paced.TryAcquireRenderCredit(uint64_t(tick+1)*90000000));
  until([&]{return paced.Stats().submitted==rendered;});
  CHECK(paced.Stats().queued==AUDIO_OUTPUT_PRIME_FRAMES&&paced.Stats().dropped==0);
 }
 vp->callback->OnVoiceProcessingPassStart(0);CHECK(paced.Stats().starvationPasses==0);
 vp->callback->OnVoiceProcessingPassStart(123);vp->callback->OnVoiceProcessingPassStart(456);
 CHECK(paced.Stats().starvationPasses==2&&paced.Stats().starvationBytes==579);
 paced.Close();CHECK(!paced.TryAcquireRenderCredit(1000000000000ull));
 std::printf("PASS %u PCM conversion, ownership, bounded queue, completion and close checks\n",checks.load());
}
'''

def main():
    source=(RUNTIME/'audio_output.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='ttt2-pcm-output-') as tmp:
        folder=Path(tmp)
        (folder/'fake_xaudio2.h').write_text(FAKE)
        fake=source.replace('#include <xaudio2.h>', '#include "fake_xaudio2.h"')
        exe=compile_one(folder,'output',HARNESS.replace('@PRODUCTION@',fake),[RUNTIME,folder])
        for mode in range(6):
            subprocess.run([str(exe),str(mode)],check=True,timeout=15)
        # Endian failure control must fail an exact position/value check.
        bad=fake.replace('const uint32_t bits = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |\n                                  (uint32_t(p[2]) << 8) | uint32_t(p[3]);',
                         'const uint32_t bits = (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);')
        assert bad!=fake
        exe=compile_one(folder,'bad_endian',HARNESS.replace('@PRODUCTION@',bad),[RUNTIME,folder])
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        assert result.returncode!=0 and 'out.pcm[s*6+c]' in result.stderr, result.stdout+result.stderr
        print('PASS negative control: wrong endianness detected')
        unpaced=fake.replace('if (!state_->renderCredits) return false;', 'if (false) return false;')
        assert unpaced!=fake
        exe=compile_one(folder,'unpaced',HARNESS.replace('@PRODUCTION@',unpaced),[RUNTIME,folder])
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        assert result.returncode!=0 and '!paced.TryAcquireRenderCredit' in result.stderr,result.stdout+result.stderr
        print('PASS negative control: independent timer/unbounded rendering is detected')
        sdk='#pragma comment(lib,"xaudio2.lib")\n#pragma comment(lib,"ole32.lib")\n'+source+'\nint main(){return AUDIO_PCM_BYTES==6144?0:1;}\n'
        exe=compile_one(folder,'real_sdk',sdk,[RUNTIME])
        subprocess.run([str(exe)],check=True,timeout=10)
        print('PASS real Windows SDK production compile/link (no device initialized)')

if __name__=='__main__':main()
