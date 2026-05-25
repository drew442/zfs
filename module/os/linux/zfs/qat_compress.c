// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#if defined(_KERNEL) && defined(HAVE_QAT)
#include <linux/bitops.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <sys/zfs_context.h>
#include <sys/byteorder.h>
#include <sys/zio.h>
#include <sys/qat.h>
#include "icp/icp_sal_poll.h"

/*
 * Max instances in a QAT device, each instance is a channel to submit
 * jobs to QAT hardware, this is only for pre-allocating instance and
 * session arrays; the actual number of instances are defined in the
 * QAT driver's configuration file.
 */
#define	QAT_DC_MAX_INSTANCES	48
#define	QAT_DC_STACK_MAX_PAGES	\
	((QAT_DC_DEFAULT_MAX_BUF_SIZE >> PAGE_SHIFT) + 2)
#define	QAT_DC_ABS_MAX_PAGES	((QAT_DC_ABS_MAX_BUF_SIZE >> PAGE_SHIFT) + 2)
#define	QAT_DC_BUFFER_REUSE_SLOTS	32

/*
 * ZLIB head and foot size
 */
#define	ZLIB_HEAD_SZ		2
#define	ZLIB_FOOT_SZ		4
#define	QAT_DC_CALLBACK_MAGIC	0x51444342

typedef struct qat_dc_sync_req qat_dc_sync_req_t;

typedef struct qat_dc_buffer_slot {
	Cpa8U *buffer_meta_src;
	Cpa8U *buffer_meta_dst;
	CpaBufferList *buf_list_src;
	CpaBufferList *buf_list_dst;
	qat_dc_sync_req_t *sync_req;
	qat_dc_async_t *async_req;
	void *coalesced_dst;
	Cpa32U coalesced_dst_size;
	void *scratch;
	Cpa32U scratch_size;
	struct page **in_pages;
	Cpa32U in_pages_count;
	size_t in_pages_size;
	struct page **out_pages;
	Cpa32U out_pages_count;
	size_t out_pages_size;
	struct page **scratch_pages;
	Cpa32U scratch_pages_count;
	size_t scratch_pages_size;
} qat_dc_buffer_slot_t;

typedef struct qat_dc_buffer_pool {
	qat_dc_buffer_slot_t slots[QAT_DC_BUFFER_REUSE_SLOTS];
	unsigned long busy;
	Cpa32U max_src_bufs;
	Cpa32U max_dst_bufs;
	Cpa32U src_meta_size;
	Cpa32U dst_meta_size;
	Cpa32U src_list_size;
	Cpa32U dst_list_size;
	boolean_t initialized;
} qat_dc_buffer_pool_t;

typedef enum qat_dc_callback_type {
	QAT_DC_CALLBACK_SYNC = 1,
	QAT_DC_CALLBACK_ASYNC = 2,
} qat_dc_callback_type_t;

typedef enum qat_dc_async_cap_mode {
	QAT_DC_CAP_BALANCED = 0,
	QAT_DC_CAP_THROUGHPUT = 1,
	QAT_DC_CAP_OFFLOAD = 2,
} qat_dc_async_cap_mode_t;

typedef struct qat_dc_callback_ctx {
	uint32_t magic;
	qat_dc_callback_type_t type;
	union {
		qat_dc_sync_req_t *sync;
		qat_dc_async_t *async;
	} u;
} qat_dc_callback_ctx_t;

typedef enum qat_dc_sync_state {
	QAT_DC_SYNC_ACTIVE = 0,
	QAT_DC_SYNC_TIMED_OUT = 1,
	QAT_DC_SYNC_COMPLETED = 2,
} qat_dc_sync_state_t;

struct qat_dc_sync_req {
	struct completion complete;
	qat_dc_callback_ctx_t callback_ctx;
	qat_compress_dir_t dir;
	volatile uint32_t state;
	boolean_t recoverable_timeout;
	boolean_t retained;
	boolean_t from_slot;
	CpaStatus callback_status;
	CpaDcRqResults dc_results;
	hrtime_t wait_start;
	Cpa16U inst;
	qat_dc_buffer_slot_t *buffer_slot;
	Cpa8U *buffer_meta_src;
	Cpa8U *buffer_meta_dst;
	CpaBufferList *buf_list_src;
	CpaBufferList *buf_list_dst;
	void *coalesced_src;
	void *coalesced_dst;
	uint64_t retained_bytes;
	struct list_head retained_node;
};

struct qat_dc_async {
	char *src;
	char *dst;
	char *orig_dst;
	char *add;
	int src_len;
	int dst_len;
	size_t add_len;
	size_t c_len;
	Cpa16U inst;
	Cpa32U hdr_sz;
	CpaDcRqResults dc_results;
	CpaStatus submit_status;
	CpaStatus callback_status;
	volatile uint32_t complete;
	void (*resume)(void *);
	void *resume_arg;
	CpaBufferList *buf_list_src;
	CpaBufferList *buf_list_dst;
	qat_dc_buffer_slot_t *buffer_slot;
	Cpa8U *buffer_meta_src;
	Cpa8U *buffer_meta_dst;
	struct page **in_pages;
	struct page **out_pages;
	struct page **scratch_pages;
	struct page *in_pages_stack[QAT_DC_STACK_MAX_PAGES];
	struct page *out_pages_stack[QAT_DC_STACK_MAX_PAGES];
	struct page *scratch_pages_stack[QAT_DC_STACK_MAX_PAGES];
	Cpa32U src_pages;
	Cpa32U dst_pages;
	Cpa32U add_pages;
	size_t in_pages_size;
	size_t out_pages_size;
	size_t scratch_pages_size;
	void *coalesced_src;
	void *coalesced_dst;
	boolean_t add_from_slot;
	boolean_t from_slot;
	boolean_t in_pages_from_slot;
	boolean_t out_pages_from_slot;
	boolean_t scratch_pages_from_slot;
	hrtime_t submit_end;
	volatile uint32_t armed;
	boolean_t active;
	boolean_t timed_out;
	boolean_t abandoned;
	void (*abandon_cleanup)(void *);
	void *abandon_arg;
	struct list_head active_node;
	qat_dc_callback_ctx_t callback_ctx;
};

static CpaInstanceHandle dc_inst_handles[QAT_DC_MAX_INSTANCES];
static CpaDcSessionHandle session_handles[QAT_DC_MAX_INSTANCES];
static CpaBufferList **buffer_array[QAT_DC_MAX_INSTANCES];
static qat_dc_buffer_pool_t buffer_pools[QAT_DC_MAX_INSTANCES];
static Cpa16U num_inst = 0;
static Cpa32U inst_num = 0;
static boolean_t qat_dc_init_done = B_FALSE;
static struct task_struct *qat_dc_poll_task;
static struct task_struct *qat_dc_watchdog_task;
static DECLARE_WAIT_QUEUE_HEAD(qat_dc_poll_wq);
static DECLARE_WAIT_QUEUE_HEAD(qat_dc_watchdog_wq);
static LIST_HEAD(qat_dc_active_async_reqs);
static DEFINE_SPINLOCK(qat_dc_async_lock);
static int qat_dc_effective_decompress_disable(void);
static int qat_dc_effective_level(void);
static const char *qat_dc_effective_hufftype(void);
static int qat_dc_effective_min_buf_size(void);
static int qat_dc_effective_max_buf_size(void);
static int qat_dc_effective_coalesce_src(void);
static int qat_dc_effective_coalesce_dst(void);
static int qat_dc_effective_quarantine_dst(void);
static int qat_dc_effective_private_dst(void);
static int qat_dc_effective_async(void);
static int qat_dc_effective_async_submit_retries(void);
static int qat_dc_effective_async_retry_us(void);
static int qat_dc_effective_async_max_inflight(void);
static int qat_dc_effective_poll(void);
static int qat_dc_effective_poll_interval_us(void);
static int qat_dc_effective_poll_quota(void);
static int qat_dc_effective_watchdog(void);
static int qat_dc_effective_watchdog_timeout_ms(void);
static int qat_dc_effective_watchdog_interval_ms(void);
int zfs_qat_compress_disable = 0;
char *zfs_qat_decompress_disable = "profile";
static int zfs_qat_decompress_disable_value = 0;
char *zfs_qat_cpa_dc_level = "profile";
static int zfs_qat_cpa_dc_level_value = 1;
char *zfs_qat_cpa_dc_hufftype = "profile";
static const char *zfs_qat_cpa_dc_hufftype_value = "dynamic";
char *zfs_qat_dc_min_buf_size = "profile";
static int zfs_qat_dc_min_buf_size_value = QAT_DC_MIN_BUF_SIZE;
char *zfs_qat_dc_max_buf_size = "profile";
static int zfs_qat_dc_max_buf_size_value = QAT_DC_DEFAULT_MAX_BUF_SIZE;
int zfs_qat_dc_max_instances = QAT_DC_MAX_INSTANCES;
char *zfs_qat_dc_coalesce_src = "profile";
static int zfs_qat_dc_coalesce_src_value = 0;
char *zfs_qat_dc_coalesce_dst = "profile";
static int zfs_qat_dc_coalesce_dst_value = 0;
char *zfs_qat_dc_quarantine_dst = "profile";
static int zfs_qat_dc_quarantine_dst_value = 0;
char *zfs_qat_dc_async = "profile";
static int zfs_qat_dc_async_value = 0;
char *zfs_qat_dc_async_submit_retries = "profile";
static int zfs_qat_dc_async_submit_retries_value = 8;
char *zfs_qat_dc_async_retry_us = "profile";
static int zfs_qat_dc_async_retry_us_value = 100;
char *zfs_qat_dc_async_max_inflight = "profile";
static int zfs_qat_dc_async_max_inflight_value = 96;
char *zfs_qat_dc_async_cap_policy = "profile";
char *zfs_qat_dc_poll = "profile";
static int zfs_qat_dc_poll_value = 0;
char *zfs_qat_dc_poll_interval_us = "profile";
static int zfs_qat_dc_poll_interval_us_value = 0;
char *zfs_qat_dc_poll_quota = "profile";
static int zfs_qat_dc_poll_quota_value = 0;
char *zfs_qat_dc_watchdog = "profile";
static int zfs_qat_dc_watchdog_value = 1;
char *zfs_qat_dc_watchdog_timeout_ms = "profile";
static int zfs_qat_dc_watchdog_timeout_ms_value = 5000;
char *zfs_qat_dc_watchdog_interval_ms = "profile";
static int zfs_qat_dc_watchdog_interval_ms_value = 250;
char *zfs_qat_dc_profile = "balanced";
int zfs_qat_dc_profile_recordsize = 128 * 1024;
char *zfs_qat_dc_ratio_profile = "balanced";
char *zfs_qat_dc_expected_ratio = "unknown";
static uint32_t qat_dc_runtime_failed;
static uint64_t qat_dc_total_inflight;
static uint64_t qat_dc_inflight_start_ns;
static uint64_t qat_dc_last_progress_ns;
static LIST_HEAD(qat_dc_retained_sync_reqs);
static DEFINE_SPINLOCK(qat_dc_retained_lock);

boolean_t
qat_dc_compress_use_accel(size_t s_len)
{
	int min_buf_size = qat_dc_effective_min_buf_size();
	int max_buf_size = qat_dc_effective_max_buf_size();

	return (!zfs_qat_compress_disable &&
	    qat_dc_runtime_failed == 0 &&
	    qat_dc_init_done &&
	    s_len >= min_buf_size &&
	    s_len <= max_buf_size);
}

boolean_t
qat_dc_decompress_use_accel(size_t s_len)
{
	int min_buf_size = qat_dc_effective_min_buf_size();
	int max_buf_size = qat_dc_effective_max_buf_size();

	return (!zfs_qat_compress_disable &&
	    qat_dc_runtime_failed == 0 &&
	    !qat_dc_effective_decompress_disable() &&
	    qat_dc_init_done &&
	    s_len >= min_buf_size &&
	    s_len <= max_buf_size);
}

static boolean_t
qat_dc_valid_level(int level)
{
	return (level >= 1 && level <= 4);
}

static int
qat_dc_profile_level(const char *ratio_profile, const char *expected_ratio)
{
	(void) expected_ratio;

	if (strcmp(ratio_profile, "ratio") == 0)
		return (4);

	return (1);
}

static int
qat_dc_effective_level(void)
{
	if (strcmp(zfs_qat_cpa_dc_level, "profile") == 0)
		return (qat_dc_profile_level(zfs_qat_dc_ratio_profile,
		    zfs_qat_dc_expected_ratio));

	return (zfs_qat_cpa_dc_level_value);
}

static boolean_t
qat_dc_hufftype(const char *value, CpaDcHuffType *huff_type)
{
	if (strcmp(value, "static") == 0 || strcmp(value, "static\n") == 0) {
		*huff_type = CPA_DC_HT_STATIC;
		return (B_TRUE);
	}

	if (strcmp(value, "dynamic") == 0 ||
	    strcmp(value, "dynamic\n") == 0) {
		*huff_type = CPA_DC_HT_FULL_DYNAMIC;
		return (B_TRUE);
	}

	return (B_FALSE);
}

static boolean_t
qat_dc_valid_max_instances(int max_instances)
{
	return (max_instances >= 1 && max_instances <= QAT_DC_MAX_INSTANCES);
}

static boolean_t
qat_dc_valid_max_buf_size(int max_buf_size)
{
	switch (max_buf_size) {
	case 128 * 1024:
	case 256 * 1024:
	case 512 * 1024:
	case 1024 * 1024:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

static boolean_t
qat_dc_valid_min_buf_size(int min_buf_size)
{
	switch (min_buf_size) {
	case 8 * 1024:
	case 16 * 1024:
	case 32 * 1024:
	case 64 * 1024:
	case 128 * 1024:
	case 256 * 1024:
	case 512 * 1024:
	case 1024 * 1024:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

static Cpa32U
qat_dc_page_count(const void *data, Cpa32U len)
{
	unsigned long page_off;

	if (len == 0)
		return (0);

	page_off = (unsigned long)data & ~PAGE_MASK;
	return ((Cpa32U)((page_off + len + PAGE_SIZE - 1) >> PAGE_SHIFT));
}

static Cpa32U
qat_dc_worst_page_count(Cpa32U len)
{
	if (len == 0)
		return (0);

	return ((len >> PAGE_SHIFT) + 2);
}

static int
qat_dc_profile_min_buf_size(const char *dc_profile, const char *expected_ratio)
{
	if (strcmp(dc_profile, "latency") == 0 ||
	    strcmp(dc_profile, "throughput") == 0)
		return (512 * 1024);

	if (strcmp(dc_profile, "balanced") == 0 &&
	    (strcmp(expected_ratio, "low") == 0 ||
	    strcmp(expected_ratio, "medium") == 0))
		return (512 * 1024);

	return (QAT_DC_MIN_BUF_SIZE);
}

static int
qat_dc_effective_min_buf_size(void)
{
	if (strcmp(zfs_qat_dc_min_buf_size, "profile") == 0)
		return (qat_dc_profile_min_buf_size(zfs_qat_dc_profile,
		    zfs_qat_dc_expected_ratio));

	return (zfs_qat_dc_min_buf_size_value);
}

static int
qat_dc_profile_max_buf_size(const char *expected_ratio, int profile_recordsize)
{
	if (strcmp(expected_ratio, "medium") == 0)
		return (MIN(profile_recordsize, 512 * 1024));

	return (profile_recordsize);
}

static int
qat_dc_effective_max_buf_size(void)
{
	if (strcmp(zfs_qat_dc_max_buf_size, "profile") == 0)
		return (qat_dc_profile_max_buf_size(zfs_qat_dc_expected_ratio,
		    zfs_qat_dc_profile_recordsize));

	return (zfs_qat_dc_max_buf_size_value);
}

static boolean_t
qat_dc_profile(const char *value)
{
	return (strcmp(value, "balanced") == 0 ||
	    strcmp(value, "balanced\n") == 0 ||
	    strcmp(value, "latency") == 0 ||
	    strcmp(value, "latency\n") == 0 ||
	    strcmp(value, "throughput") == 0 ||
	    strcmp(value, "throughput\n") == 0 ||
	    strcmp(value, "offload") == 0 ||
	    strcmp(value, "offload\n") == 0);
}

static boolean_t
qat_dc_ratio_profile(const char *value)
{
	return (strcmp(value, "balanced") == 0 ||
	    strcmp(value, "balanced\n") == 0 ||
	    strcmp(value, "performance") == 0 ||
	    strcmp(value, "performance\n") == 0 ||
	    strcmp(value, "ratio") == 0 ||
	    strcmp(value, "ratio\n") == 0);
}

static boolean_t
qat_dc_expected_ratio(const char *value)
{
	return (strcmp(value, "unknown") == 0 ||
	    strcmp(value, "unknown\n") == 0 ||
	    strcmp(value, "low") == 0 ||
	    strcmp(value, "low\n") == 0 ||
	    strcmp(value, "medium") == 0 ||
	    strcmp(value, "medium\n") == 0 ||
	    strcmp(value, "high") == 0 ||
	    strcmp(value, "high\n") == 0);
}

static int
qat_dc_profile_decompress_disable(const char *dc_profile)
{
	if (strcmp(dc_profile, "latency") == 0 ||
	    strcmp(dc_profile, "throughput") == 0)
		return (1);

	return (0);
}

static int
qat_dc_effective_decompress_disable(void)
{
	if (strcmp(zfs_qat_decompress_disable, "profile") == 0)
		return (qat_dc_profile_decompress_disable(zfs_qat_dc_profile));

	return (zfs_qat_decompress_disable_value);
}

static const char *
qat_dc_profile_hufftype(const char *ratio_profile, const char *expected_ratio)
{
	if (strcmp(ratio_profile, "performance") == 0)
		return ("static");

	if (strcmp(ratio_profile, "balanced") == 0 &&
	    strcmp(expected_ratio, "low") == 0)
		return ("static");

	return ("dynamic");
}

static const char *
qat_dc_effective_hufftype(void)
{
	if (strcmp(zfs_qat_cpa_dc_hufftype, "profile") == 0)
		return (qat_dc_profile_hufftype(zfs_qat_dc_ratio_profile,
		    zfs_qat_dc_expected_ratio));

	return (zfs_qat_cpa_dc_hufftype_value);
}

static int
qat_dc_profile_coalesce_src(void)
{
	return (0);
}

static int
qat_dc_effective_coalesce_src(void)
{
	if (strcmp(zfs_qat_dc_coalesce_src, "profile") == 0)
		return (qat_dc_profile_coalesce_src());

	return (zfs_qat_dc_coalesce_src_value);
}

static int
qat_dc_profile_coalesce_dst(void)
{
	return (0);
}

static int
qat_dc_effective_coalesce_dst(void)
{
	if (strcmp(zfs_qat_dc_coalesce_dst, "profile") == 0)
		return (qat_dc_profile_coalesce_dst());

	return (zfs_qat_dc_coalesce_dst_value);
}

static int
qat_dc_profile_quarantine_dst(void)
{
	return (0);
}

static int
qat_dc_effective_quarantine_dst(void)
{
	if (strcmp(zfs_qat_dc_quarantine_dst, "profile") == 0)
		return (qat_dc_profile_quarantine_dst());

	return (zfs_qat_dc_quarantine_dst_value);
}

static int
qat_dc_effective_private_dst(void)
{
	return (qat_dc_effective_coalesce_dst() ||
	    qat_dc_effective_quarantine_dst());
}

static int
qat_dc_profile_async(const char *dc_profile)
{
	if (strcmp(dc_profile, "throughput") == 0 ||
	    strcmp(dc_profile, "offload") == 0)
		return (1);

	return (0);
}

static int
qat_dc_effective_async(void)
{
	if (qat_dc_effective_quarantine_dst())
		return (0);

	if (strcmp(zfs_qat_dc_async, "profile") == 0)
		return (qat_dc_profile_async(zfs_qat_dc_profile));

	return (zfs_qat_dc_async_value);
}

static int
qat_dc_profile_async_submit_retries(void)
{
	return (8);
}

static int
qat_dc_effective_async_submit_retries(void)
{
	if (strcmp(zfs_qat_dc_async_submit_retries, "profile") == 0)
		return (qat_dc_profile_async_submit_retries());

	return (zfs_qat_dc_async_submit_retries_value);
}

static int
qat_dc_profile_async_retry_us(void)
{
	return (100);
}

static int
qat_dc_effective_async_retry_us(void)
{
	if (strcmp(zfs_qat_dc_async_retry_us, "profile") == 0)
		return (qat_dc_profile_async_retry_us());

	return (zfs_qat_dc_async_retry_us_value);
}

static int
qat_dc_profile_async_max_inflight(void)
{
	return (96);
}

static int
qat_dc_effective_async_max_inflight(void)
{
	if (strcmp(zfs_qat_dc_async_max_inflight, "profile") == 0)
		return (qat_dc_profile_async_max_inflight());

	return (zfs_qat_dc_async_max_inflight_value);
}

static int
qat_dc_profile_poll(void)
{
	return (0);
}

static int
qat_dc_effective_poll(void)
{
	if (strcmp(zfs_qat_dc_poll, "profile") == 0)
		return (qat_dc_profile_poll());

	return (zfs_qat_dc_poll_value);
}

static int
qat_dc_profile_poll_interval_us(void)
{
	return (0);
}

static int
qat_dc_effective_poll_interval_us(void)
{
	if (strcmp(zfs_qat_dc_poll_interval_us, "profile") == 0)
		return (qat_dc_profile_poll_interval_us());

	return (zfs_qat_dc_poll_interval_us_value);
}

static int
qat_dc_profile_poll_quota(void)
{
	return (0);
}

static int
qat_dc_effective_poll_quota(void)
{
	if (strcmp(zfs_qat_dc_poll_quota, "profile") == 0)
		return (qat_dc_profile_poll_quota());

	return (zfs_qat_dc_poll_quota_value);
}

static int
qat_dc_profile_watchdog(void)
{
	return (1);
}

static int
qat_dc_effective_watchdog(void)
{
	if (strcmp(zfs_qat_dc_watchdog, "profile") == 0)
		return (qat_dc_profile_watchdog());

	return (zfs_qat_dc_watchdog_value);
}

static int
qat_dc_profile_watchdog_timeout_ms(void)
{
	return (5000);
}

static int
qat_dc_effective_watchdog_timeout_ms(void)
{
	if (strcmp(zfs_qat_dc_watchdog_timeout_ms, "profile") == 0)
		return (qat_dc_profile_watchdog_timeout_ms());

	return (zfs_qat_dc_watchdog_timeout_ms_value);
}

static int
qat_dc_profile_watchdog_interval_ms(void)
{
	return (250);
}

static int
qat_dc_effective_watchdog_interval_ms(void)
{
	if (strcmp(zfs_qat_dc_watchdog_interval_ms, "profile") == 0)
		return (qat_dc_profile_watchdog_interval_ms());

	return (zfs_qat_dc_watchdog_interval_ms_value);
}

static boolean_t
qat_dc_validate_poll_mode(void)
{
	boolean_t expected = qat_dc_effective_poll() ? B_TRUE : B_FALSE;

	for (Cpa16U i = 0; i < num_inst; i++) {
		CpaInstanceInfo2 instance_info = {0};
		CpaStatus status;
		boolean_t is_polled;

		status = cpaDcInstanceGetInfo2(dc_inst_handles[i],
		    &instance_info);
		if (status != CPA_STATUS_SUCCESS) {
			cmn_err(CE_WARN, "QAT DC instance %u poll mode query "
			    "failed with status %d", i, status);
			return (B_FALSE);
		}

		is_polled = (instance_info.isPolled == CPA_TRUE);
		if (is_polled != expected) {
			cmn_err(CE_WARN, "QAT DC instance %u poll mode mismatch: "
			    "driver=%s zfs=%s. Set DcNIsPolled and "
			    "zfs_qat_dc_poll to matching values before QAT DC "
			    "init", i, is_polled ? "poll" : "interrupt",
			    expected ? "poll" : "interrupt");
			return (B_FALSE);
		}
	}

	return (B_TRUE);
}

static boolean_t
qat_dc_valid_profile_recordsize(int recordsize)
{
	switch (recordsize) {
	case 128 * 1024:
	case 256 * 1024:
	case 512 * 1024:
	case 1024 * 1024:
		return (B_TRUE);
	default:
		return (B_FALSE);
	}
}

static boolean_t
qat_dc_async_valid_cap_policy(const char *value)
{
	return (strcmp(value, "profile") == 0 ||
	    strcmp(value, "profile\n") == 0 ||
	    strcmp(value, "fixed") == 0 ||
	    strcmp(value, "fixed\n") == 0 ||
	    strcmp(value, "recordsize") == 0 ||
	    strcmp(value, "recordsize\n") == 0 ||
	    strcmp(value, "throughput") == 0 ||
	    strcmp(value, "throughput\n") == 0);
}

static boolean_t
qat_dc_async_recordsize_cap(int src_len, int *cap,
    qat_dc_async_cap_mode_t mode)
{
	uint_t cap_instances;
	int per_inst_cap;
	boolean_t offload = (mode == QAT_DC_CAP_OFFLOAD);
	boolean_t throughput = (mode == QAT_DC_CAP_THROUGHPUT);

	if (src_len < 128 * 1024)
		return (B_FALSE);

	/*
	 * Base caps on active initialized DC instances, not card count. The
	 * balanced policy is intentionally capped at the measured DC6 ceiling:
	 * linear DC12 scaling increased QAT share but regressed latency in
	 * follow-up testing. The throughput profile raises only the measured
	 * 1M+ cap for hosts with more active DC instances.
	 */
	cap_instances = MIN(num_inst, 6);
	if (throughput && src_len >= 1024 * 1024)
		cap_instances = num_inst;
	if (offload && src_len >= 512 * 1024)
		cap_instances = num_inst;

	if (cap_instances == 0)
		return (B_FALSE);

	if (src_len == 128 * 1024) {
		per_inst_cap = 128;
	} else if (src_len == 256 * 1024) {
		per_inst_cap = 32;
	} else if (src_len >= 512 * 1024) {
		per_inst_cap = offload ? 32 : 16;
	} else {
		return (B_TRUE);
	}

	*cap = per_inst_cap * (int)cap_instances;
	return (B_TRUE);
}

static qat_dc_async_cap_mode_t
qat_dc_profile_cap_mode(void)
{
	if (strcmp(zfs_qat_dc_expected_ratio, "medium") == 0)
		return (QAT_DC_CAP_BALANCED);

	if (strcmp(zfs_qat_dc_profile, "offload") == 0 &&
	    zfs_qat_dc_profile_recordsize >= 512 * 1024)
		return (QAT_DC_CAP_OFFLOAD);

	if (strcmp(zfs_qat_dc_profile, "throughput") == 0 &&
	    zfs_qat_dc_profile_recordsize >= 1024 * 1024)
		return (QAT_DC_CAP_THROUGHPUT);

	return (QAT_DC_CAP_BALANCED);
}

static boolean_t
qat_dc_async_effective_cap(int src_len, int *cap)
{
	*cap = qat_dc_effective_async_max_inflight();

	if (strcmp(zfs_qat_dc_async_cap_policy, "profile") == 0) {
		return (qat_dc_async_recordsize_cap(src_len, cap,
		    qat_dc_profile_cap_mode()));
	}

	if (strcmp(zfs_qat_dc_async_cap_policy, "recordsize") == 0)
		return (qat_dc_async_recordsize_cap(src_len, cap,
		    QAT_DC_CAP_BALANCED));

	if (strcmp(zfs_qat_dc_async_cap_policy, "throughput") == 0)
		return (qat_dc_async_recordsize_cap(src_len, cap,
		    QAT_DC_CAP_THROUGHPUT));

	return (B_TRUE);
}

static CpaDcCompLvl
qat_dc_level(void)
{
	switch (qat_dc_effective_level()) {
	case 2:
		return (CPA_DC_L2);
	case 3:
		return (CPA_DC_L3);
	case 4:
		return (CPA_DC_L4);
	default:
		return (CPA_DC_L1);
	}
}

static CpaDcHuffType
qat_dc_selected_hufftype(void)
{
	CpaDcHuffType huff_type = CPA_DC_HT_FULL_DYNAMIC;

	(void) qat_dc_hufftype(qat_dc_effective_hufftype(), &huff_type);

	return (huff_type);
}

static size_t
qat_dc_compress_scratch_len(int src_len, int dst_len)
{
	Cpa32U bound = 0;
	Cpa32U qat_dst_len = 0;
	size_t add_len = 0;
	CpaStatus status;
	hrtime_t start;
	hrtime_t end;

	QAT_STAT_BUMP(dc_compress_bound_requests);
	QAT_STAT_INCR(dc_compress_dst_total_bytes, dst_len);

	start = gethrtime();
	status = cpaDcDeflateCompressBound(dc_inst_handles[0],
	    qat_dc_selected_hufftype(), src_len, &bound);
	end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_bound_ns, start, end);

	if (status != CPA_STATUS_SUCCESS) {
		QAT_STAT_BUMP(dc_compress_bound_fails);
		add_len = dst_len;
		goto out;
	}

	QAT_STAT_INCR(dc_compress_bound_total_bytes, bound);

	if (dst_len > ZLIB_HEAD_SZ)
		qat_dst_len = (Cpa32U)(dst_len - ZLIB_HEAD_SZ);

	if (bound > qat_dst_len)
		add_len = bound - qat_dst_len;

out:
	QAT_STAT_INCR(dc_compress_scratch_bytes, add_len);
	if ((size_t)dst_len > add_len)
		QAT_STAT_INCR(dc_compress_scratch_saved_bytes,
		    (size_t)dst_len - add_len);

	return (add_len);
}

static void qat_dc_inflight_exit(qat_compress_dir_t dir, hrtime_t now);
static void qat_dc_async_inflight_exit(void);
static void qat_dc_async_cleanup(qat_dc_async_t *req);
static void qat_dc_buffer_pool_release(Cpa16U inst, qat_dc_buffer_slot_t *slot);
static uint64_t qat_dc_inflight_total(void);
static void qat_dc_async_timeout_active(void);
static void qat_dc_runtime_disable(const char *reason, uint64_t inflight,
    uint64_t stall_ms);

static void
qat_dc_sync_req_prepare(qat_dc_sync_req_t *req, boolean_t from_slot)
{
	memset(req, 0, sizeof (*req));
	init_completion(&req->complete);
	INIT_LIST_HEAD(&req->retained_node);
	req->callback_ctx.magic = QAT_DC_CALLBACK_MAGIC;
	req->callback_ctx.type = QAT_DC_CALLBACK_SYNC;
	req->callback_ctx.u.sync = req;
	req->from_slot = from_slot;
}

static qat_dc_sync_req_t *
qat_dc_buffer_slot_sync_req(qat_dc_buffer_slot_t *slot)
{
	qat_dc_sync_req_t *req;

	if (slot == NULL)
		return (NULL);

	if (slot->sync_req == NULL) {
		req = kmem_alloc(sizeof (*req), KM_SLEEP);
		if (req == NULL)
			return (NULL);
		slot->sync_req = req;
	}

	qat_dc_sync_req_prepare(slot->sync_req, B_TRUE);
	return (slot->sync_req);
}

static void
qat_dc_async_req_prepare(qat_dc_async_t *req, boolean_t from_slot)
{
	memset(req, 0, offsetof(qat_dc_async_t, in_pages_stack));
	/*
	 * The embedded stack page arrays are overwritten up to the published
	 * page counts before use. Avoid clearing them on each slot reuse.
	 */
	memset(&req->src_pages, 0, sizeof (*req) -
	    offsetof(qat_dc_async_t, src_pages));
	req->dc_results.checksum = 1;
	req->submit_status = CPA_STATUS_FAIL;
	INIT_LIST_HEAD(&req->active_node);
	req->callback_ctx.magic = QAT_DC_CALLBACK_MAGIC;
	req->callback_ctx.type = QAT_DC_CALLBACK_ASYNC;
	req->callback_ctx.u.async = req;
	req->from_slot = from_slot;
}

static qat_dc_async_t *
qat_dc_buffer_slot_async_req(qat_dc_buffer_slot_t *slot)
{
	qat_dc_async_t *req;

	if (slot == NULL)
		return (NULL);

	if (slot->async_req == NULL) {
		req = kmem_alloc(sizeof (*req), KM_SLEEP);
		if (req == NULL)
			return (NULL);
		slot->async_req = req;
	}

	qat_dc_async_req_prepare(slot->async_req, B_TRUE);
	return (slot->async_req);
}

static void
qat_dc_sync_req_unretain(qat_dc_sync_req_t *req, boolean_t bump_release)
{
	unsigned long flags;

	if (req == NULL)
		return;

	if (!req->retained)
		return;

	spin_lock_irqsave(&qat_dc_retained_lock, flags);
	if (!list_empty(&req->retained_node))
		list_del_init(&req->retained_node);
	if (qat_stats.dc_compress_quarantine_dst_retained.value.ui64 > 0) {
		(void) atomic_dec_64_nv(
		    &qat_stats.dc_compress_quarantine_dst_retained.value.ui64);
	}
	if (qat_stats.dc_compress_quarantine_dst_retained_bytes.value.ui64 >=
	    req->retained_bytes) {
		qat_stats.dc_compress_quarantine_dst_retained_bytes.value.ui64 -=
		    req->retained_bytes;
	} else {
		qat_stats.dc_compress_quarantine_dst_retained_bytes.value.ui64 = 0;
	}
	spin_unlock_irqrestore(&qat_dc_retained_lock, flags);

	req->retained = B_FALSE;
	if (bump_release)
		QAT_STAT_BUMP(dc_compress_quarantine_dst_retained_released);
}

static void
qat_dc_sync_req_free_retained(qat_dc_sync_req_t *req)
{
	qat_dc_buffer_slot_t *slot;
	Cpa16U inst = 0;
	boolean_t from_slot;

	if (req == NULL)
		return;

	qat_dc_sync_req_unretain(req, B_TRUE);
	slot = req->buffer_slot;
	inst = req->inst;
	from_slot = req->from_slot;

	if (slot != NULL) {
		QAT_PHYS_CONTIG_FREE(req->coalesced_src);
		qat_dc_buffer_pool_release(inst, slot);
	} else {
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_src);
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_dst);
		QAT_PHYS_CONTIG_FREE(req->buf_list_src);
		QAT_PHYS_CONTIG_FREE(req->buf_list_dst);
		QAT_PHYS_CONTIG_FREE(req->coalesced_dst);
		QAT_PHYS_CONTIG_FREE(req->coalesced_src);
	}

	if (!from_slot)
		kmem_free(req, sizeof (*req));
}

static void
qat_dc_sync_req_retain(qat_dc_sync_req_t *req, uint64_t retained_bytes)
{
	unsigned long flags;

	ASSERT(!req->retained);
	req->retained = B_TRUE;
	req->retained_bytes = retained_bytes;
	spin_lock_irqsave(&qat_dc_retained_lock, flags);
	list_add_tail(&req->retained_node, &qat_dc_retained_sync_reqs);
	QAT_STAT_BUMP(dc_compress_quarantine_dst_retained);
	QAT_STAT_INCR(dc_compress_quarantine_dst_retained_bytes,
	    retained_bytes);
	spin_unlock_irqrestore(&qat_dc_retained_lock, flags);
}

static void
qat_dc_sync_req_drain_retained(void)
{
	LIST_HEAD(to_free);
	qat_dc_sync_req_t *req;
	qat_dc_sync_req_t *tmp;
	unsigned long flags;

	spin_lock_irqsave(&qat_dc_retained_lock, flags);
	list_splice_init(&qat_dc_retained_sync_reqs, &to_free);
	spin_unlock_irqrestore(&qat_dc_retained_lock, flags);

	list_for_each_entry_safe(req, tmp, &to_free, retained_node) {
		list_del_init(&req->retained_node);
		qat_dc_sync_req_free_retained(req);
	}
}

static void
qat_dc_async_active_add(qat_dc_async_t *req)
{
	unsigned long flags;

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	if (!req->active) {
		list_add_tail(&req->active_node, &qat_dc_active_async_reqs);
		req->active = B_TRUE;
	}
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);
}

static void
qat_dc_async_active_remove_locked(qat_dc_async_t *req)
{
	if (req->active) {
		list_del_init(&req->active_node);
		req->active = B_FALSE;
	}
}

static void
qat_dc_async_active_remove(qat_dc_async_t *req)
{
	unsigned long flags;

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	qat_dc_async_active_remove_locked(req);
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);
}

static void
qat_dc_async_abandoned_cleanup_task(void *arg)
{
	qat_dc_async_t *req = arg;
	void (*cleanup)(void *);
	void *cleanup_arg;

	cleanup = req->abandon_cleanup;
	cleanup_arg = req->abandon_arg;
	if (cleanup != NULL)
		cleanup(cleanup_arg);

	qat_dc_async_cleanup(req);
}

static void
qat_dc_callback(void *p_callback, CpaStatus status)
{
	qat_dc_callback_ctx_t *ctx = p_callback;
	qat_dc_sync_req_t *sync_req;
	qat_dc_async_t *req;
	hrtime_t end;
	void (*resume)(void *) = NULL;
	void *resume_arg = NULL;
	boolean_t abandoned;
	unsigned long flags;

	if (ctx == NULL || ctx->magic != QAT_DC_CALLBACK_MAGIC)
		return;

	if (ctx->type == QAT_DC_CALLBACK_SYNC) {
		sync_req = ctx->u.sync;
		sync_req->callback_status = status;
		end = gethrtime();
		qat_dc_inflight_exit(sync_req->dir, end);
		if (sync_req->dir == QAT_COMPRESS) {
			QAT_STAT_ADD_TIME(dc_compress_wait_ns,
			    sync_req->wait_start, end);
		} else {
			QAT_STAT_ADD_TIME(dc_decompress_wait_ns,
			    sync_req->wait_start, end);
		}
		if (atomic_cas_32((uint32_t *)&sync_req->state,
		    QAT_DC_SYNC_ACTIVE, QAT_DC_SYNC_COMPLETED) ==
		    QAT_DC_SYNC_ACTIVE) {
			complete(&sync_req->complete);
		} else if (atomic_cas_32((uint32_t *)&sync_req->state,
		    QAT_DC_SYNC_TIMED_OUT, QAT_DC_SYNC_COMPLETED) ==
		    QAT_DC_SYNC_TIMED_OUT) {
			QAT_STAT_BUMP(dc_watchdog_late_completions);
			qat_dc_sync_req_free_retained(sync_req);
		}
		return;
	}

	req = ctx->u.async;
	req->callback_status = status;
	end = gethrtime();
	qat_dc_inflight_exit(QAT_COMPRESS, end);
	qat_dc_async_inflight_exit();
	QAT_STAT_ADD_TIME(dc_compress_wait_ns, req->submit_end, end);
	QAT_STAT_BUMP(dc_compress_async_completions);

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	req->complete = 1;
	qat_dc_async_active_remove_locked(req);
	abandoned = req->abandoned;
	if (!abandoned && req->armed && req->resume != NULL) {
		resume = req->resume;
		resume_arg = req->resume_arg;
	}
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);

	if (abandoned) {
		QAT_STAT_BUMP(dc_watchdog_late_completions);
		if (taskq_dispatch(system_taskq,
		    qat_dc_async_abandoned_cleanup_task, req, TQ_NOSLEEP) ==
		    TASKQID_INVALID)
			qat_dc_async_abandoned_cleanup_task(req);
		return;
	}

	if (resume != NULL) {
		QAT_STAT_BUMP(dc_compress_async_resumes);
		resume(resume_arg);
	}
}

static void
qat_dc_poll_instances(Cpa32U quota)
{
	CpaStatus poll_status;
	hrtime_t start;
	hrtime_t end;

	for (Cpa16U i = 0; i < num_inst; i++) {
		start = gethrtime();
		poll_status = icp_sal_DcPollInstance(dc_inst_handles[i], quota);
		end = gethrtime();
		QAT_STAT_BUMP(dc_poll_calls);
		QAT_STAT_ADD_TIME(dc_poll_ns, start, end);

		switch (poll_status) {
		case CPA_STATUS_SUCCESS:
			QAT_STAT_BUMP(dc_poll_success);
			break;
		case CPA_STATUS_RETRY:
			QAT_STAT_BUMP(dc_poll_retries);
			break;
		default:
			QAT_STAT_BUMP(dc_poll_fails);
			break;
		}
	}
}

static boolean_t
qat_dc_wait_request(qat_dc_sync_req_t *req, uint64_t retained_bytes)
{
	unsigned long remaining;
	unsigned long timeout;
	hrtime_t now;

	if (qat_dc_effective_poll())
		wake_up(&qat_dc_poll_wq);

	if (!qat_dc_effective_watchdog()) {
		wait_for_completion(&req->complete);
		return (B_FALSE);
	}

	timeout = MSEC_TO_TICK(qat_dc_effective_watchdog_timeout_ms());
	if (timeout < 1)
		timeout = 1;

	remaining = wait_for_completion_timeout(&req->complete, timeout);
	if (remaining != 0)
		return (B_FALSE);

	QAT_STAT_BUMP(dc_watchdog_request_timeouts);
	now = gethrtime();
	qat_dc_runtime_disable("QAT DC request timeout",
	    qat_dc_inflight_total(),
	    ((uint64_t)now - (uint64_t)req->wait_start) /
	    (NANOSEC / MILLISEC));

	if (!req->recoverable_timeout) {
		QAT_STAT_BUMP(dc_watchdog_request_unrecoverable);
		wait_for_completion(&req->complete);
		return (B_FALSE);
	}

	qat_dc_sync_req_retain(req, retained_bytes);
	if (atomic_cas_32((uint32_t *)&req->state, QAT_DC_SYNC_ACTIVE,
	    QAT_DC_SYNC_TIMED_OUT) != QAT_DC_SYNC_ACTIVE) {
		qat_dc_sync_req_unretain(req, B_FALSE);
		return (B_FALSE);
	}

	QAT_STAT_BUMP(dc_watchdog_request_recoveries);
	return (B_TRUE);
}

static uint64_t
qat_dc_inflight_total(void)
{
	return (qat_dc_total_inflight);
}

static void
qat_dc_runtime_progress(hrtime_t now)
{
	atomic_swap_64(&qat_dc_last_progress_ns, (uint64_t)now);
	qat_stats.dc_watchdog_last_progress_ns.value.ui64 = (uint64_t)now;
}

static void
qat_dc_runtime_disable(const char *reason, uint64_t inflight,
    uint64_t stall_ms)
{
	hrtime_t now = gethrtime();

	if (atomic_cas_32(&qat_dc_runtime_failed, 0, 1) != 0)
		return;

	zfs_qat_compress_disable = 1;
	qat_stats.dc_watchdog_health.value.ui64 = 0;
	qat_stats.dc_watchdog_last_stall_ns.value.ui64 = (uint64_t)now;
	QAT_STAT_BUMP(dc_watchdog_runtime_disables);
	cmn_err(CE_WARN, "QAT DC watchdog disabled new QAT DC submissions: "
	    "%s, inflight=%llu, stalled_ms=%llu. Existing requests are left "
	    "to complete because timed-out QAT DMA cannot safely fall back "
	    "into the same output buffer", reason,
	    (u_longlong_t)inflight, (u_longlong_t)stall_ms);
}

static void
qat_dc_async_timeout_active(void)
{
	qat_dc_async_t *req;
	void (*resume)(void *);
	void *resume_arg;
	unsigned long flags;

	for (;;) {
		resume = NULL;
		resume_arg = NULL;

		spin_lock_irqsave(&qat_dc_async_lock, flags);
		list_for_each_entry(req, &qat_dc_active_async_reqs, active_node) {
			if (!req->complete && !req->timed_out &&
			    !req->abandoned && req->resume != NULL) {
				req->timed_out = B_TRUE;
				resume = req->resume;
				resume_arg = req->resume_arg;
				req->resume = NULL;
				req->resume_arg = NULL;
				QAT_STAT_BUMP(dc_watchdog_request_timeouts);
				break;
			}
		}
		spin_unlock_irqrestore(&qat_dc_async_lock, flags);

		if (resume == NULL)
			break;

		QAT_STAT_BUMP(dc_compress_async_resumes);
		resume(resume_arg);
	}
}

static void
qat_dc_watchdog_check(void)
{
	uint64_t inflight;
	uint64_t start;
	uint64_t last;
	uint64_t timeout_ns;
	hrtime_t now;

	if (!qat_dc_effective_watchdog() || qat_dc_runtime_failed != 0)
		return;

	QAT_STAT_BUMP(dc_watchdog_checks);
	inflight = qat_dc_inflight_total();
	if (inflight == 0)
		return;

	start = qat_dc_inflight_start_ns;
	last = qat_dc_last_progress_ns;
	if (start == 0 || last == 0)
		return;

	now = gethrtime();
	timeout_ns = MSEC2NSEC(qat_dc_effective_watchdog_timeout_ms());
	if ((uint64_t)now - start <= timeout_ns ||
	    (uint64_t)now - last <= timeout_ns) {
		return;
	}

	QAT_STAT_BUMP(dc_watchdog_stalls);
	qat_dc_runtime_disable("no QAT DC completion progress",
	    inflight, ((uint64_t)now - last) / (NANOSEC / MILLISEC));
	qat_dc_async_timeout_active();
}

static int
qat_dc_watchdog_thread(void *arg)
{
	(void) arg;

	while (!kthread_should_stop()) {
		int interval_ms = qat_dc_effective_watchdog_interval_ms();

		if (!qat_dc_effective_watchdog())
			interval_ms = 1000;
		if (interval_ms < 1)
			interval_ms = 1;

		(void) wait_event_interruptible_timeout(qat_dc_watchdog_wq,
		    kthread_should_stop(), MSEC_TO_TICK(interval_ms));

		if (!kthread_should_stop())
			qat_dc_watchdog_check();
	}

	return (0);
}

static int
qat_dc_watchdog_start(void)
{
	qat_dc_watchdog_task = kthread_run(qat_dc_watchdog_thread, NULL,
	    "zfs_qat_dc_watchdog");
	if (IS_ERR(qat_dc_watchdog_task)) {
		qat_dc_watchdog_task = NULL;
		return (-1);
	}

	return (0);
}

static void
qat_dc_watchdog_stop(void)
{
	if (qat_dc_watchdog_task != NULL) {
		kthread_stop(qat_dc_watchdog_task);
		qat_dc_watchdog_task = NULL;
	}
}

static int
qat_dc_poll_thread(void *arg)
{
	(void) arg;

	while (!kthread_should_stop()) {
		Cpa32U quota = (Cpa32U)qat_dc_effective_poll_quota();
		int interval_us = qat_dc_effective_poll_interval_us();

		if (!qat_dc_effective_poll() || qat_dc_inflight_total() == 0) {
			wait_event_interruptible(qat_dc_poll_wq,
			    kthread_should_stop() ||
			    (qat_dc_effective_poll() &&
			    qat_dc_inflight_total() > 0));
			continue;
		}

		qat_dc_poll_instances(quota);

		if (interval_us > 0) {
			usleep_range(interval_us, interval_us + 10);
		} else {
			cpu_relax();
			cond_resched();
		}
	}

	return (0);
}

static int
qat_dc_poll_start(void)
{
	if (!qat_dc_effective_poll())
		return (0);

	qat_dc_poll_task = kthread_run(qat_dc_poll_thread, NULL,
	    "zfs_qat_dc_poll");
	if (IS_ERR(qat_dc_poll_task)) {
		qat_dc_poll_task = NULL;
		return (-1);
	}

	return (0);
}

static void
qat_dc_poll_stop(void)
{
	if (qat_dc_poll_task != NULL) {
		kthread_stop(qat_dc_poll_task);
		qat_dc_poll_task = NULL;
	}
}

static void
qat_dc_update_stat_max(kstat_named_t *max_stat, uint64_t value)
{
	uint64_t max;

	for (;;) {
		max = max_stat->value.ui64;
		if (value <= max)
			return;
		if (atomic_cas_64(&max_stat->value.ui64, max, value) == max)
			return;
	}
}

static void
qat_dc_stat_bump(kstat_named_t *stat)
{
	atomic_add_64(&stat->value.ui64, 1);
}

static void
qat_dc_record_page_array_path(boolean_t used, boolean_t heap,
    kstat_named_t *stack_stat, kstat_named_t *heap_stat)
{
	if (!used)
		return;

	qat_dc_stat_bump(heap ? heap_stat : stack_stat);
}

static void
qat_dc_inflight_enter(qat_compress_dir_t dir, hrtime_t now)
{
	uint64_t inflight;
	uint64_t total_inflight;

	if (dir == QAT_COMPRESS) {
		inflight = atomic_inc_64_nv(
		    &qat_stats.dc_compress_inflight.value.ui64);
		qat_dc_update_stat_max(&qat_stats.dc_compress_inflight_max,
		    inflight);
	} else {
		inflight = atomic_inc_64_nv(
		    &qat_stats.dc_decompress_inflight.value.ui64);
		qat_dc_update_stat_max(
		    &qat_stats.dc_decompress_inflight_max, inflight);
	}

	total_inflight = atomic_inc_64_nv(&qat_dc_total_inflight);
	if (total_inflight == 1) {
		atomic_swap_64(&qat_dc_inflight_start_ns, (uint64_t)now);
		qat_dc_runtime_progress(now);
	}

	if (qat_dc_effective_poll())
		wake_up(&qat_dc_poll_wq);
}

static void
qat_dc_inflight_exit(qat_compress_dir_t dir, hrtime_t now)
{
	uint64_t total_inflight;

	if (dir == QAT_COMPRESS) {
		(void) atomic_dec_64_nv(
		    &qat_stats.dc_compress_inflight.value.ui64);
	} else {
		(void) atomic_dec_64_nv(
		    &qat_stats.dc_decompress_inflight.value.ui64);
	}

	qat_dc_runtime_progress(now);
	total_inflight = atomic_dec_64_nv(&qat_dc_total_inflight);
	if (total_inflight == 0)
		atomic_swap_64(&qat_dc_inflight_start_ns, 0);
}

static void
qat_dc_record_compress_shape(Cpa32U src_buffers, Cpa32U dst_buffers,
    Cpa32U add_buffers)
{
	Cpa32U dst_total_buffers = dst_buffers + add_buffers;

	QAT_STAT_INCR(dc_compress_src_buffers, src_buffers);
	QAT_STAT_INCR(dc_compress_dst_buffers, dst_buffers);
	QAT_STAT_INCR(dc_compress_add_buffers, add_buffers);
	QAT_STAT_INCR(dc_compress_dst_total_buffers, dst_total_buffers);

	qat_dc_update_stat_max(&qat_stats.dc_compress_src_buffers_max,
	    src_buffers);
	qat_dc_update_stat_max(&qat_stats.dc_compress_dst_buffers_max,
	    dst_buffers);
	qat_dc_update_stat_max(&qat_stats.dc_compress_add_buffers_max,
	    add_buffers);
	qat_dc_update_stat_max(&qat_stats.dc_compress_dst_total_buffers_max,
	    dst_total_buffers);
}

typedef struct qat_dc_buffer_shape {
	Cpa32U src_unaligned_64;
	Cpa32U src_len_not_64;
	Cpa32U dst_unaligned_64;
	Cpa32U dst_len_not_64;
	Cpa32U add_unaligned_64;
	Cpa32U add_len_not_64;
	uint64_t src_first_bytes;
	uint64_t src_last_bytes;
	uint64_t dst_first_bytes;
	uint64_t dst_last_bytes;
	uint64_t add_first_bytes;
	uint64_t add_last_bytes;
} qat_dc_buffer_shape_t;

static void
qat_dc_note_flat_buffer(CpaFlatBuffer *buf, Cpa32U *unaligned_64,
    Cpa32U *len_not_64, uint64_t *first_bytes, uint64_t *last_bytes,
    boolean_t first)
{
	if ((((uintptr_t)buf->pData) & 63) != 0)
		(*unaligned_64)++;
	if ((buf->dataLenInBytes & 63) != 0)
		(*len_not_64)++;
	if (first)
		*first_bytes = buf->dataLenInBytes;
	*last_bytes = buf->dataLenInBytes;
}

static void
qat_dc_record_compress_buffer_shape(qat_dc_buffer_shape_t *shape)
{
	QAT_STAT_INCR(dc_compress_src_buf_unaligned_64,
	    shape->src_unaligned_64);
	QAT_STAT_INCR(dc_compress_src_buf_len_not_64,
	    shape->src_len_not_64);
	QAT_STAT_INCR(dc_compress_src_first_bytes,
	    shape->src_first_bytes);
	QAT_STAT_INCR(dc_compress_src_last_bytes,
	    shape->src_last_bytes);
	QAT_STAT_INCR(dc_compress_dst_buf_unaligned_64,
	    shape->dst_unaligned_64);
	QAT_STAT_INCR(dc_compress_dst_buf_len_not_64,
	    shape->dst_len_not_64);
	QAT_STAT_INCR(dc_compress_dst_first_bytes,
	    shape->dst_first_bytes);
	QAT_STAT_INCR(dc_compress_dst_last_bytes,
	    shape->dst_last_bytes);
	QAT_STAT_INCR(dc_compress_add_buf_unaligned_64,
	    shape->add_unaligned_64);
	QAT_STAT_INCR(dc_compress_add_buf_len_not_64,
	    shape->add_len_not_64);
	QAT_STAT_INCR(dc_compress_add_first_bytes,
	    shape->add_first_bytes);
	QAT_STAT_INCR(dc_compress_add_last_bytes,
	    shape->add_last_bytes);
}

static boolean_t
qat_dc_try_coalesce_src(char **src, int src_len, void **coalesced_src,
    boolean_t force)
{
	CpaStatus status;
	hrtime_t start;
	hrtime_t end;

	if (!force && !qat_dc_effective_coalesce_src())
		return (B_FALSE);

	QAT_STAT_BUMP(dc_compress_coalesce_requests);

	start = gethrtime();
	status = QAT_PHYS_CONTIG_ALLOC(coalesced_src, src_len);
	end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_coalesce_alloc_ns, start, end);

	if (status != CPA_STATUS_SUCCESS) {
		QAT_STAT_BUMP(dc_compress_coalesce_fails);
		*coalesced_src = NULL;
		return (B_FALSE);
	}

	start = gethrtime();
	memcpy(*coalesced_src, *src, src_len);
	end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_coalesce_copy_ns, start, end);

	*src = *coalesced_src;
	QAT_STAT_BUMP(dc_compress_coalesce_success);
	QAT_STAT_INCR(dc_compress_coalesce_bytes, src_len);

	return (B_TRUE);
}

static CpaStatus
qat_dc_alloc_dst_coalesce_buffer(void **coalesced_dst, Cpa32U alloc_len)
{
	CpaStatus status;
	hrtime_t start;
	hrtime_t end;

	start = gethrtime();
	status = QAT_PHYS_CONTIG_ALLOC(coalesced_dst, alloc_len);
	end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_alloc_ns, start, end);

	if (status == CPA_STATUS_SUCCESS)
		QAT_STAT_INCR(dc_compress_dst_coalesce_alloc_bytes,
		    alloc_len);

	return (status);
}

static boolean_t
qat_dc_try_coalesce_dst(char **dst, int dst_len, int add_len,
    qat_dc_buffer_slot_t *buffer_slot, void **coalesced_dst)
{
	void *new_dst = NULL;
	CpaStatus status;
	Cpa32U alloc_len;
	hrtime_t start;
	hrtime_t end;

	if (!qat_dc_effective_private_dst())
		return (B_FALSE);

	QAT_STAT_BUMP(dc_compress_dst_coalesce_requests);

	if (add_len < 0 || dst_len < 0) {
		QAT_STAT_BUMP(dc_compress_dst_coalesce_fails);
		*coalesced_dst = NULL;
		return (B_FALSE);
	}
	alloc_len = (Cpa32U)(dst_len + add_len);

	if (buffer_slot != NULL) {
		if (buffer_slot->coalesced_dst != NULL &&
		    buffer_slot->coalesced_dst_size >= alloc_len) {
			*dst = buffer_slot->coalesced_dst;
			*coalesced_dst = NULL;
			QAT_STAT_BUMP(dc_compress_dst_coalesce_reuse_hits);
			QAT_STAT_BUMP(dc_compress_dst_coalesce_success);
			return (B_TRUE);
		}

		QAT_STAT_BUMP(dc_compress_dst_coalesce_reuse_misses);
		status = qat_dc_alloc_dst_coalesce_buffer(&new_dst, alloc_len);
		if (status != CPA_STATUS_SUCCESS) {
			QAT_STAT_BUMP(dc_compress_dst_coalesce_fails);
			*coalesced_dst = NULL;
			return (B_FALSE);
		}

		if (buffer_slot->coalesced_dst != NULL) {
			start = gethrtime();
			QAT_PHYS_CONTIG_FREE(buffer_slot->coalesced_dst);
			end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_free_ns,
			    start, end);
		}

		buffer_slot->coalesced_dst = new_dst;
		buffer_slot->coalesced_dst_size = alloc_len;
		*dst = buffer_slot->coalesced_dst;
		*coalesced_dst = NULL;
		QAT_STAT_BUMP(dc_compress_dst_coalesce_success);
		return (B_TRUE);
	}

	QAT_STAT_BUMP(dc_compress_dst_coalesce_reuse_misses);
	status = qat_dc_alloc_dst_coalesce_buffer(coalesced_dst, alloc_len);
	if (status != CPA_STATUS_SUCCESS) {
		QAT_STAT_BUMP(dc_compress_dst_coalesce_fails);
		*coalesced_dst = NULL;
		return (B_FALSE);
	}

	*dst = *coalesced_dst;
	QAT_STAT_BUMP(dc_compress_dst_coalesce_success);

	return (B_TRUE);
}

static void *
qat_dc_buffer_slot_scratch(qat_dc_buffer_slot_t *slot, Cpa32U add_len)
{
	void *scratch;
	hrtime_t start;
	hrtime_t end;

	if (slot == NULL || add_len == 0)
		return (NULL);

	if (slot->scratch != NULL && slot->scratch_size >= add_len)
		return (slot->scratch);

	scratch = zio_data_buf_alloc(add_len);
	if (scratch == NULL)
		return (NULL);

	if (slot->scratch != NULL) {
		start = gethrtime();
		zio_data_buf_free(slot->scratch, slot->scratch_size);
		end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_free_ns, start, end);
	}

	slot->scratch = scratch;
	slot->scratch_size = add_len;
	return (slot->scratch);
}

static struct page **
qat_dc_buffer_slot_page_array(qat_dc_buffer_slot_t *slot,
    struct page ***slot_pages, Cpa32U *slot_pages_count,
    size_t *slot_pages_size, Cpa32U num_pages)
{
	struct page **pages;
	size_t pages_size;
	hrtime_t start;
	hrtime_t end;

	if (slot == NULL || num_pages == 0)
		return (NULL);

	if (*slot_pages != NULL && *slot_pages_count >= num_pages)
		return (*slot_pages);

	pages_size = num_pages * sizeof (*pages);
	pages = kmem_alloc(pages_size, KM_SLEEP);
	if (pages == NULL)
		return (NULL);

	if (*slot_pages != NULL) {
		start = gethrtime();
		kmem_free(*slot_pages, *slot_pages_size);
		end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns, start, end);
	}

	*slot_pages = pages;
	*slot_pages_count = num_pages;
	*slot_pages_size = pages_size;
	return (*slot_pages);
}

static struct page **
qat_dc_buffer_slot_in_pages(qat_dc_buffer_slot_t *slot, Cpa32U num_src_buf)
{
	return (qat_dc_buffer_slot_page_array(slot, &slot->in_pages,
	    &slot->in_pages_count, &slot->in_pages_size, num_src_buf));
}

static struct page **
qat_dc_buffer_slot_out_pages(qat_dc_buffer_slot_t *slot, Cpa32U num_dst_buf)
{
	return (qat_dc_buffer_slot_page_array(slot, &slot->out_pages,
	    &slot->out_pages_count, &slot->out_pages_size, num_dst_buf));
}

static struct page **
qat_dc_buffer_slot_scratch_pages(qat_dc_buffer_slot_t *slot,
    Cpa32U num_add_buf)
{
	return (qat_dc_buffer_slot_page_array(slot, &slot->scratch_pages,
	    &slot->scratch_pages_count, &slot->scratch_pages_size,
	    num_add_buf));
}

static void
qat_dc_buffer_pool_clean(Cpa16U inst)
{
	qat_dc_buffer_pool_t *pool = &buffer_pools[inst];

	for (int i = 0; i < QAT_DC_BUFFER_REUSE_SLOTS; i++) {
		qat_dc_buffer_slot_t *slot = &pool->slots[i];

		QAT_PHYS_CONTIG_FREE(slot->buffer_meta_src);
		QAT_PHYS_CONTIG_FREE(slot->buffer_meta_dst);
		QAT_PHYS_CONTIG_FREE(slot->buf_list_src);
		QAT_PHYS_CONTIG_FREE(slot->buf_list_dst);
		if (slot->sync_req != NULL)
			kmem_free(slot->sync_req, sizeof (*slot->sync_req));
		if (slot->async_req != NULL)
			kmem_free(slot->async_req, sizeof (*slot->async_req));
		QAT_PHYS_CONTIG_FREE(slot->coalesced_dst);
		if (slot->scratch != NULL)
			zio_data_buf_free(slot->scratch, slot->scratch_size);
		if (slot->in_pages != NULL)
			kmem_free(slot->in_pages, slot->in_pages_size);
		if (slot->out_pages != NULL)
			kmem_free(slot->out_pages, slot->out_pages_size);
		if (slot->scratch_pages != NULL)
			kmem_free(slot->scratch_pages,
			    slot->scratch_pages_size);
	}

	memset(pool, 0, sizeof (*pool));
}

static void
qat_dc_buffer_pool_init(Cpa16U inst, CpaInstanceHandle dc_inst_handle,
    Cpa32U max_src_bufs, Cpa32U max_dst_bufs)
{
	qat_dc_buffer_pool_t *pool = &buffer_pools[inst];
	CpaStatus status = CPA_STATUS_SUCCESS;

	qat_dc_buffer_pool_clean(inst);

	pool->max_src_bufs = max_src_bufs;
	pool->max_dst_bufs = max_dst_bufs;
	pool->src_list_size = sizeof (CpaBufferList) +
	    (max_src_bufs * sizeof (CpaFlatBuffer));
	pool->dst_list_size = sizeof (CpaBufferList) +
	    (max_dst_bufs * sizeof (CpaFlatBuffer));

	status = cpaDcBufferListGetMetaSize(dc_inst_handle, max_src_bufs,
	    &pool->src_meta_size);
	if (status == CPA_STATUS_SUCCESS)
		status = cpaDcBufferListGetMetaSize(dc_inst_handle,
		    max_dst_bufs, &pool->dst_meta_size);
	if (status != CPA_STATUS_SUCCESS)
		goto fail;

	for (int i = 0; i < QAT_DC_BUFFER_REUSE_SLOTS; i++) {
		qat_dc_buffer_slot_t *slot = &pool->slots[i];

		status = QAT_PHYS_CONTIG_ALLOC(&slot->buffer_meta_src,
		    pool->src_meta_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&slot->buffer_meta_dst,
			    pool->dst_meta_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&slot->buf_list_src,
			    pool->src_list_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&slot->buf_list_dst,
			    pool->dst_list_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
	}

	pool->initialized = B_TRUE;
	return;

fail:
	qat_dc_buffer_pool_clean(inst);
}

static qat_dc_buffer_slot_t *
qat_dc_buffer_pool_acquire(Cpa16U inst, Cpa32U num_src_buf, Cpa32U num_dst_buf)
{
	qat_dc_buffer_pool_t *pool = &buffer_pools[inst];

	if (!pool->initialized ||
	    num_src_buf > pool->max_src_bufs ||
	    num_dst_buf > pool->max_dst_bufs) {
		QAT_STAT_BUMP(dc_buffer_reuse_misses);
		return (NULL);
	}

	for (int i = 0; i < QAT_DC_BUFFER_REUSE_SLOTS; i++) {
		if (!test_and_set_bit(i, &pool->busy)) {
			QAT_STAT_BUMP(dc_buffer_reuse_hits);
			return (&pool->slots[i]);
		}
	}

	QAT_STAT_BUMP(dc_buffer_reuse_misses);
	return (NULL);
}

static void
qat_dc_buffer_pool_release(Cpa16U inst, qat_dc_buffer_slot_t *slot)
{
	qat_dc_buffer_pool_t *pool = &buffer_pools[inst];

	for (int i = 0; i < QAT_DC_BUFFER_REUSE_SLOTS; i++) {
		if (slot == &pool->slots[i]) {
			clear_bit(i, &pool->busy);
			return;
		}
	}
}

static void
qat_dc_clean(void)
{
	Cpa16U buff_num = 0;
	Cpa16U num_inter_buff_lists = 0;

	qat_dc_watchdog_stop();
	qat_dc_poll_stop();

	for (Cpa16U i = 0; i < num_inst; i++)
		cpaDcStopInstance(dc_inst_handles[i]);

	qat_dc_sync_req_drain_retained();

	for (Cpa16U i = 0; i < num_inst; i++) {
		QAT_PHYS_CONTIG_FREE(session_handles[i]);
		qat_dc_buffer_pool_clean(i);
		/* free intermediate buffers  */
		if (buffer_array[i] != NULL) {
			cpaDcGetNumIntermediateBuffers(
			    dc_inst_handles[i], &num_inter_buff_lists);
			for (buff_num = 0; buff_num < num_inter_buff_lists;
			    buff_num++) {
				CpaBufferList *buffer_inter =
				    buffer_array[i][buff_num];
				if (buffer_inter->pBuffers) {
					QAT_PHYS_CONTIG_FREE(
					    buffer_inter->pBuffers->pData);
					QAT_PHYS_CONTIG_FREE(
					    buffer_inter->pBuffers);
				}
				QAT_PHYS_CONTIG_FREE(
				    buffer_inter->pPrivateMetaData);
				QAT_PHYS_CONTIG_FREE(buffer_inter);
			}
		}
	}

	num_inst = 0;
	qat_stats.dc_instances.value.ui64 = 0;
	qat_dc_init_done = B_FALSE;
}

int
qat_dc_init(void)
{
	CpaStatus status = CPA_STATUS_SUCCESS;
	Cpa32U sess_size = 0;
	Cpa32U ctx_size = 0;
	Cpa16U num_inter_buff_lists = 0;
	Cpa16U buff_num = 0;
	Cpa16U max_inst = 0;
	Cpa32U inter_buff_size = 0;
	Cpa32U max_src_bufs = 0;
	Cpa32U max_dst_bufs = 0;
	Cpa32U buff_meta_size = 0;
	int max_buf_size = 0;
	CpaDcSessionSetupData sd = {0};

	if (qat_dc_init_done)
		return (0);

	status = cpaDcGetNumInstances(&num_inst);
	if (status != CPA_STATUS_SUCCESS)
		return (-1);

	/* if the user has configured no QAT compression units just return */
	if (num_inst == 0)
		return (0);

	if (!qat_dc_valid_max_instances(zfs_qat_dc_max_instances))
		return (-1);

	max_buf_size = qat_dc_effective_max_buf_size();
	if (!qat_dc_valid_max_buf_size(max_buf_size))
		return (-1);

	max_inst = (Cpa16U)zfs_qat_dc_max_instances;
	if (num_inst > max_inst)
		num_inst = max_inst;

	inter_buff_size = 2 * (Cpa32U)max_buf_size;
	max_src_bufs = ((Cpa32U)max_buf_size >> PAGE_SHIFT) + 2;
	max_dst_bufs = 2 * max_src_bufs;

	status = cpaDcGetInstances(num_inst, &dc_inst_handles[0]);
	if (status != CPA_STATUS_SUCCESS)
		return (-1);

	if (!qat_dc_validate_poll_mode())
		return (-1);

	for (Cpa16U i = 0; i < num_inst; i++) {
		cpaDcSetAddressTranslation(dc_inst_handles[i],
		    (void*)virt_to_phys);

		qat_dc_buffer_pool_init(i, dc_inst_handles[i],
		    max_src_bufs, max_dst_bufs);

		status = cpaDcBufferListGetMetaSize(dc_inst_handles[i],
		    1, &buff_meta_size);

		if (status == CPA_STATUS_SUCCESS)
			status = cpaDcGetNumIntermediateBuffers(
			    dc_inst_handles[i], &num_inter_buff_lists);

		if (status == CPA_STATUS_SUCCESS && num_inter_buff_lists != 0)
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_array[i],
			    num_inter_buff_lists *
			    sizeof (CpaBufferList *));

		for (buff_num = 0; buff_num < num_inter_buff_lists;
		    buff_num++) {
			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num],
				    sizeof (CpaBufferList));

			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->
				    pPrivateMetaData,
				    buff_meta_size);

			if (status == CPA_STATUS_SUCCESS)
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->pBuffers,
				    sizeof (CpaFlatBuffer));

			if (status == CPA_STATUS_SUCCESS) {
				/*
				 *  implementation requires an intermediate
				 *  buffer approximately twice the size of
				 *  output buffer, which is 2x max buffer
				 *  size here.
				 */
				status = QAT_PHYS_CONTIG_ALLOC(
				    &buffer_array[i][buff_num]->pBuffers->
				    pData, inter_buff_size);
				if (status != CPA_STATUS_SUCCESS)
					goto fail;

				buffer_array[i][buff_num]->numBuffers = 1;
				buffer_array[i][buff_num]->pBuffers->
				    dataLenInBytes = inter_buff_size;
			}
		}

		status = cpaDcStartInstance(dc_inst_handles[i],
		    num_inter_buff_lists, buffer_array[i]);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		sd.compLevel = qat_dc_level();
		sd.compType = CPA_DC_DEFLATE;
		sd.huffType = qat_dc_selected_hufftype();
		sd.sessDirection = CPA_DC_DIR_COMBINED;
		sd.sessState = CPA_DC_STATELESS;
#if (CPA_DC_API_VERSION_NUM_MAJOR == 1 && CPA_DC_API_VERSION_NUM_MINOR < 6)
		sd.deflateWindowSize = 7;
#endif
		sd.checksum = CPA_DC_ADLER32;
		status = cpaDcGetSessionSize(dc_inst_handles[i],
		    &sd, &sess_size, &ctx_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		QAT_PHYS_CONTIG_ALLOC(&session_handles[i], sess_size);
		if (session_handles[i] == NULL)
			goto fail;

		status = cpaDcInitSession(dc_inst_handles[i],
		    session_handles[i],
		    &sd, NULL, qat_dc_callback);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
	}

	qat_dc_init_done = B_TRUE;
	qat_stats.dc_instances.value.ui64 = num_inst;
	atomic_swap_32(&qat_dc_runtime_failed, 0);
	atomic_swap_64(&qat_dc_total_inflight, 0);
	atomic_swap_64(&qat_dc_inflight_start_ns, 0);
	qat_dc_runtime_progress(gethrtime());
	qat_stats.dc_watchdog_health.value.ui64 = 1;
	if (qat_dc_watchdog_start() != 0)
		goto fail;
	if (qat_dc_poll_start() != 0)
		goto fail;
	return (0);
fail:
	qat_dc_clean();
	return (-1);
}

void
qat_dc_fini(void)
{
	if (!qat_dc_init_done)
		return;

	qat_dc_clean();
}

/*
 * The "add" parameter is an additional buffer which is passed
 * to QAT as a scratch buffer alongside the destination buffer
 * in case the "compressed" data ends up being larger than the
 * original source data. This is necessary to prevent QAT from
 * generating buffer overflow warnings for incompressible data.
 */
static int
qat_compress_impl(qat_compress_dir_t dir, char *src, int src_len,
    char *dst, int dst_len, char *add, int add_len, size_t *c_len)
{
	CpaInstanceHandle dc_inst_handle;
	CpaDcSessionHandle session_handle;
	CpaBufferList *buf_list_src = NULL;
	CpaBufferList *buf_list_dst = NULL;
	qat_dc_buffer_slot_t *buffer_slot = NULL;
	CpaFlatBuffer *flat_buf_src = NULL;
	CpaFlatBuffer *flat_buf_dst = NULL;
	Cpa8U *buffer_meta_src = NULL;
	Cpa8U *buffer_meta_dst = NULL;
	Cpa32U buffer_meta_size = 0;
	CpaStatus status = CPA_STATUS_FAIL;
	Cpa32U hdr_sz = 0;
	Cpa32U compressed_sz;
	Cpa32U coalesced_dst_len = 0;
	Cpa32U num_src_buf;
	Cpa32U num_dst_buf;
	Cpa32U num_add_buf;
	Cpa32U src_buffer_list_mem_size;
	Cpa32U dst_buffer_list_mem_size;
	Cpa32U bytes_left;
	Cpa32U src_pages = 0;
	Cpa32U dst_pages = 0;
	Cpa32U add_pages = 0;
	Cpa32U adler32 = 0;
	char *data;
	struct page *page;
	struct page *in_pages_stack[QAT_DC_STACK_MAX_PAGES];
	struct page *out_pages_stack[QAT_DC_STACK_MAX_PAGES];
	struct page *scratch_pages_stack[QAT_DC_STACK_MAX_PAGES];
	struct page **in_pages = in_pages_stack;
	struct page **out_pages = out_pages_stack;
	struct page **scratch_pages = NULL;
	void *coalesced_src = NULL;
	void *coalesced_dst = NULL;
	char *orig_dst = dst;
	size_t in_pages_size = 0;
	size_t out_pages_size = 0;
	size_t scratch_pages_size = 0;
	Cpa32U page_off = 0;
	qat_dc_sync_req_t *sync_req = NULL;
	Cpa32U page_num = 0;
	Cpa16U i;
	hrtime_t op_start = gethrtime();
	hrtime_t scratch_start;
	hrtime_t scratch_end;
	hrtime_t phase_start;
	hrtime_t phase_end;
	hrtime_t free_start;
	hrtime_t free_end;
	boolean_t src_coalesced;
	boolean_t dst_coalesced;
	boolean_t dst_coalesce_requested;
	boolean_t dst_quarantine_requested;
	boolean_t local_add_alloc = B_FALSE;
	boolean_t request_timed_out = B_FALSE;
	boolean_t retain_qat_resources = B_FALSE;
	boolean_t sync_compress_submitted = B_FALSE;
	boolean_t in_pages_from_slot = B_FALSE;
	boolean_t out_pages_from_slot = B_FALSE;
	boolean_t scratch_pages_from_slot = B_FALSE;
	boolean_t sync_req_from_slot = B_FALSE;
	qat_dc_buffer_shape_t buffer_shape = { 0 };
	uint64_t retained_bytes = 0;

	dst_quarantine_requested = (dir == QAT_COMPRESS &&
	    qat_dc_effective_quarantine_dst());
	src_coalesced = (dir == QAT_COMPRESS &&
	    qat_dc_try_coalesce_src(&src, src_len, &coalesced_src,
	    dst_quarantine_requested));
	dst_coalesce_requested = (dir == QAT_COMPRESS &&
	    qat_dc_effective_private_dst());
	dst_coalesced = B_FALSE;
	if (dst_quarantine_requested)
		QAT_STAT_BUMP(dc_compress_quarantine_dst_requests);
	if (dst_quarantine_requested && !src_coalesced) {
		QAT_STAT_BUMP(dc_compress_quarantine_dst_fails);
		goto fail;
	}

	num_src_buf = src_coalesced ? 1 : qat_dc_page_count(src, src_len);
	num_dst_buf = dst_coalesce_requested ? 1 :
	    qat_dc_page_count(dst, dst_len);
	num_add_buf = dst_coalesce_requested ? 0 :
	    qat_dc_worst_page_count(add_len);

	src_buffer_list_mem_size = sizeof (CpaBufferList) +
	    (num_src_buf * sizeof (CpaFlatBuffer));
	dst_buffer_list_mem_size = sizeof (CpaBufferList) +
	    ((num_dst_buf + num_add_buf) * sizeof (CpaFlatBuffer));

	if (num_src_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_add_buf > QAT_DC_ABS_MAX_PAGES)
		goto fail;

	i = (Cpa32U)atomic_inc_32_nv(&inst_num) % num_inst;
	dc_inst_handle = dc_inst_handles[i];
	session_handle = session_handles[i];

	buffer_slot = qat_dc_buffer_pool_acquire(i, num_src_buf,
	    num_dst_buf + num_add_buf);

	if (dst_coalesce_requested) {
		dst_coalesced = qat_dc_try_coalesce_dst(&dst, dst_len,
		    add_len, buffer_slot, &coalesced_dst);
		if (dst_coalesced) {
			coalesced_dst_len = (Cpa32U)(dst_len + add_len);
			if (dst_quarantine_requested)
				QAT_STAT_BUMP(
				    dc_compress_quarantine_dst_success);
		} else {
			if (dst_quarantine_requested) {
				QAT_STAT_BUMP(
				    dc_compress_quarantine_dst_fails);
				goto fail;
			}

			if (buffer_slot != NULL) {
				qat_dc_buffer_pool_release(i, buffer_slot);
				buffer_slot = NULL;
			}

			num_dst_buf = qat_dc_page_count(dst, dst_len);
			num_add_buf = qat_dc_worst_page_count(add_len);
			dst_buffer_list_mem_size = sizeof (CpaBufferList) +
			    ((num_dst_buf + num_add_buf) *
			    sizeof (CpaFlatBuffer));

			if (num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
			    num_add_buf > QAT_DC_ABS_MAX_PAGES)
				goto fail;

			buffer_slot = qat_dc_buffer_pool_acquire(i,
			    num_src_buf, num_dst_buf + num_add_buf);
		}
	}

	if (add_len > 0 && !dst_coalesced && add == NULL) {
		scratch_start = gethrtime();
		add = qat_dc_buffer_slot_scratch(buffer_slot, add_len);
		if (add == NULL) {
			add = zio_data_buf_alloc(add_len);
			local_add_alloc = B_TRUE;
		}
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_alloc_ns,
		    scratch_start, scratch_end);
		if (add == NULL)
			goto fail;
	}

	qat_dc_record_page_array_path(dir == QAT_COMPRESS && !src_coalesced &&
	    num_src_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_src,
	    &qat_stats.dc_compress_page_array_heap_src);
	qat_dc_record_page_array_path(dir == QAT_COMPRESS && !dst_coalesced &&
	    num_dst_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_dst,
	    &qat_stats.dc_compress_page_array_heap_dst);
	qat_dc_record_page_array_path(dir == QAT_COMPRESS &&
	    add_len > 0 && !dst_coalesced &&
	    num_add_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_scratch,
	    &qat_stats.dc_compress_page_array_heap_scratch);

	if (num_src_buf > QAT_DC_STACK_MAX_PAGES) {
		in_pages_size = num_src_buf * sizeof (*in_pages);
		if (buffer_slot != NULL) {
			phase_start = gethrtime();
			in_pages = qat_dc_buffer_slot_in_pages(buffer_slot,
			    num_src_buf);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
			in_pages_from_slot = (in_pages != NULL);
		} else {
			phase_start = gethrtime();
			in_pages = kmem_alloc(in_pages_size, KM_SLEEP);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
		}
		if (in_pages == NULL)
			goto fail;
		if (dir == QAT_COMPRESS && in_pages_from_slot)
			QAT_STAT_BUMP(dc_compress_page_array_slot_src);
		else if (dir == QAT_COMPRESS)
			QAT_STAT_BUMP(dc_compress_page_array_heap_src);
	}

	if (num_dst_buf > QAT_DC_STACK_MAX_PAGES) {
		out_pages_size = num_dst_buf * sizeof (*out_pages);
		if (buffer_slot != NULL) {
			phase_start = gethrtime();
			out_pages = qat_dc_buffer_slot_out_pages(buffer_slot,
			    num_dst_buf);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
			out_pages_from_slot = (out_pages != NULL);
		} else {
			phase_start = gethrtime();
			out_pages = kmem_alloc(out_pages_size, KM_SLEEP);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
		}
		if (out_pages == NULL)
			goto fail;
		if (dir == QAT_COMPRESS && out_pages_from_slot)
			QAT_STAT_BUMP(dc_compress_page_array_slot_dst);
		else if (dir == QAT_COMPRESS)
			QAT_STAT_BUMP(dc_compress_page_array_heap_dst);
	}

	if (add_len > 0 && !dst_coalesced) {
		scratch_pages_size = num_add_buf * sizeof (*scratch_pages);
		if (num_add_buf <= QAT_DC_STACK_MAX_PAGES) {
			scratch_pages = scratch_pages_stack;
		} else if (buffer_slot != NULL) {
			phase_start = gethrtime();
			scratch_pages = qat_dc_buffer_slot_scratch_pages(
			    buffer_slot, num_add_buf);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
			scratch_pages_from_slot = (scratch_pages != NULL);
		} else {
			phase_start = gethrtime();
			scratch_pages = kmem_alloc(scratch_pages_size, KM_SLEEP);
			phase_end = gethrtime();
			if (dir == QAT_COMPRESS)
				QAT_STAT_ADD_TIME(
				    dc_compress_page_array_alloc_ns,
				    phase_start, phase_end);
		}
		if (scratch_pages == NULL)
			goto fail;
		if (dir == QAT_COMPRESS && scratch_pages_from_slot)
			QAT_STAT_BUMP(dc_compress_page_array_slot_scratch);
		else if (dir == QAT_COMPRESS &&
		    num_add_buf > QAT_DC_STACK_MAX_PAGES)
			QAT_STAT_BUMP(dc_compress_page_array_heap_scratch);
	}

	if (buffer_slot != NULL) {
		buffer_meta_src = buffer_slot->buffer_meta_src;
		buffer_meta_dst = buffer_slot->buffer_meta_dst;
		buf_list_src = buffer_slot->buf_list_src;
		buf_list_dst = buffer_slot->buf_list_dst;
	} else {
		phase_start = gethrtime();
		status = cpaDcBufferListGetMetaSize(dc_inst_handle,
		    num_src_buf, &buffer_meta_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_src,
			    buffer_meta_size);
		if (status == CPA_STATUS_SUCCESS) {
			status = cpaDcBufferListGetMetaSize(dc_inst_handle,
			    num_dst_buf + num_add_buf, &buffer_meta_size);
		}
		if (status == CPA_STATUS_SUCCESS) {
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_dst,
			    buffer_meta_size);
		}
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&buf_list_src,
			    src_buffer_list_mem_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&buf_list_dst,
			    dst_buffer_list_mem_size);
		phase_end = gethrtime();
		if (dir == QAT_COMPRESS)
			QAT_STAT_ADD_TIME(dc_compress_buffer_list_alloc_ns,
			    phase_start, phase_end);

		if (status != CPA_STATUS_SUCCESS)
			goto fail;
	}

	flat_buf_src = (CpaFlatBuffer *)(buf_list_src + 1);
	buf_list_src->pBuffers = flat_buf_src; /* always point to first one */

	flat_buf_dst = (CpaFlatBuffer *)(buf_list_dst + 1);
	buf_list_dst->pBuffers = flat_buf_dst; /* always point to first one */

	buf_list_src->numBuffers = 0;
	buf_list_src->pPrivateMetaData = buffer_meta_src;
	if (src_coalesced) {
		flat_buf_src->pData = src;
		flat_buf_src->dataLenInBytes = src_len;
		if (dir == QAT_COMPRESS) {
			qat_dc_note_flat_buffer(flat_buf_src,
			    &buffer_shape.src_unaligned_64,
			    &buffer_shape.src_len_not_64,
			    &buffer_shape.src_first_bytes,
			    &buffer_shape.src_last_bytes, B_TRUE);
		}
		buf_list_src->numBuffers = 1;
		src_pages = 0;
	} else {
		bytes_left = src_len;
		data = src;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			in_pages[page_num] = page;
			flat_buf_src->pData = kmap(page) + page_off;
			flat_buf_src->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			if (dir == QAT_COMPRESS) {
				qat_dc_note_flat_buffer(flat_buf_src,
				    &buffer_shape.src_unaligned_64,
				    &buffer_shape.src_len_not_64,
				    &buffer_shape.src_first_bytes,
				    &buffer_shape.src_last_bytes,
				    page_num == 0);
			}

			bytes_left -= flat_buf_src->dataLenInBytes;
			data += flat_buf_src->dataLenInBytes;
			flat_buf_src++;
			buf_list_src->numBuffers++;
			page_num++;
		}
		src_pages = page_num;
	}

	buf_list_dst->numBuffers = 0;
	buf_list_dst->pPrivateMetaData = buffer_meta_dst;
	if (dst_coalesced) {
		flat_buf_dst->pData = dst;
		flat_buf_dst->dataLenInBytes = coalesced_dst_len;
		if (dir == QAT_COMPRESS) {
			qat_dc_note_flat_buffer(flat_buf_dst,
			    &buffer_shape.dst_unaligned_64,
			    &buffer_shape.dst_len_not_64,
			    &buffer_shape.dst_first_bytes,
			    &buffer_shape.dst_last_bytes, B_TRUE);
		}
		buf_list_dst->numBuffers = 1;
		dst_pages = 0;
		add_pages = 0;
	} else {
		bytes_left = dst_len;
		data = dst;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			flat_buf_dst->pData = kmap(page) + page_off;
			out_pages[page_num] = page;
			flat_buf_dst->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			if (dir == QAT_COMPRESS) {
				qat_dc_note_flat_buffer(flat_buf_dst,
				    &buffer_shape.dst_unaligned_64,
				    &buffer_shape.dst_len_not_64,
				    &buffer_shape.dst_first_bytes,
				    &buffer_shape.dst_last_bytes,
				    page_num == 0);
			}

			bytes_left -= flat_buf_dst->dataLenInBytes;
			data += flat_buf_dst->dataLenInBytes;
			flat_buf_dst++;
			buf_list_dst->numBuffers++;
			page_num++;
		}
		dst_pages = page_num;

		/* map additional scratch pages into the destination buffer list */
		bytes_left = add_len;
		data = add;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			flat_buf_dst->pData = kmap(page) + page_off;
			scratch_pages[page_num] = page;
			flat_buf_dst->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			if (dir == QAT_COMPRESS) {
				qat_dc_note_flat_buffer(flat_buf_dst,
				    &buffer_shape.add_unaligned_64,
				    &buffer_shape.add_len_not_64,
				    &buffer_shape.add_first_bytes,
				    &buffer_shape.add_last_bytes,
				    page_num == 0);
			}

			bytes_left -= flat_buf_dst->dataLenInBytes;
			data += flat_buf_dst->dataLenInBytes;
			flat_buf_dst++;
			buf_list_dst->numBuffers++;
			page_num++;
		}
		add_pages = page_num;
	}

	if (buffer_slot != NULL) {
		phase_start = gethrtime();
		sync_req = qat_dc_buffer_slot_sync_req(buffer_slot);
		phase_end = gethrtime();
		if (dir == QAT_COMPRESS)
			QAT_STAT_ADD_TIME(dc_compress_req_alloc_ns,
			    phase_start, phase_end);
		sync_req_from_slot = (sync_req != NULL);
	} else {
		phase_start = gethrtime();
		sync_req = kmem_alloc(sizeof (*sync_req), KM_SLEEP);
		phase_end = gethrtime();
		if (dir == QAT_COMPRESS)
			QAT_STAT_ADD_TIME(dc_compress_req_alloc_ns,
			    phase_start, phase_end);
		if (sync_req != NULL)
			qat_dc_sync_req_prepare(sync_req, B_FALSE);
	}
	if (sync_req == NULL)
		goto fail;
	if (dir == QAT_COMPRESS && sync_req_from_slot)
		QAT_STAT_BUMP(dc_compress_req_slot);
	sync_req->dir = dir;
	sync_req->state = QAT_DC_SYNC_ACTIVE;
	sync_req->callback_status = CPA_STATUS_FAIL;
	sync_req->dc_results.checksum = 1;
	sync_req->recoverable_timeout = (dir == QAT_COMPRESS &&
	    dst_quarantine_requested && src_coalesced && dst_coalesced);
	sync_req->inst = i;
	sync_req->buffer_slot = buffer_slot;
	sync_req->buffer_meta_src = buffer_meta_src;
	sync_req->buffer_meta_dst = buffer_meta_dst;
	sync_req->buf_list_src = buf_list_src;
	sync_req->buf_list_dst = buf_list_dst;
	sync_req->coalesced_src = coalesced_src;
	sync_req->coalesced_dst = coalesced_dst;
	if (sync_req->recoverable_timeout)
		retained_bytes = (uint64_t)src_len + coalesced_dst_len;

	if (dir == QAT_COMPRESS) {
		QAT_STAT_BUMP(comp_requests);
		QAT_STAT_INCR(comp_total_in_bytes, src_len);
		QAT_STAT_BUMP(dc_compress_sync_submits);
		sync_compress_submitted = B_TRUE;
		qat_dc_record_compress_shape(buf_list_src->numBuffers,
		    dst_coalesced ? buf_list_dst->numBuffers : dst_pages,
		    add_pages);
		qat_dc_record_compress_buffer_shape(&buffer_shape);

		cpaDcGenerateHeader(session_handle,
		    buf_list_dst->pBuffers, &hdr_sz);
		buf_list_dst->pBuffers->pData += hdr_sz;
		buf_list_dst->pBuffers->dataLenInBytes -= hdr_sz;
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_setup_ns, op_start, phase_end);
		phase_start = gethrtime();
		sync_req->wait_start = phase_start;
		qat_dc_inflight_enter(dir, phase_start);
		status = cpaDcCompressData(
		    dc_inst_handle, session_handle,
		    buf_list_src, buf_list_dst,
		    &sync_req->dc_results, CPA_DC_FLUSH_FINAL,
		    &sync_req->callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_submit_ns, phase_start, phase_end);
		if (status != CPA_STATUS_SUCCESS) {
			qat_dc_inflight_exit(dir, phase_end);
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		phase_start = gethrtime();
		request_timed_out = qat_dc_wait_request(sync_req, retained_bytes);
		phase_end = gethrtime();
		if (request_timed_out) {
			retain_qat_resources = B_TRUE;
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		if (sync_req->callback_status != CPA_STATUS_SUCCESS ||
		    sync_req->dc_results.status != CPA_STATUS_SUCCESS) {
			if (sync_req->dc_results.status == CPA_DC_OVERFLOW)
				QAT_STAT_BUMP(dc_compress_overflows);
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		compressed_sz = sync_req->dc_results.produced;
		if (compressed_sz + hdr_sz + ZLIB_FOOT_SZ > dst_len) {
			QAT_STAT_BUMP(dc_compress_incompressible);
			status = CPA_STATUS_INCOMPRESSIBLE;
			goto fail;
		}

		/* get adler32 checksum and append footer */
		*(Cpa32U*)(dst + hdr_sz + compressed_sz) =
		    BSWAP_32(sync_req->dc_results.checksum);

		*c_len = hdr_sz + compressed_sz + ZLIB_FOOT_SZ;
		QAT_STAT_BUMP(dc_compress_sync_completions);
		if (dst_coalesced) {
			free_start = gethrtime();
			memcpy(orig_dst, dst, *c_len);
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_copy_ns,
			    free_start, free_end);
			QAT_STAT_INCR(dc_compress_dst_coalesce_copy_bytes,
			    *c_len);
			if (dst_quarantine_requested) {
				QAT_STAT_INCR(
				    dc_compress_quarantine_dst_copy_bytes,
				    *c_len);
			}
		}
		QAT_STAT_INCR(comp_total_out_bytes, *c_len);
	} else {
		ASSERT3U(dir, ==, QAT_DECOMPRESS);
		QAT_STAT_BUMP(decomp_requests);
		QAT_STAT_INCR(decomp_total_in_bytes, src_len);

		buf_list_src->pBuffers->pData += ZLIB_HEAD_SZ;
		buf_list_src->pBuffers->dataLenInBytes -= ZLIB_HEAD_SZ;
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_decompress_setup_ns, op_start, phase_end);
		phase_start = gethrtime();
		sync_req->wait_start = phase_start;
		qat_dc_inflight_enter(dir, phase_start);
		status = cpaDcDecompressData(dc_inst_handle, session_handle,
		    buf_list_src, buf_list_dst, &sync_req->dc_results,
		    CPA_DC_FLUSH_FINAL, &sync_req->callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_decompress_submit_ns, phase_start, phase_end);

		if (CPA_STATUS_SUCCESS != status) {
			qat_dc_inflight_exit(dir, phase_end);
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		phase_start = gethrtime();
		request_timed_out = qat_dc_wait_request(sync_req, retained_bytes);
		phase_end = gethrtime();
		if (request_timed_out) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		if (sync_req->callback_status != CPA_STATUS_SUCCESS ||
		    sync_req->dc_results.status != CPA_STATUS_SUCCESS) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		/* verify adler checksum */
		adler32 = *(Cpa32U *)(src + sync_req->dc_results.consumed +
		    ZLIB_HEAD_SZ);
		if (adler32 != BSWAP_32(sync_req->dc_results.checksum)) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}
		*c_len = sync_req->dc_results.produced;
		QAT_STAT_INCR(decomp_total_out_bytes, *c_len);
	}

fail:
	phase_start = gethrtime();

	if (status != CPA_STATUS_SUCCESS && status != CPA_STATUS_INCOMPRESSIBLE)
		QAT_STAT_BUMP(dc_fails);

	for (page_num = 0; page_num < src_pages; page_num++)
		kunmap(in_pages[page_num]);

	for (page_num = 0; page_num < dst_pages; page_num++)
		kunmap(out_pages[page_num]);

	for (page_num = 0; page_num < add_pages; page_num++)
		kunmap(scratch_pages[page_num]);

	if (!retain_qat_resources) {
		if (buffer_slot != NULL) {
			qat_dc_buffer_pool_release(i, buffer_slot);
		} else {
			if (dir == QAT_COMPRESS)
				free_start = gethrtime();
			QAT_PHYS_CONTIG_FREE(buffer_meta_src);
			QAT_PHYS_CONTIG_FREE(buffer_meta_dst);
			QAT_PHYS_CONTIG_FREE(buf_list_src);
			QAT_PHYS_CONTIG_FREE(buf_list_dst);
			if (dir == QAT_COMPRESS) {
				free_end = gethrtime();
				QAT_STAT_ADD_TIME(dc_compress_buffer_list_free_ns,
				    free_start, free_end);
			}
		}
	}

	if (in_pages != in_pages_stack && in_pages != NULL &&
	    !in_pages_from_slot) {
		if (dir == QAT_COMPRESS)
			free_start = gethrtime();
		kmem_free(in_pages, in_pages_size);
		if (dir == QAT_COMPRESS) {
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns,
			    free_start, free_end);
		}
	}

	if (out_pages != out_pages_stack && out_pages != NULL &&
	    !out_pages_from_slot) {
		if (dir == QAT_COMPRESS)
			free_start = gethrtime();
		kmem_free(out_pages, out_pages_size);
		if (dir == QAT_COMPRESS) {
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns,
			    free_start, free_end);
		}
	}

	if (scratch_pages != NULL && scratch_pages != scratch_pages_stack &&
	    !scratch_pages_from_slot) {
		if (dir == QAT_COMPRESS)
			free_start = gethrtime();
		kmem_free(scratch_pages, scratch_pages_size);
		if (dir == QAT_COMPRESS) {
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns,
			    free_start, free_end);
		}
	}

	if (local_add_alloc && add != NULL) {
		scratch_start = gethrtime();
		zio_data_buf_free(add, add_len);
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_free_ns, scratch_start,
		    scratch_end);
	}

	if (!retain_qat_resources && coalesced_src != NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(coalesced_src);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_coalesce_free_ns, free_start,
		    free_end);
	}

	if (!retain_qat_resources && coalesced_dst != NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(coalesced_dst);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_free_ns, free_start,
		    free_end);
	}

	phase_end = gethrtime();
	if (dir == QAT_COMPRESS) {
		QAT_STAT_ADD_TIME(dc_compress_cleanup_ns, phase_start,
		    phase_end);
	} else {
		QAT_STAT_ADD_TIME(dc_decompress_cleanup_ns, phase_start,
		    phase_end);
	}

	if (dir == QAT_COMPRESS && status != CPA_STATUS_SUCCESS &&
	    sync_compress_submitted)
		QAT_STAT_BUMP(dc_compress_sync_fallbacks);

	if (!retain_qat_resources && sync_req != NULL && !sync_req_from_slot) {
		if (dir == QAT_COMPRESS)
			free_start = gethrtime();
		kmem_free(sync_req, sizeof (*sync_req));
		if (dir == QAT_COMPRESS) {
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_req_free_ns, free_start,
			    free_end);
		}
	}

	return (status);
}

/*
 * Entry point for QAT accelerated compression / decompression.
 */
int
qat_compress(qat_compress_dir_t dir, char *src, int src_len,
    char *dst, int dst_len, size_t *c_len)
{
	int ret;
	size_t add_len = 0;
	void *add = NULL;
	hrtime_t scratch_start;
	hrtime_t scratch_end;

	if (dir == QAT_COMPRESS) {
		add_len = qat_dc_compress_scratch_len(src_len, dst_len);
	}

	ret = qat_compress_impl(dir, src, src_len, dst,
	    dst_len, add, add_len, c_len);

	if (dir == QAT_COMPRESS && add != NULL) {
		scratch_start = gethrtime();
		zio_data_buf_free(add, add_len);
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_free_ns, scratch_start,
		    scratch_end);
	}

	return (ret);
}

boolean_t
qat_dc_compress_async_enabled(void)
{
	return (qat_dc_effective_async());
}

static void
qat_dc_async_record_submit_fail(CpaStatus status)
{
	switch (status) {
	case CPA_STATUS_RETRY:
		QAT_STAT_BUMP(dc_compress_async_fail_retry);
		break;
	case CPA_STATUS_RESOURCE:
		QAT_STAT_BUMP(dc_compress_async_fail_resource);
		break;
	default:
		QAT_STAT_BUMP(dc_compress_async_fail_other);
		break;
	}
}

static boolean_t
qat_dc_async_inflight_try_enter(int src_len)
{
	uint64_t cur;
	uint64_t next;
	int max_inflight;

	if (!qat_dc_async_effective_cap(src_len, &max_inflight))
		return (B_FALSE);

	for (;;) {
		cur = qat_stats.dc_compress_async_inflight.value.ui64;
		if (max_inflight > 0 && cur >= (uint64_t)max_inflight)
			return (B_FALSE);

		next = cur + 1;
		if (atomic_cas_64(
		    &qat_stats.dc_compress_async_inflight.value.ui64,
		    cur, next) == cur) {
			qat_dc_update_stat_max(
			    &qat_stats.dc_compress_async_inflight_max, next);
			return (B_TRUE);
		}
	}
}

static void
qat_dc_async_inflight_exit(void)
{
	(void) atomic_dec_64_nv(
	    &qat_stats.dc_compress_async_inflight.value.ui64);
}

static void
qat_dc_async_cleanup(qat_dc_async_t *req)
{
	qat_dc_buffer_slot_t *slot;
	hrtime_t start;
	hrtime_t end;
	hrtime_t free_start;
	hrtime_t free_end;
	Cpa16U inst;
	boolean_t from_slot;

	if (req == NULL)
		return;

	slot = req->buffer_slot;
	inst = req->inst;
	from_slot = req->from_slot;

	start = gethrtime();

	for (Cpa32U page_num = 0; page_num < req->src_pages; page_num++)
		kunmap(req->in_pages[page_num]);

	for (Cpa32U page_num = 0; page_num < req->dst_pages; page_num++)
		kunmap(req->out_pages[page_num]);

	for (Cpa32U page_num = 0; page_num < req->add_pages; page_num++)
		kunmap(req->scratch_pages[page_num]);

	if (slot == NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_src);
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_dst);
		QAT_PHYS_CONTIG_FREE(req->buf_list_src);
		QAT_PHYS_CONTIG_FREE(req->buf_list_dst);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_buffer_list_free_ns, free_start,
		    free_end);
	}

	if (req->in_pages != NULL && req->in_pages != req->in_pages_stack &&
	    !req->in_pages_from_slot) {
		free_start = gethrtime();
		kmem_free(req->in_pages, req->in_pages_size);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns, free_start,
		    free_end);
	}

	if (req->out_pages != NULL && req->out_pages != req->out_pages_stack &&
	    !req->out_pages_from_slot) {
		free_start = gethrtime();
		kmem_free(req->out_pages, req->out_pages_size);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns, free_start,
		    free_end);
	}

	if (req->scratch_pages != NULL &&
	    req->scratch_pages != req->scratch_pages_stack &&
	    !req->scratch_pages_from_slot) {
		free_start = gethrtime();
		kmem_free(req->scratch_pages, req->scratch_pages_size);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_page_array_free_ns, free_start,
		    free_end);
	}

	if (req->add != NULL && !req->add_from_slot) {
		free_start = gethrtime();
		zio_data_buf_free(req->add, req->add_len);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_free_ns, free_start,
		    free_end);
	}

	if (req->coalesced_src != NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(req->coalesced_src);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_coalesce_free_ns, free_start,
		    free_end);
	}

	if (req->coalesced_dst != NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(req->coalesced_dst);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_free_ns, free_start,
		    free_end);
	}

	end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_cleanup_ns, start, end);

	if (slot != NULL)
		qat_dc_buffer_pool_release(inst, slot);

	if (!from_slot) {
		free_start = gethrtime();
		kmem_free(req, sizeof (*req));
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_req_free_ns, free_start,
		    free_end);
	}
}

qat_dc_async_t *
qat_dc_compress_async_submit(char *src, int src_len, char *dst, int dst_len,
    void (*resume)(void *), void *resume_arg)
{
	qat_dc_async_t *req = NULL;
	qat_dc_buffer_slot_t *buffer_slot = NULL;
	CpaInstanceHandle dc_inst_handle;
	CpaDcSessionHandle session_handle;
	CpaFlatBuffer *flat_buf_src = NULL;
	CpaFlatBuffer *flat_buf_dst = NULL;
	Cpa8U *buffer_meta_src = NULL;
	Cpa8U *buffer_meta_dst = NULL;
	Cpa32U buffer_meta_size = 0;
	Cpa32U num_src_buf;
	Cpa32U num_dst_buf;
	Cpa32U num_add_buf;
	Cpa32U coalesced_dst_len = 0;
	Cpa32U src_buffer_list_mem_size;
	Cpa32U dst_buffer_list_mem_size;
	Cpa32U bytes_left;
	Cpa32U page_off = 0;
	Cpa32U page_num = 0;
	Cpa16U inst = 0;
	CpaStatus status = CPA_STATUS_FAIL;
	char *req_src = src;
	char *req_dst = dst;
	char *data;
	struct page *page;
	void *coalesced_src = NULL;
	void *coalesced_dst = NULL;
	size_t add_len;
	hrtime_t op_start = gethrtime();
	hrtime_t phase_start;
	hrtime_t phase_end;
	int attempt = 0;
	int retry_limit;
	boolean_t async_inflight = B_FALSE;
	boolean_t src_coalesced;
	boolean_t dst_coalesced = B_FALSE;
	boolean_t dst_coalesce_requested;
	boolean_t req_from_slot = B_FALSE;
	qat_dc_buffer_shape_t buffer_shape = { 0 };

	if (!qat_dc_compress_async_enabled() ||
	    src_len < 0 || dst_len < 0 ||
	    !qat_dc_compress_use_accel(src_len))
		return (NULL);

	QAT_STAT_BUMP(dc_compress_async_submits);

	if (!qat_dc_async_inflight_try_enter(src_len)) {
		QAT_STAT_BUMP(dc_compress_async_cap_skips);
		QAT_STAT_BUMP(dc_compress_async_fallbacks);
		return (NULL);
	}
	async_inflight = B_TRUE;

	add_len = qat_dc_compress_scratch_len(src_len, dst_len);
	src_coalesced = qat_dc_try_coalesce_src(&req_src, src_len,
	    &coalesced_src, B_FALSE);
	dst_coalesce_requested = qat_dc_effective_coalesce_dst();

	num_src_buf = src_coalesced ? 1 :
	    qat_dc_page_count(req_src, src_len);
	num_dst_buf = dst_coalesce_requested ? 1 :
	    qat_dc_page_count(req_dst, dst_len);
	num_add_buf = dst_coalesce_requested ? 0 :
	    qat_dc_worst_page_count(add_len);

	if (num_src_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_add_buf > QAT_DC_ABS_MAX_PAGES)
		goto fail_pre_req;

	inst = (Cpa32U)atomic_inc_32_nv(&inst_num) % num_inst;
	dc_inst_handle = dc_inst_handles[inst];
	session_handle = session_handles[inst];

	buffer_slot = qat_dc_buffer_pool_acquire(inst, num_src_buf,
	    num_dst_buf + num_add_buf);

	if (dst_coalesce_requested) {
		dst_coalesced = qat_dc_try_coalesce_dst(&req_dst, dst_len,
		    add_len, buffer_slot, &coalesced_dst);
		if (dst_coalesced) {
			coalesced_dst_len = (Cpa32U)(dst_len + add_len);
		} else {
			if (buffer_slot != NULL) {
				qat_dc_buffer_pool_release(inst, buffer_slot);
				buffer_slot = NULL;
			}

			num_dst_buf = qat_dc_page_count(req_dst, dst_len);
			num_add_buf = qat_dc_worst_page_count(add_len);
			if (num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
			    num_add_buf > QAT_DC_ABS_MAX_PAGES)
				goto fail_pre_req;

			buffer_slot = qat_dc_buffer_pool_acquire(inst,
			    num_src_buf, num_dst_buf + num_add_buf);
		}
	}

	if (buffer_slot != NULL) {
		phase_start = gethrtime();
		req = qat_dc_buffer_slot_async_req(buffer_slot);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_req_alloc_ns, phase_start,
		    phase_end);
		req_from_slot = (req != NULL);
	} else {
		phase_start = gethrtime();
		req = kmem_alloc(sizeof (*req), KM_SLEEP);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_req_alloc_ns, phase_start,
		    phase_end);
		if (req != NULL)
			qat_dc_async_req_prepare(req, B_FALSE);
	}
	if (req == NULL)
		goto fail_pre_req;
	if (req_from_slot)
		QAT_STAT_BUMP(dc_compress_req_slot);
	req->src = req_src;
	req->dst = req_dst;
	req->orig_dst = dst;
	req->src_len = src_len;
	req->dst_len = dst_len;
	req->resume = resume;
	req->resume_arg = resume_arg;
	req->add_len = add_len;
	req->inst = inst;
	req->buffer_slot = buffer_slot;
	req->coalesced_src = coalesced_src;
	req->coalesced_dst = coalesced_dst;

	if (req->add_len > 0 && !dst_coalesced) {
		phase_start = gethrtime();
		req->add = qat_dc_buffer_slot_scratch(req->buffer_slot,
		    req->add_len);
		if (req->add != NULL) {
			req->add_from_slot = B_TRUE;
		} else {
			req->add = zio_data_buf_alloc(req->add_len);
		}
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_alloc_ns, phase_start,
		    phase_end);
		if (req->add == NULL)
			goto fail;
	}

	qat_dc_record_page_array_path(!src_coalesced &&
	    num_src_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_src,
	    &qat_stats.dc_compress_page_array_heap_src);
	qat_dc_record_page_array_path(!dst_coalesced &&
	    num_dst_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_dst,
	    &qat_stats.dc_compress_page_array_heap_dst);
	qat_dc_record_page_array_path(req->add_len > 0 && !dst_coalesced &&
	    num_add_buf <= QAT_DC_STACK_MAX_PAGES, B_FALSE,
	    &qat_stats.dc_compress_page_array_stack_scratch,
	    &qat_stats.dc_compress_page_array_heap_scratch);

	src_buffer_list_mem_size = sizeof (CpaBufferList) +
	    (num_src_buf * sizeof (CpaFlatBuffer));
	dst_buffer_list_mem_size = sizeof (CpaBufferList) +
	    ((num_dst_buf + num_add_buf) * sizeof (CpaFlatBuffer));

	phase_start = gethrtime();
	if (num_src_buf <= QAT_DC_STACK_MAX_PAGES) {
		req->in_pages = req->in_pages_stack;
	} else if (req->buffer_slot != NULL) {
		req->in_pages = qat_dc_buffer_slot_in_pages(req->buffer_slot,
		    num_src_buf);
		req->in_pages_from_slot = (req->in_pages != NULL);
	} else {
		req->in_pages_size = num_src_buf * sizeof (*req->in_pages);
		req->in_pages = kmem_zalloc(req->in_pages_size, KM_SLEEP);
	}
	if (num_dst_buf <= QAT_DC_STACK_MAX_PAGES) {
		req->out_pages = req->out_pages_stack;
	} else if (req->buffer_slot != NULL) {
		req->out_pages = qat_dc_buffer_slot_out_pages(req->buffer_slot,
		    num_dst_buf);
		req->out_pages_from_slot = (req->out_pages != NULL);
	} else {
		req->out_pages_size = num_dst_buf * sizeof (*req->out_pages);
		req->out_pages = kmem_zalloc(req->out_pages_size, KM_SLEEP);
	}
	if (num_add_buf > 0 && num_add_buf <= QAT_DC_STACK_MAX_PAGES) {
		req->scratch_pages = req->scratch_pages_stack;
	} else if (num_add_buf > 0 && req->buffer_slot != NULL) {
		req->scratch_pages = qat_dc_buffer_slot_scratch_pages(
		    req->buffer_slot, num_add_buf);
		req->scratch_pages_from_slot = (req->scratch_pages != NULL);
	} else if (num_add_buf > 0) {
		req->scratch_pages_size =
		    num_add_buf * sizeof (*req->scratch_pages);
		req->scratch_pages = kmem_zalloc(req->scratch_pages_size,
		    KM_SLEEP);
	}
	phase_end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_page_array_alloc_ns, phase_start,
	    phase_end);
	if (req->in_pages == NULL || req->out_pages == NULL ||
	    (num_add_buf > 0 && req->scratch_pages == NULL))
		goto fail;
	if (!src_coalesced && num_src_buf > QAT_DC_STACK_MAX_PAGES) {
		if (req->in_pages_from_slot) {
			QAT_STAT_BUMP(dc_compress_page_array_slot_src);
		} else {
			QAT_STAT_BUMP(dc_compress_page_array_heap_src);
		}
	}
	if (!dst_coalesced && num_dst_buf > QAT_DC_STACK_MAX_PAGES) {
		if (req->out_pages_from_slot) {
			QAT_STAT_BUMP(dc_compress_page_array_slot_dst);
		} else {
			QAT_STAT_BUMP(dc_compress_page_array_heap_dst);
		}
	}
	if (req->add_len > 0 && !dst_coalesced) {
		if (req->scratch_pages_from_slot) {
			QAT_STAT_BUMP(dc_compress_page_array_slot_scratch);
		} else if (num_add_buf > QAT_DC_STACK_MAX_PAGES) {
			QAT_STAT_BUMP(dc_compress_page_array_heap_scratch);
		}
	}

	if (req->buffer_slot != NULL) {
		buffer_meta_src = req->buffer_slot->buffer_meta_src;
		buffer_meta_dst = req->buffer_slot->buffer_meta_dst;
		req->buf_list_src = req->buffer_slot->buf_list_src;
		req->buf_list_dst = req->buffer_slot->buf_list_dst;
	} else {
		phase_start = gethrtime();
		status = cpaDcBufferListGetMetaSize(dc_inst_handle, num_src_buf,
		    &buffer_meta_size);
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_src,
			    buffer_meta_size);
		if (status == CPA_STATUS_SUCCESS) {
			req->buffer_meta_src = buffer_meta_src;
			status = cpaDcBufferListGetMetaSize(dc_inst_handle,
			    num_dst_buf + num_add_buf, &buffer_meta_size);
		}
		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_dst,
			    buffer_meta_size);
		if (status == CPA_STATUS_SUCCESS) {
			req->buffer_meta_dst = buffer_meta_dst;
			status = QAT_PHYS_CONTIG_ALLOC(&req->buf_list_src,
			    src_buffer_list_mem_size);
		}

		if (status == CPA_STATUS_SUCCESS)
			status = QAT_PHYS_CONTIG_ALLOC(&req->buf_list_dst,
			    dst_buffer_list_mem_size);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_buffer_list_alloc_ns,
		    phase_start, phase_end);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
	}

	flat_buf_src = (CpaFlatBuffer *)(req->buf_list_src + 1);
	req->buf_list_src->pBuffers = flat_buf_src;
	req->buf_list_src->numBuffers = 0;
	req->buf_list_src->pPrivateMetaData = buffer_meta_src;

	if (src_coalesced) {
		flat_buf_src->pData = req->src;
		flat_buf_src->dataLenInBytes = src_len;
		qat_dc_note_flat_buffer(flat_buf_src,
		    &buffer_shape.src_unaligned_64,
		    &buffer_shape.src_len_not_64,
		    &buffer_shape.src_first_bytes,
		    &buffer_shape.src_last_bytes, B_TRUE);
		req->buf_list_src->numBuffers = 1;
		req->src_pages = 0;
	} else {
		bytes_left = src_len;
		data = req->src;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			req->in_pages[page_num] = page;
			flat_buf_src->pData = kmap(page) + page_off;
			flat_buf_src->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			qat_dc_note_flat_buffer(flat_buf_src,
			    &buffer_shape.src_unaligned_64,
			    &buffer_shape.src_len_not_64,
			    &buffer_shape.src_first_bytes,
			    &buffer_shape.src_last_bytes, page_num == 0);

			bytes_left -= flat_buf_src->dataLenInBytes;
			data += flat_buf_src->dataLenInBytes;
			flat_buf_src++;
			req->buf_list_src->numBuffers++;
			page_num++;
		}
		req->src_pages = page_num;
	}

	flat_buf_dst = (CpaFlatBuffer *)(req->buf_list_dst + 1);
	req->buf_list_dst->pBuffers = flat_buf_dst;
	req->buf_list_dst->numBuffers = 0;
	req->buf_list_dst->pPrivateMetaData = buffer_meta_dst;

	if (dst_coalesced) {
		flat_buf_dst->pData = req->dst;
		flat_buf_dst->dataLenInBytes = coalesced_dst_len;
		qat_dc_note_flat_buffer(flat_buf_dst,
		    &buffer_shape.dst_unaligned_64,
		    &buffer_shape.dst_len_not_64,
		    &buffer_shape.dst_first_bytes,
		    &buffer_shape.dst_last_bytes, B_TRUE);
		req->buf_list_dst->numBuffers = 1;
		req->dst_pages = 0;
		req->add_pages = 0;
	} else {
		bytes_left = dst_len;
		data = req->dst;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			req->out_pages[page_num] = page;
			flat_buf_dst->pData = kmap(page) + page_off;
			flat_buf_dst->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			qat_dc_note_flat_buffer(flat_buf_dst,
			    &buffer_shape.dst_unaligned_64,
			    &buffer_shape.dst_len_not_64,
			    &buffer_shape.dst_first_bytes,
			    &buffer_shape.dst_last_bytes, page_num == 0);

			bytes_left -= flat_buf_dst->dataLenInBytes;
			data += flat_buf_dst->dataLenInBytes;
			flat_buf_dst++;
			req->buf_list_dst->numBuffers++;
			page_num++;
		}
		req->dst_pages = page_num;

		bytes_left = req->add_len;
		data = req->add;
		page_num = 0;
		while (bytes_left > 0) {
			page_off = ((long)data & ~PAGE_MASK);
			page = qat_mem_to_page(data);
			req->scratch_pages[page_num] = page;
			flat_buf_dst->pData = kmap(page) + page_off;
			flat_buf_dst->dataLenInBytes =
			    min((long)PAGE_SIZE - page_off, (long)bytes_left);
			qat_dc_note_flat_buffer(flat_buf_dst,
			    &buffer_shape.add_unaligned_64,
			    &buffer_shape.add_len_not_64,
			    &buffer_shape.add_first_bytes,
			    &buffer_shape.add_last_bytes, page_num == 0);

			bytes_left -= flat_buf_dst->dataLenInBytes;
			data += flat_buf_dst->dataLenInBytes;
			flat_buf_dst++;
			req->buf_list_dst->numBuffers++;
			page_num++;
		}
		req->add_pages = page_num;
	}

	QAT_STAT_BUMP(comp_requests);
	QAT_STAT_INCR(comp_total_in_bytes, src_len);
	qat_dc_record_compress_shape(req->buf_list_src->numBuffers,
	    dst_coalesced ? req->buf_list_dst->numBuffers : req->dst_pages,
	    req->add_pages);
	qat_dc_record_compress_buffer_shape(&buffer_shape);

	cpaDcGenerateHeader(session_handle, req->buf_list_dst->pBuffers,
	    &req->hdr_sz);
	req->buf_list_dst->pBuffers->pData += req->hdr_sz;
	req->buf_list_dst->pBuffers->dataLenInBytes -= req->hdr_sz;
	phase_end = gethrtime();
	QAT_STAT_ADD_TIME(dc_compress_setup_ns, op_start, phase_end);

	req->callback_ctx.magic = QAT_DC_CALLBACK_MAGIC;
	req->callback_ctx.type = QAT_DC_CALLBACK_ASYNC;
	req->callback_ctx.u.async = req;

	retry_limit = (qat_dc_effective_async_submit_retries() > 0) ?
	    qat_dc_effective_async_submit_retries() : 0;
	for (;;) {
		phase_start = gethrtime();
		req->submit_end = phase_start;
		qat_dc_inflight_enter(QAT_COMPRESS, phase_start);
		qat_dc_async_active_add(req);
		status = cpaDcCompressData(dc_inst_handle, session_handle,
		    req->buf_list_src, req->buf_list_dst, &req->dc_results,
		    CPA_DC_FLUSH_FINAL, &req->callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_submit_ns, phase_start,
		    phase_end);
		if (status == CPA_STATUS_SUCCESS)
			break;

		qat_dc_async_active_remove(req);
		qat_dc_inflight_exit(QAT_COMPRESS, phase_end);
		if (status != CPA_STATUS_RETRY || attempt >= retry_limit)
			break;

		attempt++;
		QAT_STAT_BUMP(dc_compress_async_submit_retries);
		if (qat_dc_effective_async_retry_us() > 0) {
			usleep_range(qat_dc_effective_async_retry_us(),
			    qat_dc_effective_async_retry_us() + 10);
		}
	}

	if (status != CPA_STATUS_SUCCESS) {
		req->submit_status = status;
		goto fail;
	}

	if (attempt > 0)
		QAT_STAT_BUMP(dc_compress_async_retry_success);
	req->submit_status = status;
	req->submit_end = phase_end;
	return (req);

fail_pre_req:
	if (async_inflight)
		qat_dc_async_inflight_exit();
	if (buffer_slot != NULL)
		qat_dc_buffer_pool_release(inst, buffer_slot);
	QAT_PHYS_CONTIG_FREE(coalesced_src);
	if (buffer_slot == NULL)
		QAT_PHYS_CONTIG_FREE(coalesced_dst);
	QAT_STAT_BUMP(dc_compress_async_submit_fails);
	QAT_STAT_BUMP(dc_compress_async_fallbacks);
	qat_dc_async_record_submit_fail(status);
	return (NULL);

fail:
	if (async_inflight)
		qat_dc_async_inflight_exit();
	QAT_STAT_BUMP(dc_compress_async_submit_fails);
	QAT_STAT_BUMP(dc_compress_async_fallbacks);
	qat_dc_async_record_submit_fail(status);
	qat_dc_async_cleanup(req);
	return (NULL);
}

void
qat_dc_compress_async_arm(qat_dc_async_t *req)
{
	void (*resume)(void *) = NULL;
	void *resume_arg = NULL;
	unsigned long flags;

	if (req == NULL)
		return;

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	req->armed = 1;
	if (req->complete && !req->abandoned && req->resume != NULL) {
		resume = req->resume;
		resume_arg = req->resume_arg;
	}
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);

	if (resume != NULL) {
		QAT_STAT_BUMP(dc_compress_async_resumes);
		resume(resume_arg);
	}
}

boolean_t
qat_dc_compress_async_complete(qat_dc_async_t *req)
{
	boolean_t complete;
	unsigned long flags;

	if (req == NULL)
		return (B_FALSE);

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	complete = (req->complete != 0);
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);

	return (complete);
}

boolean_t
qat_dc_compress_async_timed_out(qat_dc_async_t *req)
{
	boolean_t timed_out;
	unsigned long flags;

	if (req == NULL)
		return (B_FALSE);

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	timed_out = (req->timed_out != 0);
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);

	return (timed_out);
}

boolean_t
qat_dc_compress_async_abandon(qat_dc_async_t *req, void (*cleanup)(void *),
    void *cleanup_arg)
{
	boolean_t abandoned = B_FALSE;
	unsigned long flags;

	if (req == NULL)
		return (B_FALSE);

	spin_lock_irqsave(&qat_dc_async_lock, flags);
	if (!req->complete && !req->abandoned) {
		req->abandoned = B_TRUE;
		req->abandon_cleanup = cleanup;
		req->abandon_arg = cleanup_arg;
		req->resume = NULL;
		req->resume_arg = NULL;
		qat_dc_async_active_remove_locked(req);
		abandoned = B_TRUE;
	}
	spin_unlock_irqrestore(&qat_dc_async_lock, flags);

	if (abandoned)
		QAT_STAT_BUMP(dc_watchdog_request_recoveries);

	return (abandoned);
}

int
qat_dc_compress_async_finish(qat_dc_async_t *req, size_t *c_len)
{
	Cpa32U compressed_sz;
	CpaStatus status;
	hrtime_t start;
	hrtime_t end;

	ASSERT(req != NULL);
	ASSERT(req->complete != 0);

	status = req->callback_status;
	if (status != CPA_STATUS_SUCCESS ||
	    req->dc_results.status != CPA_STATUS_SUCCESS) {
		if (req->dc_results.status == CPA_DC_OVERFLOW)
			QAT_STAT_BUMP(dc_compress_overflows);
		status = CPA_STATUS_FAIL;
		goto out;
	}

	compressed_sz = req->dc_results.produced;
	if (compressed_sz + req->hdr_sz + ZLIB_FOOT_SZ > req->dst_len) {
		QAT_STAT_BUMP(dc_compress_incompressible);
		status = CPA_STATUS_INCOMPRESSIBLE;
		goto out;
	}

	*(Cpa32U *)(req->dst + req->hdr_sz + compressed_sz) =
	    BSWAP_32(req->dc_results.checksum);

	req->c_len = req->hdr_sz + compressed_sz + ZLIB_FOOT_SZ;
	*c_len = req->c_len;
	if (req->dst != req->orig_dst) {
		start = gethrtime();
		memcpy(req->orig_dst, req->dst, req->c_len);
		end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_copy_ns, start, end);
		QAT_STAT_INCR(dc_compress_dst_coalesce_copy_bytes,
		    req->c_len);
	}
	QAT_STAT_INCR(comp_total_out_bytes, req->c_len);
	status = CPA_STATUS_SUCCESS;

out:
	if (status != CPA_STATUS_SUCCESS) {
		QAT_STAT_BUMP(dc_compress_async_fallbacks);
	}
	if (status != CPA_STATUS_SUCCESS && status != CPA_STATUS_INCOMPRESSIBLE) {
		QAT_STAT_BUMP(dc_fails);
	}

	qat_dc_async_cleanup(req);
	return (status);
}

void
qat_dc_compress_async_cancel(qat_dc_async_t *req)
{
	if (req == NULL)
		return;

	QAT_STAT_BUMP(dc_compress_async_cancels);
	if (qat_dc_compress_async_abandon(req, NULL, NULL))
		return;

	qat_dc_async_active_remove(req);
	qat_dc_async_cleanup(req);
}

static int
param_set_qat_compress(const char *val, zfs_kernel_param_t *kp)
{
	int ret;
	int *pvalue = kp->arg;

	ret = param_set_int(val, kp);
	if (ret)
		return (ret);
	/*
	 * zfs_qat_compress_disable = 0: enable the master QAT DC path.
	 * Try to initialize QAT DC instances if they have not been initialized.
	 */
	if (*pvalue == 0 && !qat_dc_init_done) {
		ret = qat_dc_init();
		if (ret != 0) {
			zfs_qat_compress_disable = 1;
			return (ret);
		}
	}

	if (*pvalue == 0 && qat_dc_runtime_failed != 0) {
		if (qat_dc_inflight_total() != 0 ||
		    qat_stats.dc_compress_quarantine_dst_retained.value.ui64 !=
		    0) {
			zfs_qat_compress_disable = 1;
			return (-EBUSY);
		}

		atomic_swap_32(&qat_dc_runtime_failed, 0);
		atomic_swap_64(&qat_dc_inflight_start_ns, 0);
		qat_dc_runtime_progress(gethrtime());
		qat_stats.dc_watchdog_health.value.ui64 = 1;
	}
	return (ret);
}

static boolean_t
qat_dc_param_profile(const char *val)
{
	return (strcmp(val, "profile") == 0 || strcmp(val, "profile\n") == 0);
}

static int
qat_dc_parse_int_range(const char *val, int min, int max, int *out)
{
	int ret;
	int new_value;

	ret = kstrtoint(val, 0, &new_value);
	if (ret != 0)
		return (ret);

	if (new_value < min || new_value > max)
		return (-EINVAL);

	*out = new_value;
	return (0);
}

static int
param_set_qat_decompress(const char *val, zfs_kernel_param_t *kp)
{
	int new_value;
	char **pvalue = kp->arg;
	int ret = 0;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		new_value = qat_dc_effective_decompress_disable();
	} else {
		ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
		if (ret != 0)
			return (ret);

		zfs_qat_decompress_disable_value = new_value;
		*pvalue = "manual";
	}

	/*
	 * zfs_qat_decompress_disable = 0: enable QAT decompression policy.
	 * The master compression disable still gates all QAT DC use.
	 */
	if (new_value == 0 && !zfs_qat_compress_disable && !qat_dc_init_done) {
		ret = qat_dc_init();
		if (ret != 0) {
			zfs_qat_compress_disable = 1;
			return (ret);
		}
	}
	return (ret);
}

static int
param_get_qat_decompress(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_decompress_disable_value));
}

static int
param_set_qat_cpa_dc_level(const char *val, zfs_kernel_param_t *kp)
{
	unsigned int new_value;
	int old_value;
	int ret;
	char **pvalue = kp->arg;

	if (qat_dc_param_profile(val)) {
		if (qat_dc_init_done &&
		    qat_dc_effective_level() !=
		    qat_dc_profile_level(zfs_qat_dc_ratio_profile,
		    zfs_qat_dc_expected_ratio)) {
			return (-EBUSY);
		}

		*pvalue = "profile";
		return (0);
	}

	ret = kstrtouint(val, 0, &new_value);
	if (ret != 0)
		return (ret);

	if (!qat_dc_valid_level((int)new_value))
		return (-EINVAL);

	old_value = qat_dc_effective_level();
	if (qat_dc_init_done && (int)new_value != old_value)
		return (-EBUSY);

	zfs_qat_cpa_dc_level_value = (int)new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_cpa_dc_level(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_cpa_dc_level_value));
}

static int
param_set_qat_dc_max_instances(const char *val, zfs_kernel_param_t *kp)
{
	int ret;
	int old_value;
	int *pvalue = kp->arg;

	old_value = *pvalue;
	ret = param_set_int(val, kp);
	if (ret != 0)
		return (ret);

	if (!qat_dc_valid_max_instances(*pvalue)) {
		*pvalue = old_value;
		return (-EINVAL);
	}

	if (qat_dc_init_done && *pvalue != old_value) {
		*pvalue = old_value;
		return (-EBUSY);
	}

	return (0);
}

static int
param_set_qat_cpa_dc_hufftype(const char *val, zfs_kernel_param_t *kp)
{
	CpaDcHuffType huff_type;
	CpaDcHuffType old_huff_type;
	char **pvalue = kp->arg;

	if (qat_dc_param_profile(val)) {
		if (!qat_dc_hufftype(qat_dc_profile_hufftype(
		    zfs_qat_dc_ratio_profile, zfs_qat_dc_expected_ratio),
		    &huff_type)) {
			return (-EINVAL);
		}

		(void) qat_dc_hufftype(qat_dc_effective_hufftype(),
		    &old_huff_type);
		if (qat_dc_init_done && huff_type != old_huff_type)
			return (-EBUSY);

		*pvalue = "profile";
		return (0);
	}

	if (!qat_dc_hufftype(val, &huff_type))
		return (-EINVAL);

	(void) qat_dc_hufftype(qat_dc_effective_hufftype(), &old_huff_type);
	if (qat_dc_init_done && huff_type != old_huff_type)
		return (-EBUSY);

	zfs_qat_cpa_dc_hufftype_value =
	    (huff_type == CPA_DC_HT_STATIC) ? "static" : "dynamic";
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_cpa_dc_hufftype(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%s\n", zfs_qat_cpa_dc_hufftype_value));
}

static int
param_set_qat_dc_min_buf_size(const char *val, zfs_kernel_param_t *kp)
{
	unsigned int new_value;
	char **pvalue = kp->arg;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = kstrtouint(val, 0, &new_value);
	if (ret != 0)
		return (ret);

	if (!qat_dc_valid_min_buf_size((int)new_value))
		return (-EINVAL);

	zfs_qat_dc_min_buf_size_value = (int)new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_min_buf_size(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_min_buf_size_value));
}

static int
param_set_qat_dc_max_buf_size(const char *val, zfs_kernel_param_t *kp)
{
	int old_value;
	unsigned int new_value;
	char **pvalue = kp->arg;
	int ret;

	if (qat_dc_param_profile(val)) {
		if (qat_dc_init_done &&
		    qat_dc_effective_max_buf_size() !=
		    qat_dc_profile_max_buf_size(zfs_qat_dc_expected_ratio,
		    zfs_qat_dc_profile_recordsize)) {
			return (-EBUSY);
		}

		*pvalue = "profile";
		return (0);
	}

	ret = kstrtouint(val, 0, &new_value);
	if (ret != 0)
		return (ret);

	if (!qat_dc_valid_max_buf_size((int)new_value))
		return (-EINVAL);

	old_value = qat_dc_effective_max_buf_size();
	if (qat_dc_init_done && (int)new_value != old_value)
		return (-EBUSY);

	zfs_qat_dc_max_buf_size_value = (int)new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_max_buf_size(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_max_buf_size_value));
}

static int
param_set_qat_dc_coalesce_src(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_coalesce_src_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_coalesce_src(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_coalesce_src_value));
}

static int
param_set_qat_dc_coalesce_dst(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_coalesce_dst_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_coalesce_dst(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_coalesce_dst_value));
}

static int
param_set_qat_dc_quarantine_dst(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_quarantine_dst_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_quarantine_dst(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_quarantine_dst_value));
}

static int
param_set_qat_dc_async(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_async_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_async(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_async_value));
}

static int
param_set_qat_dc_poll(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		if (qat_dc_init_done && qat_dc_effective_poll() !=
		    qat_dc_profile_poll()) {
			return (-EBUSY);
		}

		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	if (qat_dc_init_done && new_value != qat_dc_effective_poll())
		return (-EBUSY);

	zfs_qat_dc_poll_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_poll(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_poll_value));
}

static int
param_set_qat_dc_poll_interval_us(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, INT_MAX, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_poll_interval_us_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_poll_interval_us(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_poll_interval_us_value));
}

static int
param_set_qat_dc_poll_quota(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, INT_MAX, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_poll_quota_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_poll_quota(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_poll_quota_value));
}

static int
param_set_qat_dc_watchdog(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		wake_up(&qat_dc_watchdog_wq);
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, 1, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_watchdog_value = new_value;
	*pvalue = "manual";
	wake_up(&qat_dc_watchdog_wq);
	return (0);
}

static int
param_get_qat_dc_watchdog(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_watchdog_value));
}

static int
param_set_qat_dc_watchdog_timeout_ms(const char *val,
    zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 1, 3600000, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_watchdog_timeout_ms_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_watchdog_timeout_ms(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n",
	    zfs_qat_dc_watchdog_timeout_ms_value));
}

static int
param_set_qat_dc_watchdog_interval_ms(const char *val,
    zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		wake_up(&qat_dc_watchdog_wq);
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 10, 60000, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_watchdog_interval_ms_value = new_value;
	*pvalue = "manual";
	wake_up(&qat_dc_watchdog_wq);
	return (0);
}

static int
param_get_qat_dc_watchdog_interval_ms(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n",
	    zfs_qat_dc_watchdog_interval_ms_value));
}

static int
param_set_qat_dc_async_submit_retries(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, INT_MAX, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_async_submit_retries_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_async_submit_retries(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n",
	    zfs_qat_dc_async_submit_retries_value));
}

static int
param_set_qat_dc_async_retry_us(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, INT_MAX, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_async_retry_us_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_async_retry_us(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_async_retry_us_value));
}

static int
param_set_qat_dc_async_max_inflight(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;
	int new_value;
	int ret;

	if (qat_dc_param_profile(val)) {
		*pvalue = "profile";
		return (0);
	}

	ret = qat_dc_parse_int_range(val, 0, INT_MAX, &new_value);
	if (ret != 0)
		return (ret);

	zfs_qat_dc_async_max_inflight_value = new_value;
	*pvalue = "manual";
	return (0);
}

static int
param_get_qat_dc_async_max_inflight(char *buffer, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (strcmp(*pvalue, "profile") == 0)
		return (sprintf(buffer, "profile\n"));

	return (sprintf(buffer, "%d\n", zfs_qat_dc_async_max_inflight_value));
}

static int
param_set_qat_dc_async_cap_policy(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (!qat_dc_async_valid_cap_policy(val))
		return (-EINVAL);

	if (strncmp(val, "profile", 7) == 0)
		*pvalue = "profile";
	else if (strncmp(val, "recordsize", 10) == 0)
		*pvalue = "recordsize";
	else if (strncmp(val, "throughput", 10) == 0)
		*pvalue = "throughput";
	else
		*pvalue = "fixed";
	return (0);
}

static int
param_set_qat_dc_profile(const char *val, zfs_kernel_param_t *kp)
{
	char **pvalue = kp->arg;

	if (!qat_dc_profile(val))
		return (-EINVAL);

	if (strncmp(val, "latency", 7) == 0)
		*pvalue = "latency";
	else if (strncmp(val, "throughput", 10) == 0)
		*pvalue = "throughput";
	else if (strncmp(val, "offload", 7) == 0)
		*pvalue = "offload";
	else
		*pvalue = "balanced";
	return (0);
}

static int
param_set_qat_dc_ratio_profile(const char *val, zfs_kernel_param_t *kp)
{
	const char *new_value;
	char **pvalue = kp->arg;

	if (!qat_dc_ratio_profile(val))
		return (-EINVAL);

	if (strncmp(val, "performance", 11) == 0)
		new_value = "performance";
	else if (strncmp(val, "ratio", 5) == 0)
		new_value = "ratio";
	else
		new_value = "balanced";

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_cpa_dc_level, "profile") == 0 &&
	    qat_dc_profile_level(new_value, zfs_qat_dc_expected_ratio) !=
	    qat_dc_effective_level()) {
		return (-EBUSY);
	}

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_cpa_dc_hufftype, "profile") == 0 &&
	    strcmp(qat_dc_profile_hufftype(new_value,
	    zfs_qat_dc_expected_ratio),
	    qat_dc_effective_hufftype()) != 0) {
		return (-EBUSY);
	}

	*pvalue = (char *)new_value;
	return (0);
}

static int
param_set_qat_dc_profile_recordsize(const char *val, zfs_kernel_param_t *kp)
{
	int ret;
	int old_value;
	int *pvalue = kp->arg;

	old_value = *pvalue;
	ret = param_set_int(val, kp);
	if (ret != 0)
		return (ret);

	if (!qat_dc_valid_profile_recordsize(*pvalue)) {
		*pvalue = old_value;
		return (-EINVAL);
	}

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_dc_max_buf_size, "profile") == 0 &&
	    qat_dc_profile_max_buf_size(zfs_qat_dc_expected_ratio,
	    *pvalue) != qat_dc_profile_max_buf_size(
	    zfs_qat_dc_expected_ratio, old_value)) {
		*pvalue = old_value;
		return (-EBUSY);
	}

	return (0);
}

static int
param_set_qat_dc_expected_ratio(const char *val, zfs_kernel_param_t *kp)
{
	const char *new_value;
	char **pvalue = kp->arg;

	if (!qat_dc_expected_ratio(val))
		return (-EINVAL);

	if (strncmp(val, "low", 3) == 0)
		new_value = "low";
	else if (strncmp(val, "medium", 6) == 0)
		new_value = "medium";
	else if (strncmp(val, "high", 4) == 0)
		new_value = "high";
	else
		new_value = "unknown";

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_cpa_dc_level, "profile") == 0 &&
	    qat_dc_profile_level(zfs_qat_dc_ratio_profile, new_value) !=
	    qat_dc_effective_level()) {
		return (-EBUSY);
	}

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_cpa_dc_hufftype, "profile") == 0 &&
	    strcmp(qat_dc_profile_hufftype(zfs_qat_dc_ratio_profile,
	    new_value), qat_dc_effective_hufftype()) != 0) {
		return (-EBUSY);
	}

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_dc_max_buf_size, "profile") == 0 &&
	    qat_dc_profile_max_buf_size(new_value,
	    zfs_qat_dc_profile_recordsize) != qat_dc_effective_max_buf_size()) {
		return (-EBUSY);
	}

	*pvalue = (char *)new_value;
	return (0);
}

module_param_call(zfs_qat_compress_disable, param_set_qat_compress,
    param_get_int, &zfs_qat_compress_disable, 0644);
MODULE_PARM_DESC(zfs_qat_compress_disable,
    "Enable/Disable QAT compression and decompression");

module_param_call(zfs_qat_decompress_disable, param_set_qat_decompress,
    param_get_qat_decompress, &zfs_qat_decompress_disable, 0644);
MODULE_PARM_DESC(zfs_qat_decompress_disable,
    "Enable/Disable QAT decompression: profile, 0, or 1");

module_param_call(zfs_qat_cpa_dc_level, param_set_qat_cpa_dc_level,
    param_get_qat_cpa_dc_level, &zfs_qat_cpa_dc_level, 0644);
MODULE_PARM_DESC(zfs_qat_cpa_dc_level,
    "QAT compression level: profile, 1, 2, 3, or 4");

module_param_call(zfs_qat_cpa_dc_hufftype, param_set_qat_cpa_dc_hufftype,
    param_get_qat_cpa_dc_hufftype, &zfs_qat_cpa_dc_hufftype, 0644);
MODULE_PARM_DESC(zfs_qat_cpa_dc_hufftype,
    "QAT compression Huffman type: profile, dynamic, or static");

module_param_call(zfs_qat_dc_min_buf_size, param_set_qat_dc_min_buf_size,
    param_get_qat_dc_min_buf_size, &zfs_qat_dc_min_buf_size, 0644);
MODULE_PARM_DESC(zfs_qat_dc_min_buf_size,
    "Minimum QAT compression buffer size: profile, 8192, 16384, 32768, "
    "65536, 131072, 262144, 524288, or 1048576");

module_param_call(zfs_qat_dc_max_buf_size, param_set_qat_dc_max_buf_size,
    param_get_qat_dc_max_buf_size, &zfs_qat_dc_max_buf_size, 0644);
MODULE_PARM_DESC(zfs_qat_dc_max_buf_size,
    "Maximum QAT compression buffer size: profile, 131072, 262144, "
    "524288, or 1048576");

module_param_call(zfs_qat_dc_max_instances, param_set_qat_dc_max_instances,
    param_get_int, &zfs_qat_dc_max_instances, 0644);
MODULE_PARM_DESC(zfs_qat_dc_max_instances,
    "Maximum QAT compression instances to use");

module_param_call(zfs_qat_dc_coalesce_src, param_set_qat_dc_coalesce_src,
    param_get_qat_dc_coalesce_src, &zfs_qat_dc_coalesce_src, 0644);
MODULE_PARM_DESC(zfs_qat_dc_coalesce_src,
    "Enable/Disable experimental QAT compression source coalescing: "
    "profile, 0, or 1");

module_param_call(zfs_qat_dc_coalesce_dst, param_set_qat_dc_coalesce_dst,
    param_get_qat_dc_coalesce_dst, &zfs_qat_dc_coalesce_dst, 0644);
MODULE_PARM_DESC(zfs_qat_dc_coalesce_dst,
    "Enable/Disable experimental QAT compression destination coalescing: "
    "profile, 0, or 1");

module_param_call(zfs_qat_dc_quarantine_dst,
    param_set_qat_dc_quarantine_dst, param_get_qat_dc_quarantine_dst,
    &zfs_qat_dc_quarantine_dst, 0644);
MODULE_PARM_DESC(zfs_qat_dc_quarantine_dst,
    "Enable/Disable experimental QAT compression destination quarantine: "
    "profile, 0, or 1");

module_param_call(zfs_qat_dc_async, param_set_qat_dc_async,
    param_get_qat_dc_async, &zfs_qat_dc_async, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async,
    "Enable/Disable asynchronous QAT compression: profile, "
    "0, or 1");

module_param_call(zfs_qat_dc_async_submit_retries,
    param_set_qat_dc_async_submit_retries,
    param_get_qat_dc_async_submit_retries,
    &zfs_qat_dc_async_submit_retries, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async_submit_retries,
    "QAT async compression submit retries after CPA_STATUS_RETRY: profile "
    "or integer");

module_param_call(zfs_qat_dc_async_retry_us, param_set_qat_dc_async_retry_us,
    param_get_qat_dc_async_retry_us, &zfs_qat_dc_async_retry_us, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async_retry_us,
    "QAT async compression submit retry backoff in microseconds: profile "
    "or integer");

module_param_call(zfs_qat_dc_async_max_inflight,
    param_set_qat_dc_async_max_inflight,
    param_get_qat_dc_async_max_inflight,
    &zfs_qat_dc_async_max_inflight, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async_max_inflight,
    "Maximum in-flight asynchronous QAT compression requests: "
    "profile or integer");

module_param_call(zfs_qat_dc_async_cap_policy,
    param_set_qat_dc_async_cap_policy, param_get_charp,
    &zfs_qat_dc_async_cap_policy, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async_cap_policy,
    "QAT async cap policy: profile, fixed, recordsize, or throughput");

module_param_call(zfs_qat_dc_poll, param_set_qat_dc_poll,
    param_get_qat_dc_poll, &zfs_qat_dc_poll, 0644);
MODULE_PARM_DESC(zfs_qat_dc_poll,
    "Enable/Disable synchronous QAT DC polling: profile, 0, or 1. "
    "Requires QAT DC instances configured with matching DcNIsPolled mode");

module_param_call(zfs_qat_dc_poll_interval_us,
    param_set_qat_dc_poll_interval_us, param_get_qat_dc_poll_interval_us,
    &zfs_qat_dc_poll_interval_us, 0644);
MODULE_PARM_DESC(zfs_qat_dc_poll_interval_us,
    "Synchronous QAT DC poll sleep interval in microseconds: "
    "profile or integer");

module_param_call(zfs_qat_dc_poll_quota,
    param_set_qat_dc_poll_quota, param_get_qat_dc_poll_quota,
    &zfs_qat_dc_poll_quota, 0644);
MODULE_PARM_DESC(zfs_qat_dc_poll_quota,
    "Synchronous QAT DC poll response quota: profile or integer");

module_param_call(zfs_qat_dc_watchdog,
    param_set_qat_dc_watchdog, param_get_qat_dc_watchdog,
    &zfs_qat_dc_watchdog, 0644);
MODULE_PARM_DESC(zfs_qat_dc_watchdog,
    "Enable/Disable QAT DC no-progress runtime watchdog: profile, 0, or 1");

module_param_call(zfs_qat_dc_watchdog_timeout_ms,
    param_set_qat_dc_watchdog_timeout_ms,
    param_get_qat_dc_watchdog_timeout_ms,
    &zfs_qat_dc_watchdog_timeout_ms, 0644);
MODULE_PARM_DESC(zfs_qat_dc_watchdog_timeout_ms,
    "QAT DC watchdog no-progress timeout in milliseconds: profile or "
    "integer");

module_param_call(zfs_qat_dc_watchdog_interval_ms,
    param_set_qat_dc_watchdog_interval_ms,
    param_get_qat_dc_watchdog_interval_ms,
    &zfs_qat_dc_watchdog_interval_ms, 0644);
MODULE_PARM_DESC(zfs_qat_dc_watchdog_interval_ms,
    "QAT DC watchdog check interval in milliseconds: profile or integer");

module_param_call(zfs_qat_dc_profile, param_set_qat_dc_profile,
    param_get_charp, &zfs_qat_dc_profile, 0644);
MODULE_PARM_DESC(zfs_qat_dc_profile,
    "QAT compression profile: balanced, latency, throughput, or offload");

module_param_call(zfs_qat_dc_profile_recordsize,
    param_set_qat_dc_profile_recordsize, param_get_int,
    &zfs_qat_dc_profile_recordsize, 0644);
MODULE_PARM_DESC(zfs_qat_dc_profile_recordsize,
    "QAT compression profile target record size");

module_param_call(zfs_qat_dc_ratio_profile, param_set_qat_dc_ratio_profile,
    param_get_charp, &zfs_qat_dc_ratio_profile, 0644);
MODULE_PARM_DESC(zfs_qat_dc_ratio_profile,
    "QAT compression ratio profile: balanced, performance, or ratio");

module_param_call(zfs_qat_dc_expected_ratio,
    param_set_qat_dc_expected_ratio, param_get_charp,
    &zfs_qat_dc_expected_ratio, 0644);
MODULE_PARM_DESC(zfs_qat_dc_expected_ratio,
    "Expected compression ratio class: unknown, low, medium, or high");

#endif
