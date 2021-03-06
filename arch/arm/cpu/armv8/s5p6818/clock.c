/*
 * (C) Copyright 2009
 * jung hyun kim, Nexell Co, <jhkim@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#if (0)
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <asm/clkdev.h>
#include <mach/platform.h>
#include <mach/devices.h>
#else
#include <common.h>
#include <command.h>
#include <linux/err.h>
#include <asm/io.h>

#include <platform.h>
#include <mach-api.h>

#define pr_info(m...)	printk(m)

#if 0
#define	pr_debug(m...)	printf(m)
#else
#define	pr_debug(m...)	do { } while (0)
#endif
#endif

/*
 * clock generator macros
 */
#define	_InPLL0_		(0)
#define	_InPLL1_		(1)
#define	_InPLL2_		(2)
#define	_InPLL3_		(3)
#define	_InEXT1_		(4)
#define	_InEXT2_		(5)
#define	_InCLKn_		(7)

#define	INPUT_CLKS		6		/* PLL0, PLL1, PLL2, PLL3, EXT1, EXT2 */

#if defined(CONFIG_NEXELL_DFS_BCLK)
	#if defined(CONFIG_S5P6818_DFS_BCLK_PLL_0)
	#define CONFIG_S5P6818_BCLKFREQ_PLLDEV 	0
	#elif defined(CONFIG_S5P6818_DFS_BCLK_PLL_1)
	#define CONFIG_S5P6818_BCLKFREQ_PLLDEV 	1
	#else
	#define CONFIG_S5P6818_BCLKFREQ_PLLDEV	0
	#endif
#define	DVFS_BCLK_PLL	~(1<<CONFIG_S5P6818_BCLKFREQ_PLLDEV)
#else
#define	DVFS_BCLK_PLL	(-1UL)
#endif

#ifdef  CONFIG_ARM_S5P6818_CPUFREQ
#define	DVFS_CPU_PLL	~(1<<CONFIG_S5P6818_CPUFREQ_PLLDEV)
#define	nxp_cpu_pll_change_frequency(c, l)	do { } while(0);
#else
#define	DVFS_CPU_PLL	(-1UL)
#endif

#define IGNORE_PLLs		(DVFS_CPU_PLL & DVFS_BCLK_PLL)
#define	INPUT_MASK 		(((1<<INPUT_CLKS) - 1) & IGNORE_PLLs)

#define	_PLL0_ 			(1 << _InPLL0_)
#define	_PLL1_ 			(1 << _InPLL1_)
#define	_PLL2_ 			(1 << _InPLL2_)
#define	_PLL3_ 			(1 << _InPLL3_)
#define	_EXTCLK1_ 		(1 << _InEXT1_)
#define	_EXTCLK2_ 		(1 << _InEXT2_)

#define	_PLL_0_1_		(_PLL0_    | _PLL1_)
#define	_PLL_0_2_		(_PLL_0_1_ | _PLL2_)
#define	_PLL_0_3_		(_PLL_0_2_ | _PLL3_)
#define	_CLKOUTn_		(0)

#define	_PCLK_			(1<<16)
#define	_BCLK_			(1<<17)
#define	_GATE_PCLK_		(1<<20)
#define	_GATE_BCLK_		(1<<21)
#define	PLCK_MASK		(_GATE_PCLK_ | _PCLK_)
#define	BLCK_MASK		(_GATE_BCLK_ | _BCLK_)

struct nxp_clk_periph {
	const char	*dev_name;
	int			 dev_id;
	char  		 periph_id;
	void	    *base_addr;
	/* clock config */
	char   		 level;
	unsigned int clk_mask0;
	unsigned int clk_mask1;
	short 		 clk_src0;
	short 		 clk_div0;
	char  		 clk_inv0;
	short 		 clk_src1;
	short 		 clk_div1;
	char  		 clk_inv1;
	long		 clk_ext1;
	long		 clk_ext2;
	spinlock_t	 lock;
};

struct nxp_clk_dev {
	struct clk  clk;
	struct clk *link;
	const char *name;
	struct nxp_clk_periph *peri;
};

struct clk_gen_reg {
	volatile U32 CLKENB;
	volatile U32 CLKGEN[4];
};

#define CLK_PERI_1S(name, devid, id, base, mk) [id] = {	\
	.dev_name = name, .dev_id = devid,.periph_id = id, .level = 1, 	\
	.base_addr = (void*)base, .clk_mask0 = mk, }

#define CLK_PERI_2S(name, devid, id, base, mk, mk2) [id] = {	\
	.dev_name = name, .dev_id = devid, .periph_id = id, .level = 2, 	\
	.base_addr = (void*)base, .clk_mask0 = mk, .clk_mask1 = mk2, }

static const char * clk_core[] = {
	CORECLK_NAME_PLL0, CORECLK_NAME_PLL1, CORECLK_NAME_PLL2, CORECLK_NAME_PLL3,
	CORECLK_NAME_FCLK, CORECLK_NAME_MCLK, CORECLK_NAME_BCLK, CORECLK_NAME_PCLK,
	CORECLK_NAME_HCLK,
};

static struct nxp_clk_periph clk_periphs [] = {
	CLK_PERI_1S(DEV_NAME_TIMER		,  0, CLK_ID_TIMER_0	, PHY_BASEADDR_CLKGEN14, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_TIMER		,  1, CLK_ID_TIMER_1	, PHY_BASEADDR_CLKGEN0 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_TIMER		,  2, CLK_ID_TIMER_2	, PHY_BASEADDR_CLKGEN1 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_TIMER		,  3, CLK_ID_TIMER_3	, PHY_BASEADDR_CLKGEN2 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  0, CLK_ID_UART_0	    , PHY_BASEADDR_CLKGEN22, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  1, CLK_ID_UART_1	    , PHY_BASEADDR_CLKGEN24, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  2, CLK_ID_UART_2	    , PHY_BASEADDR_CLKGEN23, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  3, CLK_ID_UART_3	    , PHY_BASEADDR_CLKGEN25, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  4, CLK_ID_UART_4	    , PHY_BASEADDR_CLKGEN26, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_UART		,  5, CLK_ID_UART_5	    , PHY_BASEADDR_CLKGEN27, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_PWM		,  0, CLK_ID_PWM_0	    , PHY_BASEADDR_CLKGEN13, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_PWM		,  1, CLK_ID_PWM_1	    , PHY_BASEADDR_CLKGEN3 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_PWM		,  2, CLK_ID_PWM_2	    , PHY_BASEADDR_CLKGEN4 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_PWM		,  3, CLK_ID_PWM_3	    , PHY_BASEADDR_CLKGEN5 , (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_I2C		,  0, CLK_ID_I2C_0	    , PHY_BASEADDR_CLKGEN6 , (_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_I2C		,  1, CLK_ID_I2C_1	    , PHY_BASEADDR_CLKGEN7 , (_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_I2C		,  2, CLK_ID_I2C_2	    , PHY_BASEADDR_CLKGEN8 , (_GATE_PCLK_)),
	CLK_PERI_2S(DEV_NAME_I2S		,  0, CLK_ID_I2S_0	    , PHY_BASEADDR_CLKGEN15, (_PLL_0_3_|_EXTCLK1_), (_CLKOUTn_)),
	CLK_PERI_2S(DEV_NAME_I2S		,  1, CLK_ID_I2S_1	    , PHY_BASEADDR_CLKGEN16, (_PLL_0_3_|_EXTCLK1_), (_CLKOUTn_)),
	CLK_PERI_2S(DEV_NAME_I2S		,  2, CLK_ID_I2S_2	    , PHY_BASEADDR_CLKGEN17, (_PLL_0_3_|_EXTCLK1_), (_CLKOUTn_)),
	CLK_PERI_1S(DEV_NAME_SDHC		,  0, CLK_ID_SDHC_0	    , PHY_BASEADDR_CLKGEN18, (_PLL_0_2_|_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_SDHC		,  1, CLK_ID_SDHC_1	    , PHY_BASEADDR_CLKGEN19, (_PLL_0_2_|_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_SDHC		,  2, CLK_ID_SDHC_2	    , PHY_BASEADDR_CLKGEN20, (_PLL_0_2_|_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_SPI		,  0, CLK_ID_SPI_0	    , PHY_BASEADDR_CLKGEN37, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_SPI		,  1, CLK_ID_SPI_1	    , PHY_BASEADDR_CLKGEN38, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_SPI		,  2, CLK_ID_SPI_2		, PHY_BASEADDR_CLKGEN39, (_PLL_0_2_)),
	#if 0
	CLK_PERI_2S(DEV_NAME_USB2HOST   , -1, CLK_ID_USB2HOST   , PHY_BASEADDR_CLKGEN32, (_PLL_0_3_), (_PLL_0_3_|_EXTCLK1_)),
	CLK_PERI_1S(DEV_NAME_VIP		,  0, CLK_ID_VIP_0	    , PHY_BASEADDR_CLKGEN30, (_PLL_0_3_|_EXTCLK1_|_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_VIP		,  1, CLK_ID_VIP_1	    , PHY_BASEADDR_CLKGEN31, (_PLL_0_3_|_EXTCLK1_|_EXTCLK2_|_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_MIPI		, -1, CLK_ID_MIPI		, PHY_BASEADDR_CLKGEN9 , (_PLL_0_2_)),
	CLK_PERI_2S(DEV_NAME_GMAC		, -1, CLK_ID_GMAC		, PHY_BASEADDR_CLKGEN10, (_PLL_0_3_|_EXTCLK1_), (_CLKOUTn_)),
	CLK_PERI_1S(DEV_NAME_SPDIF_TX	, -1, CLK_ID_SPDIF_TX	, PHY_BASEADDR_CLKGEN11, (_PLL_0_2_)),
	CLK_PERI_1S(DEV_NAME_MPEGTSI	, -1, CLK_ID_MPEGTSI	, PHY_BASEADDR_CLKGEN12, (_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_MALI		, -1, CLK_ID_MALI		, PHY_BASEADDR_CLKGEN21, (_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_DIT		, -1, CLK_ID_DIT		, PHY_BASEADDR_CLKGEN28, (_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_PPM		, -1, CLK_ID_PPM		, PHY_BASEADDR_CLKGEN29, (_PLL_0_2_)),
	CLK_PERI_2S(DEV_NAME_USB2HOST	, -1, CLK_ID_USB2HOST	, PHY_BASEADDR_CLKGEN32, (_PLL_0_3_), (_PLL_0_3_|_EXTCLK1_)),
	CLK_PERI_1S(DEV_NAME_CODA		, -1, CLK_ID_CODA		, PHY_BASEADDR_CLKGEN33, (_GATE_PCLK_|_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_CRYPTO		, -1, CLK_ID_CRYPTO	    , PHY_BASEADDR_CLKGEN34, (_GATE_PCLK_)),
	CLK_PERI_1S(DEV_NAME_SCALER		, -1, CLK_ID_SCALER	    , PHY_BASEADDR_CLKGEN35, (_GATE_BCLK_)),
	CLK_PERI_1S(DEV_NAME_PDM		, -1, CLK_ID_PDM		, PHY_BASEADDR_CLKGEN36, (_GATE_PCLK_)),
	#endif
};

#define	CLK_PERI_NUM		((int)ARRAY_SIZE(clk_periphs))
#define	CLK_CORE_NUM		((int)ARRAY_SIZE(clk_core))
#define	CLK_DEVS_NUM		(CLK_CORE_NUM + CLK_PERI_NUM)
#define	MAX_DIVIDER			((1<<8) - 1)	// 256, align 2

static struct nxp_clk_dev	(clk_devices[CLK_DEVS_NUM]);
#define	clk_dev_get(n)		((struct nxp_clk_dev *)&clk_devices[n])
#define	clk_container(p)	(container_of(p, struct nxp_clk_dev, clk))

/*
 * Core frequencys
 */
struct _core_hz_ {
	unsigned long pll[4];					/* PLL */
	unsigned long cpu_fclk, cpu_bclk;						/* cpu */
	unsigned long mem_fclk, mem_dclk, mem_bclk, mem_pclk;	/* ddr */
	unsigned long bus_bclk, bus_pclk;						/* bus */
	unsigned long cci4_bclk, cci4_pclk;						/* cci */
	/* ip */
	unsigned long g3d_bclk;
	unsigned long coda_bclk, coda_pclk;
	unsigned long disp_bclk, disp_pclk;
	unsigned long hdmi_pclk;
};

static struct _core_hz_ core_hz;	/* core clock */
#define	CORE_HZ_SIZE	(sizeof(core_hz)/4)

static unsigned int support_dvfs = 1;

/*
 * CLKGEN HW
 */
static inline void clk_gen_bclk(void *base, int on)
{
	struct clk_gen_reg *reg = base;
	unsigned int val = 0;

	val	 = readl(&reg->CLKENB) & ~(0x3);
	val |= (on ? 3 : 0) & 0x3;	/* always BCLK */

	writel(val, &reg->CLKENB);
}

static inline void clk_gen_pclk(void *base, int on)
{
	struct clk_gen_reg *reg = base;
	unsigned int val = 0;

	if (!on)
		return;

	val	 = readl(&reg->CLKENB) & ~(1 << 3);
	val |= (1 << 3);

	writel(val, &reg->CLKENB);
}

static inline void clk_gen_rate(void *base, int level, int src, int div)
{
	struct clk_gen_reg *reg = base;
	unsigned int val = 0;

	#ifdef CONFIG_NXP_CPUFREQ_PLLDEV
	if (CONFIG_NXP_CPUFREQ_PLLDEV == src)
		printk("*** %s: Fail pll.%d for CPU  DFS ***\n", __func__, src);
	#endif
	#ifdef CONFIG_NXP_BCLKFREQ_PLLDEV
	if (CONFIG_NXP_BCLKFREQ_PLLDEV == src)
		printk("*** %s: Fail pll.%d for BCLK DFS ***\n", __func__, src);
	#endif

	val  = readl(&reg->CLKGEN[level<<1]);

	val &= ~(0x07   << 2);
	val |=  (src    << 2);	/* source */
	val	&= ~(0xFF   << 5);
	val	|=  (div-1) << 5;	/* divider */

	writel(val, &reg->CLKGEN[level<<1]);
}

static inline void clk_gen_inv(void *base, int level, int inv)
{
	struct clk_gen_reg *reg = base;
	unsigned int val = 0;

	val  = readl(&reg->CLKGEN[level<<1]) & ~(1 << 1);
	val	|= (inv<< 1);

	writel(val, &reg->CLKGEN[level<<1]);
}

static inline void clk_gen_enb(void *base, int on)
{
	struct clk_gen_reg *reg = base;
	unsigned int val = 0;

	val	 = readl(&reg->CLKENB) & ~(1 << 2);
	val	|= ((on ? 1 : 0) << 2);

	writel(val, &reg->CLKENB);
}

/*
 *	CORE FREQUENCY
 *
 *	PLL0 [P,M,S]	-------	|	| -----	[DIV0] ---	CPU-G0
 *							| M	| -----	[DIV1] ---	BCLK/PCLK
 *	PLL1 [P,M,S]	-------	|	| -----	[DIV2] ---	DDR
 *							| U	| -----	[DIV3] ---	3D
 *	PLL2 [P,M,S,K]	-------	| 	| -----	[DIV4] ---	CODA
 *							| X	| -----	[DIV5] ---	DISPLAY
 *	PLL3 [P,M,S,K]	-------	|	| -----	[DIV6] ---	HDMI
 *							| 	| -----	[DIV7] ---	CPU-G1
 *							| 	| -----	[DIV8] ---	CCI-400(FASTBUS)
 *
 */
static struct NX_CLKPWR_RegisterSet * const clkpwr =
	(struct NX_CLKPWR_RegisterSet *)IO_ADDRESS(PHY_BASEADDR_CLKPWR_MODULE);

#define	getquotient(v, d)	(v/d)

#define	PLL_DIV_CPUG0	0
#define	PLL_DIV_BUS		1
#define	PLL_DIV_MEM		2
#define	PLL_DIV_G3D		3
#define	PLL_DIV_CODA	4
#define	PLL_DIV_DISP	5
#define	PLL_DIV_HDMI	6
#define	PLL_DIV_CPUG1	7
#define	PLL_DIV_CCI4	8

#define	DVO0		3
#define	DVO1		9
#define	DVO2		15
#define	DVO3		21

static unsigned int pll_get_rate(unsigned int pllN, unsigned int xtal)
{
    unsigned int val, val1, nP, nM, nS, nK;
    unsigned int temp = 0;
    val   = clkpwr->PLLSETREG[pllN];
    val1  = clkpwr->PLLSETREG_SSCG[pllN];
	xtal /= 1000;	/* Unit Khz */

    nP= (val >> 18) & 0x03F;
    nM= (val >>  8) & 0x3FF;
    nS= (val >>  0) & 0x0FF;
    nK= (val1>> 16) & 0xFFFF;

    if ((pllN > 1) && nK) {
        temp = (unsigned int)(getquotient((getquotient((nK * 1000), 65536) * xtal), nP)>>nS);
    }

    temp = (unsigned int)((getquotient((nM * xtal), nP)>>nS)*1000) + temp;
    return temp;
}

static unsigned int pll_get_dvo(int dvo)
{
    return (clkpwr->DVOREG[dvo] & 0x7);
}

static unsigned int pll_get_div(int dvo)
{
    unsigned int val = clkpwr->DVOREG[dvo];
	return  ((((val>>DVO3)&0x3F)+1)<<24)  |
			((((val>>DVO2)&0x3F)+1)<<16) |
			((((val>>DVO1)&0x3F)+1)<<8)  |
			((((val>>DVO0)&0x3F)+1)<<0);
}

#define	PLLN_RATE(n)		(pll_get_rate(n, CFG_SYS_PLLFIN))	/* 0~ 3 */
#define	CPU_FCLK_RATE(n)	(pll_get_rate(pll_get_dvo(n), CFG_SYS_PLLFIN) / ((pll_get_div(n)>> 0)&0x3F))
#define	CPU_BCLK_RATE(n)	(pll_get_rate(pll_get_dvo(n), CFG_SYS_PLLFIN) /		\
				  			((pll_get_div(n)>> 0)&0x3F) / ((pll_get_div(n)>> 8)&0x3F))

#define	MEM_FCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_MEM), CFG_SYS_PLLFIN) / 	\
							((pll_get_div(PLL_DIV_MEM)>> 0)&0x3F) / ((pll_get_div(PLL_DIV_MEM)>> 8)&0x3F))

#define	MEM_DCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_MEM), CFG_SYS_PLLFIN) /		\
				  			((pll_get_div(PLL_DIV_MEM)>> 0)&0x3F))

#define	MEM_BCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_MEM), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_MEM)>> 0)&0x3F) /						\
			  				((pll_get_div(PLL_DIV_MEM)>> 8)&0x3F) /						\
			  				((pll_get_div(PLL_DIV_MEM)>>16)&0x3F))
#define	MEM_PCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_MEM), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_MEM)>> 0)&0x3F) /						\
			  				((pll_get_div(PLL_DIV_MEM)>> 8)&0x3F) /						\
			  				((pll_get_div(PLL_DIV_MEM)>>16)&0x3F) /						\
			  				((pll_get_div(PLL_DIV_MEM)>>24)&0x3F))

#define	BUS_BCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_BUS), CFG_SYS_PLLFIN) /		\
					  		((pll_get_div(PLL_DIV_BUS)>> 0)&0x3F))
#define	BUS_PCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_BUS), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_BUS)>> 0)&0x3F) / ((pll_get_div(PLL_DIV_BUS)>> 8)&0x3F))

#define	G3D_BCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_G3D), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_G3D)>> 0)&0x3F))

#define	MPG_BCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_CODA), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_CODA)>> 0)&0x3F))
#define	MPG_PCLK_RATE()		(pll_get_rate(pll_get_dvo(PLL_DIV_CODA), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_CODA)>> 0)&0x3F)	/						\
			  				((pll_get_div(PLL_DIV_CODA)>> 8)&0x3F))

#define	DISP_BCLK_RATE()	(pll_get_rate(pll_get_dvo(PLL_DIV_DISP), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_DISP)>> 0)&0x3F))
#define	DISP_PCLK_RATE()	(pll_get_rate(pll_get_dvo(PLL_DIV_DISP), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_DISP)>> 0)&0x3F)	/						\
			  				((pll_get_div(PLL_DIV_DISP)>> 8)&0x3F))

#define	HDMI_PCLK_RATE()	(pll_get_rate(pll_get_dvo(PLL_DIV_HDMI), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_HDMI)>> 0)&0x3F))

#define	CCI4_BCLK_RATE()	(pll_get_rate(pll_get_dvo(PLL_DIV_CCI4), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_CCI4)>> 0)&0x3F))
#define	CCI4_PCLK_RATE()	(pll_get_rate(pll_get_dvo(PLL_DIV_CCI4), CFG_SYS_PLLFIN) /		\
			  				((pll_get_div(PLL_DIV_CCI4)>> 0)&0x3F)	/						\
			  				((pll_get_div(PLL_DIV_CCI4)>> 8)&0x3F))

static unsigned long core_update_rate(int type)
{
	unsigned long rate = 0;
	switch (type) {
	case  0: rate = core_hz.pll[0] 	= PLLN_RATE(0);    break;
	case  1: rate = core_hz.pll[1] 	= PLLN_RATE(1);    break;
	case  2: rate = core_hz.pll[2] 	= PLLN_RATE(2);    break;
	case  3: rate = core_hz.pll[3] 	= PLLN_RATE(3);    break;
	case  4: rate = core_hz.cpu_fclk 	= CPU_FCLK_RATE(PLL_DIV_CPUG0);  break;
	case  5: rate = core_hz.mem_fclk  	= MEM_FCLK_RATE();  break;
	case  6: rate = core_hz.bus_bclk 	= BUS_BCLK_RATE();  break;
	case  7: rate = core_hz.bus_pclk  	= BUS_PCLK_RATE();  break;
	case  8: rate = core_hz.cpu_bclk 	= CPU_BCLK_RATE(PLL_DIV_CPUG0);  break;
	case  9: rate = core_hz.mem_dclk  	= MEM_DCLK_RATE();  break;
	case 10: rate = core_hz.mem_bclk  	= MEM_BCLK_RATE();  break;
	case 11: rate = core_hz.mem_pclk  	= MEM_PCLK_RATE();  break;
	case 12: rate = core_hz.g3d_bclk 	= G3D_BCLK_RATE(); 	break;
	case 13: rate = core_hz.coda_bclk 	= MPG_BCLK_RATE(); 	break;
	case 14: rate = core_hz.coda_pclk 	= MPG_PCLK_RATE(); 	break;
	case 15: rate = core_hz.disp_bclk 	= DISP_BCLK_RATE(); break;
	case 16: rate = core_hz.disp_pclk 	= DISP_PCLK_RATE(); break;
	case 17: rate = core_hz.hdmi_pclk 	= HDMI_PCLK_RATE(); break;
	case 18: rate = core_hz.cci4_bclk 	= CCI4_BCLK_RATE(); break;
	case 19: rate = core_hz.cci4_pclk 	= CCI4_PCLK_RATE(); break;
	};
	return rate;
}

static unsigned long core_get_rate(int type)
{
	unsigned long rate = 0;

	switch (type) {
	case  0: rate = core_hz.pll[0];		break;
	case  1: rate = core_hz.pll[1];		break;
	case  2: rate = core_hz.pll[2];		break;
	case  3: rate = core_hz.pll[3];		break;
	case  4: rate = core_hz.cpu_fclk;	break;
	case  5: rate = core_hz.mem_fclk;  	break;
	case  6: rate = core_hz.bus_bclk;  	break;
	case  7: rate = core_hz.bus_pclk;  	break;
	case  8: rate = core_hz.cpu_bclk;  	break;
	case  9: rate = core_hz.mem_dclk;  	break;
	case 10: rate = core_hz.mem_bclk;  	break;
	case 11: rate = core_hz.mem_pclk;  	break;
	case 12: rate = core_hz.g3d_bclk; 	break;
	case 13: rate = core_hz.coda_bclk; 	break;
	case 14: rate = core_hz.coda_pclk; 	break;
	case 15: rate = core_hz.disp_bclk; 	break;
	case 16: rate = core_hz.disp_pclk; 	break;
	case 17: rate = core_hz.hdmi_pclk; 	break;
	case 18: rate = core_hz.cci4_bclk; 	break;
	case 19: rate = core_hz.cci4_pclk; 	break;
	default: printk("unknown core clock type %d ...\n", type);
			break;
	};
	return rate;
}

static long core_set_rate(struct clk *clk, long rate)
{
	return clk->rate;
}

void core_rate_init(void)
{
	int i;
	for (i = 0; CORE_HZ_SIZE > i; i++)
		core_update_rate(i);
}

/*
 * Clock Interfaces
 */
static inline long clk_divide(long rate, long request,
				int align, int *divide)
{
	int div = (rate/request);
	int max = MAX_DIVIDER & ~(align-1);
	int adv = (div & ~(align-1)) + align;

	if (!div) {
		if (divide)
			*divide = 1;
		return rate;
	}

	if (1 != div)
		div &= ~(align-1);

	if (div != adv &&
		abs(request - rate/div) > abs(request - rate/adv))
		div = adv;

	div = (div > max ? max : div);
	if (divide)
		*divide = div;

	return (rate/div);
}

struct clk *clk_get_sys(const char *dev_id, const char *con_id)
{
	/* SMP private timer */
	if (! strcmp(dev_id, "smp_twd"))
		return clk_get(NULL, CORECLK_NAME_HCLK);

	return clk_get(NULL, (char *)dev_id);
}

void clk_put(struct clk *clk)
{
}

struct clk *clk_get(struct device *dev, const char *id)
{
	struct nxp_clk_dev *cdev = clk_dev_get(0);
    struct clk *clk = NULL;
    const char *str = NULL, *c = NULL;
	int i, devid;

	if (dev)
		str = dev_name(dev);

	if (id)
		str = id;

	for (i = 0; CLK_DEVS_NUM > i; i++, cdev++) {
		if (NULL == cdev->name)
			continue;
		if (!strncmp(cdev->name, str, strlen(cdev->name))) {
			c = strrchr((const char*)str, (int)'.');
			if (NULL == c || !cdev->peri)
				break;
	    	devid = simple_strtoul(++c, NULL, 10);
    		if (cdev->peri->dev_id == devid)
	    		break;
		}
	}

	if (CLK_DEVS_NUM > i)
		clk = &cdev->clk;
	else
		clk = &(clk_dev_get(7))->clk;	/* pclk */

	return clk ? clk : ERR_PTR(-ENOENT);
}

long clk_round_rate(struct clk *clk, unsigned long rate)
{
	struct nxp_clk_dev *pll = NULL, *cdev = clk_container(clk);
	struct nxp_clk_periph *peri = cdev->peri;
	unsigned long request = rate, rate_hz = 0, flags;
	unsigned long clock_hz, freq_hz = 0;
	unsigned int mask;
	int level, div[2] = { 0, };
	int i, n, clk2 = 0;
	short s1 = 0, s2 = 0, d1 = 0, d2 = 0;

	if (NULL == peri)
		return core_set_rate(clk, rate);

	level = peri->level;
	mask  = peri->clk_mask0;
	pr_debug("clk: %s.%d reqeust = %ld [input=0x%x]\n",
			peri->dev_name, peri->dev_id, rate, mask);

	if (!(INPUT_MASK & mask)) {
		if (PLCK_MASK & mask)
			return core_get_rate(CORECLK_ID_PCLK);
		else if (BLCK_MASK & mask)
			return core_get_rate(CORECLK_ID_BCLK);
		else
			return clk->rate;
	}

next:
	for (n = 0; INPUT_CLKS > n; n++) {
		if (!(((mask & INPUT_MASK) >> n) & 0x1))
			continue;

		if (_InEXT1_ == n) {
			rate = peri->clk_ext1;
		} else if (_InEXT2_ == n) {
			rate = peri->clk_ext2;
		} else {
			pll  = clk_dev_get(n);
			rate = pll->clk.rate;
		}

		if (! rate)
			continue;

		clock_hz = rate;
		for (i = 0; level > i ; i++)
			rate = clk_divide(rate, request, 2, &div[i]);

		if (rate_hz && (abs(rate-request) > abs(rate_hz-request)))
			continue;

		pr_debug("clk: %s.%d, pll.%d[%u] request[%ld] calc[%ld]\n",
			peri->dev_name, peri->dev_id, n, pll->clk.rate, request, rate);

		if (clk2) {
			s1 = -1, d1 = -1;	/* not use */
			s2 =  n, d2 = div[0];
		} else {
			s1 = n, d1 = div[0];
			s2 = _InCLKn_, d2 = div[1];
		}
		rate_hz = rate;
		freq_hz = clock_hz;
	}

	/* search 2th clock from input */
	if (!clk2 && abs(rate_hz-request) &&
		peri->clk_mask1 & ((1<<INPUT_CLKS) - 1)) {
		clk2 = 1;
		mask = peri->clk_mask1;
		level = 1;
		goto next;
	}

	spin_lock_irqsave(&peri->lock, flags);

	peri->clk_src0 = s1;
	peri->clk_div0 = d1;
	peri->clk_src1 = s2;
	peri->clk_div1 = d2;

	clk->rate = rate_hz;

	spin_unlock_irqrestore(&peri->lock, flags);

	pr_debug("clk: %s.%d, level[%d] src[%d,%d] %ld /(div0: %d * div1: %d) = %ld, %ld diff (%ld)\n",
		peri->dev_name, peri->dev_id, peri->level, peri->clk_src0, peri->clk_src1, freq_hz,
		peri->clk_div0, peri->clk_div1, rate_hz, request, abs(rate_hz-request));

	return clk->rate;
}

unsigned long clk_get_rate(struct clk *clk)
{
	struct nxp_clk_dev *cdev = clk_container(clk);

	if (cdev->link)
		clk = cdev->link;

	return clk->rate;
}

int clk_set_rate(struct clk *clk, unsigned long rate)
{
	struct nxp_clk_dev *cdev = clk_container(clk);
	struct nxp_clk_periph *peri = cdev->peri;
	unsigned long flags;
	int i;

	if (NULL == peri)
		return core_set_rate(clk, rate);

	clk_round_rate(clk, rate);

	spin_lock_irqsave(&peri->lock, flags);

	for (i = 0; peri->level > i ; i++)	{

		int s = (0 == i ? peri->clk_src0: peri->clk_src1);
		int d = (0 == i ? peri->clk_div0: peri->clk_div1);

		if (-1 == s)
			continue;

		clk_gen_rate(peri->base_addr, i, s, d);

		pr_debug("clk: %s.%d (%p) set_rate [%d] src[%d] div[%d]\n",
			peri->dev_name, peri->dev_id, peri->base_addr, i, s, d);
	}

	spin_unlock_irqrestore(&peri->lock, flags);

	return clk->rate;
}

int clk_enable(struct clk *clk)
{
	struct nxp_clk_dev *cdev = clk_container(clk);
	struct nxp_clk_periph *peri = cdev->peri;
	unsigned long flags;
	int i = 0, inv = 0;

	if (! peri)
		return 0;

	spin_lock_irqsave(&peri->lock, flags);
	pr_debug("clk: %s.%d enable (BCLK=%s, PCLK=%s)\n",
		peri->dev_name, peri->dev_id, _GATE_BCLK_ & peri->clk_mask0 ? "ON":"PASS",
		_GATE_PCLK_ & peri->clk_mask0 ? "ON":"PASS");

	if (!(INPUT_MASK & peri->clk_mask0)) {
		/* Gated BCLK/PCLK enable */
		if (_GATE_BCLK_ & peri->clk_mask0)
			clk_gen_bclk(peri->base_addr, 1);

		if (_GATE_PCLK_ & peri->clk_mask0)
			clk_gen_pclk(peri->base_addr, 1);

		spin_unlock_irqrestore(&peri->lock, flags);
		return 0;
	}

	/* invert */
	inv = peri->clk_inv0;
	for (; peri->level > i; i++, inv = peri->clk_inv1)
		clk_gen_inv(peri->base_addr, i, inv);

	/* Gated BCLK/PCLK enable */
	if (_GATE_BCLK_ & peri->clk_mask0)
		clk_gen_bclk(peri->base_addr, 1);

	if (_GATE_PCLK_ & peri->clk_mask0)
		clk_gen_pclk(peri->base_addr, 1);

	/* restore clock rate */
	for (i = 0; peri->level > i ; i++)	{
		int s = (0 == i ? peri->clk_src0: peri->clk_src1);
		int d = (0 == i ? peri->clk_div0: peri->clk_div1);
		if (-1 == s)
			continue;
		clk_gen_rate(peri->base_addr, i, s, d);
	}

	clk_gen_enb(peri->base_addr, 1);

	spin_unlock_irqrestore(&peri->lock, flags);
	return 0;
}

void clk_disable(struct clk *clk)
{
	struct nxp_clk_dev *cdev = clk_container(clk);
	struct nxp_clk_periph *peri = cdev->peri;
	unsigned long flags;

	if (! peri)
		return;

	spin_lock_irqsave(&peri->lock, flags);
	pr_debug("clk: %s.%d disable\n", peri->dev_name, peri->dev_id);

	if (!(INPUT_MASK & peri->clk_mask0)) {
		/* Gated BCLK/PCLK disable */
		if (_GATE_BCLK_ & peri->clk_mask0)
			clk_gen_bclk(peri->base_addr, 0);

		if (_GATE_PCLK_ & peri->clk_mask0)
			clk_gen_pclk(peri->base_addr, 0);

		spin_unlock_irqrestore(&peri->lock, flags);
		return;
	}

	clk_gen_rate(peri->base_addr, 0, 7, 256);	/* for power save */
	clk_gen_enb (peri->base_addr, 0);

	/* Gated BCLK/PCLK disable */
	if (_GATE_BCLK_ & peri->clk_mask0)
		clk_gen_bclk(peri->base_addr, 0);

	if (_GATE_PCLK_ & peri->clk_mask0)
		clk_gen_pclk(peri->base_addr, 0);

	spin_unlock_irqrestore(&peri->lock, flags);
	return;
}

EXPORT_SYMBOL(clk_get_sys);
EXPORT_SYMBOL(clk_round_rate);
EXPORT_SYMBOL(clk_get_rate);
EXPORT_SYMBOL(clk_set_rate);
EXPORT_SYMBOL(clk_put);
EXPORT_SYMBOL(clk_get);
EXPORT_SYMBOL(clk_enable);
EXPORT_SYMBOL(clk_disable);

/*
 * Core clocks APIs
 */
unsigned int nxp_cpu_clock_hz(int type)
{
	return core_get_rate(type);
}

void __init nxp_cpu_clock_init(void)
{
	struct nxp_clk_dev *cdev = clk_devices;
	struct nxp_clk_periph *peri = clk_periphs;
	struct clk *clk = NULL;
	int i = 0;

	core_rate_init();

	for (i = 0; (CLK_CORE_NUM+CLK_PERI_NUM) > i; i++, cdev++) {
		if (CLK_CORE_NUM > i) {
			cdev->name = clk_core[i];
			clk = &cdev->clk;
			clk->rate = core_get_rate(i);
			continue;
		}

		peri = &clk_periphs[i-CLK_CORE_NUM];
		peri->base_addr = IO_ADDRESS(peri->base_addr);
		spin_lock_init(&peri->lock);

		cdev->peri = peri;
		cdev->name = peri->dev_name;

		if (!(INPUT_MASK & peri->clk_mask0)) {
			if (BLCK_MASK & peri->clk_mask0)
				cdev->clk.rate = core_get_rate(CORECLK_ID_BCLK);
			if (PLCK_MASK & peri->clk_mask0)
				cdev->clk.rate = core_get_rate(CORECLK_ID_PCLK);
		}

		/* prevent uart clock disable for low level debug message */
		#ifndef CONFIG_DEBUG_NXP_UART
		if (peri->dev_name) {
			#ifdef CONFIG_BACKLIGHT_PWM
			if (!strcmp(peri->dev_name,DEV_NAME_PWM))
				continue;
			#endif
			clk_gen_enb (peri->base_addr, 0);
			clk_gen_bclk(peri->base_addr, 0);
			clk_gen_pclk(peri->base_addr, 0);
		}
		#endif
	}

	printk("CPU : Clock Generator= %d EA, ", CLK_DEVS_NUM);
#ifdef CONFIG_ARM_NXP_CPUFREQ
	printk("DVFS = %s, PLL.%d\n", support_dvfs?"support":"can't support", CONFIG_NXP_CPUFREQ_PLLDEV);
#else
	printk("DVFS = Off\n");
#endif
}

void nxp_cpu_clock_print(void)
{
	int pll, cpu;

	core_rate_init();

	printk("PLL : [0] = %10lu, [1] = %10lu, [2] = %10lu, [3] = %10lu\n",
		core_hz.pll[0], core_hz.pll[1], core_hz.pll[2], core_hz.pll[3]);

	/* CPU0, 1  : DIV 0, 7 */
	pll = pll_get_dvo(PLL_DIV_CPUG0), cpu = pll, support_dvfs = 1;
	printk("(%d) PLL%d: CPU  FCLK = %10lu, HCLK = %9lu (G0)\n", PLL_DIV_CPUG0,
		pll, core_hz.cpu_fclk, core_hz.cpu_bclk);
	pll = pll_get_dvo(PLL_DIV_CPUG1), cpu = pll, support_dvfs = 1;
	printk("(%d) PLL%d: CPU  FCLK = %10lu, HCLK = %9lu (G1)\n", PLL_DIV_CPUG1,
		pll, (ulong)CPU_FCLK_RATE(PLL_DIV_CPUG1), (ulong)CPU_BCLK_RATE(PLL_DIV_CPUG1));

	/* MEM */
	pll = pll_get_dvo(PLL_DIV_MEM), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: MEM  FCLK = %10lu, DCLK = %9lu, BCLK = %9lu, PCLK = %9lu\n", PLL_DIV_MEM,
		pll, core_hz.mem_fclk, core_hz.mem_dclk, core_hz.mem_bclk, core_hz.mem_pclk);

	/* BUS */
	pll = pll_get_dvo(PLL_DIV_BUS), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: BUS  BCLK = %10lu, PCLK = %9lu\n", PLL_DIV_BUS,
		pll, core_hz.bus_bclk, core_hz.bus_pclk);

	/* CCI */
	pll = pll_get_dvo(PLL_DIV_CCI4), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: CCI4 BCLK = %10lu, PCLK = %9lu\n", PLL_DIV_CCI4, pll, core_hz.cci4_bclk, core_hz.cci4_pclk);

	/* G3D */
	if (pll == cpu) support_dvfs = 0;
	pll = pll_get_dvo(PLL_DIV_G3D), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: G3D  BCLK = %10lu\n", PLL_DIV_G3D, pll, core_hz.g3d_bclk);

	/* MPEG */
	pll = pll_get_dvo(PLL_DIV_CODA), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: CODA BCLK = %10lu, PCLK = %9lu\n", PLL_DIV_CODA,
		pll, core_hz.coda_bclk, core_hz.coda_pclk);

	/* DISPLAY */
	pll = pll_get_dvo(PLL_DIV_DISP), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: DISP BCLK = %10lu, PCLK = %9lu\n", PLL_DIV_DISP,
		pll, core_hz.disp_bclk, core_hz.disp_pclk);

	/* HDMI */
	pll = pll_get_dvo(PLL_DIV_HDMI), support_dvfs = pll == cpu ? 0 : 1;
	printk("(%d) PLL%d: HDMI PCLK = %10lu\n", PLL_DIV_HDMI, pll, core_hz.hdmi_pclk);
}
