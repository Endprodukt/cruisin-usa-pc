// Live game state read out of the main CPU's RAM (player car block), for motion / force feedback effects.
// Field offsets come from SYS.EQU (CARBLK); the player's block pointer lives at PLYCBLK, located by its code references.
#pragma once

#include <cstdint>

struct Telemetry
{
	bool valid = false;          // in a race with a player car
	float speed = 0;             // CARSPEED, game units (pix / 16 ms)
	float skid = 0;              // 0..1
	float throttle = 0;          // 0..1
	float brake = 0;             // 0..1
	float turn = 0;              // angle of the front wheels
	float traction = 0;          // traction coefficient (0 = maximum)
	float rpm = 0;               // about 0..50
	float y_vel = 0;             // car body angular momentum about Y (yaw rate)
	float x_mom = 0, z_mom = 0;  // pitch / roll momentum
	float x_lean = 0, z_lean = 0;
	float y_rot = 0, v_rot = 0, d_rot = 0, over_rot = 0;   // body heading, travel direction, spin rate per frame, over-rotation (radians)
	float dist_to_center = 0;    // distance to the road's centre line
	float road_friction = 0, offroad_friction = 0;
	int onroad = 0;              // 0 nothing, 300 road, 310 shoulder, other = off road
	int bump = 0;                // 0 = none, 1..15 intensity
	int spin = 0;                // spin-out flag
	int air_front = 0, air_rear = 0;
	int gear = 0;                // 0 = neutral, 1..4
	float susp_yv[5] = {};       // vertical velocity of centre / RF / LF / LR / RR suspension points
	float susp_dy[5] = {};       // height above the road at those points
	int collided[5] = {};        // road object touched at those points
};

double c3x_to_double(uint32_t w);   // TMS320C3x 32-bit short float
