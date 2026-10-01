// license:BSD-3-Clause
// copyright-holders:Aaron Giles
// Freestanding adaptation of MAME's TMS320C3x core for the Cruis'n USA PC port.

#include "tms320c3x.h"

//  CONSTANTS
//**************************************************************************

// indexes into the register file
enum
{
	TMR_R0 = 0,
	TMR_R1,
	TMR_R2,
	TMR_R3,
	TMR_R4,
	TMR_R5,
	TMR_R6,
	TMR_R7,
	TMR_AR0,
	TMR_AR1,
	TMR_AR2,
	TMR_AR3,
	TMR_AR4,
	TMR_AR5,
	TMR_AR6,
	TMR_AR7,
	TMR_DP,
	TMR_IR0,
	TMR_IR1,
	TMR_BK,
	TMR_SP,
	TMR_ST,
	TMR_IE,
	TMR_IF,
	TMR_IOF,
	TMR_RS,
	TMR_RE,
	TMR_RC,
	TMR_R8,     // 3204x only
	TMR_R9,     // 3204x only
	TMR_R10,    // 3204x only
	TMR_R11,    // 3204x only
	TMR_TEMP1,  // used by the interpreter
	TMR_TEMP2,  // used by the interpreter
	TMR_TEMP3   // used by the interpreter
};

// flags
const int CFLAG     = 0x0001;
const int VFLAG     = 0x0002;
const int ZFLAG     = 0x0004;
const int NFLAG     = 0x0008;
const int UFFLAG    = 0x0010;
const int LVFLAG    = 0x0020;
const int LUFFLAG   = 0x0040;
const int OVMFLAG   = 0x0080;
const int RMFLAG    = 0x0100;
//const int CFFLAG    = 0x0400;
//const int CEFLAG    = 0x0800;
//const int CCFLAG    = 0x1000;
const int GIEFLAG   = 0x2000;

#define IREG(rnum)  (m_r[rnum].i32[0])

float tms320c3x_device::tmsreg::as_float() const
{
	int_double id;

	// map 0 to 0
	if (mantissa() == 0 && exponent() == -128)
		return 0;

	// handle positive numbers
	else if (mantissa() >= 0)
	{
		int exp = (exponent() + 127) << 23;
		id.i[0] = exp + (mantissa() >> 8);
	}

	// handle negative numbers
	else
	{
		int exp = (exponent() + 127) << 23;
		int32_t man = -mantissa();
		id.i[0] = 0x80000000 + exp + ((man >> 8) & 0x00ffffff);
	}

	// return the converted float
	return id.f[0];
}


//-------------------------------------------------
//  as_double - interpret the contents of a tmsreg
//  as a DSP-encoded floating-point value, and
//  extract a 64-bit IEEE double from it
//-------------------------------------------------

double tms320c3x_device::tmsreg::as_double() const
{
	int_double id;

	// map 0 to 0
	if (mantissa() == 0 && exponent() == -128)
		return 0;

	// handle positive numbers
	else if (mantissa() >= 0)
	{
		int exp = (exponent() + 1023) << 20;
		id.i[BYTE_XOR_BE(0)] = exp + (mantissa() >> 11);
		id.i[BYTE_XOR_BE(1)] = (mantissa() << 21) & 0xffe00000;
	}

	// handle negative numbers
	else
	{
		int exp = (exponent() + 1023) << 20;
		int32_t man = -mantissa();
		id.i[BYTE_XOR_BE(0)] = 0x80000000 + exp + ((man >> 11) & 0x001fffff);
		id.i[BYTE_XOR_BE(1)] = (man << 21) & 0xffe00000;
	}

	// return the converted double
	return id.d;
}


//-------------------------------------------------
//  from_double - import a 64-bit IEEE double into
//  the DSP's internal floating point format
//-------------------------------------------------

void tms320c3x_device::tmsreg::from_double(double val)
{
	// extract mantissa and exponent from the IEEE input
	int_double id;
	id.d = val;
	int32_t mantissa = ((id.i[BYTE_XOR_BE(0)] & 0x000fffff) << 11) | ((id.i[BYTE_XOR_BE(1)] & 0xffe00000) >> 21);
	int32_t exponent = ((id.i[BYTE_XOR_BE(0)] & 0x7ff00000) >> 20) - 1023;

	// if we're too small, map to 0
	if (exponent < -128)
	{
		set_mantissa(0);
		set_exponent(-128);
	}

	// if we're too large, map to the maximum value
	else if (exponent > 127)
	{
		if ((int32_t)id.i[BYTE_XOR_BE(0)] >= 0)
			set_mantissa(0x7fffffff);
		else
			set_mantissa(0x80000001);
		set_exponent(127);
	}

	// if we're positive, map directly
	else if ((int32_t)id.i[BYTE_XOR_BE(0)] >= 0)
	{
		set_mantissa(mantissa);
		set_exponent(exponent);
	}

	// if we're negative with a non-zero mantissa, remove the leading sign bit
	else if (mantissa != 0)
	{
		set_mantissa(0x80000000 | -mantissa);
		set_exponent(exponent);
	}

	// if we're negative with a zero mantissa, normalize
	else
	{
		set_mantissa(0x80000000);
		set_exponent(exponent - 1);
	}
}

tms320c3x_device::tms320c3x_device(bus_t *bus, uint32_t chiptype, int clock_per_inst)
	: m_bus(bus), m_chip_type(chiptype), m_pc(0), m_bkmask(0), m_primary_bus_control(0),
	  m_irq_state(0), m_delayed(false), m_irq_pending(false), m_is_idling(false), m_icount(0),
	  m_clock_per_inst(clock_per_inst), m_iotemp(0), m_mcbl_mode(false), m_hold_state(false),
	  m_is_lopower(false)
{
	memset(&m_r, 0, sizeof(m_r));
}

void tms320c3x_device::reset()
{
	m_pc = RMEM(0);

	IREG(TMR_IE) = 0;
	IREG(TMR_IF) = 0;
	IREG(TMR_ST) = 0;
	IREG(TMR_IOF) = 0;
	IREG(TMR_IF) |= m_irq_state & 0x0f;
	m_primary_bus_control = 0x000010f8;
	m_delayed = m_irq_pending = m_is_idling = m_is_lopower = false;
}

float tms320c3x_device::fp_to_float(uint32_t floatdata)
{
	tmsreg gen(floatdata << 8, (int32_t)floatdata >> 24);
	return gen.as_float();
}


//-------------------------------------------------
//  fp_to_double - convert a 32-bit value from DSP
//  floating-point format a 64-bit IEEE double
//-------------------------------------------------

double tms320c3x_device::fp_to_double(uint32_t floatdata)
{
	tmsreg gen(floatdata << 8, (int32_t)floatdata >> 24);
	return gen.as_double();
}


//-------------------------------------------------
//  float_to_fp - convert a 32-bit IEEE float to
//  a 32-bit DSP floating-point value
//-------------------------------------------------

uint32_t tms320c3x_device::float_to_fp(float fval)
{
	tmsreg gen(fval);
	return (gen.exponent() << 24) | ((uint32_t)gen.mantissa() >> 8);
}


//-------------------------------------------------
//  double_to_fp - convert a 64-bit IEEE double to
//  a 32-bit DSP floating-point value
//-------------------------------------------------

uint32_t tms320c3x_device::double_to_fp(double dval)
{
	tmsreg gen(dval);
	return (gen.exponent() << 24) | ((uint32_t)gen.mantissa() >> 8);
}

void tms320c3x_device::check_irqs()
{
	uint16_t validints = IREG(TMR_IF) & IREG(TMR_IE) & 0x0fff;
	if (validints == 0 || (IREG(TMR_ST) & GIEFLAG) == 0)
		return;

	int whichtrap = 0;
	for (int i = 0; i < 12; i++)
		if (validints & (1 << i))
		{
			whichtrap = i + 1;
			break;
		}

	m_is_idling = false;
	if (!m_delayed)
	{
		uint16_t intmask = 1 << (whichtrap - 1);

		IREG(TMR_IF) &= ~intmask;
		trap(whichtrap);

		if (m_chip_type == CHIP_TYPE_TMS320C31 || (IREG(TMR_ST) & 0x4000) == 0)
			IREG(TMR_IF) |= m_irq_state & 0x0f;
	}
	else
		m_irq_pending = true;
}

void tms320c3x_device::set_input(int inputnum, int state)
{
	uint16_t intmask = 1 << inputnum;
	if (state == ASSERT_LINE)
	{
		m_irq_state |= intmask;
		IREG(TMR_IF) |= intmask;
	}
	else
		m_irq_state &= ~intmask;

	if (m_chip_type != CHIP_TYPE_TMS320C32 || (IREG(TMR_ST) & 0x4000) == 0)
		IREG(TMR_IF) |= m_irq_state & 0x0f;
}

int tms320c3x_device::run(int cycles)
{
	m_icount = cycles;
	check_irqs();

	if (m_is_idling)
	{
		m_icount = 0;
		return cycles;
	}

	while (m_icount > 0)
	{
		if ((IREG(TMR_ST) & RMFLAG) && m_pc == IREG(TMR_RE) + 1)
		{
			if ((int32_t)--IREG(TMR_RC) >= 0)
				m_pc = IREG(TMR_RS);
			else
			{
				IREG(TMR_ST) &= ~RMFLAG;
				if (m_delayed)
				{
					m_delayed = false;
					if (m_irq_pending)
					{
						m_irq_pending = false;
						check_irqs();
					}
				}
			}
			continue;
		}

		if (hook_filter[m_pc & 255])
		{
			bool hit = false;
			for (uint32_t h : hook_pc) hit |= m_pc == h;
			if (hit && on_hook && on_hook()) continue;
		}

		execute_one();
	}
	return cycles - m_icount;
}

void tms320c3x_device::primary_bus_control_w(uint32_t data, uint32_t mem_mask)
{
	if ((m_primary_bus_control ^ data) & HIZ)
	{
		if (m_primary_bus_control & HOLDST)
			m_primary_bus_control &= ~HOLDST;
		else
			m_primary_bus_control |= HOLDST;
	}
	m_primary_bus_control = (m_primary_bus_control & ~(mem_mask | WMASK)) | (data & mem_mask & WMASK);
}

#include "320c3x_ops.ipp"
