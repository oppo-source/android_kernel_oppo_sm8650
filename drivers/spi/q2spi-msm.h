/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _SPI_Q2SPI_MSM_H_
#define _SPI_Q2SPI_MSM_H_

#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/ipc_logging.h>
#include <linux/kthread.h>
#include <linux/msm_gpi.h>
#include <linux/poll.h>
#include <linux/soc/qcom/geni-se.h>
#include <linux/qcom-geni-se-common.h>
#include <linux/types.h>
#include <uapi/linux/q2spi/q2spi.h>
#include "q2spi-gsi.h"

#define DATA_WORD_LEN			4
#define SMA_BUF_SIZE			4096
#define MAX_CR_SIZE			24 /* Max CR size is 24 bytes per CR */
#define MAX_RX_CRS			4
#define RX_DMA_CR_BUF_SIZE		(MAX_CR_SIZE * MAX_RX_CRS)
#define Q2SPI_MAX_BUF			2
#define XFER_TIMEOUT_OFFSET		(500 * 4)
#define TIMEOUT_MSECONDS		10 /* 10 milliseconds */
#define RETRIES				1
#define Q2SPI_MAX_DATA_LEN		4096
#define Q2SPI_MAX_TX_RETRIES		3
/* Host commands */
#define HC_DB_REPORT_LEN_READ		1
#define HC_DB_REPORT_BODY_READ		2
#define HC_ABORT			3
#define HC_DATA_READ			5
#define HC_DATA_WRITE			6
#define HC_SMA_READ			5
#define HC_SMA_WRITE			6
#define HC_SOFT_RESET			0xF
#define CM_FLOW				1
#define MC_FLOW				0
#define CLIENT_INTERRUPT		1
#define SEGMENT_LST			1
#define LOCAL_REG_ACCESS		0
#define SYSTEM_MEMORY_ACCESS		1
#define CLIENT_ADDRESS			1
#define NO_CLIENT_ADDRESS		0

#define HC_SOFT_RESET_FLAGS		0xF
#define HC_SOFT_RESET_CODE		0x2

/* Client Requests */
#define ADDR_LESS_WR_ACCESS		3
#define ADDR_LESS_RD_ACCESS		4
#define BULK_ACCESS_STATUS		8
#define CR_ADDR_LESS_RD			0xF4

#define Q2SPI_HEADER_LEN		7 /* 7 bytes header excluding checksum we use in SW */
#define DMA_Q2SPI_SIZE			2048
#define MAX_DW_LEN_1			4 /* 4DWlen */
#define MAX_DW_LEN_2			1024 /* for 1K DWlen */
#define CS_LESS_MODE			0
#define INTR_HIGH_POLARITY		1

#define MAX_TX_SG			(3)
#define NUM_Q2SPI_XFER			(10)
#define Q2SPI_START_TID_ID		(0)
#define Q2SPI_END_TID_ID		(8)

/* Q2SPI specific SE GENI registers */
#define IO_MACRO_IO3_DATA_IN_SEL_MASK	GENMASK(15, 14)
#define IO_MACRO_IO3_DATA_IN_SEL_SHIFT	14
#define IO_MACRO_IO3_DATA_IN_SEL	1
#define SE_SPI_TRANS_CFG		0x25c
#define CS_TOGGLE			BIT(1)
#define SPI_NOT_USED_CFG1		BIT(2)
#define SE_SPI_PRE_POST_CMD_DLY		0x274
#define SPI_DELAYS_COUNTERS		0x278
#define M_GP_CNT4_TAN			0
#define M_GP_CNT4_TAN_MASK		GENMASK(9, 0)
#define M_GP_CNT5_TE2D			GENMASK(19, 10)
#define M_GP_CNT5_TE2D_SHIFT		10
#define M_GP_CNT6_CN			GENMASK(29, 20)
#define M_GP_CNT6_CN_SHIFT		20
#define SE_GENI_CFG_REG95		0x27C
#define M_GP_CNT7			GENMASK(9, 0)
#define M_GP_CNT7_TSN			0
#define SPI_INTER_WORDS_DLY		0
#define SPI_CS_CLK_DLY			0x80 /* 80 from VI SW, 128 from ganges SW */
#define SPI_PIPE_DLY_TPM		0x320 /* 800 from VI SW */
#define SE_GENI_CFG_REG103		0x29C
#define S_GP_CNT5			GENMASK(19, 10)
#define S_GP_CNT5_SHIFT			10
#define S_GP_CNT5_TDN			0
#define SE_GENI_CFG_REG104		0x2A0
#define S_GP_CNT7			GENMASK(9, 0)
#define S_GP_CNT7_SSN			0x80 /* 80 from VI SW, 128 from ganges SW */
#define M_GP_CNT6_CN_DELAY		0x50 /*63 from VI SW, trying with 80 from SW */

#define SE_SPI_WORD_LEN			0x268
#define WORD_LEN_MSK			GENMASK(9, 0)
#define MIN_WORD_LEN			4
#define NUMBER_OF_DATA_LINES		GENMASK(1, 0)
#define PARAM_14			BIT(14)
#define SE_GENI_CGC_CTRL		0x28
#define SE_GENI_CFG_SEQ_START		0x84
#define SE_GENI_CFG_STATUS		0x88
#define SE_UART_TX_TRANS_CFG		0x25C
#define CFG_SEQ_DONE			BIT(1)
#define SPI_CS_CLK_DL			0
#define SPI_PRE_POST_CMD_DLY		0

#define SE_SPI_CPHA			0x224
#define CPHA				BIT(0)
#define SE_SPI_CPOL			0x230
#define CPOL				BIT(2)
#define SPI_LSB_TO_MSB			0
#define SPI_MSB_TO_LSB			1

#define SE_SPI_TX_TRANS_LEN		0x26c
#define SE_SPI_RX_TRANS_LEN		0x270
#define TRANS_LEN_MSK			GENMASK(23, 0)

/* HRF FLOW Info */
#define HRF_ENTRY_OPCODE		3
#define HRF_ENTRY_TYPE			3
#define HRF_ENTRY_FLOW			0
#define HRF_ENTRY_PARITY		0
#define HRF_ENTRY_DATA_LEN		16 /* HRF entry always has DW=3 */

#define LRA_SINGLE_REG_LENGTH		4

/* M_CMD OP codes for Q2SPI */
#define Q2SPI_TX_ONLY			(1)
#define Q2SPI_RX_ONLY			(2)
#define Q2SPI_TX_RX			(7)

/* M_CMD params for Q2SPI */
#define PRE_CMD_DELAY			BIT(0)
#define TIMESTAMP_BEFORE		BIT(1)
#define TIMESTAMP_AFTER			BIT(3)
#define POST_CMD_DELAY			BIT(4)
#define Q2SPI_MODE			GENMASK(11, 8)
#define Q2SPI_MODE_SHIFT		8
#define SINGLE_SDR_MODE			0
#define Q2SPI_CMD			BIT(14)

#define CS_MODE				CS_LESS_MODE
#define Q2SPI_INTR_POL			INTR_HIGH_POLARITY

#define CR_BULK_DATA_size		1
#define CR_DMA_DATA_size		7

#define PINCTRL_DEFAULT "default"
#define PINCTRL_ACTIVE  "active"
#define PINCTRL_SLEEP   "sleep"

/* Max Minor devices */
#define MAX_DEV				2
#define DEVICE_NAME_MAX_LEN		64

#define QSPI_NUM_CS			2
#define QSPI_BYTES_PER_WORD		4

#define Q2SPI_INFO(q2spi_ptr, x...) do { \
if (q2spi_ptr) { \
	ipc_log_string(q2spi_ptr->ipc, x); \
	if (q2spi_ptr->dev) \
		q2spi_trace_log(q2spi_ptr->dev, x); \
	pr_info(x); \
} \
} while (0)

#define Q2SPI_DEBUG(q2spi_ptr, x...) do { \
if (q2spi_ptr) { \
	GENI_SE_DBG(q2spi_ptr->ipc, false, q2spi_ptr->dev, x); \
	if (q2spi_ptr->dev) \
		q2spi_trace_log(q2spi_ptr->dev, x); \
} \
} while (0)

#define Q2SPI_ERROR(q2spi_ptr, x...) do { \
if (q2spi_ptr) { \
	GENI_SE_ERR(q2spi_ptr->ipc, true, q2spi_ptr->dev, x); \
	if (q2spi_ptr->dev) \
		q2spi_trace_log(q2spi_ptr->dev, x); \
} \
} while (0)

#define DATA_BYTES_PER_LINE	(64)
#define Q2SPI_DATA_DUMP_SIZE	(16)

static unsigned int q2spi_max_speed;
/* global storage for device Major number */
static int q2spi_cdev_major;

enum abort_code {
	TERMINATE_CMD = 0,
	ERR_DUPLICATE_ID = 1,
	ERR_NOT_VALID = 2,
	ERR_ACCESS_BLOCKED = 3,
	ERR_DWLEN = 4,
	OTHERS = 5,
};

enum q2spi_pkt_in_use_state {
	IN_USE_FALSE = 0,
	IN_USE_TRUE = 1,
	IN_DELETION = 2,
	DELETED = 3,
};

struct q2spi_mc_hrf_entry {
	u8 cmd:4;
	u8 flow:1;
	u8 type:2;
	u8 parity:1;
	u8 resrv_0:4;
	u8 flow_id:4;
	u8 resrv_1:4;
	u8 dwlen_part1:4;
	u8 dwlen_part2:8;
	u8 dwlen_part3:8;
	u8 arg1:8;
	u8 arg2:8;
	u8 arg3:8;
	u8 reserved[8];
};

/**
 * struct q2spi_ch_header structure of cr header
 * @flow: flow direction of cr hdr, 1: CM flow, 0: MC flow
 */
struct q2spi_cr_header {
	u8 cmd:4;
	u8 flow:1;
	u8 type:2;
	u8 parity:1;
};

struct q2spi_client_bulk_access_pkt {
	u8 cmd:4;
	u8 flow:1;
	u8 rsvd:2;
	u8 parity:1;
	u8 status:4;
	u8 flow_id:4;
	u8 reserved[2];
};

struct q2spi_client_dma_pkt {
	u8 seg_len:4;
	u8 flow_id:4;
	u8 interrupt:1;
	u8 seg_last:1;
	u8 channel:2;
	u8 dw_len_part1:4;
	u8 dw_len_part2:8;
	u8 dw_len_part3:8;
	u8 arg1:8;
	u8 arg2:8;
	u8 arg3:8;
};

struct q2spi_host_variant1_pkt {
	u8 cmd:4;
	u8 flow:1;
	u8 interrupt:1;
	u8 seg_last:1;
	u8 rsvd:1;
	u8 dw_len:2;
	u8 access_type:1;
	u8 address_mode:1;
	u8 flow_id:4;
	u8 reg_offset;
	u8 reserved[4];
	u8 data_buf[16];
	u8 status;
};

struct q2spi_host_variant4_5_pkt {
	u8 cmd:4;
	u8 flow:1;
	u8 interrupt:1;
	u8 seg_last:1;
	u8 rsvd:1;
	u8 dw_len_part1:2;
	u8 access_type:1;
	u8 address_mode:1;
	u8 flow_id:4;
	u8 dw_len_part2;
	u8 rsvd_1[4];
	u8 data_buf[4096];
	u8 status;
};

struct q2spi_host_abort_pkt {
	u8 cmd:4;
	u8 rsvd:4;
	u8 code:4;
	u8 flow_id:4;
	u8 reserved[5];
};

struct q2spi_host_soft_reset_pkt {
	u8 cmd:4;
	u8 flags:4;
	u8 code:4;
	u8 rsvd:4;
	u8 rsvd_1[5];
};

enum cr_var_type {
	VARIANT_T_3 = 1, /* T:3 DMA CR type */
	VARIANT_T_4 = 2,
	VARIANT_T_5 = 3,
};

enum var_type {
	VARIANT_1_LRA = 1,
	VARIANT_1_HRF = 2,
	VARIANT_2 = 3,
	VARIANT_3 = 4,
	VARIANT_4 = 5,
	VARIANT_5 = 6,
	VARIANT_5_HRF = 7,
	VAR_ABORT = 8,
	VAR_SOFT_RESET = 9,
};

/**
 * struct q2spi_chrdev - structure for character device
 * q2spi_dev: q2spi device
 * @cdev: cdev pointer
 * @major: major number of q2spi device
 * @minor: minor number of q2spi device
 * @dev: basic device structure.
 * @dev_name: name of the device
 * @class_dev: pointer to char dev class
 * @q2spi_class: pointer to q2spi class
 */
struct q2spi_chrdev {
	dev_t q2spi_dev;
	struct cdev cdev[MAX_DEV];
	int major;
	int minor;
	struct device *dev;
	char dev_name[DEVICE_NAME_MAX_LEN];
	struct device *class_dev;
	struct class *q2spi_class;
};

/**
 * struct q2spi_dma_transfer - q2spi transfer dmadata
 * @tx_buf: TX data buffer
 * @rx_buf: RX data buffer
 * @tx_len: length of the Tx transfer
 * @rx_len: length of the rx transfer
 * @tx_dma: dma pointer for Tx transfer
 * @rx_dma: dma pointer for Rx transfer
 * @cmd: q2spi cmd type
 * @tid: Unique Transaction ID. Used for q2spi messages.
 * @queue: struct list head
 * @q2spi_pkt: pointer to q2spi_pkt
 */
struct q2spi_dma_transfer {
	void *tx_buf;
	void *rx_buf;
	unsigned int tx_len;
	unsigned int rx_len;
	unsigned int tx_data_len;
	unsigned int rx_data_len;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
	enum cmd_type cmd;
	int tid;
	struct list_head queue;
	struct q2spi_packet *q2spi_pkt;
};

/**
 * struct q2spi_geni - structure to store Q2SPI GENI information
 *
 * @wrapper_dev: qupv3 wrapper device pointer
 * @dev: q2spi device pointer
 * @base: pointer to ioremap()'d registers
 * @m_ahb_clk: master ahb clock for the controller
 * @s_ahb_clk: slave ahb clock for the controller
 * @se_clk: serial engine clock
 * @geni_pinctrl: pin-controller's instance
 * @geni_gpio_active: active state pin control
 * @geni_gpio_sleep: sleep state pin control
 * q2spi_chrdev: cdev structure
 * @geni_se: stores info parsed from device tree
 * @gsi: stores GSI structure information
 * @qup_gsi_err: flahg to set incase of gsi errors
 * @xfer: reference to q2spi_dma_transfer structure
 * @db_xfer: reference to q2spi_dma_transfer structure for doorbell
 * @req: reference to q2spi request structure
 * @c_req: reference to q2spi client request structure
 * @setup_config0: used to mark config0 setup completion
 * @irq: IRQ of the SE
 * @tx_queue_list: list for HC packets
 * @cr_queue_list: list for CR pakcets
 * @cr_hc_queue_list: list for CR doorbell received for HC
 * @kworker: kthread worker to process the q2spi requests
 * @send_messages: work function to process the q2spi requests
 * @gsi_lock: lock to protect gsi operations
 * @txn_lock: lock to protect transfer id allocation and free
 * @queue_lock: lock to protect HC operations
 * @cr_queue_lock: lock to protect CR operations
 * @max_speed_hz: stores maxspeed of the SCLK frequency
 * @cur_speed_hz: stores maxspeed of the SCLK frequency
 * @oversampling: stores sampling value based on major and minor version
 * @xfer_mode: stored mode of transfer
 * @curr_xfer_mode: stored current  mode of transfer
 * @gsi_mode: flag for gsi mode
 * @tx_cb: completion for tx dma
 * @rx_cb: completion for rx dma
 * @db_rx_cb: completion for doobell rx dma
 * @rx_avail: used to notify the client for avaialble rx data
 * @tid_idr: tid id allocator
 * @readq: waitqueue for rx data
 * @hrf_flow: flag to indicate HRF flow
 * @doorbell_up: completion for doorbell receive
 * @var1_buf: virtual pointer for varient1
 * @var1_dma_buf: physical dma pointer for varient1
 * @var5_buf: virtual pointer for varient5
 * @var5_dma_buf: physical dma pointer for varient5
 * @cr_buf: virtual pointer for CR
 * @cr_dma_buf: physical dma pointer for CR
 * @var1_buf_used: pointer to store varient1 buffer used
 * @var5_buf_used: pointer to store varient5 buffer used
 * @cr_buf_used: pointer to store CR buffer used
 * @sync_wait: completion for q2spi_transfer wait
 * @sma_wait: completion for SMA
 * @hw_state_is_bad: used when HW is in un-recoverable state
 * @max_dump_data_size: max size of data to be dumped as part of dump_ipc function
 * @doorbell_pending: Set when independent doorbell CR received
 * @retry: used when independent doorbell processing is pending to retry the request from host
 * @alloc_count: reflects count of memory allocations done by q2spi_kzalloc
 * @resources_on: flag which reflects geni resources are turned on/off
 * @port_release: reflects if q2spi port is being closed
 * @is_suspend: reflects if q2spi driver is in system suspend
 */
struct q2spi_geni {
	struct device *wrapper_dev;
	struct device *dev;
	void __iomem *base;
	struct clk *m_ahb_clk;
	struct clk *s_ahb_clk;
	struct clk *se_clk;
	struct pinctrl *geni_pinctrl;
	struct pinctrl_state *geni_gpio_active;
	struct pinctrl_state *geni_gpio_sleep;
	struct q2spi_chrdev chrdev;
	struct geni_se se;
	struct q2spi_gsi *gsi;
	bool qup_gsi_err;
	struct q2spi_dma_transfer *xfer;
	struct q2spi_dma_transfer *db_xfer;
	struct q2spi_request *req;
	struct q2spi_client_request *c_req;
	bool setup_config0;
	int irq;
	struct list_head tx_queue_list;
	struct list_head cr_queue_list;
	struct list_head hc_cr_queue_list;
	struct kthread_worker *kworker;
	struct kthread_work send_messages;
	/* lock to protect gsi operations one at a time */
	struct mutex gsi_lock;
	/* lock to protect transfer id allocation and free */
	spinlock_t txn_lock;
	/* lock to protect HC operations one at a time*/
	struct mutex queue_lock;
	/* lock to protect CR of operations one at a time*/
	spinlock_t cr_queue_lock;
	u32 max_speed_hz;
	u32 cur_speed_hz;
	int oversampling;
	int xfer_mode;
	int cur_xfer_mode;
	bool gsi_mode; /* GSI Mode */
	struct completion tx_cb;
	struct completion rx_cb;
	struct completion db_rx_cb;
	atomic_t rx_avail;
	struct idr tid_idr;
	wait_queue_head_t readq;
	void *rx_buf;
	dma_addr_t rx_dma;
	bool hrf_flow;
	struct completion doorbell_up;
	void *var1_buf[Q2SPI_MAX_BUF];
	dma_addr_t var1_dma_buf[Q2SPI_MAX_BUF];
	void *var5_buf[Q2SPI_MAX_BUF];
	dma_addr_t var5_dma_buf[Q2SPI_MAX_BUF];
	void *cr_buf[Q2SPI_MAX_BUF];
	dma_addr_t cr_dma_buf[Q2SPI_MAX_BUF];
	void *var1_buf_used[Q2SPI_MAX_BUF];
	void *var5_buf_used[Q2SPI_MAX_BUF];
	void *cr_buf_used[Q2SPI_MAX_BUF];
	void *bulk_buf[Q2SPI_MAX_BUF];
	dma_addr_t bulk_dma_buf[Q2SPI_MAX_BUF];
	void *bulk_buf_used[Q2SPI_MAX_BUF];
	dma_addr_t dma_buf;
	struct completion sync_wait;
	struct completion sma_wait;
	void *ipc;
	struct work_struct q2spi_doorbell_work;
	struct workqueue_struct *doorbell_wq;
	struct q2spi_cr_packet *cr_pkt;
	bool doorbell_setup;
	struct qup_q2spi_cr_header_event q2spi_cr_hdr_event;
	wait_queue_head_t read_wq;
	bool hw_state_is_bad;
	int max_data_dump_size;
	atomic_t doorbell_pending;
	atomic_t retry;
	atomic_t alloc_count;
	bool resources_on;
	bool port_release;
	bool is_suspend;
};

/**
 * struct q2spi_cr_packet - structure for Q2SPI CR packet
 *
 * @cr_hdr: array of q2spi_cr_header structures
 * @var3_pkt: pointer for q2spi_client_dma_pkt structure
 * @bulk_pkt: pointer for q2spi_client_bulk_access_pkt structure
 * @vtype: variant type
 * @hrf_flow_id: flow id used for transaction
 * @list: list for CR packets
 * @no_of_valid_crs: number of valid CRs in CR packet
 * @type: type of CR header corresponding to
 * @xfer: pointer to dma_transfer structure
 * @q2spi_pkt: pointer to q2spi_pkt
 */
struct q2spi_cr_packet {
	struct q2spi_cr_header cr_hdr[4];
	struct q2spi_client_dma_pkt var3_pkt; /* 4.2.2.3 Variant 4 T=3 */
	struct q2spi_client_bulk_access_pkt bulk_pkt; /* 4.2.2.5 Bulk Access Status */
	enum cr_var_type vtype;
	u8 hrf_flow_id;
	struct list_head list;
	int no_of_valid_crs;
	u8 type; /* 01 -> bulk, 02 -> var3  (01 10 10 01) */
	struct q2spi_dma_transfer *xfer;
	struct q2spi_packet *q2spi_pkt;
};

/**
 * struct q2spi_packet - structure for Q2SPI packet
 *
 * @m_cmd_param: cmd corresponding to q2spi_packet
 * @var1_pkt: pointer for HC variant1_pkt structure
 * @var4_pkt: pointer for HC_variant4_5_pkt structure
 * @var5_pkt: pointer for HC variant4_5_pkt structure
 * @abort_pkt: pointer for abort_pkt structure
 * @soft_reset_pkt: pointer for q2spi_soft_reset_pkt structure
 * @xfer: pointer to dma_transfer structure
 * @vtype: variant type.
 * @valid: packet valid or not.
 * @hrf_flow_id: flow id usedyy for transaction.
 * @status: success of failure xfer status
 * @var1_tx_dma: variant_1 tx_dma buffer pointer
 * @var5_tx_dma: variant_5 tx_dma buffer pointer
 * @soft_reset_tx_dma: soft_reset tx_dma buffer pointer
 * @sync: sync or async mode of transfer
 * @q2spi: pointer for q2spi_geni structure
 * @list: list for hc packets.
 * @in_use: Represents if packet is under use
 * @data_length: Represents data length of the packet transfer
 */
struct q2spi_packet {
	unsigned int m_cmd_param;
	struct q2spi_host_variant1_pkt *var1_pkt; /* 4.4.3.1 Variant 1 */
	struct q2spi_host_variant4_5_pkt *var4_pkt; /*4.4.3.3 Variant 4 */
	struct q2spi_host_variant4_5_pkt *var5_pkt; /*4.4.3.3 Variant 5 */
	struct q2spi_host_abort_pkt *abort_pkt; /* 4.4.4 Abort Command */
	struct q2spi_host_soft_reset_pkt *soft_reset_pkt; /*4.4.6.2 Soft Reset Command */
	struct q2spi_dma_transfer *xfer;
	enum var_type vtype;
	bool valid;
	u8 hrf_flow_id;
	enum xfer_status status;
	dma_addr_t var1_tx_dma;
	dma_addr_t var5_tx_dma;
	dma_addr_t soft_reset_tx_dma;
	bool sync;
	struct q2spi_geni *q2spi;
	struct list_head list;
	u8 in_use;
	unsigned int data_length;
};

void q2spi_doorbell(struct q2spi_geni *q2spi, const struct qup_q2spi_cr_header_event *event);
void q2spi_gsi_ch_ev_cb(struct dma_chan *ch, struct msm_gpi_cb const *cb, void *ptr);
void q2spi_geni_se_dump_regs(struct q2spi_geni *q2spi);
void q2spi_dump_ipc(struct q2spi_geni *q2spi, void *ipc_ctx, char *prefix, char *str, int size);
void q2spi_trace_log(struct device *dev, const char *fmt, ...);
void dump_ipc(struct q2spi_geni *q2spi, void *ctx, char *prefix, char *str, int size);
void *q2spi_kzalloc(struct q2spi_geni *q2spi, int size, int line);
void q2spi_kfree(struct q2spi_geni *q2spi, void *ptr, int line);
int q2spi_setup_gsi_xfer(struct q2spi_packet *q2spi_pkt);
int q2spi_alloc_xfer_tid(struct q2spi_geni *q2spi);
int q2spi_geni_gsi_setup(struct q2spi_geni *q2spi);
void q2spi_geni_gsi_release(struct q2spi_geni *q2spi);
int check_gsi_transfer_completion(struct q2spi_geni *q2spi);
int check_gsi_transfer_completion_rx(struct q2spi_geni *q2spi);
int q2spi_read_reg(struct q2spi_geni *q2spi, int reg_offset);
void q2spi_dump_client_error_regs(struct q2spi_geni *q2spi);
int q2spi_geni_resources_on(struct q2spi_geni *q2spi);
void q2spi_geni_resources_off(struct q2spi_geni *q2spi);

#endif /* _SPI_Q2SPI_MSM_H_ */
