#include "audio_wasapi.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace {
const CLSID CLSID_MMDeviceEnumerator_ = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator_ = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient_ = __uuidof(IAudioClient);
const IID IID_IAudioRenderClient_ = __uuidof(IAudioRenderClient);
constexpr int OUT_RATE = 48000;
}

struct AudioOut::Impl
{
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	HANDLE event = nullptr;
	std::thread thread;
	std::atomic<bool> running{false};
	UINT32 buffer_frames = 0;

	// source ring (mono float)
	std::mutex mtx;
	std::vector<float> ring = std::vector<float>(1 << 16);
	size_t rd = 0, wr = 0;     // monotonically increasing, index modulo size
	double src_rate = 31250.0;
	double frac = 0;           // fractional read position between ring[rd] and ring[rd+1]
	float last = 0;
	double ratio_adj = 1.0;
	std::atomic<float> volume{1.0f};
	std::atomic<float> target_s{0.06f};
	std::atomic<float> fill_ms{0};

	size_t available() const { return wr - rd; }

	void render_loop()
	{
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		while (running)
		{
			WaitForSingleObject(event, 100);
			if (!running) break;
			UINT32 padding = 0;
			if (FAILED(client->GetCurrentPadding(&padding))) continue;
			UINT32 frames = buffer_frames - padding;
			if (frames == 0) continue;
			BYTE *data = nullptr;
			if (FAILED(render->GetBuffer(frames, &data))) continue;
			int16_t *out = reinterpret_cast<int16_t *>(data);
			fill(out, frames);
			render->ReleaseBuffer(frames, 0);
		}
		CoUninitialize();
	}

	void fill(int16_t *out, UINT32 frames)
	{
		std::lock_guard<std::mutex> lk(mtx);
		const double target = src_rate * double(target_s.load());   // aim for the configured amount buffered
		double have = double(available());
		fill_ms = float(have / src_rate * 1000.0);
		// gentle rate control: consume a bit faster if we are far ahead, slower if starving
		double err = (have - target) / target;
		ratio_adj = 1.0 + std::clamp(err * 0.02, -0.03, 0.03);
		double step = src_rate / OUT_RATE * ratio_adj;
		float vol = volume;
		const size_t mask = ring.size() - 1;

		for (UINT32 i = 0; i < frames; i++)
		{
			float s;
			if (available() >= 2)
			{
				float a = ring[rd & mask], b = ring[(rd + 1) & mask];
				s = a + (b - a) * float(frac);
				frac += step;
				while (frac >= 1.0 && available() >= 2)
				{
					frac -= 1.0;
					rd++;
				}
				last = s;
			}
			else
			{
				// underrun: fade to silence
				last *= 0.995f;
				s = last;
			}
			s *= vol;
			int v = int(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
			out[i * 2] = int16_t(v);
			out[i * 2 + 1] = int16_t(v);
		}
	}
};

AudioOut::AudioOut() : m_impl(new Impl) {}
AudioOut::~AudioOut() { stop(); }

bool AudioOut::start()
{
	Impl &d = *m_impl;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator_, (void **)&en)))
		return false;
	HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
	en->Release();
	if (FAILED(hr)) return false;
	hr = dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, (void **)&d.client);
	dev->Release();
	if (FAILED(hr)) return false;

	WAVEFORMATEX fmt{};
	fmt.wFormatTag = WAVE_FORMAT_PCM;
	fmt.nChannels = 2;
	fmt.nSamplesPerSec = OUT_RATE;
	fmt.wBitsPerSample = 16;
	fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
	fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

	const REFERENCE_TIME buf = 400000;    // 40 ms
	hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
	                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
	                          buf, 0, &fmt, nullptr);
	if (FAILED(hr)) return false;
	d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	d.client->SetEventHandle(d.event);
	d.client->GetBufferSize(&d.buffer_frames);
	if (FAILED(d.client->GetService(IID_IAudioRenderClient_, (void **)&d.render))) return false;

	// prime with silence
	BYTE *data = nullptr;
	if (SUCCEEDED(d.render->GetBuffer(d.buffer_frames, &data)))
		d.render->ReleaseBuffer(d.buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);

	d.running = true;
	d.thread = std::thread([&d] { d.render_loop(); });
	SetThreadPriority(d.thread.native_handle(), THREAD_PRIORITY_HIGHEST);
	return SUCCEEDED(d.client->Start());
}

void AudioOut::stop()
{
	Impl &d = *m_impl;
	if (d.running)
	{
		d.running = false;
		SetEvent(d.event);
		if (d.thread.joinable()) d.thread.join();
		if (d.client) d.client->Stop();
	}
	if (d.render) { d.render->Release(); d.render = nullptr; }
	if (d.client) { d.client->Release(); d.client = nullptr; }
	if (d.event) { CloseHandle(d.event); d.event = nullptr; }
}

void AudioOut::push(const int16_t *samples, int count, double rate)
{
	Impl &d = *m_impl;
	std::lock_guard<std::mutex> lk(d.mtx);
	if (rate > 1000) d.src_rate = rate;
	const size_t mask = d.ring.size() - 1;
	// never let the ring overrun the reader: drop oldest samples instead
	if (d.available() + size_t(count) >= d.ring.size())
		d.rd = d.wr + size_t(count) - d.ring.size() + 1024;
	for (int i = 0; i < count; i++)
		d.ring[(d.wr++) & mask] = samples[i] * (1.0f / 32768.0f);
}

void AudioOut::clear()
{
	Impl &d = *m_impl;
	std::lock_guard<std::mutex> lk(d.mtx);
	d.rd = d.wr;
	d.frac = 0;
}

void AudioOut::set_volume(float v) { m_impl->volume = std::clamp(v, 0.0f, 2.0f); }
void AudioOut::set_latency_ms(int ms) { m_impl->target_s = std::clamp(ms, 20, 400) / 1000.0f; }
float AudioOut::latency_ms() const { return m_impl->fill_ms; }
