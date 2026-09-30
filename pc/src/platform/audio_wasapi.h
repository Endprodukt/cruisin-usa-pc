// WASAPI shared-mode audio output for the DCS PCM stream (mono, variable source rate).
#pragma once

#include <cstdint>
#include <memory>

class AudioOut
{
public:
	AudioOut();
	~AudioOut();

	bool start();                                   // opens the default render device
	void stop();
	void push(const int16_t *samples, int count, double rate);   // called from the emulation thread
	void clear();
	void set_volume(float v);                       // 0..1
	float latency_ms() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
