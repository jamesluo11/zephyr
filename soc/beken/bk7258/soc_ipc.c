/*
 * Copyright (c) 2026 Beken Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal slave-side implementation of the BK7258 inter-core mailbox (mbox0):
 * report "powered up" to CP and keep a periodic heartbeat going.
 *
 * Why this file has to exist
 * --------------------------
 * After boot CP (CPU0) releases the AP (CPU1) and enters a supervision state
 * machine (ap/middleware/driver/mailbox/mb_ipc_heartbeat.c:191-236):
 *
 *   1. It sets MB_IPC_START_CORE_FLAG and waits 2 s for MB_IPC_POWER_UP_FLAG.
 *      Timing out only prints "IPC retry to start core1" and carries on.
 *   2. It then supervises the heartbeat: no heartbeat within
 *      HB_TIMEOUT_MS = 2000*3 = 6 s triggers BK_ASSERT(false), which resets
 *      the whole chip.
 *
 * So a single-core Zephyr that never talks across cores gets reset by CP about
 * 8 s after power-on: the console shows a board rebooting over and over with no
 * application output at all.
 *
 * What is implemented here is the equivalent of the vendor slave task
 * (mb_ipc_heartbeat.c:341-350):
 *       ipc_send_power_up();
 *       while (1) { rtos_delay_milliseconds(2000); ipc_send_heart_beat(0); }
 *
 * The link used to be one-directional only (fire and forget, plus draining the
 * receive FIFO). It now works in both directions:
 *   - Received messages are no longer dropped: the 16-byte command block is
 *     parsed and handed to a callback (soc_bk7258_mbox_set_rx_cb).
 *   - CP's flash operation notices (logical channel 0x4B) are ACKed
 *     automatically.
 *   - Every command we receive gets a logical-channel ACK. Without it
 *     CP's mb_ipc server socket stays RX_BUSY forever: CONNECT succeeds yet
 *     every later SEND fails, which is deeply misleading.
 *   - Verified on hardware: three IPC_TEST_CMD round trips all passed, and the
 *     AP -> CP LOG channel makes strings we send appear on CP's console.
 *
 * About the physical channel tx_state staying BUSY: the vendor mb_chnl_write()
 * is synchronous and only returns the channel to IDLE once the peer ACKs. That
 * state machine is not implemented here, and measurements show it does not
 * matter, because tx_seq increments every time and CP's
 * mb_phy_chnl_rx_ack_isr() matches on seq plus logical_chnl, not on tx_state.
 * This was established by measurement, not by inference.
 *
 * Registers and protocol constants cross-checked against:
 *   ap/middleware/soc/bk7258_ap/soc/mbox0_struct.h      (register layout)
 *   ap/middleware/soc/common/hal/mbox0_hal.c:69-110     (send/recv sequence)
 *   ap/middleware/driver/mailbox/mbox0_drv.c:37,95      (channel ownership)
 *   ap/middleware/driver/mailbox/mbox0_adapter.c:73-129 (message = addr+len)
 *   ap/middleware/driver/mailbox/mailbox_channel.c:194  (physical cmd block)
 */

#include <soc.h>

#include <errno.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(soc_bk7258_ipc, CONFIG_SOC_LOG_LEVEL);

/*
 * Command block: mb_chnl_cmd_t = { hdr, param1, param2, param3 }, 16 bytes.
 *
 * It has to live in shared memory: the mailbox FIFO carries the *address* of
 * this block rather than the data (mbox0_adapter.c:119-120) and CP reads it
 * from there. A stack variable would be clobbered as soon as the function
 * returns.
 */
static uint32_t mb_cmd_buf[4];
static uint8_t mb_tx_seq;

/*
 * Heartbeat payload.
 *
 * This is the second, and the subtlest, trap found on hardware on 2026-09-18:
 * an AP -> CP IPC command block is not a bare mb_chnl_cmd_t, it is wrapped in
 * an IPC header:
 *
 *       typedef union {
 *           struct {
 *               mb_chnl_hdr_t chnl_hdr;     // word0: cmd[7:0]
 *               void         *cmd_buff;     // word1: payload pointer
 *               u16           cmd_data_len; // word2 low 16 bits: length
 *           };
 *           mb_chnl_cmd_t mb_cmd;           // {hdr, param1, param2, param3}
 *       } ipc_cmd_t;                        // cp/mb_ipc_cmd.c:49-59
 *
 * so param1 is cmd_buff (a pointer) and param2 is cmd_data_len (a length).
 *
 * CP handles it like this (cp/mb_ipc_cmd.c:663-674):
 *       case IPC_CPU1_HEART_BEAT_INDICATION:
 *           if (chnl_cb->cmd_len >= sizeof(u32))    // cmd_len = cmd_data_len
 *               mb_ipc_heartbeat_notify(...);
 *
 * With cmd_data_len = 0 the heartbeat is silently dropped: CP neither reports
 * an error nor refreshes its heartbeat timestamp, and 6 s later BK_ASSERT
 * resets the whole chip. The symptom is "POWER_UP was accepted (no IPC retry
 * warning) but we never survive 6 seconds", which is very easy to misread as
 * "CP does not recognise our core".
 *
 * The POWER_UP case does not look at cmd_len ("no params"), so it goes through
 * even with a zero payload, which is exactly why it hid the cmd_len problem
 * for so long.
 *
 * CP memcpy's these 4 bytes away, so they must be stable shared memory and not
 * a stack variable.
 */
static uint32_t mb_hb_param;

/* Make sure channel N is enabled. CPU0 configures every channel at boot; this
 * only acts as a fallback when it did not.
 */
static void mb_chn_ensure_enabled(uint32_t chn)
{
	uint32_t en = sys_read32(SOC_MBOX0_CHN_ADDR(chn, SOC_MBOX0_CHN_ENABLE));

	if ((en & BIT(0)) != 0U) {
		/* CP already configured it: leave it alone rather than disturb a
		 * FIFO that may be in use.
		 */
		return;
	}

	/*
	 * Fallback configuration: fifo_start / fifo_length, and the start
	 * address has to be written before the enable bit. Values copied from
	 * ap/middleware/driver/bk7258_ap/mbox0_fifo_cfg.h:
	 *   chn0: start=0 len=2, chn1: start=2 len=4, chn2: start=6 len=2
	 */
	static const uint8_t fifo_start[3] = {0U, 2U, 6U};
	static const uint8_t fifo_len[3] = {2U, 4U, 2U};

	if (chn >= 3U) {
		return;
	}

	sys_write32(fifo_start[chn] & 0x3FU, SOC_MBOX0_CHN_ADDR(chn, SOC_MBOX0_CHN_FIFO_CFG));
	sys_write32(BIT(0) | ((uint32_t)fifo_len[chn] << 1),
		    SOC_MBOX0_CHN_ADDR(chn, SOC_MBOX0_CHN_ENABLE));
}

/* Receive callback (NULL means drain and discard) and statistics */
static soc_bk7258_mbox_rx_cb_t mb_rx_cb;
static uint32_t mb_rx_count;
static uint32_t mb_ack_count;
static uint32_t mb_flash_notify_count;
static uint32_t mb_txack_count; /* ACKs we have sent back to CP */

/* Dedicated command block for ACKs. It cannot be mb_cmd_buf: the heartbeat
 * overwrites that every 2 s and we do not control when the peer reads these
 * 16 bytes.
 */
static uint32_t mb_ack_buf[4];

void soc_bk7258_mbox_set_rx_cb(soc_bk7258_mbox_rx_cb_t cb)
{
	mb_rx_cb = cb;
}

uint32_t soc_bk7258_mbox_txack_count(void)
{
	return mb_txack_count;
}

void soc_bk7258_mbox_stats(uint32_t *rx, uint32_t *ack, uint32_t *flash_notify)
{
	if (rx != NULL) {
		*rx = mb_rx_count;
	}
	if (ack != NULL) {
		*ack = mb_ack_count;
	}
	if (flash_notify != NULL) {
		*flash_notify = mb_flash_notify_count;
	}
}

/*
 * Drain our own receive FIFO.
 *
 * The FIFO only holds two things: an address (rdata0) and a length (rdata1).
 * The real 16-byte command block sits in shared memory at that address and has
 * to be read from there (mbox0_adapter.c:73-129).
 *
 * Order matters: read mail_sid first, because reading it is what starts the
 * receive and pops the entry, then take rdata0/rdata1. Reversing the order
 * reads leftovers from the previous entry.
 */
/*
 * Push "address + length" into the peer FIFO.
 *
 * The mailbox never carries the data itself, only the address of the command
 * block (mbox0_adapter.c:73-129). Writing MAIL_TID is what actually triggers
 * the transfer, so the data has to be visible in memory first (DMB).
 */
static int mb_tx_addr(uint32_t addr, uint32_t len)
{
	/* Drop this one when the peer FIFO is full */
	if ((sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_FIFO_STAT)) &
	     SOC_MBOX0_FIFO_STAT_FULL_BIT) != 0U) {
		return -EBUSY;
	}

	/* The data has to be visible to the peer before the doorbell */
	barrier_dmem_fence_full();

	sys_write32(addr, SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_TDATA0));
	sys_write32(len, SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_TDATA1));
	sys_write32(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_MAIL_TID));

	return 0;
}

/*
 * Automatic logical-channel ACK.
 *
 * The vendor mailbox_channel.c:441-449 says "RE-USE the cmd buffer for ACK",
 * which reads like an in-place reuse but is not: it ends up in
 * bk_mailbox_send_safe(), and bk_mailbox_send() in mbox0_adapter.c:73-127
 * memcpy's the content into a rotating slot of its own, mailbox_buff[dst][box],
 * then pushes the *slot address* into the FIFO:
 *
 *       mailbox_tx_box[dst] = (mailbox_tx_box[dst] + 1) % TX_BOX_NUM;
 *       memcpy(&mailbox_buff[dst][box], data, sizeof(mailbox_data_t));
 *       message.data[0] = (u32)&mailbox_buff[dst][box];
 *
 * So patching the buffer at addr and pushing it back is wrong: that is the
 * *sender's* transmit slot, which the sender will reuse for its next message
 * at any time (the slots rotate), and the header we write there can collide
 * with that next send. The correct move is to send back a copy, which is what
 * the small rotating ACK pool below is for.
 */
#define MB_ACK_POOL_NUM 8U
static uint32_t mb_ack_pool[MB_ACK_POOL_NUM][4];
static uint8_t mb_ack_pool_idx;

static void mb_auto_ack(uint32_t addr, uint32_t hdr)
{
	uint32_t *b = &mb_ack_pool[mb_ack_pool_idx][0];

	mb_ack_pool_idx = (mb_ack_pool_idx + 1U) % MB_ACK_POOL_NUM;

	/* Only the header changes: clear state[11:8] and set ACK_BOX in ctrl.
	 * cmd / tx_seq / logical_chnl must be preserved verbatim, because the
	 * peer claims this ACK by the latter two.
	 */
	b[0] = (hdr & 0xFFFFF0FFU) | (SOC_MB_CTRL_ACK_BOX << 12);
	b[1] = sys_read32(addr + 4U);
	b[2] = sys_read32(addr + 8U);
	b[3] = sys_read32(addr + 12U);

	/* Count only what was actually sent: a full peer FIFO drops the ACK
	 * silently, and that drop is the most common cause of the peer timing
	 * out waiting for an ACK, so the counter has to show it.
	 */
	if (mb_tx_addr((uint32_t)b, SOC_MB_IPC_MSG_LEN) == 0) {
		mb_txack_count++;
	}
}

void soc_bk7258_mbox_pump(void)
{
	uint32_t guard = 8U; /* FIFO is 4 deep; 8 guards against a spin */

	while (guard-- != 0U) {
		if ((sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_FIFO_STAT)) &
		     SOC_MBOX0_FIFO_STAT_EMPT_BIT) != 0U) {
			break;
		}

		(void)sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_MAIL_SID));
		uint32_t addr =
			sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_RDATA0));
		uint32_t len =
			sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_RDATA1));

		mb_rx_count++;

		/*
		 * The address comes from the peer, so do not dereference it
		 * blindly. Only pointers inside SRAM (0x28000000..0x280A0000)
		 * are accepted: a zero or a garbled pointer would throw the AP
		 * straight into a HardFault, which shows up as "I sent a test
		 * command and the board vanished" and is very hard to trace.
		 */
		bool valid = (addr >= SOC_SRAM0_DATA_BASE) &&
			     (addr < (SOC_SRAM0_DATA_BASE + 0xA0000U)) && ((addr & 0x3U) == 0U);
		uint32_t w0 = 0U, w1 = 0U, w2 = 0U, w3 = 0U;

		if (valid) {
			w0 = sys_read32(addr + 0U);
			w1 = sys_read32(addr + 4U);
			w2 = sys_read32(addr + 8U);
			w3 = sys_read32(addr + 12U);
		}

		/*
		 * The ACK has to go out *before* the callback runs.
		 *
		 * The peer's mb_ipc_send() blocks on this ACK before releasing
		 * its semaphore and times out with -8 (MB_IPC_TX_TIMEOUT). If
		 * the callback prints to the console, the time printk takes (a
		 * few dozen characters at 115200 is already milliseconds)
		 * pushes the ACK back for nothing. Worse, the peer may send
		 * something else meanwhile, after which our ACK is judged stale
		 * (tx_seq no longer matches) and dropped -- and that drop is
		 * not logged anywhere, so it can only be inferred from the
		 * peer's timeout.
		 */
		if (valid && !SOC_MB_HDR_IS_ACK(w0) && !SOC_MB_HDR_IS_SYNC_TX(w0)) {
			if ((w0 >> 24) == SOC_MB_CHNL_CP_FLASH) {
				/*
				 * CP flash operation notice: this needs an
				 * *application-level* ACK with w1 set to
				 * IPC_FLASH_OP_ACK (2)
				 * (flash_notify.c:65-100).
				 */
				mb_flash_notify_count++;
				(void)soc_bk7258_mbox_ack(addr, w0, SOC_MB_FLASH_OP_ACK, w2,
							  SOC_MB_ACK_STATE_COMPLETE);
			} else {
				mb_auto_ack(addr, w0);
			}
		}

		if (mb_rx_cb != NULL) {
			mb_rx_cb(w0, w1, w2, w3, len);
		}

		/*
		 * The ACK has already been sent above; this note is only here so
		 * nobody later thinks it was missed. CP's flash operation
		 * notices take the other branch (filling IPC_FLASH_OP_ACK),
		 * see cp/middleware/driver/flash/flash_notify.c:65-100,150-176.
		 * Without an answer CP waits 5 ms for nothing on every erase or
		 * program and believes the AP has stopped touching flash when
		 * it has not -- two cores driving the same device is exactly the
		 * whole-chip reset described in this file.
		 */
	}
}

int soc_bk7258_mbox_post(const uint32_t *cmd, uint32_t len)
{
	if (cmd == NULL) {
		return -EINVAL;
	}

	return mb_tx_addr((uint32_t)cmd, len);
}

int soc_bk7258_mbox_ack(uint32_t addr, uint32_t hdr, uint32_t w1, uint32_t w2, uint32_t w3)
{
	/*
	 * An ACK is the received command block sent straight back with three
	 * changes:
	 *   (1) clear state[11:8] (leaving it set reads as CHNL_STATE_COM_FAIL)
	 *   (2) set CHNL_CTRL_ACK_BOX in ctrl[15:12] (bit 0 -> header bit 12)
	 *   (3) fill ack_data1 / ack_state, i.e. word1 / word3
	 * cmd[7:0], tx_seq[23:16] and logical_chnl[31:24] must be preserved
	 * verbatim: the peer's mb_phy_chnl_rx_ack_isr() claims the ACK by
	 * tx_seq plus logical_chnl.
	 *
	 * Why the ACK does not use "the other" physical box: mb_chnl_write does
	 * distinguish BOX0 (command) from BOX1 (ACK), but the code in
	 * bk_mailbox_send() that picks a buffer by box is commented out
	 * (mbox0_adapter.c:88-98). In practice both go through the same FIFO and
	 * are told apart only by that one bit in hdr.ctrl.
	 */
	uint32_t h = (hdr & 0xFFFFF0FFU) | (SOC_MB_CTRL_ACK_BOX << 12);

	if ((sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_FIFO_STAT)) &
	     SOC_MBOX0_FIFO_STAT_FULL_BIT) != 0U) {
		return -EBUSY;
	}

	mb_ack_buf[0] = h;
	mb_ack_buf[1] = w1;
	mb_ack_buf[2] = w2;
	mb_ack_buf[3] = w3;

	mb_ack_count++;

	return mb_tx_addr((uint32_t)&mb_ack_buf[0], SOC_MB_IPC_MSG_LEN);
}

int soc_bk7258_mbox_send(uint32_t chnl, uint32_t cmd, uint32_t p1, uint32_t p2)
{
	/* Drop this tick when the destination FIFO is full: losing one heartbeat
	 * is harmless, the next one follows 2 s later.
	 */
	if ((sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_FIFO_STAT)) &
	     SOC_MBOX0_FIFO_STAT_FULL_BIT) != 0U) {
		return -EBUSY;
	}

	mb_tx_seq++;
	mb_cmd_buf[0] = SOC_MB_HDR_CH(chnl, cmd, mb_tx_seq);
	mb_cmd_buf[1] = p1; /* == ipc_cmd.cmd_buff     (payload pointer) */
	mb_cmd_buf[2] = p2; /* == ipc_cmd.cmd_data_len (length, low 16) */
	mb_cmd_buf[3] = 0U;

	/*
	 * The data has to be visible to the peer before the doorbell. A DMB is
	 * enough with no D-Cache; if a D-Cache is ever enabled, or the command
	 * block moves to a cacheable region, this has to become a cache clean.
	 */
	barrier_dmem_fence_full();

	sys_write32((uint32_t)&mb_cmd_buf[0],
		    SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_TDATA0));
	sys_write32(SOC_MB_IPC_MSG_LEN, SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_TDATA1));
	/* Writing mail_tid = destination CPU is what actually triggers the send */
	sys_write32(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_MAIL_TID));

	return 0;
}

void soc_bk7258_ipc_power_up(void)
{
	int rc;

	mb_chn_ensure_enabled(SOC_MB_SELF_CPU);
	soc_bk7258_mbox_pump();
	/* POWER_UP: the vendor ipc_send_cmd(..., NULL, 0, ...), no payload */
	rc = soc_bk7258_mbox_send(SOC_MB_CHNL_HW_CTRL, SOC_MB_IPC_CMD_POWER_UP, 0U, 0U);

	printk("[ipc] power_up rc=%d chn1_en=%08x chn1_st=%08x chn0_st=%08x\n", rc,
	       sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_ENABLE)),
	       sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_FIFO_STAT)),
	       sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_FIFO_STAT)));
}

int soc_bk7258_ipc_heartbeat(void)
{
	soc_bk7258_mbox_pump();

	/*
	 * HEART_BEAT: the vendor ipc_send_heart_beat(param) is
	 * ipc_send_cmd(..., (u8 *)&param, sizeof(param), ...), that is
	 * cmd_buff = &param and cmd_data_len = 4.
	 *
	 * cmd_data_len must be >= 4 or CP drops it outright (see the comment on
	 * mb_hb_param).
	 */
	mb_hb_param = 0U;
	return soc_bk7258_mbox_send(SOC_MB_CHNL_HW_CTRL, SOC_MB_IPC_CMD_HEARTBEAT,
				    (uint32_t)&mb_hb_param, sizeof(mb_hb_param));
}

/* Diagnostic counters (bring-up only, can go away once this is stable) */
static uint32_t mb_hb_count;
static uint32_t mb_hb_busy;

/*
 * Heartbeat tick
 *
 * A k_timer rather than a thread: its callback runs in interrupt context, so a
 * wedged application thread does not stop it. The flip side is intended by the
 * vendor design: if the application really hangs, the heartbeat stops, CP
 * resets the chip, and the mechanism acts as a watchdog.
 */

static struct k_timer mb_hb_timer;

static void mb_hb_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	int rc = soc_bk7258_ipc_heartbeat();

	mb_hb_count++;
	if (rc != 0) {
		mb_hb_busy++;
	}

	/*
	 * Bring-up diagnostic: print whether the heartbeat actually went out.
	 * Reading the output:
	 *   [hb] lines at all         -> the timer runs and the send path is
	 *                                being executed
	 *   [hb] with rc=-16          -> the peer FIFO stays full (CP drains it)
	 *   no [hb] at all            -> the k_timer never started
	 *   [hb] rc=0 but CP still reports a heartbeat timeout
	 *                             -> the command was not accepted by CP
	 *
	 * Only the first 3 ticks, then every 10th, plus every error, so the
	 * console does not flood.
	 */
	if ((rc != 0) || (mb_hb_count <= 3U) || ((mb_hb_count % 10U) == 0U)) {
		printk("[hb] #%u rc=%d busy=%u chn0_st=%08x chn1_st=%08x\n", mb_hb_count, rc,
		       mb_hb_busy,
		       sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_PEER_CPU, SOC_MBOX0_CHN_FIFO_STAT)),
		       sys_read32(SOC_MBOX0_CHN_ADDR(SOC_MB_SELF_CPU, SOC_MBOX0_CHN_FIFO_STAT)));
	}
}

static int soc_bk7258_ipc_init(void)
{
	/*
	 * The power-up notification is issued separately at PRE_KERNEL_1 (see
	 * soc.c), much earlier than this point. CP waits 2 s, but the earlier
	 * this happens the better, so that no later step can push it past that
	 * window.
	 */
	k_timer_init(&mb_hb_timer, mb_hb_handler, NULL);
	/* Vendor MB_IPC_HEARTBEAT_TIME is 2000 ms; CP tolerates 3 missed ticks */
	k_timer_start(&mb_hb_timer, K_MSEC(2000), K_MSEC(2000));

	LOG_INF("mbox0 heartbeat started: every 2000 ms to CPU%d (self chn=%d)", SOC_MB_PEER_CPU,
		SOC_MB_SELF_CPU);

	return 0;
}

SYS_INIT(soc_bk7258_ipc_init, POST_KERNEL, 0);
