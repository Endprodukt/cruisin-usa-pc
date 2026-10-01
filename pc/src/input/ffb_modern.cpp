#include "ffb_modern.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double TWO_PI = 6.283185307179586;
constexpr float PI_F = 3.14159265f;
constexpr float TOP_SPEED = 290.0f;          // CARSPEED at full speed (measured)
constexpr int ROAD = 0x300, SHOULDER = 0x310;

float smooth(float cur, float target, float dt, float tau)
{
	float k = 1.0f - std::exp(-dt / tau);
	return cur + (target - cur) * k;
}

float wrap(float a)   // to -pi..pi
{
	while (a > PI_F) a -= 2.0f * PI_F;
	while (a < -PI_F) a += 2.0f * PI_F;
	return a;
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

void FfbModern::frame(const Telemetry &t, float arcade, float steer)
{
	m_arcade = arcade;
	m_steer = steer;
	if (!t.valid)
	{
		m_have_prev = false;
		m_t = Telemetry{};
		m_speed_n = 0;
		m_sat_target = 0;
		m_slip = m_prev_slip = 0;
		m_steps = 0;
		return;
	}
	// The game computes a new frame every one or two vblanks (two in the original races, one with the smooth-frames mod). Calls
	// in between see the same state and are skipped; changes per frame are normalised to two vblanks, the rate the thresholds
	// below were tuned at.
	m_steps++;
	m_bump_cool -= 1.0f / 58.0f;
	m_wall_cool -= 1.0f / 58.0f;
	if (m_have_prev && t.speed == m_prev.speed && t.y_rot == m_prev.y_rot && t.v_rot == m_prev.v_rot && t.susp_yv[1] == m_prev.susp_yv[1] &&
	    t.hits_object == m_prev.hits_object && t.hits_light == m_prev.hits_light && t.hits_wall == m_prev.hits_wall && t.spin == m_prev.spin)
		return;
	const float per2 = 2.0f / float(std::clamp(m_steps, 1, 4));
	m_steps = 0;
	m_t = t;
	m_speed_n = std::clamp(t.speed / TOP_SPEED, 0.0f, 1.2f);
	const float v = m_speed_n;

	// ---- front tyre slip angle ----------------------------------------------------------------------------------
	// The game's angles grow to the left. The front wheels point at (heading + wheel angle); the car travels along vrot.
	// The tyres' angle to the travel direction is the slip; the aligning torque tries to reduce it, so it is
	// "slip positive to the right -> push the wheel to the left".
	float slip_left = wrap((t.y_rot + t.turn) - t.v_rot);
	float slip_right = -slip_left;                       // positive = tyres point more right than the car moves
	if (t.speed < 4.0f) slip_right = 0;                  // hardly moving: no meaningful direction of travel
	m_slip = slip_right;

	// peak-and-fall-off: the force grows with slip up to the grip limit, then drops (understeer feels light)
	float grip = 1.0f - std::clamp((t.skid - 0.35f) / 0.65f, 0.0f, 1.0f) * 0.55f * m_c.understeer;
	float peak = 0.22f;                                   // slip (rad) at the force maximum
	float mag = std::tanh(std::fabs(slip_right) / peak);
	float fall = std::fabs(slip_right) > 0.5f ? std::max(0.55f, 1.0f - (std::fabs(slip_right) - 0.5f) * 0.5f) : 1.0f;
	float speed_scale = 0.12f + 0.88f * std::clamp(t.speed / 170.0f, 0.0f, 1.0f);
	float sat_right = -(slip_right >= 0 ? 1.0f : -1.0f) * mag * fall * grip * speed_scale * 0.60f;   // resists the slip
	m_sat_target = sat_right * m_c.aligning;

	// ---- spin-out ---------------------------------------------------------------------------------------------------
	// The game turns the car by d_rot every frame (0.1 rad in a real spin, 0.02 in the straight kick-back after a hard hit).
	// The front wheels are dragged along the direction of travel, so the wheel is thrown against the rotation and stays there
	// until the car stops turning: one held force instead of the aligning torque, which would flip with every half turn.
	const float rot_right = -t.d_rot;
	if (t.spin && std::fabs(rot_right) > 0.004f) m_spin_dir = rot_right > 0 ? -1.0f : 1.0f;
	if (!t.spin) m_spin_dir = 0;
	m_spin_target = m_spin_dir * std::min(0.85f, 0.35f + std::fabs(rot_right) * 4.5f) * m_c.spin;
	if (t.spin) m_sat_target = 0;

	if (m_have_prev)
	{
		const Telemetry &p = m_prev;

		// abrupt change of the slip angle between two frames = something hit the car: a kick that pulls the same way
		// the aligning torque will pull (towards the direction the car now travels relative to the wheels)
		float dslip = wrap(m_slip - m_prev_slip);
		if (std::fabs(dslip) * per2 > 0.06f && t.speed > 15.0f && !t.spin && !p.spin)
		{
			float amp = std::min(0.9f, (std::fabs(dslip) - 0.04f) * 3.0f) * (0.4f + 0.6f * v) * m_c.impact;
			m_impact = (dslip > 0 ? -1.0f : 1.0f) * amp;   // slip grew to the right -> wheel jerked left
			m_impact_decay = 8.0f;
		}

		// leaving the road: a tug towards the side the wheel dropped off
		bool was_road = p.onroad == ROAD, now_road = t.onroad == ROAD;
		if (was_road && !now_road && t.onroad != 0)
		{
			// dist_to_center is positive to the left of the road centre in the game's convention (like its angles)
			float side = t.dist_to_center >= 0 ? -1.0f : 1.0f;
			m_kick = side * m_c.kerb * (0.10f + 0.22f * v);
			m_kick_decay = 9.0f;
		}

		// bump reported by the game
		if (t.bump > p.bump) add_jolt(m_c.bump * (0.08f + 0.04f * std::min(t.bump, 10)) * (0.3f + 0.7f * v), 28.0f, 9.0f);

		// vertical suspension speed spikes (potholes, road seams, jumps)
		float worst = 0;
		for (int i = 1; i < 5; i++) worst = std::max(worst, std::fabs(t.susp_yv[i] - p.susp_yv[i]) * per2);
		if (worst > 6.0f && t.air_front == 0 && t.air_rear == 0 && m_bump_cool <= 0)
		{
			add_jolt(m_c.bump * std::min(0.35f, worst / 80.0f) * (0.3f + 0.7f * v), 34.0f, 12.0f);
			m_bump_cool = 0.15f;
		}

		// objects the car runs into (counted at the game's sound calls): a short push to the side the object was on (they stand at
		// the road's edges: left of the centre line -> the left front wheel hit it) plus one short knock
		const float side = t.dist_to_center >= 0 ? -1.0f : 1.0f;
		const bool hit_obj = t.hits_object != p.hits_object, hit_bush = t.hits_light != p.hits_light;
		const bool hit_wall = t.hits_wall != p.hits_wall && m_wall_cool <= 0;
		if (hit_obj)
		{
			m_thud += side * m_c.bump * (0.30f + 0.25f * v);
			add_jolt(m_c.bump * (0.22f + 0.18f * v), 14.0f, 18.0f);
		}
		if (hit_bush) add_jolt(m_c.bump * (0.08f + 0.10f * v), 18.0f, 16.0f);
		if (hit_wall)
		{
			m_thud += side * m_c.collision * (0.35f + 0.3f * v);
			add_jolt(m_c.collision * (0.25f + 0.2f * v), 12.0f, 14.0f);
			m_wall_cool = 0.3f;
		}

		// collision: a big loss of speed in one frame (one knock, not a rattle). Skipped when one of the hits above explains it.
		float dv = p.speed - t.speed;
		if (dv * per2 > 5.0f && (t.bump > 0 || t.spin || dv * per2 > 10.0f) && !hit_obj && !hit_wall && !hit_bush)
			add_jolt(m_c.collision * std::min(0.6f, 0.2f + dv / 50.0f), 14.0f, 14.0f);

		// spin-out start: a slam in the direction the wheel is about to be held
		if (t.spin && !p.spin && m_spin_dir != 0)
		{
			m_impact = m_spin_dir * 0.35f * m_c.spin;
			m_impact_decay = 10.0f;
		}

		// touching down
		bool was_air = p.air_front || p.air_rear, now_air = t.air_front || t.air_rear;
		if (was_air && !now_air) add_jolt(m_c.landing * (0.35f + 0.5f * v), 20.0f, 7.0f);
	}
	m_prev = t;
	m_prev_slip = m_slip;
	m_have_prev = true;
}

float FfbModern::step(double dt)
{
	const float fdt = float(dt);
	const Telemetry &t = m_t;
	float out = 0;   // in a race the force is synthesised from the car state; the arcade's own force is not mixed in
	float vib = 0;

	if (t.valid)
	{
		const float v = m_speed_n;

		// aligning torque and centring, smoothed a little so that single frames do not click
		m_sat = smooth(m_sat, m_sat_target, fdt, 0.012f);
		float centre = -m_steer * (0.04f + 0.30f * std::clamp(t.speed / 170.0f, 0.0f, 1.0f)) * m_c.centering;
		bool air_all = t.air_front && t.air_rear;
		bool air_front = t.air_front != 0;
		m_air = smooth(m_air, (air_all || air_front) ? 1.0f : 0.0f, fdt, (air_all || air_front) ? 0.08f : 0.03f);
		float airs = 1.0f - std::min(0.9f, m_air * std::min(1.0f, m_c.air));
		out += m_c.master * (m_sat + centre) * airs;

		// directional kick from impacts and spins
		m_impact *= std::exp(-m_impact_decay * fdt);
		if (std::fabs(m_impact) < 0.002f) m_impact = 0;
		out += m_c.master * std::clamp(m_impact, -0.9f, 0.9f);

		// spin-out: fast onto the side, slowly back when the car has stopped turning
		m_spin_force = smooth(m_spin_force, m_spin_target, fdt, std::fabs(m_spin_target) > std::fabs(m_spin_force) ? 0.035f : 0.25f);
		out += m_c.master * m_spin_force;

		// object hits: a push of ~60 ms
		m_thud *= std::exp(-16.0f * fdt);
		if (std::fabs(m_thud) < 0.002f) m_thud = 0;
		out += m_c.master * std::clamp(m_thud, -0.7f, 0.7f);

		// continuous vibration targets
		float surf = 0, hz = 20.0f + 70.0f * v;
		if (t.onroad == SHOULDER) surf = 0.05f + 0.07f * v;
		else if (t.onroad != ROAD && t.onroad != 0) surf = 0.12f + 0.16f * v;
		surf *= m_c.surface * (v > 0.03f ? 1.0f : 0.0f);
		m_surface_amp = smooth(m_surface_amp, surf, fdt, 0.06f);
		m_surface_hz = hz;

		float skid = t.skid > 0.25f ? (0.10f + 0.12f * t.skid) * t.skid * m_c.skid : 0.0f;
		if (v < 0.05f) skid = 0;
		m_skid_amp = smooth(m_skid_amp, skid, fdt, 0.05f);

		float eng = (0.008f + 0.04f * std::clamp(t.rpm / 50.0f, 0.0f, 1.0f)) * m_c.engine;
		m_engine_amp = smooth(m_engine_amp, eng, fdt, 0.15f);

		m_ph_surface += TWO_PI * m_surface_hz * dt;
		m_ph_skid += TWO_PI * 37.0 * dt;
		m_ph_engine += TWO_PI * (22.0 + 1.6 * t.rpm) * dt;
		float s = m_surface_amp * (0.65f * float(std::sin(m_ph_surface)) + 0.35f * float(std::sin(m_ph_surface * 2.31 + 1.0)));
		float k = m_skid_amp * float(std::sin(m_ph_skid));
		float e = m_engine_amp * float(std::sin(m_ph_engine));
		out += m_c.master * (s + k + e) * (t.spin == 1 ? 0.5f : 1.0f);   // a spin is felt as the held throw, not as rattle
		vib += m_c.master * (std::fabs(m_surface_amp) + m_skid_amp + m_engine_amp);

		// kerb tug
		m_kick *= std::exp(-m_kick_decay * fdt);
		if (std::fabs(m_kick) < 0.002f) m_kick = 0;
		out += m_c.master * std::clamp(m_kick, -0.45f, 0.45f);
		vib += std::fabs(m_kick) * 0.5f + std::fabs(m_impact) * 0.5f + std::fabs(m_thud) * 0.5f;

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
		m_sat = m_surface_amp = m_skid_amp = m_engine_amp = m_air = m_light = 0;
		m_kick = m_impact = m_thud = 0;
		m_spin_force = m_spin_target = m_spin_dir = 0;
		for (Jolt &j : m_jolts) j.amp = 0;
		out = m_c.menu * m_arcade;   // menus: the arcade's own force (attract, track select, results)
	}

	m_fx = out - m_arcade;
	m_vib_level = std::clamp(vib, 0.0f, 1.0f);
	return std::clamp(out, -1.0f, 1.0f);
}
