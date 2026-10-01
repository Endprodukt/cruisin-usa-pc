// "Modern" force feedback: built from the game's own car state (heading, travel direction, front wheel angle, grip, surface,
// bumps, collisions) instead of only echoing the arcade's position servo. Pure logic, no device access: the host feeds it one
// telemetry snapshot per emulated frame and samples it at a few hundred Hz so that vibrations stay smooth.
//
// Sign convention everywhere in this class: force and steering are positive towards "steer right" (the same as the arcade's
// wheel force byte). The game's own angles are positive to the left (a right turn makes yrot decrease), so they are flipped on entry.
#pragma once

#include "machine/telemetry.h"

struct FfbModernConfig
{
	// all in 0..2 (1 = default strength of that effect)
	float master = 1.0f;      // overall strength of all synthesised parts
	float aligning = 1.0f;    // self-aligning torque of the front tyres: counter-steer in slides and spins, resistance in corners
	float centering = 0.35f;  // light speed dependent self-centring on top
	float menu = 1.0f;        // the arcade's own force outside a race (attract, selection screens, results)
	float impact = 1.0f;      // directional kick when the car's heading or direction changes abruptly (collisions)
	float surface = 1.0f;     // off the road (grass, dirt, gravel: all of it is "shoulder" to the game), scaled by speed
	float object = 1.0f;      // running into roadside objects: signs, posts, barrels, bushes, animals
	float standstill = 1.0f;  // resistance of the wheel while the car stands or crawls (tyres scrubbing on the spot)
	float engine_ms_idle = 45.0f, engine_ms_max = 10.0f;   // period of the engine vibration at idle and at full revs
	float kerb = 1.0f;        // sideways tug when a wheel drops off the road
	float bump = 1.0f;        // bumps: road seams, potholes, rails
	float collision = 1.0f;   // other cars hitting the car (from any side), walls, trees, sudden loss of speed
	float spin = 1.0f;        // spin-out: the wheel is thrown to one side and held there while the car rotates
	float landing = 1.0f;     // touching down after a jump
	float engine = 0.25f;     // engine vibration
	float skid = 1.0f;        // tyre rattle at the limit of grip
	float air = 1.0f;         // the wheel goes light while airborne
	float understeer = 1.0f;  // the wheel lightens when the tyres slide
};

class FfbModern
{
public:
	void configure(const FfbModernConfig &c) { m_c = c; }
	// once per emulated frame. `arcade` is the game's own force (-1..+1), `steer` the wheel position (-1 left .. +1 right)
	void frame(const Telemetry &t, float arcade, float steer);
	// advance by dt seconds and return the total force, -1..+1
	float step(double dt);
	// 0..1 strength of the vibration part (for rumble motors)
	float vibration() const { return m_vib_level; }
	// the front tyre slip angle used last (radians, positive = the tyres point more to the right than the car travels)
	float slip() const { return m_slip; }
	float effects() const { return m_fx; }

private:
	struct Jolt { float amp = 0, hz = 25, phase = 0, decay = 8; };   // decaying oscillation
	void add_jolt(float amp, float hz, float decay);

	FfbModernConfig m_c;
	Telemetry m_t, m_prev;
	bool m_have_prev = false;
	int m_steps = 0;   // calls since the game state last changed (vblanks per game frame)
	float m_arcade = 0, m_steer = 0;
	float m_speed_n = 0;

	// physics state, updated per frame and smoothed per sample
	float m_sat_target = 0, m_sat = 0;          // aligning torque
	float m_slip = 0, m_prev_slip = 0;
	float m_impact = 0, m_impact_decay = 12;     // directional kick (decays)
	float m_spin_target = 0, m_spin_force = 0;   // held force during a spin-out
	float m_spin_dir = 0;
	float m_thud = 0;                            // short one-sided push when an object is hit
	float m_wall_cool = 0, m_car_cool = 0;
	float m_wreck_force = 0; double m_wreck_ph = 0;   // somersault: the wheel is torn from side to side
	float m_steer_last = 0, m_steer_vel = 0;     // wheel speed (for the resistance at a standstill)
	bool m_off_road = false;

	// smoothed continuous effects
	float m_surface_amp = 0, m_skid_amp = 0, m_engine_amp = 0, m_air = 0, m_light = 0, m_grip_loss = 0;
	float m_surface_hz = 20;
	double m_ph_surface = 0, m_ph_skid = 0, m_ph_engine = 0;
	float m_kick = 0, m_kick_decay = 10;    // kerb tug
	Jolt m_jolts[6];
	int m_next_jolt = 0;
	float m_bump_cool = 0;
	float m_fx = 0, m_vib_level = 0;
};
