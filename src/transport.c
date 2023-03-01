// Copyright 2023, Brian Swetland <swetland@frotz.net>
// Licensed under the Apache License, Version 2.0.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "usb.h"
#include "arm-debug.h"
#include "cmsis-dap-protocol.h"
#include "transport.h"

#define WITH_TRACE 0

#if WITH_TRACE
#define TRACE(fmt...) fprintf(stderr, fmt)
#else
#define TRACE(fmt...) do {} while (0)
#endif

#define ERROR(fmt...) fprintf(stderr, fmt)

#if WITH_TRACE
static void dump(const char* str, const void* ptr, unsigned len) {
	const uint8_t* x = ptr;
	TRACE("%s", str);
	while (len > 0) {
		TRACE(" %02x", *x++);
		len--;
	}
	TRACE("\n");
}
#else
#define dump(...) do {} while (0)
#endif

#define DC_ATTACHED 0 // attached and ready to do txns
#define DC_FAILURE  1 // last txn failed, need to re-attach
#define DC_DETACHED 2 // have not yet attached
#define DC_OFFLINE  3 // usb connection not available

#define INVALID 0xFFFFFFFFU

struct debug_context {
	usb_handle* usb;
	unsigned status;

	// dap protocol info
	uint32_t max_packet_count;
	uint32_t max_packet_size;

	// dap internal state cache
	uint32_t cfg_idle;
	uint32_t cfg_wait;
	uint32_t cfg_match;
	uint32_t cfg_mask;

	// configured DP.SELECT register value
	uint32_t dp_select;
	// last known state of DP.SELECT on the target
	uint32_t dp_select_cache;

	// transfer queue state
	uint8_t txbuf[1024];
	uint32_t* rxptr[256];
	uint8_t *txnext;
	uint32_t** rxnext;
	uint32_t txavail;
	uint32_t rxavail;
	int qerror;
};


typedef struct debug_context DC;


int dap_get_info(DC* dc, unsigned di, void *out, unsigned minlen, unsigned maxlen) {
	uint8_t	buf[256 + 2];
	buf[0] = DAP_Info;
	buf[1] = di;
	if (usb_write(dc->usb, buf, 2) != 2) {
		return DC_ERR_IO;
	}
	int sz = usb_read(dc->usb, buf, 256 + 2);
	if ((sz < 2) || (buf[0] != DAP_Info)) {
		return DC_ERR_PROTOCOL;
	}
	if ((buf[1] < minlen) || (buf[1] > maxlen)) {
		return DC_ERR_PROTOCOL;
	}
	memcpy(out, buf + 2, buf[1]);
	return buf[1];
}

int dap_cmd(DC* dc, const void* tx, unsigned txlen, void* rx, unsigned rxlen) {
	uint8_t cmd = ((const uint8_t*) tx)[0];
	dump("TX>", tx, txlen);
	if (usb_write(dc->usb, tx, txlen) != txlen) {
		ERROR("dap_cmd(0x%02x): usb write error\n", cmd);
		return DC_ERR_IO;
	}
	int sz = usb_read(dc->usb, rx, rxlen);
	if (sz < 1) {
		ERROR("dap_cmd(0x%02x): usb read error\n", cmd);
		return DC_ERR_IO;
	}
	dump("RX>", rx, rxlen);
	if (((uint8_t*) rx)[0] != cmd) {
		ERROR("dap_cmd(0x%02x): unsupported (0x%02x)\n",
			cmd, ((uint8_t*) rx)[0]);
		return DC_ERR_UNSUPPORTED;
	}
	return sz;
}

int dap_cmd_std(DC* dc, const char* name, uint8_t* io,
		unsigned txlen, unsigned rxlen) {
	int r = dap_cmd(dc, io, txlen, io, rxlen);
	if (r < 0) {
		return r;
	}
	if (io[1] != 0) {
		ERROR("%s status 0x%02x\n", name, io[1]);
		return DC_ERR_REMOTE;
	}
	return 0;
}

int dap_connect(DC* dc) {
	uint8_t io[2] = { DAP_Connect, PORT_SWD };
	return dap_cmd_std(dc, "dap_connect()", io, 2, 2);
}

int dap_swd_configure(DC* dc, unsigned cfg) {
	uint8_t io[2] = { DAP_SWD_Configure, cfg };
	return dap_cmd_std(dc, "dap_swd_configure()", io, 2, 2);
}

static int dap_xfer_config(DC* dc, unsigned idle, unsigned wait, unsigned match) {
	// clamp to allowed max values
	if (idle > 255) idle = 255;
	if (wait > 65535) wait = 65535;
	if (match > 65535) match = 65535;

	// do nothing if unchanged from last set values
	if ((dc->cfg_idle == idle) &&
		(dc->cfg_wait == wait) &&
		(dc->cfg_match == match)) {
		return 0;
	}

	// cache new values
	dc->cfg_idle = idle;
	dc->cfg_wait = wait;
	dc->cfg_match = match;

	// inform the probe
	uint8_t io[6] = { DAP_TransferConfigure, idle, wait, wait >> 8, match, match >> 8};
	return dap_cmd_std(dc, "dap_transfer_configure()", io, 6, 2); 
}

static void dc_q_clear(DC* dc) {
	dc->txnext = dc->txbuf + 3;
	dc->rxnext = dc->rxptr;
	dc->txavail = dc->max_packet_size - 3;
	dc->rxavail = dc->max_packet_size - 3;
	dc->qerror = 0;
	// TODO: less conservative mode: don't always invalidate
	dc->dp_select_cache = INVALID;
	dc->cfg_mask = INVALID;
	dc->txbuf[0] = DAP_Transfer;
	dc->txbuf[1] = 0; // Index 0 for SWD
	dc->txbuf[2] = 0; // Count 0 initially
}

void dc_q_init(DC* dc) {
	// TODO: handle error cleanup, re-attach, etc
	dc_q_clear(dc);
}

// unpack the status bits into a useful status code
static int dc_decode_status(unsigned n) {
	unsigned ack = n & RSP_ACK_MASK;
	if (n & RSP_ProtocolError) {
		ERROR("DAP SWD Parity Error\n");
		return DC_ERR_SWD_PARITY;
	}
	switch (ack) {
	case RSP_ACK_OK:
		break;
	case RSP_ACK_WAIT:
		ERROR("DAP SWD WAIT (Timeout)\n");
		return DC_ERR_TIMEOUT;
	case RSP_ACK_FAULT:
		ERROR("DAP SWD FAULT\n");
		return DC_ERR_SWD_FAULT;
	case RSP_ACK_MASK: // all bits set
		ERROR("DAP SWD SILENT\n");
		return DC_ERR_SWD_SILENT;
	default:
		ERROR("DAP SWD BOGUS\n");
		return DC_ERR_SWD_BOGUS;
	}
	if (n & RSP_ValueMismatch) {
		ERROR("DAP Value Mismatch\n");
		return DC_ERR_MATCH;
	}
	return DC_OK;
}

int dc_q_exec(DC* dc) {
	// if we're already in error, don't generate more usb traffic
	if (dc->qerror) {
		int r = dc->qerror;
		dc_q_clear(dc);
		return r;
	}
	// if we have no work to do, succeed
	if (dc->txbuf[2] == 0) {
		return 0;
	}
	int sz = dc->txnext - dc->txbuf;
	dump("TX>", dc->txbuf, sz);
	if (usb_write(dc->usb, dc->txbuf, sz) != sz) {
		ERROR("dc_q_exec() usb write error\n");
		return DC_ERR_IO;
	}
	sz = 3 + (dc->rxnext - dc->rxptr) * 4;
	uint8_t rxbuf[1024];
	memset(rxbuf, 0xEE, 1024); // DEBUG
	int n = usb_read(dc->usb, rxbuf, sz);
	if (n < 0) {
		ERROR("dc_q_exec() usb read error\n");
		return DC_ERR_IO;
	}
	dump("RX>", rxbuf, sz);
	if ((n < 3) || (rxbuf[0] != DAP_Transfer)) {
		ERROR("dc_q_exec() bad response\n");
		return DC_ERR_PROTOCOL;
	}
	int r = dc_decode_status(rxbuf[2]);
	if (r == DC_OK) {
		// how many response words available?
		n = (n - 3) / 4;
		uint8_t* rxptr = rxbuf + 3;
		for (unsigned i = 0; i < n; i++) {
			memcpy(dc->rxptr[i], rxptr, 4);
			rxptr += 4;
		}
	}

	dc_q_clear(dc);
	return r;
}

// internal use only -- queue raw dp reads and writes
// these do not check req for correctness
static void dc_q_raw_rd(DC* dc, unsigned req, uint32_t* val) {
	if ((dc->txavail < 1) || (dc->rxavail < 4)) {
		// exec q to make space for more work,
		// but if there's an error, latch it
		if ((dc->qerror = dc_q_exec(dc)) != DC_OK) {
			return;
		}
	}
	dc->txnext[0] = req;
	dc->rxnext[0] = val;
	dc->txnext += 1;
	dc->rxnext += 1;
	dc->txbuf[2] += 1;
	dc->txavail -= 1;
	dc->rxavail -= 4;
}

static void dc_q_raw_wr(DC* dc, unsigned req, uint32_t val) {
	if (dc->txavail < 5) {
		// exec q to make space for more work,
		// but if there's an error, latch it
		if ((dc->qerror = dc_q_exec(dc)) != DC_OK) {
			return;
		}
	}
	dc->txnext[0] = req;
	memcpy(dc->txnext + 1, &val, 4);
	dc->txnext += 5;
	dc->txavail -= 5;
	dc->txbuf[2] += 1;
}

// adjust DP.SELECT for desired DP access, if necessary
void dc_q_dp_sel(DC* dc, uint32_t dpaddr) {
	// DP address is BANK:4 REG:4
	if (dpaddr & 0xFFFFFF03U) {
		ERROR("invalid DP addr 0x%08x\n", dpaddr);
		dc->qerror = DC_ERR_FAILED;
		return;
	}
	// only register 4 cares about the value of DP.SELECT.DPBANK
	// so do nothing unless we're setting a register 4 variant
	if ((dpaddr & 0xF) != 0x4) {
		return;
	}
	uint32_t mask = DP_SELECT_DPBANK(0xFU);
	uint32_t addr = DP_SELECT_DPBANK((dpaddr >> 4));
	uint32_t select = (dc->dp_select & mask) | addr;
	if (select != dc->dp_select_cache) {
		dc->dp_select_cache = select;
		dc_q_raw_wr(dc, XFER_DP | XFER_WR | DP_SELECT, select);
	}
}

// adjust DP.SELECT for desired AP access, if necessary
void dc_q_ap_sel(DC* dc, uint32_t apaddr) {
	// AP address is AP:8 BANK:4 REG:4
	if (apaddr & 0xFFFF0003U) {
		ERROR("invalid DP addr 0x%08x\n", apaddr);
		dc->qerror = DC_ERR_FAILED;
		return;
	}
	// we always return DPBANK to 0 when adjusting AP & APBANK
	// since it preceeds an AP write which will need DPBANK at 0
	uint32_t select =
		DP_SELECT_AP((apaddr & 0xFF00U) << 16) |
		DP_SELECT_APBANK(apaddr >> 4);
	if (select != dc->dp_select_cache) {
		dc->dp_select_cache = select;
		dc_q_raw_wr(dc, XFER_DP | XFER_WR | DP_SELECT, select);
	}
}

// DP and AP reads and writes
// DP.SELECT will be adjusted as necessary to ensure proper addressing
void dc_q_dp_rd(DC* dc, unsigned dpaddr, uint32_t* val) {
	if (dc->qerror) return;
	dc_q_dp_sel(dc, dpaddr);
	dc_q_raw_rd(dc, XFER_DP | XFER_RD | (dpaddr & 0x0C), val);
}

void dc_q_dp_wr(DC* dc, unsigned dpaddr, uint32_t val) {
	if (dc->qerror) return;
	dc_q_dp_sel(dc, dpaddr);
	dc_q_raw_wr(dc, XFER_DP | XFER_WR | (dpaddr & 0x0C), val);
}

void dc_q_ap_rd(DC* dc, unsigned apaddr, uint32_t* val) {
	if (dc->qerror) return;
	dc_q_ap_sel(dc, apaddr);
	dc_q_raw_rd(dc, XFER_AP | XFER_RD | (apaddr & 0x0C), val);
}

void dc_q_ap_wr(DC* dc, unsigned apaddr, uint32_t val) {
	if (dc->qerror) return;
	dc_q_ap_sel(dc, apaddr);
	dc_q_raw_wr(dc, XFER_AP | XFER_WR | (apaddr & 0x0C), val);
}

void dc_q_set_mask(DC* dc, uint32_t mask) {
	if (dc->qerror) return;
	if (dc->cfg_mask == mask) return;
	dc->cfg_mask = mask;
	dc_q_raw_wr(dc, XFER_WR | XFER_MatchMask, mask);
}

void dc_set_match_retry(dctx_t* dc, unsigned num) {
	if (dc->qerror) return;
	dap_xfer_config(dc, dc->cfg_idle, dc->cfg_wait, num);
}

void dc_q_ap_match(DC* dc, unsigned apaddr, uint32_t val) {
	if (dc->qerror) return;
	dc_q_ap_sel(dc, apaddr);
	dc_q_raw_wr(dc, XFER_AP | XFER_RD | XFER_ValueMatch | (apaddr & 0x0C), val);
}

void dc_q_dp_match(DC* dc, unsigned apaddr, uint32_t val) {
	if (dc->qerror) return;
	dc_q_ap_sel(dc, apaddr);
	dc_q_raw_wr(dc, XFER_DP | XFER_RD | XFER_ValueMatch | (apaddr & 0x0C), val);
}

// convenience wrappers for single reads and writes
int dc_dp_rd(DC* dc, unsigned dpaddr, uint32_t* val) {
	dc_q_init(dc);
	dc_q_dp_rd(dc, dpaddr, val);
	return dc_q_exec(dc);
}
int dc_dp_wr(DC* dc, unsigned dpaddr, uint32_t val) {
	dc_q_init(dc);
	dc_q_dp_wr(dc, dpaddr, val);
	return dc_q_exec(dc);
}
int dc_ap_rd(DC* dc, unsigned apaddr, uint32_t* val) {
	dc_q_init(dc);
	dc_q_ap_rd(dc, apaddr, val);
	return dc_q_exec(dc);
}
int dc_ap_wr(DC* dc, unsigned apaddr, uint32_t val) {
	dc_q_init(dc);
	dc_q_ap_wr(dc, apaddr, val);
	return dc_q_exec(dc);
}

// SWD Attach Sequence:
// 1. Send >50 1s and then the JTAG to SWD escape code
//    (in case this is a JTAG-SWD DAP in JTAG mode)
// 2. Send >8 1s and then the Selection Alert Sequence
//    and then the SWD Activation Code
//    (in case this is a SWD v2 DAP in Dormant State)
// 3. Send >50 1s and then 4 0s -- the Line Reset Sequence
// 4. If multidrop, issue a write to DP.TARGETSEL, but
//    ignore the ACK response
// 5. Issue a read from DP.IDR

static uint8_t attach_cmd[54] = {
	DAP_SWD_Sequence, 5,

	//    [--- 64 1s ----------------------------------]
	0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	//    [JTAG2SWD]  [- 16 1s ]  [---------------------
	0x00, 0x9E, 0xE7, 0xFF, 0xFF, 0x92, 0xF3, 0x09, 0x62,
	//    ----- Selection Alert Sequence ---------------
	0x00, 0x95, 0x2D, 0x85, 0x86, 0xE9, 0xAF, 0xDD, 0xE3,
        //    ---------------------]  [Act Code]  [---------
	0x00, 0xA2, 0x0E, 0xBC, 0x19, 0xA0, 0xF1, 0xFF, 0xFF,
	//    ----- Line Reset Sequence -------]
	0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,

	//    WR DP TARGETSEL
	0x08, 0x99,
	//    5 bits idle
	0x85,
	//    WR VALUE:32, PARTY:1, ZEROs:7
	0x28, 0x00, 0x00, 0x00, 0x00, 0x00
};

int dc_attach(DC* dc, unsigned flags, uint32_t tgt, uint32_t* idcode) {
	uint8_t rsp[3];

	if (flags & DC_MULTIDROP) {
		// Copy and patch the attach sequence to include
		// the DP.TARGETSEL write and insert the target
		// id and parity
		uint8_t cmd[54];
		memcpy(cmd, attach_cmd, 54);
		cmd[1] = 8;
		memcpy(cmd + 49, &tgt, sizeof(tgt));
		cmd[53] = __builtin_parity(tgt);
		dap_cmd(dc, cmd, 54, rsp, 3);
	} else {
		// use the common part of the attach sequence, as-is
		dap_cmd(dc, attach_cmd, 45, rsp, 2);
	}

	// Issue a bare DP.IDR read, as required after a line reset
	// or line reset + target select
	dc_q_init(dc);
	dc_q_raw_rd(dc, XFER_DP | XFER_RD | XFER_00, idcode);
	int r = dc_q_exec(dc);
	return r;
}

static usb_handle* usb_connect(void) {
	usb_handle *usb;
	
	usb = usb_open(0x1fc9, 0x0143, 0);
	if (usb == 0) {
		usb = usb_open(0x2e8a, 0x000c, 42);
		if (usb == 0) {
			ERROR("cannot find device\n");
			return NULL;
		}
	}
	return usb;
}

// setup a newly connected DAP device
static int dap_configure(DC* dc) {
	uint8_t buf[256 + 2];
	uint32_t n32;
	uint16_t n16;
	uint8_t n8;

	// invalidate cached state
	dc->cfg_idle = INVALID;
	dc->cfg_wait = INVALID;
	dc->cfg_match = INVALID;
	dc->cfg_mask = INVALID;

	// setup default packet limits
	dc->max_packet_count = 1;
	dc->max_packet_size = 64;

	// flush queue
	dc_q_clear(dc);

	buf[0] = DAP_Info;
	for (unsigned n = 0; n < 10; n++) {
		int sz = dap_get_info(dc, n, buf, 0, 255);
		if (sz > 0) {
			buf[sz] = 0;
			printf("0x%02x: '%s'\n", n, (char*) buf);
		}
	}

	buf[0] = 0; buf[1] = 0;
	if (dap_get_info(dc, DI_Capabilities, buf, 1, 2) > 0) {
		printf("Capabilities: 0x%02x 0x%02x\n", buf[0], buf[1]);
		printf("Capabilities:");
		if (buf[0] & I0_SWD) printf(" SWD");
		if (buf[0] & I0_JTAG) printf(" JTAG");
		if (buf[0] & I0_SWO_UART) printf(" SWO(UART)");
		if (buf[0] & I0_SWO_Manchester) printf(" SWO(Manchester)");
		if (buf[0] & I0_Atomic_Commands) printf(" ATOMIC");
		if (buf[0] & I0_Test_Domain_Timer) printf(" TIMER");
		if (buf[0] & I0_SWO_Streaming_Trace) printf(" SWO(Streaming)");
		if (buf[0] & I0_UART_Comm_Port) printf(" UART");
		if (buf[1] & I1_USB_COM_Port) printf(" USBCOM");
		printf("\n");
	}
	if (dap_get_info(dc, DI_UART_RX_Buffer_Size, &n32, 4, 4) == 4) {
		printf("UART RX Buffer Size: %u\n", n32);
	}
	if (dap_get_info(dc, DI_UART_TX_Buffer_Size, &n32, 4, 4) == 4) {
		printf("UART TX Buffer Size: %u\n", n32);
	}
	if (dap_get_info(dc, DI_SWO_Trace_Buffer_Size, &n32, 4, 4) == 4) {
		printf("SWO Trace Buffer Size: %u\n", n32);
	}
	if (dap_get_info(dc, DI_Max_Packet_Count, &n8, 1, 1) == 1) {
		printf("Max Packet Count: %u\n", n8);
		dc->max_packet_count = n8;
	}
	if (dap_get_info(dc, DI_Max_Packet_Size, &n16, 2, 2) == 2) {
		printf("Max Packet Size: %u\n", n16);
		dc->max_packet_size = n16;
	}
	if ((dc->max_packet_count < 1) || (dc->max_packet_size < 64)) {
		ERROR("dc_init() impossible packet configuration\n");
		return DC_ERR_PROTOCOL;
	}

	// invalidate register cache
	dc->dp_select_cache = INVALID;

	// clip to our buffer size
	if (dc->max_packet_size > 1024) {
		dc->max_packet_size = 1024;
	}

	dap_connect(dc);
	dap_swd_configure(dc, CFG_Turnaround_1);
	dap_xfer_config(dc, 8, 64, 0);
	return DC_OK;
}

int dc_create(DC** out) {
	DC* dc;

	if ((dc = calloc(1, sizeof(DC))) == NULL) {
		return DC_ERR_FAILED;
	}
	
	if ((dc->usb = usb_connect()) == NULL) {
		free(dc);
		return DC_ERR_OFFLINE;
	}

	int r = dap_configure(dc);
	if (r < 0) {
		free(dc);
	} else {
		*out = dc;
	}
	return r;
}
