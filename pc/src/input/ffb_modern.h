// "Modern" force feedback: effects synthesised from the game's internal car state (surface, bumps, collisions, skid, air time)
// on top of the force the game computes itself. Pure logic, no device access: the host feeds it one telemetry snapshot per
// emulated frame and samples it at a few hundred Hz so that vibrations stay smooth.
#pragma once

#include "machine/telemetry.h"

struct FfbModernConfig
{
	// all in 0..2 (1 = default strength of that effect)
	float master = 1.0f;
	float surface = 1.0f;     // rumble strips / gravel / grass, scaled by speed
	float kerb = 1.0f;        // sideways tug when a wheel drops off the road
	float bump = 1.0f;        // bumps reported by the game
	float collision = 1.0f;   // sudden loss of speed
	float spin = 1.0f;        // spin-out kick
	float landing = 1.0f;     // touching down after a jump
	float engine = 0.4f;      // engine vibration
	float skid = 1.0f;        // tyre rattle at the limit of grip
	float air = 1.0f;         // the wheel goes light while airborne
	float understeer = 1.0f;  // the wheel lightens when the tyres slide
};

class FfbModern
{
public:
	void configure(const FfbModernConfig &c) { m_c = c; }
	// once per emulated frame. `vanilla` is the game's own force, -1..+1 (positive = steer right)
	void frame(const Telemetry &t, float vanilla);
	// advance by dt seconds and return the total force, -1..+1
	float step(double dt);
	// 0..1 strength of the vibration part (for rumble motors)
	float vibration() const { return m_vib_level; }
	// the last force contributed by the modern effects alone (for display / tests)
	float effects() const { return m_fx; }

private:
	struct Jolt { float amp = 0, hz = 25, phase = 0, decay = 8; };   // decaying oscillation
	void add_jolt(float amp, float hz, float decay);

	FfbModernConfig m_c;
	Telemetry m_t, m_prev;
	bool m_have_prev = false;
	float m_vanilla = 0;
	float m_speed_n = 0;

	// smoothed continuous effects
	float m_surface_amp = 0, m_skid_amp = 0, m_engine_amp = 0, m_air = 0, m_light = 0;
	float m_surface_hz = 20;
	double m_ph_surface = 0, m_ph_skid = 0, m_ph_engine = 0, m_ph_road = 0;
	float m_kick = 0, m_kick_decay = 10;    // directional impulse (kerb tug)
	Jolt m_jolts[6];
	int m_next_jolt = 0;
	float m_bump_cool = 0;
	float m_fx = 0, m_vib_level = 0;
};
