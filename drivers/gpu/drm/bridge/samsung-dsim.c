// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung MIPI DSIM bridge driver.
 *
 * Copyright (C) 2021 Amarula Solutions(India)
 * Copyright (c) 2014 Samsung Electronics Co., Ltd
 * Author: Jagan Teki <jagan@amarulasolutions.com>
 *
 * Based on exynos_drm_dsi from
 * Tomasz Figa <t.figa@samsung.com>
 */

#include <asm/unaligned.h>

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/media-bus-format.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>

#include <video/mipi_display.h>

#include <drm/bridge/samsung-dsim.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>

/* returns true iff both arguments logically differs */
#define NEQV(a, b) (!(a) ^ !(b))

/* DSIM_STATUS */
#define DSIM_STOP_STATE_DAT(x)		(((x) & 0xf) << 0)
#define DSIM_STOP_STATE_CLK		(1 << 8)
#define DSIM_TX_READY_HS_CLK		(1 << 10)
#define DSIM_PLL_STABLE			(1 << 31)

/* DSIM_SWRST */
#define DSIM_FUNCRST			(1 << 16)
#define DSIM_SWRST			(1 << 0)

/* DSIM_TIMEOUT */
#define DSIM_LPDR_TIMEOUT(x)		((x) << 0)
#define DSIM_BTA_TIMEOUT(x)		((x) << 16)

/* DSIM_CLKCTRL */
#define DSIM_ESC_PRESCALER(x)		(((x) & 0xffff) << 0)
#define DSIM_ESC_PRESCALER_MASK		(0xffff << 0)
#define DSIM_LANE_ESC_CLK_EN_CLK	(1 << 19)
#define DSIM_LANE_ESC_CLK_EN_DATA(x)	(((x) & 0xf) << 20)
#define DSIM_LANE_ESC_CLK_EN_DATA_MASK	(0xf << 20)
#define DSIM_BYTE_CLKEN			(1 << 24)
#define DSIM_BYTE_CLK_SRC(x)		(((x) & 0x3) << 25)
#define DSIM_BYTE_CLK_SRC_MASK		(0x3 << 25)
#define DSIM_PLL_BYPASS			(1 << 27)
#define DSIM_ESC_CLKEN			(1 << 28)
#define DSIM_TX_REQUEST_HSCLK		(1 << 31)

/* DSIM_CONFIG */
#define DSIM_LANE_EN_CLK		(1 << 0)
#define DSIM_LANE_EN(x)			(((x) & 0xf) << 1)
#define DSIM_NUM_OF_DATA_LANE(x)	(((x) & 0x3) << 5)
#define DSIM_SUB_PIX_FORMAT(x)		(((x) & 0x7) << 8)
#define DSIM_MAIN_PIX_FORMAT_MASK	(0x7 << 12)
#define DSIM_MAIN_PIX_FORMAT_RGB888	(0x7 << 12)
#define DSIM_MAIN_PIX_FORMAT_RGB666	(0x6 << 12)
#define DSIM_MAIN_PIX_FORMAT_RGB666_P	(0x5 << 12)
#define DSIM_MAIN_PIX_FORMAT_RGB565	(0x4 << 12)
#define DSIM_SUB_VC			(((x) & 0x3) << 16)
#define DSIM_MAIN_VC			(((x) & 0x3) << 18)
#define DSIM_HSA_MODE			(1 << 20)
#define DSIM_HBP_MODE			(1 << 21)
#define DSIM_HFP_MODE			(1 << 22)
#define DSIM_HSE_MODE			(1 << 23)
#define DSIM_AUTO_MODE			(1 << 24)
#define DSIM_VIDEO_MODE			(1 << 25)
#define DSIM_BURST_MODE			(1 << 26)
#define DSIM_SYNC_INFORM		(1 << 27)
#define DSIM_EOT_DISABLE		(1 << 28)
#define DSIM_MFLUSH_VS			(1 << 29)
/* This flag is valid only for exynos3250/3472/5260/5430 */
#define DSIM_CLKLANE_STOP		(1 << 30)

/* DSIM_ESCMODE */
#define DSIM_TX_TRIGGER_RST		(1 << 4)
#define DSIM_TX_LPDT_LP			(1 << 6)
#define DSIM_CMD_LPDT_LP		(1 << 7)
#define DSIM_FORCE_BTA			(1 << 16)
#define DSIM_FORCE_STOP_STATE		(1 << 20)
#define DSIM_STOP_STATE_CNT(x)		(((x) & 0x7ff) << 21)
#define DSIM_STOP_STATE_CNT_MASK	(0x7ff << 21)

/* DSIM_MDRESOL */
#define DSIM_MAIN_STAND_BY		(1 << 31)
#define DSIM_MAIN_VRESOL(x, num_bits)	(((x) & ((1 << (num_bits)) - 1)) << 16)
#define DSIM_MAIN_HRESOL(x, num_bits)	(((x) & ((1 << (num_bits)) - 1)) << 0)

/* DSIM_MVPORCH */
#define DSIM_CMD_ALLOW(x)		((x) << 28)
#define DSIM_STABLE_VFP(x)		((x) << 16)
#define DSIM_MAIN_VBP(x)		((x) << 0)
#define DSIM_CMD_ALLOW_MASK		(0xf << 28)
#define DSIM_STABLE_VFP_MASK		(0x7ff << 16)
#define DSIM_MAIN_VBP_MASK		(0x7ff << 0)

/* DSIM_MHPORCH */
#define DSIM_MAIN_HFP(x)		((x) << 16)
#define DSIM_MAIN_HBP(x)		((x) << 0)
#define DSIM_MAIN_HFP_MASK		((0xffff) << 16)
#define DSIM_MAIN_HBP_MASK		((0xffff) << 0)

/* DSIM_MSYNC */
#define DSIM_MAIN_VSA(x)		((x) << 22)
#define DSIM_MAIN_HSA(x)		((x) << 0)
#define DSIM_MAIN_VSA_MASK		((0x3ff) << 22)
#define DSIM_MAIN_HSA_MASK		((0xffff) << 0)

/* DSIM_SDRESOL */
#define DSIM_SUB_STANDY(x)		((x) << 31)
#define DSIM_SUB_VRESOL(x)		((x) << 16)
#define DSIM_SUB_HRESOL(x)		((x) << 0)
#define DSIM_SUB_STANDY_MASK		((0x1) << 31)
#define DSIM_SUB_VRESOL_MASK		((0x7ff) << 16)
#define DSIM_SUB_HRESOL_MASK		((0x7ff) << 0)

/* DSIM_INTSRC */
#define DSIM_INT_PLL_STABLE		(1 << 31)
#define DSIM_INT_SW_RST_RELEASE		(1 << 30)
#define DSIM_INT_SFR_FIFO_EMPTY		(1 << 29)
#define DSIM_INT_SFR_HDR_FIFO_EMPTY	(1 << 28)
#define DSIM_INT_BTA			(1 << 25)
#define DSIM_INT_FRAME_DONE		(1 << 24)
#define DSIM_INT_RX_TIMEOUT		(1 << 21)
#define DSIM_INT_BTA_TIMEOUT		(1 << 20)
#define DSIM_INT_RX_DONE		(1 << 18)
#define DSIM_INT_RX_TE			(1 << 17)
#define DSIM_INT_RX_ACK			(1 << 16)
#define DSIM_INT_RX_ECC_ERR		(1 << 15)
#define DSIM_INT_RX_CRC_ERR		(1 << 14)

/* DSIM_FIFOCTRL */
#define DSIM_RX_DATA_FULL		(1 << 25)
#define DSIM_RX_DATA_EMPTY		(1 << 24)
#define DSIM_SFR_HEADER_FULL		(1 << 23)
#define DSIM_SFR_HEADER_EMPTY		(1 << 22)
#define DSIM_SFR_PAYLOAD_FULL		(1 << 21)
#define DSIM_SFR_PAYLOAD_EMPTY		(1 << 20)
#define DSIM_I80_HEADER_FULL		(1 << 19)
#define DSIM_I80_HEADER_EMPTY		(1 << 18)
#define DSIM_I80_PAYLOAD_FULL		(1 << 17)
#define DSIM_I80_PAYLOAD_EMPTY		(1 << 16)
#define DSIM_SD_HEADER_FULL		(1 << 15)
#define DSIM_SD_HEADER_EMPTY		(1 << 14)
#define DSIM_SD_PAYLOAD_FULL		(1 << 13)
#define DSIM_SD_PAYLOAD_EMPTY		(1 << 12)
#define DSIM_MD_HEADER_FULL		(1 << 11)
#define DSIM_MD_HEADER_EMPTY		(1 << 10)
#define DSIM_MD_PAYLOAD_FULL		(1 << 9)
#define DSIM_MD_PAYLOAD_EMPTY		(1 << 8)
#define DSIM_RX_FIFO			(1 << 4)
#define DSIM_SFR_FIFO			(1 << 3)
#define DSIM_I80_FIFO			(1 << 2)
#define DSIM_SD_FIFO			(1 << 1)
#define DSIM_MD_FIFO			(1 << 0)

/* DSIM_PHYACCHR */
#define DSIM_AFC_EN			(1 << 14)
#define DSIM_AFC_CTL(x)			(((x) & 0x7) << 5)

/* DSIM_PLLCTRL */
#define DSIM_FREQ_BAND(x)		((x) << 24)
#define DSIM_PLL_EN			(1 << 23)
#define DSIM_PLL_P(x, offset)		((x) << (offset))
#define DSIM_PLL_M(x)			((x) << 4)
#define DSIM_PLL_S(x)			((x) << 1)

/* DSIM_PHYCTRL */
#define DSIM_PHYCTRL_ULPS_EXIT(x)	(((x) & 0x1ff) << 0)
#define DSIM_PHYCTRL_B_DPHYCTL_VREG_LP	(1 << 30)
#define DSIM_PHYCTRL_B_DPHYCTL_SLEW_UP	(1 << 14)

/* DSIM_PHYTIMING */
#define DSIM_PHYTIMING_LPX(x)		((x) << 8)
#define DSIM_PHYTIMING_HS_EXIT(x)	((x) << 0)

/* DSIM_PHYTIMING1 */
#define DSIM_PHYTIMING1_CLK_PREPARE(x)	((x) << 24)
#define DSIM_PHYTIMING1_CLK_ZERO(x)	((x) << 16)
#define DSIM_PHYTIMING1_CLK_POST(x)	((x) << 8)
#define DSIM_PHYTIMING1_CLK_TRAIL(x)	((x) << 0)

/* DSIM_PHYTIMING2 */
#define DSIM_PHYTIMING2_HS_PREPARE(x)	((x) << 16)
#define DSIM_PHYTIMING2_HS_ZERO(x)	((x) << 8)
#define DSIM_PHYTIMING2_HS_TRAIL(x)	((x) << 0)

#define DSI_MAX_BUS_WIDTH		4
#define DSI_NUM_VIRTUAL_CHANNELS	4
#define DSI_TX_FIFO_SIZE		2048
#define DSI_RX_FIFO_SIZE		256
#define DSI_XFER_TIMEOUT_MS		100
#define DSI_RX_FIFO_EMPTY		0x30800002
#define DSI_HSYNC_PKT_OVERHEAD	6

#define OLD_SCLK_MIPI_CLK_NAME		"pll_clk"

static const char *const clk_names[5] = {
	"bus_clk",
	"sclk_mipi",
	"phyclk_mipidphy0_bitclkdiv8",
	"phyclk_mipidphy0_rxclkesc0",
	"sclk_rgb_vclk_to_dsim0"
};

enum samsung_dsim_transfer_type {
	EXYNOS_DSI_TX,
	EXYNOS_DSI_RX,
};

enum reg_idx {
	DSIM_STATUS_REG,	/* Status register */
	DSIM_SWRST_REG,		/* Software reset register */
	DSIM_CLKCTRL_REG,	/* Clock control register */
	DSIM_TIMEOUT_REG,	/* Time out register */
	DSIM_CONFIG_REG,	/* Configuration register */
	DSIM_ESCMODE_REG,	/* Escape mode register */
	DSIM_MDRESOL_REG,
	DSIM_MVPORCH_REG,	/* Main display Vporch register */
	DSIM_MHPORCH_REG,	/* Main display Hporch register */
	DSIM_MSYNC_REG,		/* Main display sync area register */
	DSIM_INTSRC_REG,	/* Interrupt source register */
	DSIM_INTMSK_REG,	/* Interrupt mask register */
	DSIM_PKTHDR_REG,	/* Packet Header FIFO register */
	DSIM_PAYLOAD_REG,	/* Payload FIFO register */
	DSIM_RXFIFO_REG,	/* Read FIFO register */
	DSIM_FIFOCTRL_REG,	/* FIFO status and control register */
	DSIM_PLLCTRL_REG,	/* PLL control register */
	DSIM_PHYCTRL_REG,
	DSIM_PHYTIMING_REG,
	DSIM_PHYTIMING1_REG,
	DSIM_PHYTIMING2_REG,
	NUM_REGS
};

static const unsigned int exynos_reg_ofs[] = {
	[DSIM_STATUS_REG] =  0x00,
	[DSIM_SWRST_REG] =  0x04,
	[DSIM_CLKCTRL_REG] =  0x08,
	[DSIM_TIMEOUT_REG] =  0x0c,
	[DSIM_CONFIG_REG] =  0x10,
	[DSIM_ESCMODE_REG] =  0x14,
	[DSIM_MDRESOL_REG] =  0x18,
	[DSIM_MVPORCH_REG] =  0x1c,
	[DSIM_MHPORCH_REG] =  0x20,
	[DSIM_MSYNC_REG] =  0x24,
	[DSIM_INTSRC_REG] =  0x2c,
	[DSIM_INTMSK_REG] =  0x30,
	[DSIM_PKTHDR_REG] =  0x34,
	[DSIM_PAYLOAD_REG] =  0x38,
	[DSIM_RXFIFO_REG] =  0x3c,
	[DSIM_FIFOCTRL_REG] =  0x44,
	[DSIM_PLLCTRL_REG] =  0x4c,
	[DSIM_PHYCTRL_REG] =  0x5c,
	[DSIM_PHYTIMING_REG] =  0x64,
	[DSIM_PHYTIMING1_REG] =  0x68,
	[DSIM_PHYTIMING2_REG] =  0x6c,
};

static const unsigned int exynos5433_reg_ofs[] = {
	[DSIM_STATUS_REG] = 0x04,
	[DSIM_SWRST_REG] = 0x0C,
	[DSIM_CLKCTRL_REG] = 0x10,
	[DSIM_TIMEOUT_REG] = 0x14,
	[DSIM_CONFIG_REG] = 0x18,
	[DSIM_ESCMODE_REG] = 0x1C,
	[DSIM_MDRESOL_REG] = 0x20,
	[DSIM_MVPORCH_REG] = 0x24,
	[DSIM_MHPORCH_REG] = 0x28,
	[DSIM_MSYNC_REG] = 0x2C,
	[DSIM_INTSRC_REG] = 0x34,
	[DSIM_INTMSK_REG] = 0x38,
	[DSIM_PKTHDR_REG] = 0x3C,
	[DSIM_PAYLOAD_REG] = 0x40,
	[DSIM_RXFIFO_REG] = 0x44,
	[DSIM_FIFOCTRL_REG] = 0x4C,
	[DSIM_PLLCTRL_REG] = 0x94,
	[DSIM_PHYCTRL_REG] = 0xA4,
	[DSIM_PHYTIMING_REG] = 0xB4,
	[DSIM_PHYTIMING1_REG] = 0xB8,
	[DSIM_PHYTIMING2_REG] = 0xBC,
};

enum reg_value_idx {
	RESET_TYPE,
	PLL_TIMER,
	STOP_STATE_CNT,
	PHYCTRL_ULPS_EXIT,
	PHYCTRL_VREG_LP,
	PHYCTRL_SLEW_UP,
	PHYTIMING_LPX,
	PHYTIMING_HS_EXIT,
	PHYTIMING_CLK_PREPARE,
	PHYTIMING_CLK_ZERO,
	PHYTIMING_CLK_POST,
	PHYTIMING_CLK_TRAIL,
	PHYTIMING_HS_PREPARE,
	PHYTIMING_HS_ZERO,
	PHYTIMING_HS_TRAIL
};

static const unsigned int reg_values[] = {
	[RESET_TYPE] = DSIM_SWRST,
	[PLL_TIMER] = 500,
	[STOP_STATE_CNT] = 0xf,
	[PHYCTRL_ULPS_EXIT] = DSIM_PHYCTRL_ULPS_EXIT(0x0af),
	[PHYCTRL_VREG_LP] = 0,
	[PHYCTRL_SLEW_UP] = 0,
	[PHYTIMING_LPX] = DSIM_PHYTIMING_LPX(0x06),
	[PHYTIMING_HS_EXIT] = DSIM_PHYTIMING_HS_EXIT(0x0b),
	[PHYTIMING_CLK_PREPARE] = DSIM_PHYTIMING1_CLK_PREPARE(0x07),
	[PHYTIMING_CLK_ZERO] = DSIM_PHYTIMING1_CLK_ZERO(0x27),
	[PHYTIMING_CLK_POST] = DSIM_PHYTIMING1_CLK_POST(0x0d),
	[PHYTIMING_CLK_TRAIL] = DSIM_PHYTIMING1_CLK_TRAIL(0x08),
	[PHYTIMING_HS_PREPARE] = DSIM_PHYTIMING2_HS_PREPARE(0x09),
	[PHYTIMING_HS_ZERO] = DSIM_PHYTIMING2_HS_ZERO(0x0d),
	[PHYTIMING_HS_TRAIL] = DSIM_PHYTIMING2_HS_TRAIL(0x0b),
};

static const unsigned int exynos5422_reg_values[] = {
	[RESET_TYPE] = DSIM_SWRST,
	[PLL_TIMER] = 500,
	[STOP_STATE_CNT] = 0xf,
	[PHYCTRL_ULPS_EXIT] = DSIM_PHYCTRL_ULPS_EXIT(0xaf),
	[PHYCTRL_VREG_LP] = 0,
	[PHYCTRL_SLEW_UP] = 0,
	[PHYTIMING_LPX] = DSIM_PHYTIMING_LPX(0x08),
	[PHYTIMING_HS_EXIT] = DSIM_PHYTIMING_HS_EXIT(0x0d),
	[PHYTIMING_CLK_PREPARE] = DSIM_PHYTIMING1_CLK_PREPARE(0x09),
	[PHYTIMING_CLK_ZERO] = DSIM_PHYTIMING1_CLK_ZERO(0x30),
	[PHYTIMING_CLK_POST] = DSIM_PHYTIMING1_CLK_POST(0x0e),
	[PHYTIMING_CLK_TRAIL] = DSIM_PHYTIMING1_CLK_TRAIL(0x0a),
	[PHYTIMING_HS_PREPARE] = DSIM_PHYTIMING2_HS_PREPARE(0x0c),
	[PHYTIMING_HS_ZERO] = DSIM_PHYTIMING2_HS_ZERO(0x11),
	[PHYTIMING_HS_TRAIL] = DSIM_PHYTIMING2_HS_TRAIL(0x0d),
};

static const unsigned int exynos5433_reg_values[] = {
	[RESET_TYPE] = DSIM_FUNCRST,
	[PLL_TIMER] = 22200,
	[STOP_STATE_CNT] = 0xa,
	[PHYCTRL_ULPS_EXIT] = DSIM_PHYCTRL_ULPS_EXIT(0x190),
	[PHYCTRL_VREG_LP] = DSIM_PHYCTRL_B_DPHYCTL_VREG_LP,
	[PHYCTRL_SLEW_UP] = DSIM_PHYCTRL_B_DPHYCTL_SLEW_UP,
	[PHYTIMING_LPX] = DSIM_PHYTIMING_LPX(0x07),
	[PHYTIMING_HS_EXIT] = DSIM_PHYTIMING_HS_EXIT(0x0c),
	[PHYTIMING_CLK_PREPARE] = DSIM_PHYTIMING1_CLK_PREPARE(0x09),
	[PHYTIMING_CLK_ZERO] = DSIM_PHYTIMING1_CLK_ZERO(0x2d),
	[PHYTIMING_CLK_POST] = DSIM_PHYTIMING1_CLK_POST(0x0e),
	[PHYTIMING_CLK_TRAIL] = DSIM_PHYTIMING1_CLK_TRAIL(0x09),
	[PHYTIMING_HS_PREPARE] = DSIM_PHYTIMING2_HS_PREPARE(0x0b),
	[PHYTIMING_HS_ZERO] = DSIM_PHYTIMING2_HS_ZERO(0x10),
	[PHYTIMING_HS_TRAIL] = DSIM_PHYTIMING2_HS_TRAIL(0x0c),
};

static const unsigned int imx8mm_dsim_reg_values[] = {
	[RESET_TYPE] = DSIM_SWRST,
	[PLL_TIMER] = 500,
	[STOP_STATE_CNT] = 0xf,
	[PHYCTRL_ULPS_EXIT] = 0,
	[PHYCTRL_VREG_LP] = 0,
	[PHYCTRL_SLEW_UP] = 0,
	[PHYTIMING_LPX] = DSIM_PHYTIMING_LPX(0x06),
	[PHYTIMING_HS_EXIT] = DSIM_PHYTIMING_HS_EXIT(0x0b),
	[PHYTIMING_CLK_PREPARE] = DSIM_PHYTIMING1_CLK_PREPARE(0x07),
	[PHYTIMING_CLK_ZERO] = DSIM_PHYTIMING1_CLK_ZERO(0x26),
	[PHYTIMING_CLK_POST] = DSIM_PHYTIMING1_CLK_POST(0x0d),
	[PHYTIMING_CLK_TRAIL] = DSIM_PHYTIMING1_CLK_TRAIL(0x08),
	[PHYTIMING_HS_PREPARE] = DSIM_PHYTIMING2_HS_PREPARE(0x08),
	[PHYTIMING_HS_ZERO] = DSIM_PHYTIMING2_HS_ZERO(0x0d),
	[PHYTIMING_HS_TRAIL] = DSIM_PHYTIMING2_HS_TRAIL(0x0b),
};

static const struct samsung_dsim_driver_data exynos3_dsi_driver_data = {
	.reg_ofs = exynos_reg_ofs,
	.plltmr_reg = 0x50,
	.has_freqband = 1,
	.has_clklane_stop = 1,
	.num_clks = 2,
	.max_freq = 1000,
	.wait_for_reset = 1,
	.num_bits_resol = 11,
	.pll_p_offset = 13,
	.reg_values = reg_values,
	.quirks = DSIM_QUIRK_PLAT_DATA,
};

static const struct samsung_dsim_driver_data exynos4_dsi_driver_data = {
	.reg_ofs = exynos_reg_ofs,
	.plltmr_reg = 0x50,
	.has_freqband = 1,
	.has_clklane_stop = 1,
	.num_clks = 2,
	.max_freq = 1000,
	.wait_for_reset = 1,
	.num_bits_resol = 11,
	.pll_p_offset = 13,
	.reg_values = reg_values,
	.quirks = DSIM_QUIRK_PLAT_DATA,
};

static const struct samsung_dsim_driver_data exynos5_dsi_driver_data = {
	.reg_ofs = exynos_reg_ofs,
	.plltmr_reg = 0x58,
	.num_clks = 2,
	.max_freq = 1000,
	.wait_for_reset = 1,
	.num_bits_resol = 11,
	.pll_p_offset = 13,
	.reg_values = reg_values,
	.quirks = DSIM_QUIRK_PLAT_DATA,
};

static const struct samsung_dsim_driver_data exynos5433_dsi_driver_data = {
	.reg_ofs = exynos5433_reg_ofs,
	.plltmr_reg = 0xa0,
	.has_clklane_stop = 1,
	.num_clks = 5,
	.max_freq = 1500,
	.wait_for_reset = 0,
	.num_bits_resol = 12,
	.pll_p_offset = 13,
	.reg_values = exynos5433_reg_values,
	.quirks = DSIM_QUIRK_PLAT_DATA,
};

static const struct samsung_dsim_driver_data exynos5422_dsi_driver_data = {
	.reg_ofs = exynos5433_reg_ofs,
	.plltmr_reg = 0xa0,
	.has_clklane_stop = 1,
	.num_clks = 2,
	.max_freq = 1500,
	.wait_for_reset = 1,
	.num_bits_resol = 12,
	.pll_p_offset = 13,
	.reg_values = exynos5422_reg_values,
	.quirks = DSIM_QUIRK_PLAT_DATA,
};

static const struct samsung_dsim_driver_data imx8mm_dsi_driver_data = {
	.reg_ofs = exynos5433_reg_ofs,
	.plltmr_reg = 0xa0,
	.has_clklane_stop = 1,
	.num_clks = 2,
	.max_freq = 2100,
	.wait_for_reset = 0,
	.num_bits_resol = 12,
	.pll_p_offset = 14,
	.reg_values = imx8mm_dsim_reg_values,
	.quirks = DSIM_QUIRK_FIXUP_SYNC_POL,
};

static const struct of_device_id samsung_dsim_of_match[] = {
	{
		.compatible = "samsung,exynos3250-mipi-dsi",
		.data = &exynos3_dsi_driver_data
	},
	{
		.compatible = "samsung,exynos4210-mipi-dsi",
		.data = &exynos4_dsi_driver_data
	},
	{
		.compatible = "samsung,exynos5410-mipi-dsi",
		.data = &exynos5_dsi_driver_data
	},
	{
		.compatible = "samsung,exynos5422-mipi-dsi",
		.data = &exynos5422_dsi_driver_data
	},
	{
		.compatible = "samsung,exynos5433-mipi-dsi",
		.data = &exynos5433_dsi_driver_data
	},
	{
		.compatible = "fsl,imx8mm-mipi-dsim",
		.data = &imx8mm_dsi_driver_data
	},
	{ /* sentinel. */ }
};

static inline struct samsung_dsim *host_to_dsi(struct mipi_dsi_host *h)
{
	return container_of(h, struct samsung_dsim, dsi_host);
}

static inline struct samsung_dsim *bridge_to_dsi(struct drm_bridge *b)
{
	return container_of(b, struct samsung_dsim, bridge);
}

static inline void samsung_dsim_write(struct samsung_dsim *dsi,
				      enum reg_idx idx, u32 val)
{
	writel(val, dsi->reg_base + dsi->driver_data->reg_ofs[idx]);
}

static inline u32 samsung_dsim_read(struct samsung_dsim *dsi, enum reg_idx idx)
{
	return readl(dsi->reg_base + dsi->driver_data->reg_ofs[idx]);
}

static void samsung_dsim_wait_for_reset(struct samsung_dsim *dsi)
{
	if (wait_for_completion_timeout(&dsi->completed, msecs_to_jiffies(300)))
		return;

	dev_err(dsi->dev, "timeout waiting for reset\n");
}

static void samsung_dsim_reset(struct samsung_dsim *dsi)
{
	u32 reset_val = dsi->driver_data->reg_values[RESET_TYPE];

	reinit_completion(&dsi->completed);
	samsung_dsim_write(dsi, DSIM_SWRST_REG, reset_val);
}

#ifndef MHZ
#define MHZ	(1000*1000)
#endif

static unsigned long samsung_dsim_pll_find_pms(struct samsung_dsim *dsi,
					       unsigned long fin,
					       unsigned long fout,
					       u8 *p, u16 *m, u8 *s)
{
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	unsigned long best_freq = 0;
	u32 min_delta = 0xffffffff;
	u8 p_min, p_max;
	u8 _p, best_p;
	u16 _m, best_m;
	u8 _s, best_s;

	p_min = DIV_ROUND_UP(fin, (12 * MHZ));
	p_max = fin / (6 * MHZ);

	for (_p = p_min; _p <= p_max; ++_p) {
		for (_s = 0; _s <= 5; ++_s) {
			u64 tmp;
			u32 delta;

			tmp = (u64)fout * (_p << _s);
			do_div(tmp, fin);
			_m = tmp;
			if (_m < 41 || _m > 125)
				continue;

			tmp = (u64)_m * fin;
			do_div(tmp, _p);
			if (tmp < 500 * MHZ ||
					tmp > driver_data->max_freq * MHZ)
				continue;

			tmp = (u64)_m * fin;
			do_div(tmp, _p << _s);

			delta = abs(fout - tmp);
			if (delta < min_delta) {
				best_p = _p;
				best_m = _m;
				best_s = _s;
				min_delta = delta;
				best_freq = tmp;
			}
		}
	}

	if (best_freq) {
		*p = best_p;
		*m = best_m;
		*s = best_s;
	}

	return best_freq;
}

static unsigned long samsung_dsim_set_pll(struct samsung_dsim *dsi,
					  unsigned long freq)
{
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	unsigned long fin, fout;
	int timeout;
	u8 p, s;
	u16 m;
	u32 reg;

	fin = dsi->pll_clk_rate;
	fout = samsung_dsim_pll_find_pms(dsi, fin, freq, &p, &m, &s);
	if (!fout) {
		dev_err(dsi->dev,
			"failed to find PLL PMS for requested frequency\n");
		return 0;
	}
	dev_dbg(dsi->dev, "PLL freq %lu, (p %d, m %d, s %d)\n", fout, p, m, s);

	writel(driver_data->reg_values[PLL_TIMER],
			dsi->reg_base + driver_data->plltmr_reg);

	reg = DSIM_PLL_EN | DSIM_PLL_P(p, driver_data->pll_p_offset) |
	      DSIM_PLL_M(m) | DSIM_PLL_S(s);

	if (driver_data->has_freqband) {
		static const unsigned long freq_bands[] = {
			100 * MHZ, 120 * MHZ, 160 * MHZ, 200 * MHZ,
			270 * MHZ, 320 * MHZ, 390 * MHZ, 450 * MHZ,
			510 * MHZ, 560 * MHZ, 640 * MHZ, 690 * MHZ,
			770 * MHZ, 870 * MHZ, 950 * MHZ,
		};
		int band;

		for (band = 0; band < ARRAY_SIZE(freq_bands); ++band)
			if (fout < freq_bands[band])
				break;

		dev_dbg(dsi->dev, "band %d\n", band);

		reg |= DSIM_FREQ_BAND(band);
	}

	samsung_dsim_write(dsi, DSIM_PLLCTRL_REG, reg);

	timeout = 1000;
	do {
		if (timeout-- == 0) {
			dev_err(dsi->dev, "PLL failed to stabilize\n");
			return 0;
		}
		reg = samsung_dsim_read(dsi, DSIM_STATUS_REG);
	} while ((reg & DSIM_PLL_STABLE) == 0);

	return fout;
}

static int samsung_dsim_enable_clock(struct samsung_dsim *dsi)
{
	unsigned long hs_clk, byte_clk, esc_clk;
	unsigned long esc_div;
	u32 reg;

	hs_clk = samsung_dsim_set_pll(dsi, dsi->burst_clk_rate);
	if (!hs_clk) {
		dev_err(dsi->dev, "failed to configure DSI PLL\n");
		return -EFAULT;
	}

	byte_clk = hs_clk / 8;
	esc_div = DIV_ROUND_UP(byte_clk, dsi->esc_clk_rate);
	esc_clk = byte_clk / esc_div;

	if (esc_clk > 20 * MHZ) {
		++esc_div;
		esc_clk = byte_clk / esc_div;
	}

	dev_dbg(dsi->dev, "hs_clk = %lu, byte_clk = %lu, esc_clk = %lu\n",
		hs_clk, byte_clk, esc_clk);

	reg = samsung_dsim_read(dsi, DSIM_CLKCTRL_REG);
	reg &= ~(DSIM_ESC_PRESCALER_MASK | DSIM_LANE_ESC_CLK_EN_CLK
			| DSIM_LANE_ESC_CLK_EN_DATA_MASK | DSIM_PLL_BYPASS
			| DSIM_BYTE_CLK_SRC_MASK);
	reg |= DSIM_ESC_CLKEN | DSIM_BYTE_CLKEN
			| DSIM_ESC_PRESCALER(esc_div)
			| DSIM_LANE_ESC_CLK_EN_CLK
			| DSIM_LANE_ESC_CLK_EN_DATA(BIT(dsi->lanes) - 1)
			| DSIM_BYTE_CLK_SRC(0)
			| DSIM_TX_REQUEST_HSCLK;
	samsung_dsim_write(dsi, DSIM_CLKCTRL_REG, reg);

	return 0;
}

static void samsung_dsim_set_phy_ctrl(struct samsung_dsim *dsi)
{
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	const unsigned int *reg_values = driver_data->reg_values;
	u32 reg;

	if (driver_data->has_freqband)
		return;

	/* B D-PHY: D-PHY Master & Slave Analog Block control */
	reg = reg_values[PHYCTRL_ULPS_EXIT] | reg_values[PHYCTRL_VREG_LP] |
		reg_values[PHYCTRL_SLEW_UP];
	samsung_dsim_write(dsi, DSIM_PHYCTRL_REG, reg);

	/*
	 * T LPX: Transmitted length of any Low-Power state period
	 * T HS-EXIT: Time that the transmitter drives LP-11 following a HS
	 *	burst
	 */
	reg = reg_values[PHYTIMING_LPX] | reg_values[PHYTIMING_HS_EXIT];
	samsung_dsim_write(dsi, DSIM_PHYTIMING_REG, reg);

	/*
	 * T CLK-PREPARE: Time that the transmitter drives the Clock Lane LP-00
	 *	Line state immediately before the HS-0 Line state starting the
	 *	HS transmission
	 * T CLK-ZERO: Time that the transmitter drives the HS-0 state prior to
	 *	transmitting the Clock.
	 * T CLK_POST: Time that the transmitter continues to send HS clock
	 *	after the last associated Data Lane has transitioned to LP Mode
	 *	Interval is defined as the period from the end of T HS-TRAIL to
	 *	the beginning of T CLK-TRAIL
	 * T CLK-TRAIL: Time that the transmitter drives the HS-0 state after
	 *	the last payload clock bit of a HS transmission burst
	 */
	reg = reg_values[PHYTIMING_CLK_PREPARE] |
		reg_values[PHYTIMING_CLK_ZERO] |
		reg_values[PHYTIMING_CLK_POST] |
		reg_values[PHYTIMING_CLK_TRAIL];

	samsung_dsim_write(dsi, DSIM_PHYTIMING1_REG, reg);

	/*
	 * T HS-PREPARE: Time that the transmitter drives the Data Lane LP-00
	 *	Line state immediately before the HS-0 Line state starting the
	 *	HS transmission
	 * T HS-ZERO: Time that the transmitter drives the HS-0 state prior to
	 *	transmitting the Sync sequence.
	 * T HS-TRAIL: Time that the transmitter drives the flipped differential
	 *	state after last payload data bit of a HS transmission burst
	 */
	reg = reg_values[PHYTIMING_HS_PREPARE] | reg_values[PHYTIMING_HS_ZERO] |
		reg_values[PHYTIMING_HS_TRAIL];
	samsung_dsim_write(dsi, DSIM_PHYTIMING2_REG, reg);
}

static void samsung_dsim_disable_clock(struct samsung_dsim *dsi)
{
	u32 reg;

	reg = samsung_dsim_read(dsi, DSIM_CLKCTRL_REG);
	reg &= ~(DSIM_LANE_ESC_CLK_EN_CLK | DSIM_LANE_ESC_CLK_EN_DATA_MASK
			| DSIM_ESC_CLKEN | DSIM_BYTE_CLKEN);
	samsung_dsim_write(dsi, DSIM_CLKCTRL_REG, reg);

	reg = samsung_dsim_read(dsi, DSIM_PLLCTRL_REG);
	reg &= ~DSIM_PLL_EN;
	samsung_dsim_write(dsi, DSIM_PLLCTRL_REG, reg);
}

static void samsung_dsim_enable_lane(struct samsung_dsim *dsi, u32 lane)
{
	u32 reg = samsung_dsim_read(dsi, DSIM_CONFIG_REG);

	reg |= (DSIM_NUM_OF_DATA_LANE(dsi->lanes - 1) | DSIM_LANE_EN_CLK |
			DSIM_LANE_EN(lane));
	samsung_dsim_write(dsi, DSIM_CONFIG_REG, reg);
}

static int samsung_dsim_init_link(struct samsung_dsim *dsi)
{
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	int timeout;
	u32 reg;
	u32 lanes_mask;

	/* Initialize FIFO pointers */
	reg = samsung_dsim_read(dsi, DSIM_FIFOCTRL_REG);
	reg &= ~0x1f;
	samsung_dsim_write(dsi, DSIM_FIFOCTRL_REG, reg);

	usleep_range(9000, 11000);

	reg |= 0x1f;
	samsung_dsim_write(dsi, DSIM_FIFOCTRL_REG, reg);
	usleep_range(9000, 11000);

	/* DSI configuration */
	reg = 0;

	/*
	 * The first bit of mode_flags specifies display configuration.
	 * If this bit is set[= MIPI_DSI_MODE_VIDEO], dsi will support video
	 * mode, otherwise it will support command mode.
	 */
	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		reg |= DSIM_VIDEO_MODE;

		/*
		 * The user manual describes that following bits are ignored in
		 * command mode.
		 */
		if (!(dsi->mode_flags & MIPI_DSI_MODE_VSYNC_FLUSH))
			reg |= DSIM_MFLUSH_VS;
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			reg |= DSIM_SYNC_INFORM;
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
			reg |= DSIM_BURST_MODE;
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_AUTO_VERT)
			reg |= DSIM_AUTO_MODE;
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_HSE)
			reg |= DSIM_HSE_MODE;
		if (!(dsi->mode_flags & MIPI_DSI_MODE_VIDEO_NO_HFP))
			reg |= DSIM_HFP_MODE;
		if (!(dsi->mode_flags & MIPI_DSI_MODE_VIDEO_NO_HBP))
			reg |= DSIM_HBP_MODE;
		if (!(dsi->mode_flags & MIPI_DSI_MODE_VIDEO_NO_HSA))
			reg |= DSIM_HSA_MODE;
	}

	if (!(dsi->mode_flags & MIPI_DSI_MODE_NO_EOT_PACKET))
		reg |= DSIM_EOT_DISABLE;

	switch (dsi->format) {
	case MIPI_DSI_FMT_RGB888:
		reg |= DSIM_MAIN_PIX_FORMAT_RGB888;
		break;
	case MIPI_DSI_FMT_RGB666:
		reg |= DSIM_MAIN_PIX_FORMAT_RGB666;
		break;
	case MIPI_DSI_FMT_RGB666_PACKED:
		reg |= DSIM_MAIN_PIX_FORMAT_RGB666_P;
		break;
	case MIPI_DSI_FMT_RGB565:
		reg |= DSIM_MAIN_PIX_FORMAT_RGB565;
		break;
	default:
		dev_err(dsi->dev, "invalid pixel format\n");
		return -EINVAL;
	}

	/*
	 * Use non-continuous clock mode if the periparal wants and
	 * host controller supports
	 *
	 * In non-continous clock mode, host controller will turn off
	 * the HS clock between high-speed transmissions to reduce
	 * power consumption.
	 */
	if (driver_data->has_clklane_stop &&
			dsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS) {
		reg |= DSIM_CLKLANE_STOP;
	}
	samsung_dsim_write(dsi, DSIM_CONFIG_REG, reg);

	lanes_mask = BIT(dsi->lanes) - 1;
	samsung_dsim_enable_lane(dsi, lanes_mask);

	/* Check clock and data lane state are stop state */
	timeout = 100;
	do {
		if (timeout-- == 0) {
			dev_err(dsi->dev, "waiting for bus lanes timed out\n");
			return -EFAULT;
		}

		reg = samsung_dsim_read(dsi, DSIM_STATUS_REG);
		if ((reg & DSIM_STOP_STATE_DAT(lanes_mask))
		    != DSIM_STOP_STATE_DAT(lanes_mask))
			continue;
	} while (!(reg & (DSIM_STOP_STATE_CLK | DSIM_TX_READY_HS_CLK)));

	reg = samsung_dsim_read(dsi, DSIM_ESCMODE_REG);
	reg &= ~DSIM_STOP_STATE_CNT_MASK;
	reg |= DSIM_STOP_STATE_CNT(driver_data->reg_values[STOP_STATE_CNT]);
	samsung_dsim_write(dsi, DSIM_ESCMODE_REG, reg);

	reg = DSIM_BTA_TIMEOUT(0xff) | DSIM_LPDR_TIMEOUT(0xffff);
	samsung_dsim_write(dsi, DSIM_TIMEOUT_REG, reg);

	return 0;
}

static void samsung_dsim_set_display_mode(struct samsung_dsim *dsi)
{
	struct drm_display_mode *m = &dsi->mode;
	unsigned int num_bits_resol = dsi->driver_data->num_bits_resol;
	int bpp;
	u32 reg, hfp, hbp, hsa;

	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		bpp = mipi_dsi_pixel_format_to_bpp(dsi->format) / 8;

		reg = DSIM_CMD_ALLOW(0xf)
			| DSIM_STABLE_VFP(m->vsync_start - m->vdisplay)
			| DSIM_MAIN_VBP(m->vtotal - m->vsync_end);
		samsung_dsim_write(dsi, DSIM_MVPORCH_REG, reg);

		hfp = DIV_ROUND_UP((m->hsync_start - m->hdisplay) * bpp, dsi->lanes);
		hfp -= (hfp > DSI_HSYNC_PKT_OVERHEAD) ? DSI_HSYNC_PKT_OVERHEAD : 0;

		hbp = DIV_ROUND_UP((m->htotal - m->hsync_end) * bpp, dsi->lanes);
		hbp -= (hbp > DSI_HSYNC_PKT_OVERHEAD) ? DSI_HSYNC_PKT_OVERHEAD : 0;

		hsa = DIV_ROUND_UP((m->hsync_end - m->hsync_start) * bpp, dsi->lanes);
		hsa -= (hsa > DSI_HSYNC_PKT_OVERHEAD) ? DSI_HSYNC_PKT_OVERHEAD : 0;

		reg = DSIM_MAIN_HFP(hfp) | DSIM_MAIN_HBP(hbp);
		samsung_dsim_write(dsi, DSIM_MHPORCH_REG, reg);

		reg = DSIM_MAIN_VSA(m->vsync_end - m->vsync_start)
			| DSIM_MAIN_HSA(hsa);
		samsung_dsim_write(dsi, DSIM_MSYNC_REG, reg);
	}
	reg =  DSIM_MAIN_HRESOL(m->hdisplay, num_bits_resol) |
		DSIM_MAIN_VRESOL(m->vdisplay, num_bits_resol);

	samsung_dsim_write(dsi, DSIM_MDRESOL_REG, reg);

	dev_dbg(dsi->dev, "LCD size = %dx%d\n", m->hdisplay, m->vdisplay);
}

static void samsung_dsim_set_display_enable(struct samsung_dsim *dsi, bool enable)
{
	u32 reg;

	reg = samsung_dsim_read(dsi, DSIM_MDRESOL_REG);
	if (enable)
		reg |= DSIM_MAIN_STAND_BY;
	else
		reg &= ~DSIM_MAIN_STAND_BY;
	samsung_dsim_write(dsi, DSIM_MDRESOL_REG, reg);
}

static int samsung_dsim_wait_for_hdr_fifo(struct samsung_dsim *dsi)
{
	int timeout = 2000;

	do {
		u32 reg = samsung_dsim_read(dsi, DSIM_FIFOCTRL_REG);

		if (!(reg & DSIM_SFR_HEADER_FULL))
			return 0;

		if (!cond_resched())
			usleep_range(950, 1050);
	} while (--timeout);

	return -ETIMEDOUT;
}

static void samsung_dsim_set_cmd_lpm(struct samsung_dsim *dsi, bool lpm)
{
	u32 v = samsung_dsim_read(dsi, DSIM_ESCMODE_REG);

	if (lpm)
		v |= DSIM_CMD_LPDT_LP;
	else
		v &= ~DSIM_CMD_LPDT_LP;

	samsung_dsim_write(dsi, DSIM_ESCMODE_REG, v);
}

static void samsung_dsim_force_bta(struct samsung_dsim *dsi)
{
	u32 v = samsung_dsim_read(dsi, DSIM_ESCMODE_REG);

	v |= DSIM_FORCE_BTA;
	samsung_dsim_write(dsi, DSIM_ESCMODE_REG, v);
}

static void samsung_dsim_send_to_fifo(struct samsung_dsim *dsi,
				      struct samsung_dsim_transfer *xfer)
{
	struct device *dev = dsi->dev;
	struct mipi_dsi_packet *pkt = &xfer->packet;
	const u8 *payload = pkt->payload + xfer->tx_done;
	u16 length = pkt->payload_length - xfer->tx_done;
	bool first = !xfer->tx_done;
	u32 reg;

	dev_dbg(dev, "< xfer %pK: tx len %u, done %u, rx len %u, done %u\n",
		xfer, length, xfer->tx_done, xfer->rx_len, xfer->rx_done);

	if (length > DSI_TX_FIFO_SIZE)
		length = DSI_TX_FIFO_SIZE;

	xfer->tx_done += length;

	/* Send payload */
	while (length >= 4) {
		reg = get_unaligned_le32(payload);
		samsung_dsim_write(dsi, DSIM_PAYLOAD_REG, reg);
		payload += 4;
		length -= 4;
	}

	reg = 0;
	switch (length) {
	case 3:
		reg |= payload[2] << 16;
		fallthrough;
	case 2:
		reg |= payload[1] << 8;
		fallthrough;
	case 1:
		reg |= payload[0];
		samsung_dsim_write(dsi, DSIM_PAYLOAD_REG, reg);
		break;
	}

	/* Send packet header */
	if (!first)
		return;

	reg = get_unaligned_le32(pkt->header);
	if (samsung_dsim_wait_for_hdr_fifo(dsi)) {
		dev_err(dev, "waiting for header FIFO timed out\n");
		return;
	}

	if (NEQV(xfer->flags & MIPI_DSI_MSG_USE_LPM,
		 dsi->state & DSIM_STATE_CMD_LPM)) {
		samsung_dsim_set_cmd_lpm(dsi, xfer->flags & MIPI_DSI_MSG_USE_LPM);
		dsi->state ^= DSIM_STATE_CMD_LPM;
	}

	samsung_dsim_write(dsi, DSIM_PKTHDR_REG, reg);

	if (xfer->flags & MIPI_DSI_MSG_REQ_ACK)
		samsung_dsim_force_bta(dsi);
}

static void samsung_dsim_read_from_fifo(struct samsung_dsim *dsi,
					struct samsung_dsim_transfer *xfer)
{
	u8 *payload = xfer->rx_payload + xfer->rx_done;
	bool first = !xfer->rx_done;
	struct device *dev = dsi->dev;
	u16 length;
	u32 reg;

	if (first) {
		reg = samsung_dsim_read(dsi, DSIM_RXFIFO_REG);

		switch (reg & 0x3f) {
		case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
		case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
			if (xfer->rx_len >= 2) {
				payload[1] = reg >> 16;
				++xfer->rx_done;
			}
			fallthrough;
		case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
		case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
			payload[0] = reg >> 8;
			++xfer->rx_done;
			xfer->rx_len = xfer->rx_done;
			xfer->result = 0;
			goto clear_fifo;
		case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
			dev_err(dev, "DSI Error Report: 0x%04x\n", (reg >> 8) & 0xffff);
			xfer->result = 0;
			goto clear_fifo;
		}

		length = (reg >> 8) & 0xffff;
		if (length > xfer->rx_len) {
			dev_err(dev,
				"response too long (%u > %u bytes), stripping\n",
				xfer->rx_len, length);
			length = xfer->rx_len;
		} else if (length < xfer->rx_len)
			xfer->rx_len = length;
	}

	length = xfer->rx_len - xfer->rx_done;
	xfer->rx_done += length;

	/* Receive payload */
	while (length >= 4) {
		reg = samsung_dsim_read(dsi, DSIM_RXFIFO_REG);
		payload[0] = (reg >>  0) & 0xff;
		payload[1] = (reg >>  8) & 0xff;
		payload[2] = (reg >> 16) & 0xff;
		payload[3] = (reg >> 24) & 0xff;
		payload += 4;
		length -= 4;
	}

	if (length) {
		reg = samsung_dsim_read(dsi, DSIM_RXFIFO_REG);
		switch (length) {
		case 3:
			payload[2] = (reg >> 16) & 0xff;
			fallthrough;
		case 2:
			payload[1] = (reg >> 8) & 0xff;
			fallthrough;
		case 1:
			payload[0] = reg & 0xff;
		}
	}

	if (xfer->rx_done == xfer->rx_len)
		xfer->result = 0;

clear_fifo:
	length = DSI_RX_FIFO_SIZE / 4;
	do {
		reg = samsung_dsim_read(dsi, DSIM_RXFIFO_REG);
		if (reg == DSI_RX_FIFO_EMPTY)
			break;
	} while (--length);
}

static void samsung_dsim_transfer_start(struct samsung_dsim *dsi)
{
	unsigned long flags;
	struct samsung_dsim_transfer *xfer;
	bool start = false;

again:
	spin_lock_irqsave(&dsi->transfer_lock, flags);

	if (list_empty(&dsi->transfer_list)) {
		spin_unlock_irqrestore(&dsi->transfer_lock, flags);
		return;
	}

	xfer = list_first_entry(&dsi->transfer_list,
				struct samsung_dsim_transfer, list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);

	if (xfer->packet.payload_length &&
	    xfer->tx_done == xfer->packet.payload_length)
		/* waiting for RX */
		return;

	samsung_dsim_send_to_fifo(dsi, xfer);

	if (xfer->packet.payload_length || xfer->rx_len)
		return;

	xfer->result = 0;
	complete(&xfer->completed);

	spin_lock_irqsave(&dsi->transfer_lock, flags);

	list_del_init(&xfer->list);
	start = !list_empty(&dsi->transfer_list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);

	if (start)
		goto again;
}

static bool samsung_dsim_transfer_finish(struct samsung_dsim *dsi)
{
	struct samsung_dsim_transfer *xfer;
	unsigned long flags;
	bool start = true;

	spin_lock_irqsave(&dsi->transfer_lock, flags);

	if (list_empty(&dsi->transfer_list)) {
		spin_unlock_irqrestore(&dsi->transfer_lock, flags);
		return false;
	}

	xfer = list_first_entry(&dsi->transfer_list,
				struct samsung_dsim_transfer, list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);

	dev_dbg(dsi->dev,
		"> xfer %pK, tx_len %zu, tx_done %u, rx_len %u, rx_done %u\n",
		xfer, xfer->packet.payload_length, xfer->tx_done, xfer->rx_len,
		xfer->rx_done);

	if (xfer->tx_done != xfer->packet.payload_length)
		return true;

	if (xfer->rx_done != xfer->rx_len)
		samsung_dsim_read_from_fifo(dsi, xfer);

	if (xfer->rx_done != xfer->rx_len)
		return true;

	spin_lock_irqsave(&dsi->transfer_lock, flags);

	list_del_init(&xfer->list);
	start = !list_empty(&dsi->transfer_list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);

	if (!xfer->rx_len)
		xfer->result = 0;
	complete(&xfer->completed);

	return start;
}

static void samsung_dsim_remove_transfer(struct samsung_dsim *dsi,
					 struct samsung_dsim_transfer *xfer)
{
	unsigned long flags;
	bool start;

	spin_lock_irqsave(&dsi->transfer_lock, flags);

	if (!list_empty(&dsi->transfer_list) &&
	    xfer == list_first_entry(&dsi->transfer_list,
				     struct samsung_dsim_transfer, list)) {
		list_del_init(&xfer->list);
		start = !list_empty(&dsi->transfer_list);
		spin_unlock_irqrestore(&dsi->transfer_lock, flags);
		if (start)
			samsung_dsim_transfer_start(dsi);
		return;
	}

	list_del_init(&xfer->list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);
}

static int samsung_dsim_transfer(struct samsung_dsim *dsi,
				 struct samsung_dsim_transfer *xfer)
{
	unsigned long flags;
	bool stopped;

	xfer->tx_done = 0;
	xfer->rx_done = 0;
	xfer->result = -ETIMEDOUT;
	init_completion(&xfer->completed);

	spin_lock_irqsave(&dsi->transfer_lock, flags);

	stopped = list_empty(&dsi->transfer_list);
	list_add_tail(&xfer->list, &dsi->transfer_list);

	spin_unlock_irqrestore(&dsi->transfer_lock, flags);

	if (stopped)
		samsung_dsim_transfer_start(dsi);

	wait_for_completion_timeout(&xfer->completed,
				    msecs_to_jiffies(DSI_XFER_TIMEOUT_MS));
	if (xfer->result == -ETIMEDOUT) {
		struct mipi_dsi_packet *pkt = &xfer->packet;

		samsung_dsim_remove_transfer(dsi, xfer);
		dev_err(dsi->dev, "xfer timed out: %*ph %*ph\n", 4, pkt->header,
			(int)pkt->payload_length, pkt->payload);
		return -ETIMEDOUT;
	}

	/* Also covers hardware timeout condition */
	return xfer->result;
}

static irqreturn_t samsung_dsim_irq(int irq, void *dev_id)
{
	struct samsung_dsim *dsi = dev_id;
	u32 status;

	status = samsung_dsim_read(dsi, DSIM_INTSRC_REG);
	if (!status) {
		static unsigned long j;

		if (printk_timed_ratelimit(&j, 500))
			dev_warn(dsi->dev, "spurious interrupt\n");
		return IRQ_HANDLED;
	}
	samsung_dsim_write(dsi, DSIM_INTSRC_REG, status);

	if (status & DSIM_INT_SW_RST_RELEASE) {
		u32 mask = ~(DSIM_INT_RX_DONE | DSIM_INT_SFR_FIFO_EMPTY |
			DSIM_INT_SFR_HDR_FIFO_EMPTY | DSIM_INT_RX_ECC_ERR |
			DSIM_INT_SW_RST_RELEASE);
		samsung_dsim_write(dsi, DSIM_INTMSK_REG, mask);
		complete(&dsi->completed);
		return IRQ_HANDLED;
	}

	if (!(status & (DSIM_INT_RX_DONE | DSIM_INT_SFR_FIFO_EMPTY |
			DSIM_INT_PLL_STABLE)))
		return IRQ_HANDLED;

	if (samsung_dsim_transfer_finish(dsi))
		samsung_dsim_transfer_start(dsi);

	return IRQ_HANDLED;
}

static void samsung_dsim_enable_irq(struct samsung_dsim *dsi)
{
	const struct samsung_dsim_plat_data *pdata = dsi->plat_data;

	enable_irq(dsi->irq);

	if (pdata && pdata->irq_ops && pdata->irq_ops->enable)
		pdata->irq_ops->enable(dsi);
}

static void samsung_dsim_disable_irq(struct samsung_dsim *dsi)
{
	const struct samsung_dsim_plat_data *pdata = dsi->plat_data;

	if (pdata && pdata->irq_ops && pdata->irq_ops->disable)
		pdata->irq_ops->disable(dsi);

	disable_irq(dsi->irq);
}

static int samsung_dsim_init(struct samsung_dsim *dsi)
{
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;

	if (dsi->state & DSIM_STATE_INITIALIZED)
		return 0;

	samsung_dsim_reset(dsi);
	samsung_dsim_enable_irq(dsi);

	if (driver_data->reg_values[RESET_TYPE] == DSIM_FUNCRST)
		samsung_dsim_enable_lane(dsi, BIT(dsi->lanes) - 1);

	samsung_dsim_enable_clock(dsi);
	if (driver_data->wait_for_reset)
		samsung_dsim_wait_for_reset(dsi);
	samsung_dsim_set_phy_ctrl(dsi);
	samsung_dsim_init_link(dsi);

	dsi->state |= DSIM_STATE_INITIALIZED;

	return 0;
}

static void samsung_dsim_atomic_pre_enable(struct drm_bridge *bridge,
					   struct drm_bridge_state *old_bridge_state)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);
	int ret;

	if (dsi->state & DSIM_STATE_ENABLED)
		return;

	ret = pm_runtime_resume_and_get(dsi->dev);
	if (ret < 0) {
		dev_err(dsi->dev, "failed to enable DSI device.\n");
		return;
	}

	dsi->state |= DSIM_STATE_ENABLED;

	ret = samsung_dsim_init(dsi);
	if (ret)
		return;
}

static void samsung_dsim_atomic_enable(struct drm_bridge *bridge,
				       struct drm_bridge_state *old_bridge_state)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);

	samsung_dsim_set_display_mode(dsi);
	samsung_dsim_set_display_enable(dsi, true);

	dsi->state |= DSIM_STATE_VIDOUT_AVAILABLE;
}

static void samsung_dsim_atomic_disable(struct drm_bridge *bridge,
					struct drm_bridge_state *old_bridge_state)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);

	if (!(dsi->state & DSIM_STATE_ENABLED))
		return;

	dsi->state &= ~DSIM_STATE_VIDOUT_AVAILABLE;
}

static void samsung_dsim_atomic_post_disable(struct drm_bridge *bridge,
					     struct drm_bridge_state *old_bridge_state)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);

	samsung_dsim_set_display_enable(dsi, false);

	dsi->state &= ~DSIM_STATE_ENABLED;
	pm_runtime_put_sync(dsi->dev);
}

#define MAX_INPUT_SEL_FORMATS	1

static u32 *
samsung_dsim_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
				       struct drm_bridge_state *bridge_state,
				       struct drm_crtc_state *crtc_state,
				       struct drm_connector_state *conn_state,
				       u32 output_fmt,
				       unsigned int *num_input_fmts)
{
	u32 *input_fmts;

	*num_input_fmts = 0;

	input_fmts = kcalloc(MAX_INPUT_SEL_FORMATS, sizeof(*input_fmts),
			     GFP_KERNEL);
	if (!input_fmts)
		return NULL;

	/* This is the DSI-end bus format */
	input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_input_fmts = 1;

	return input_fmts;
}

static int samsung_dsim_atomic_check(struct drm_bridge *bridge,
				     struct drm_bridge_state *bridge_state,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;

	if (dsi->driver_data->quirks & DSIM_QUIRK_FIXUP_SYNC_POL) {
		/* At least LCDIF + DSIM needs active low sync */
		adjusted_mode->flags |= (DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC);
		adjusted_mode->flags &= ~(DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC);
	}

	return 0;
}

static void samsung_dsim_mode_set(struct drm_bridge *bridge,
				  const struct drm_display_mode *mode,
				  const struct drm_display_mode *adjusted_mode)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);

	drm_mode_copy(&dsi->mode, adjusted_mode);
}

static int samsung_dsim_attach(struct drm_bridge *bridge,
			       enum drm_bridge_attach_flags flags)
{
	struct samsung_dsim *dsi = bridge_to_dsi(bridge);

	return drm_bridge_attach(bridge->encoder, dsi->out_bridge, bridge,
				 flags);
}

static const struct drm_bridge_funcs samsung_dsim_bridge_funcs = {
	.atomic_duplicate_state		= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state		= drm_atomic_helper_bridge_destroy_state,
	.atomic_reset			= drm_atomic_helper_bridge_reset,
	.atomic_get_input_bus_fmts	= samsung_dsim_atomic_get_input_bus_fmts,
	.atomic_check			= samsung_dsim_atomic_check,
	.atomic_pre_enable		= samsung_dsim_atomic_pre_enable,
	.atomic_enable			= samsung_dsim_atomic_enable,
	.atomic_disable			= samsung_dsim_atomic_disable,
	.atomic_post_disable		= samsung_dsim_atomic_post_disable,
	.mode_set			= samsung_dsim_mode_set,
	.attach				= samsung_dsim_attach,
};

MODULE_DEVICE_TABLE(of, samsung_dsim_of_match);

static int samsung_dsim_host_attach(struct mipi_dsi_host *host,
				    struct mipi_dsi_device *device)
{
	struct samsung_dsim *dsi = host_to_dsi(host);
	const struct samsung_dsim_plat_data *pdata = dsi->plat_data;
	struct device *dev = dsi->dev;
	struct device_node *np = dev->of_node;
	struct device_node *remote;
	struct drm_panel *panel;
	int ret;

	/**
	 * Devices can also be child nodes when we also control that device
	 * through the upstream device (ie, MIPI-DCS for a MIPI-DSI device).
	 *
	 * Lookup for a child node of the given parent that isn't either port
	 * or ports.
	 */
	for_each_available_child_of_node(np, remote) {
		if (of_node_name_eq(remote, "port") ||
		    of_node_name_eq(remote, "ports"))
			continue;

		goto of_find_panel_or_bridge;
	}

	/*
	 * of_graph_get_remote_node() produces a noisy error message if port
	 * node isn't found and the absence of the port is a legit case here,
	 * so at first we silently check whether graph presents in the
	 * device-tree node.
	 */
	if (!of_graph_is_present(np))
		return -ENODEV;

	remote = of_graph_get_remote_node(np, 1, 0);

of_find_panel_or_bridge:
	if (!remote)
		return -ENODEV;

	panel = of_drm_find_panel(remote);
	if (!IS_ERR(panel)) {
		dsi->out_bridge = devm_drm_panel_bridge_add(dev, panel);
	} else {
		dsi->out_bridge = of_drm_find_bridge(remote);
		if (!dsi->out_bridge)
			dsi->out_bridge = ERR_PTR(-EINVAL);
	}

	of_node_put(remote);

	if (IS_ERR(dsi->out_bridge)) {
		ret = PTR_ERR(dsi->out_bridge);
		DRM_DEV_ERROR(dev, "failed to find the bridge: %d\n", ret);
		return ret;
	}

	DRM_DEV_INFO(dev, "Attached %s device\n", device->name);

	drm_bridge_add(&dsi->bridge);

	if (pdata && pdata->host_ops && pdata->host_ops->attach) {
		ret = pdata->host_ops->attach(dsi, device);
		if (ret < 0)
			return ret;
	}

	dsi->lanes = device->lanes;
	dsi->format = device->format;
	dsi->mode_flags = device->mode_flags;

	return 0;
}

static int samsung_dsim_host_detach(struct mipi_dsi_host *host,
				    struct mipi_dsi_device *device)
{
	struct samsung_dsim *dsi = host_to_dsi(host);
	const struct samsung_dsim_plat_data *pdata = dsi->plat_data;
	int ret;

	if (dsi->out_bridge->funcs->detach)
		dsi->out_bridge->funcs->detach(dsi->out_bridge);

	dsi->out_bridge = NULL;

	if (pdata && pdata->host_ops && pdata->host_ops->detach) {
		ret = pdata->host_ops->detach(dsi, device);
		if (ret < 0)
			return ret;
	}

	drm_bridge_remove(&dsi->bridge);

	return 0;
}

static ssize_t samsung_dsim_host_transfer(struct mipi_dsi_host *host,
					  const struct mipi_dsi_msg *msg)
{
	struct samsung_dsim *dsi = host_to_dsi(host);
	struct samsung_dsim_transfer xfer;
	int ret;

	if (!(dsi->state & DSIM_STATE_ENABLED))
		return -EINVAL;

	ret = samsung_dsim_init(dsi);
	if (ret)
		return ret;

	ret = mipi_dsi_create_packet(&xfer.packet, msg);
	if (ret < 0)
		return ret;

	xfer.rx_len = msg->rx_len;
	xfer.rx_payload = msg->rx_buf;
	xfer.flags = msg->flags;

	ret = samsung_dsim_transfer(dsi, &xfer);
	return (ret < 0) ? ret : xfer.rx_done;
}

static const struct drm_bridge_timings samsung_dsim_timings = {
	.input_bus_flags = DRM_BUS_FLAG_DE_LOW,
};

static const struct mipi_dsi_host_ops samsung_dsim_ops = {
	.attach = samsung_dsim_host_attach,
	.detach = samsung_dsim_host_detach,
	.transfer = samsung_dsim_host_transfer,
};

static int samsung_dsim_of_read_u32(const struct device_node *np,
				    const char *propname, u32 *out_value)
{
	int ret = of_property_read_u32(np, propname, out_value);

	if (ret < 0)
		pr_err("%pOF: failed to get '%s' property\n", np, propname);

	return ret;
}

static int samsung_dsim_parse_dt(struct samsung_dsim *dsi)
{
	struct device *dev = dsi->dev;
	struct device_node *node = dev->of_node;
	int ret;

	ret = samsung_dsim_of_read_u32(node, "samsung,pll-clock-frequency",
				       &dsi->pll_clk_rate);
	if (ret < 0)
		return ret;

	ret = samsung_dsim_of_read_u32(node, "samsung,burst-clock-frequency",
				       &dsi->burst_clk_rate);
	if (ret < 0)
		return ret;

	ret = samsung_dsim_of_read_u32(node, "samsung,esc-clock-frequency",
				       &dsi->esc_clk_rate);
	if (ret < 0)
		return ret;

	return 0;
}

__weak int samsung_dsim_plat_probe(struct samsung_dsim *priv)
{
	return 0;
}

__weak void samsung_dsim_plat_remove(struct samsung_dsim *priv)
{
}

static const struct drm_bridge_timings samsung_dsim_bridge_timings = {
	.input_bus_flags = DRM_BUS_FLAG_DE_LOW,
};

static int samsung_dsim_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct samsung_dsim *dsi;
	int ret, i;

	dsi = devm_kzalloc(dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi)
		return -ENOMEM;

	init_completion(&dsi->completed);
	spin_lock_init(&dsi->transfer_lock);
	INIT_LIST_HEAD(&dsi->transfer_list);

	dsi->dsi_host.ops = &samsung_dsim_ops;
	dsi->dsi_host.dev = dev;

	dsi->dev = dev;
	dsi->driver_data = of_device_get_match_data(dev);

	dsi->supplies[0].supply = "vddcore";
	dsi->supplies[1].supply = "vddio";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(dsi->supplies),
				      dsi->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	dsi->clks = devm_kcalloc(dev, dsi->driver_data->num_clks,
				 sizeof(*dsi->clks), GFP_KERNEL);
	if (!dsi->clks)
		return -ENOMEM;

	for (i = 0; i < dsi->driver_data->num_clks; i++) {
		dsi->clks[i] = devm_clk_get(dev, clk_names[i]);
		if (IS_ERR(dsi->clks[i])) {
			if (strcmp(clk_names[i], "sclk_mipi") == 0) {
				dsi->clks[i] = devm_clk_get(dev, OLD_SCLK_MIPI_CLK_NAME);
				if (!IS_ERR(dsi->clks[i]))
					continue;
			}

			dev_info(dev, "failed to get the clock: %s\n", clk_names[i]);
			return PTR_ERR(dsi->clks[i]);
		}
	}

	dsi->reg_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dsi->reg_base))
		return PTR_ERR(dsi->reg_base);

	dsi->phy = devm_phy_optional_get(dev, "dsim");
	if (IS_ERR(dsi->phy)) {
		dev_info(dev, "failed to get dsim phy\n");
		return PTR_ERR(dsi->phy);
	}

	dsi->irq = platform_get_irq(pdev, 0);
	if (dsi->irq < 0)
		return dsi->irq;

	ret = devm_request_threaded_irq(dev, dsi->irq, NULL,
					samsung_dsim_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), dsi);
	if (ret) {
		dev_err(dev, "failed to request dsi irq\n");
		return ret;
	}

	ret = samsung_dsim_parse_dt(dsi);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, dsi);

	pm_runtime_enable(dev);

	dsi->bridge.funcs = &samsung_dsim_bridge_funcs;
	dsi->bridge.of_node = dev->of_node;
	dsi->bridge.timings = &samsung_dsim_bridge_timings;
	dsi->bridge.type = DRM_MODE_CONNECTOR_DSI;
	dsi->bridge.timings = &samsung_dsim_timings;

	if (dsi->driver_data->quirks & DSIM_QUIRK_PLAT_DATA)
		ret = samsung_dsim_plat_probe(dsi);
	else
		ret = mipi_dsi_host_register(&dsi->dsi_host);

	if (ret)
		goto err_disable_runtime;

	return 0;

err_disable_runtime:
	pm_runtime_disable(dev);

	return ret;
}

static int samsung_dsim_remove(struct platform_device *pdev)
{
	struct samsung_dsim *dsi = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);

	if (dsi->driver_data->quirks & DSIM_QUIRK_PLAT_DATA)
		samsung_dsim_plat_remove(dsi);
	else
		mipi_dsi_host_unregister(&dsi->dsi_host);

	return 0;
}

static int __maybe_unused samsung_dsim_suspend(struct device *dev)
{
	struct samsung_dsim *dsi = dev_get_drvdata(dev);
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	int ret, i;

	usleep_range(10000, 20000);

	if (dsi->state & DSIM_STATE_INITIALIZED) {
		dsi->state &= ~DSIM_STATE_INITIALIZED;

		samsung_dsim_disable_clock(dsi);

		samsung_dsim_disable_irq(dsi);
	}

	dsi->state &= ~DSIM_STATE_CMD_LPM;

	phy_power_off(dsi->phy);

	for (i = driver_data->num_clks - 1; i > -1; i--)
		clk_disable_unprepare(dsi->clks[i]);

	ret = regulator_bulk_disable(ARRAY_SIZE(dsi->supplies), dsi->supplies);
	if (ret < 0)
		dev_err(dsi->dev, "cannot disable regulators %d\n", ret);

	return 0;
}

static int __maybe_unused samsung_dsim_resume(struct device *dev)
{
	struct samsung_dsim *dsi = dev_get_drvdata(dev);
	const struct samsung_dsim_driver_data *driver_data = dsi->driver_data;
	int ret, i;

	ret = regulator_bulk_enable(ARRAY_SIZE(dsi->supplies), dsi->supplies);
	if (ret < 0) {
		dev_err(dsi->dev, "cannot enable regulators %d\n", ret);
		return ret;
	}

	for (i = 0; i < driver_data->num_clks; i++) {
		ret = clk_prepare_enable(dsi->clks[i]);
		if (ret < 0)
			goto err_clk;
	}

	ret = phy_power_on(dsi->phy);
	if (ret < 0) {
		dev_err(dsi->dev, "cannot enable phy %d\n", ret);
		goto err_clk;
	}

	return 0;

err_clk:
	while (--i > -1)
		clk_disable_unprepare(dsi->clks[i]);
	regulator_bulk_disable(ARRAY_SIZE(dsi->supplies), dsi->supplies);

	return ret;
}

static const struct dev_pm_ops samsung_dsim_pm_ops = {
	SET_RUNTIME_PM_OPS(samsung_dsim_suspend, samsung_dsim_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
};

struct platform_driver dsi_driver = {
	.probe = samsung_dsim_probe,
	.remove = samsung_dsim_remove,
	.driver = {
		   .name = "samsung-dsim",
		   .owner = THIS_MODULE,
		   .pm = &samsung_dsim_pm_ops,
		   .of_match_table = samsung_dsim_of_match,
	},
};

static int __init samsung_mipi_dsim_init(void)
{
	int ret;

	ret = platform_driver_register(&dsi_driver);

	/**
	 * Exynos drm driver stack will register the platform_driver
	 * separately in the common of it's exynos_drm_drv.c including
	 * dsi_driver. Register again would return -EBUSY, so return 0
	 * for such cases as dsi_driver is already registered.
	 */
	return ret == -EBUSY ? 0 : ret;
}
module_init(samsung_mipi_dsim_init);

static void __exit samsung_mipi_dsim_exit(void)
{
	platform_driver_unregister(&dsi_driver);
}
module_exit(samsung_mipi_dsim_exit);

MODULE_AUTHOR("Jagan Teki <jagan@amarulasolutions.com>");
MODULE_DESCRIPTION("Samsung MIPI DSIM controller bridge");
MODULE_LICENSE("GPL");
