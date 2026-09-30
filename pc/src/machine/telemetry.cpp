#include "telemetry.h"
#include "midvunit.h"

#include <cmath>

double c3x_to_double(uint32_t w)
{
	int e = int8_t(w >> 24);
	if (e == -128) return 0.0;
	uint32_t m = w & 0x7fffff;
	bool neg = (w >> 23) & 1;
	double f = double(m) / 8388608.0;
	return std::ldexp(neg ? (-2.0 + f) : (1.0 + f), e);
}

namespace {
constexpr uint32_t ADDR_MODE = 0xC8F5;      // _MODE
constexpr uint32_t ADDR_PLYCBLK = 0xE8A8;   // pointer to the player's car block
enum { CT_PYV = 4, RF_PYV = 10, LF_PYV = 16, LR_PYV = 22, RR_PYV = 28 };
}

bool MidVUnit::read_telemetry(Telemetry &t) const
{
	t = Telemetry{};
	auto word = [&](uint32_t a, uint32_t &out) -> bool {
		if (a < m_ram0.size()) { out = m_ram0[a]; return true; }
		if (a >= 0x400000 && a - 0x400000 < m_ram1.size()) { out = m_ram1[a - 0x400000]; return true; }
		return false;
	};
	uint32_t mode, ptr;
	if (!word(ADDR_MODE, mode) || (mode & 0xf) != 4) return false;       // MGAME
	if (!word(ADDR_PLYCBLK, ptr) || ptr == 0) return false;
	auto f = [&](int i) { uint32_t v = 0; return word(ptr + uint32_t(i), v) ? float(c3x_to_double(v)) : 0.0f; };
	auto n = [&](int i) { uint32_t v = 0; return word(ptr + uint32_t(i), v) ? int32_t(v) : 0; };
	uint32_t probe;
	if (!word(ptr, probe) || !word(ptr + 90, probe)) return false;
	t.valid = true;
	t.speed = f(38); t.skid = f(37); t.throttle = f(36); t.brake = f(43); t.turn = f(33); t.traction = f(34);
	t.rpm = f(57); t.y_vel = f(41); t.x_mom = f(40); t.z_mom = f(42); t.x_lean = f(66); t.z_lean = f(67);
	t.dist_to_center = f(70); t.road_friction = f(68); t.offroad_friction = f(69);
	t.onroad = n(30); t.bump = n(51); t.spin = n(49); t.air_front = n(31); t.air_rear = n(32); t.gear = n(56);
	const int yv[5] = {4, 10, 16, 22, 28};
	for (int i = 0; i < 5; i++)
	{
		t.susp_yv[i] = f(yv[i]);
		t.susp_dy[i] = f(yv[i] - 1);
		t.collided[i] = n(yv[i] + 1);
	}
	return true;
}
