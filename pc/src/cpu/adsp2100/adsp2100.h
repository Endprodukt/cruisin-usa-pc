// license:BSD-3-Clause
// copyright-holders:Aaron Giles
// Freestanding adaptation of MAME's ADSP-21xx core (ADSP-2105 only) for the Cruis'n USA PC port.
#pragma once

#include "../../compat/emu_compat.h"

#ifndef LSB_FIRST
#define LSB_FIRST 1
#endif

using u32 = uint32_t;
const int ADSP2181_IRQ0 = 0, ADSP2181_IRQ1 = 1, ADSP2181_IRQ2 = 2, ADSP2181_SPORT0_RX = 3, ADSP2181_SPORT0_TX = 4, ADSP2181_TIMER = 5, ADSP2181_IRQE = 6, ADSP2181_BDMA = 7, ADSP2181_IRQL1 = 8, ADSP2181_IRQL0 = 9;
const int ADSP2101_IRQ0 = 0;
const int ADSP2101_IRQ1 = 1;
const int ADSP2101_IRQ2 = 2;
const int ADSP2101_SPORT0_RX = 3;
const int ADSP2101_SPORT0_TX = 4;
const int ADSP2101_TIMER = 5;
const int ADSP2105_IRQ0 = 0;
const int ADSP2105_IRQ1 = 1;
const int ADSP2105_IRQ2 = 2;
const int ADSP2105_TIMER = 5;

class adsp21xx_device
{
public:
	enum
	{
		CHIP_TYPE_ADSP2100,
		CHIP_TYPE_ADSP2101,
		CHIP_TYPE_ADSP2104,
		CHIP_TYPE_ADSP2105,
		CHIP_TYPE_ADSP2115,
		CHIP_TYPE_ADSP2181
	};

	// ---- host bus -------------------------------------------------------------------
	// program: 14-bit word address, 24-bit data; data: 14-bit word address, 16-bit data.
	static constexpr int PAGE_SHIFT = 8;
	static constexpr int PAGE_WORDS = 1 << PAGE_SHIFT;
	static constexpr int PAGE_COUNT = 0x4000 >> PAGE_SHIFT;

	struct bus_t
	{
		void *ctx = nullptr;
		uint32_t (*pgm_read)(void *ctx, uint32_t addr) = nullptr;
		void     (*pgm_write)(void *ctx, uint32_t addr, uint32_t data) = nullptr;
		uint16_t (*data_read)(void *ctx, uint32_t addr) = nullptr;
		void     (*data_write)(void *ctx, uint32_t addr, uint16_t data) = nullptr;
		uint32_t *ppage[PAGE_COUNT] = {};    // fast program RAM pages (read+write), or null
		uint16_t *drpage[PAGE_COUNT] = {};   // fast data read pages
		uint16_t *dwpage[PAGE_COUNT] = {};   // fast data write pages
	};

	adsp21xx_device(bus_t *bus, uint32_t chiptype = CHIP_TYPE_ADSP2105);

	void reset();
	int  run(int cycles);                       // returns cycles consumed
	void set_input(int inputnum, int state);
	void eat_cycles(int c) { m_icount -= c; }
	int  icount() const { return m_icount; }
	void load_boot_data(uint8_t *srcdata, uint32_t *dstdata);

	uint32_t get_ibase(int index) const { return m_base[index]; }
	uint32_t ireg(int i) const { return m_i[i]; }
	void     set_ireg(int i, uint32_t v) { m_i[i] = v; }
	int32_t  mreg(int i) const { return m_m[i]; }
	uint32_t lreg(int i) const { return m_l[i]; }
	uint32_t pc() const { return m_pc; }

	std::function<void(int port, uint32_t data)> on_sport_tx;
	std::function<uint32_t(int port)>            on_sport_rx;
	std::function<void(int state)>               on_timer_enable;

protected:
	// helpers
	void create_tables();
	inline void update_mstat();
	inline uint32_t pc_stack_top();
	inline void set_pc_stack_top(uint32_t top);
	inline void pc_stack_push();
	inline void pc_stack_push_val(uint32_t val);
	inline void pc_stack_pop();
	inline uint32_t pc_stack_pop_val();
	inline uint32_t cntr_stack_top();
	inline void cntr_stack_push();
	inline void cntr_stack_pop();
	inline uint32_t loop_stack_top();
	inline void loop_stack_push(uint32_t value);
	inline void loop_stack_pop();
	inline void stat_stack_push();
	inline void stat_stack_pop();
//  inline int condition(int c);
	int slow_condition();
	inline void modify_address(uint32_t ireg, uint32_t mreg);
	inline void data_write_dag1(uint32_t op, int32_t val);
	inline uint32_t data_read_dag1(uint32_t op);
	inline void data_write_dag2(uint32_t op, int32_t val);
	inline uint32_t data_read_dag2(uint32_t op);
	inline void pgm_write_dag2(uint32_t op, int32_t val);
	inline uint32_t pgm_read_dag2(uint32_t op);
	void alu_op_ar(uint32_t op);
	void alu_op_ar_const(uint32_t op);
	void alu_op_af(uint32_t op);
	void alu_op_af_const(uint32_t op);
	void alu_op_none(uint32_t op);
	void mac_op_mr(uint32_t op);
	void mac_op_mr_xop(uint32_t op);
	void mac_op_mf(uint32_t op);
	void mac_op_mf_xop(uint32_t op);
	void shift_op(uint32_t op);
	void shift_op_imm(uint32_t op);

	// memory access
	inline uint16_t data_read(uint32_t addr);
	inline void data_write(uint32_t addr, uint16_t data);
	inline uint16_t io_read(uint32_t addr);
	inline void io_write(uint32_t addr, uint16_t data);
	inline uint32_t program_read(uint32_t addr);
	inline void program_write(uint32_t addr, uint32_t data);
	inline uint32_t opcode_read();

	// register read/write
	inline void update_i(int which);
	inline void update_l(int which);
	inline void update_dmovlay();
	inline void write_reg0(int regnum, int32_t val);
	inline void write_reg1(int regnum, int32_t val);
	inline void write_reg2(int regnum, int32_t val);
	inline void write_reg3(int regnum, int32_t val);
	inline int32_t read_reg0(int regnum);
	inline int32_t read_reg1(int regnum);
	inline int32_t read_reg2(int regnum);
	inline int32_t read_reg3(int regnum);

	// interrupts
	bool generate_irq(int which, int indx);
	void check_irqs();

	// internal state
	static const int PC_STACK_DEPTH     = 16;
	static const int CNTR_STACK_DEPTH   = 4;
	static const int STAT_STACK_DEPTH   = 4;
	static const int LOOP_STACK_DEPTH   = 4;

	// 16-bit registers that can be loaded signed or unsigned
	union adsp_reg16
	{
		uint16_t  u;
		int16_t   s;
	};

	// the SHIFT result register is 32 bits
	union adsp_shift
	{
#ifdef LSB_FIRST
		struct { adsp_reg16 sr0, sr1; } srx;
#else
		struct { adsp_reg16 sr1, sr0; } srx;
#endif
		uint32_t sr;
	};

	// the MAC result register is 40 bits
	union adsp_mac
	{
#ifdef LSB_FIRST
		struct { adsp_reg16 mr0, mr1, mr2, mrzero; } mrx;
		struct { uint32_t mr0, mr1; } mry;
#else
		struct { adsp_reg16 mrzero, mr2, mr1, mr0; } mrx;
		struct { uint32_t mr1, mr0; } mry;
#endif
		uint64_t mr;
	};

	// core registers which are replicated
	struct adsp_core
	{
		// ALU registers
		adsp_reg16  ax0, ax1;
		adsp_reg16  ay0, ay1;
		adsp_reg16  ar;
		adsp_reg16  af;

		// MAC registers
		adsp_reg16  mx0, mx1;
		adsp_reg16  my0, my1;
		adsp_mac    mr;
		adsp_reg16  mf;

		// SHIFT registers
		adsp_reg16  si;
		adsp_reg16  se;
		adsp_reg16  sb;
		adsp_shift  sr;

		// dummy registers
		adsp_reg16  zero;
	};

	// configuration
	uint32_t                        m_chip_type;

	// other CPU registers
	uint32_t            m_pc;
	uint32_t            m_ppc;
	uint32_t            m_loop;
	uint32_t            m_loop_condition;
	uint32_t            m_cntr;

	// status registers
	uint32_t            m_astat;
	uint32_t            m_sstat;
	uint32_t            m_mstat;
	uint32_t            m_mstat_prev;
	uint32_t            m_astat_clear;
	uint32_t            m_idle;

	// live set of core registers
	adsp_core           m_core;

	// memory addressing registers
	uint32_t            m_i[8];
	int32_t             m_m[8];
	uint32_t            m_l[8];
	uint32_t            m_lmask[8];
	uint32_t            m_base[8];
	uint8_t             m_px;
	uint32_t            m_pmovlay; // External Program Space overlay
	uint32_t            m_dmovlay; // External Data Space overlay

	// stacks
	uint32_t            m_loop_stack[LOOP_STACK_DEPTH];
	uint32_t            m_cntr_stack[CNTR_STACK_DEPTH];
	uint32_t            m_pc_stack[PC_STACK_DEPTH];
	uint16_t            m_stat_stack[STAT_STACK_DEPTH][3];
	int32_t             m_pc_sp;
	int32_t             m_cntr_sp;
	int32_t             m_stat_sp;
	int32_t             m_loop_sp;

	// external I/O
	uint8_t             m_flagout;
	uint8_t             m_flagin;
	uint8_t             m_fl0;
	uint8_t             m_fl1;
	uint8_t             m_fl2;
	uint16_t            m_idma_addr;
	uint16_t            m_idma_cache;
	uint8_t             m_idma_offs;

	// interrupt handling
	uint16_t            m_imask;
	uint8_t             m_icntl;
	uint16_t            m_ifc;
	uint8_t             m_irq_state[10];
	uint8_t             m_irq_latch[10];

	// other internal states
	int                 m_icount;
	int                 m_mstat_mask;
	int                 m_imask_mask;

	// register maps
	int16_t *           m_read0_ptr[16];
	uint32_t *          m_read1_ptr[16];
	uint32_t *          m_read2_ptr[16];
	void *              m_alu_xregs[8];
	void *              m_alu_yregs[4];
	void *              m_mac_xregs[8];
	void *              m_mac_yregs[4];
	void *              m_shift_xregs[8];

	// alternate core registers (at end for performance)
	adsp_core           m_alt;


	// tables
	uint8_t               m_condition_table[0x1000];
	uint16_t              m_mask_table[0x4000];
	uint16_t              m_reverse_table[0x4000];



	// flag definitions
	static const int SSFLAG     = 0x80;
	static const int MVFLAG     = 0x40;
	static const int QFLAG      = 0x20;
	static const int SFLAG      = 0x10;
	static const int CFLAG      = 0x08;
	static const int VFLAG      = 0x04;
	static const int NFLAG      = 0x02;
	static const int ZFLAG      = 0x01;

	bus_t *m_bus;
};

using adsp2105_device = adsp21xx_device;
