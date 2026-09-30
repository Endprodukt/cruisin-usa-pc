#include "ffb_modern.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double TWO_PI = 6.283185307179586;
constexpr float TOP_SPEED = 290.0f;          // CARSPEED at full speed (measured)
constexpr int ROAD = 0x300, SHOULDER = 0x310;

float smooth(float cur, float target, float dt, float tau)
{
	float k = 1.0f - std::exp(-dt / tau);
	return cur + (target - cur) * k;
}
}

void FfbModern::add_jolt(float amp, float hz, float decay)
{
	if (amp < 0.01f) return;
	Jolt &j = m_jolts[m_next_jolt];
	m_next_jolt = (m_next_jolt + 1) % 6;
	j.amp = std::min(amp, 1.0f);
	j.hz = hz;
	j.decay = decay;
	j.phase = 0;
}

void FfbModern::frame(const Telemetry &t, float vanilla)
{
	m_vanilla = vanilla;
	if (!t.valid)
	{
		m_have_prev = false;
		m_t = Telemetry{};
		m_speed_n = 0;
		return;
	}
	m_t = t;
	m_speed_n = std::clamp(t.speed / TOP_SPEED, 0.0f, 1.2f);
	const float v = m_speed_n;

	if (m_have_prev)
	{
		const Telemetry &p = m_prev;

		// leaving the road: a tug towards the side the wheel dropped off
		bool was_road = p.onroad == ROAD, now_road = t.onroad == ROAD;
		if (was_road && !now_road && t.onroad != 0)
		{
			float side = t.dist_to_center >= 0 ? 1.0f : -1.0f;
			m_kick = side * m_c.kerb * (0.10f + 0.22f * v);
			m_kick_decay = 9.0f;
		}

		// bump reported by the game
		if (t.bump > p.bump) add_jolt(m_c.bump * (0.08f + 0.04f * std::min(t.bump, 10)) * (0.3f + 0.7f * v), 28.0f, 9.0f);

		// vertical suspension speed spikes (potholes, road seams, jumps)
		float worst = 0;
		for (int i = 1; i < 5; i++) worst = std::max(worst, std::fabs(t.susp_yv[i] - p.susp_yv[i]));
		if (worst > 6.0f && t.air_front == 0 && t.air_rear == 0 && m_bump_cool <= 0)
		{
			add_jolt(m_c.bump * std::min(0.35f, worst / 80.0f) * (0.3f + 0.7f * v), 34.0f, 12.0f);
			m_bump_cool = 0.15f;
		}

		// collision: a big loss of speed in one frame
		float dv = p.speed - t.speed;
		if (dv > 5.0f && (t.bump > 0 || t.spin || dv > 10.0f))
			add_jolt(m_c.collision * std::min(0.7f, 0.2f + dv / 50.0f), 22.0f, 6.0f);

		// spin-out kick
		if (t.spin && !p.spin)
		{
			add_jolt(m_c.spin * 0.8f, 16.0f, 4.0f);
			m_kick = (m_vanilla >= 0 ? 1.0f : -1.0f) * m_c.spin * 0.6f;
			m_kick_decay = 5.0f;
		}

		// touching down
		bool was_air = p.air_front || p.air_rear, now_air = t.air_front || t.air_rear;
		if (was_air && !now_air) add_jolt(m_c.landing * (0.35f + 0.5f * v), 20.0f, 7.0f);
	}
	m_prev = t;
	m_have_prev = true;
	m_bump_cool -= 1.0f / 58.0f;
}

float FfbModern::step(double dt)
{
	const float fdt = float(dt);
	const Telemetry &t = m_t;
	float out = m_vanilla;
	float vib = 0;

	if (t.valid)
	{
		const float v = m_speed_n;

		// continuous targets
		float surf = 0, hz = 20.0f + 70.0f * v;
		if (t.onroad == SHOULDER) surf = 0.05f + 0.07f * v;
		else if (t.onroad != ROAD && t.onroad != 0) surf = 0.12f + 0.16f * v;
		surf *= m_c.surface * (v > 0.03f ? 1.0f : 0.0f);
		m_surface_amp = smooth(m_surface_amp, surf, fdt, 0.06f);
		m_surface_hz = hz;

		float skid = t.skid > 0.25f ? (0.10f + 0.12f * t.skid) * t.skid * m_c.skid : 0.0f;
		if (v < 0.05f) skid = 0;
		m_skid_amp = smooth(m_skid_amp, skid, fdt, 0.05f);

		float eng = (0.012f + 0.05f * std::clamp(t.rpm / 50.0f, 0.0f, 1.0f)) * m_c.engine;
		m_engine_amp = smooth(m_engine_amp, eng, fdt, 0.15f);

		bool air = t.air_front || t.air_rear;
		m_air = smooth(m_air, air ? 1.0f : 0.0f, fdt, air ? 0.08f : 0.03f);
		m_light = smooth(m_light, t.skid > 0.4f ? t.skid : 0.0f, fdt, 0.10f);

		// the game's force, made lighter in the air and when the tyres slide
		float scale = 1.0f - std::min(0.85f, 0.85f * m_air * std::min(1.0f, m_c.air)) - std::min(0.6f, 0.45f * m_light * m_c.understeer);
		out = m_vanilla * std::max(0.0f, scale);

		// oscillating effects
		m_ph_surface += TWO_PI * m_surface_hz * dt;
		m_ph_skid += TWO_PI * 37.0 * dt;
		m_ph_engine += TWO_PI * (22.0 + 1.6 * t.rpm) * dt;
		float s = m_surface_amp * (0.65f * float(std::sin(m_ph_surface)) + 0.35f * float(std::sin(m_ph_surface * 2.31 + 1.0)));
		float k = m_skid_amp * float(std::sin(m_ph_skid));
		float e = m_engine_amp * float(std::sin(m_ph_engine));
		out += m_c.master * (s + k + e);
		vib += m_c.master * (std::fabs(m_surface_amp) + m_skid_amp + m_engine_amp);

		// directional kick
		m_kick *= std::exp(-m_kick_decay * fdt);
		if (std::fabs(m_kick) < 0.002f) m_kick = 0;
		out += m_c.master * std::clamp(m_kick, -0.45f, 0.45f);
		vib += std::fabs(m_kick) * 0.5f;

		// jolts (summed and limited so that several at once do not slam the wheel)
		float jolt_sum = 0;
		for (Jolt &j : m_jolts)
		{
			if (j.amp < 0.005f) { j.amp = 0; continue; }
			j.phase += float(TWO_PI * j.hz * dt);
			jolt_sum += j.amp * std::sin(j.phase);
			vib += m_c.master * j.amp;
			j.amp *= std::exp(-j.decay * fdt);
		}
		out += m_c.master * std::clamp(jolt_sum, -0.6f, 0.6f);
	}
	else
	{
		m_surface_amp = m_skid_amp = m_engine_amp = m_air = m_light = 0;
		m_kick = 0;
		for (Jolt &j : m_jolts) j.amp = 0;
	}

	m_fx = out - m_vanilla;
	m_vib_level = std::clamp(vib, 0.0f, 1.0f);
	return std::clamp(out, -1.0f, 1.0f);
}
