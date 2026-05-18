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
#include <sys/zfs_context.h>
#include <sys/byteorder.h>
#include <sys/zio.h>
#include <sys/qat.h>

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

typedef struct qat_dc_buffer_slot {
	Cpa8U *buffer_meta_src;
	Cpa8U *buffer_meta_dst;
	CpaBufferList *buf_list_src;
	CpaBufferList *buf_list_dst;
	void *coalesced_dst;
	Cpa32U coalesced_dst_size;
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

typedef struct qat_dc_callback_ctx {
	uint32_t magic;
	qat_dc_callback_type_t type;
	union {
		struct completion *sync;
		qat_dc_async_t *async;
	} u;
} qat_dc_callback_ctx_t;

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
	Cpa32U src_pages;
	Cpa32U dst_pages;
	Cpa32U add_pages;
	size_t in_pages_size;
	size_t out_pages_size;
	size_t scratch_pages_size;
	void *coalesced_src;
	void *coalesced_dst;
	hrtime_t submit_end;
	volatile uint32_t armed;
	qat_dc_callback_ctx_t callback_ctx;
};

static CpaInstanceHandle dc_inst_handles[QAT_DC_MAX_INSTANCES];
static CpaDcSessionHandle session_handles[QAT_DC_MAX_INSTANCES];
static CpaBufferList **buffer_array[QAT_DC_MAX_INSTANCES];
static qat_dc_buffer_pool_t buffer_pools[QAT_DC_MAX_INSTANCES];
static Cpa16U num_inst = 0;
static Cpa32U inst_num = 0;
static boolean_t qat_dc_init_done = B_FALSE;
static int qat_dc_effective_decompress_disable(void);
static int qat_dc_effective_level(void);
static const char *qat_dc_effective_hufftype(void);
static int qat_dc_effective_max_buf_size(void);
static int qat_dc_effective_coalesce_src(void);
static int qat_dc_effective_coalesce_dst(void);
static int qat_dc_effective_async(void);
static int qat_dc_effective_async_submit_retries(void);
static int qat_dc_effective_async_retry_us(void);
static int qat_dc_effective_async_max_inflight(void);
int zfs_qat_compress_disable = 0;
char *zfs_qat_decompress_disable = "profile";
static int zfs_qat_decompress_disable_value = 0;
char *zfs_qat_cpa_dc_level = "profile";
static int zfs_qat_cpa_dc_level_value = 1;
char *zfs_qat_cpa_dc_hufftype = "profile";
static const char *zfs_qat_cpa_dc_hufftype_value = "dynamic";
char *zfs_qat_dc_max_buf_size = "profile";
static int zfs_qat_dc_max_buf_size_value = QAT_DC_DEFAULT_MAX_BUF_SIZE;
int zfs_qat_dc_max_instances = QAT_DC_MAX_INSTANCES;
char *zfs_qat_dc_coalesce_src = "profile";
static int zfs_qat_dc_coalesce_src_value = 0;
char *zfs_qat_dc_coalesce_dst = "profile";
static int zfs_qat_dc_coalesce_dst_value = 0;
char *zfs_qat_dc_async = "profile";
static int zfs_qat_dc_async_value = 0;
char *zfs_qat_dc_async_submit_retries = "profile";
static int zfs_qat_dc_async_submit_retries_value = 8;
char *zfs_qat_dc_async_retry_us = "profile";
static int zfs_qat_dc_async_retry_us_value = 100;
char *zfs_qat_dc_async_max_inflight = "profile";
static int zfs_qat_dc_async_max_inflight_value = 96;
char *zfs_qat_dc_async_cap_policy = "profile";
char *zfs_qat_dc_profile = "balanced";
int zfs_qat_dc_profile_recordsize = 128 * 1024;
char *zfs_qat_dc_ratio_profile = "balanced";

boolean_t
qat_dc_compress_use_accel(size_t s_len)
{
	int max_buf_size = qat_dc_effective_max_buf_size();

	return (!zfs_qat_compress_disable &&
	    qat_dc_init_done &&
	    s_len >= QAT_DC_MIN_BUF_SIZE &&
	    s_len <= max_buf_size);
}

boolean_t
qat_dc_decompress_use_accel(size_t s_len)
{
	int max_buf_size = qat_dc_effective_max_buf_size();

	return (!zfs_qat_compress_disable &&
	    !qat_dc_effective_decompress_disable() &&
	    qat_dc_init_done &&
	    s_len >= QAT_DC_MIN_BUF_SIZE &&
	    s_len <= max_buf_size);
}

static boolean_t
qat_dc_valid_level(int level)
{
	return (level >= 1 && level <= 4);
}

static int
qat_dc_profile_level(const char *ratio_profile)
{
	if (strcmp(ratio_profile, "ratio") == 0)
		return (4);

	return (1);
}

static int
qat_dc_effective_level(void)
{
	if (strcmp(zfs_qat_cpa_dc_level, "profile") == 0)
		return (qat_dc_profile_level(zfs_qat_dc_ratio_profile));

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

static int
qat_dc_effective_max_buf_size(void)
{
	if (strcmp(zfs_qat_dc_max_buf_size, "profile") == 0)
		return (zfs_qat_dc_profile_recordsize);

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
qat_dc_profile_hufftype(const char *ratio_profile)
{
	if (strcmp(ratio_profile, "performance") == 0)
		return ("static");

	return ("dynamic");
}

static const char *
qat_dc_effective_hufftype(void)
{
	if (strcmp(zfs_qat_cpa_dc_hufftype, "profile") == 0)
		return (qat_dc_profile_hufftype(zfs_qat_dc_ratio_profile));

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
qat_dc_async_recordsize_cap(int src_len, int *cap, boolean_t throughput)
{
	uint_t cap_instances;
	int per_inst_cap;

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

	if (cap_instances == 0)
		return (B_FALSE);

	if (src_len == 128 * 1024) {
		per_inst_cap = 128;
	} else if (src_len == 256 * 1024) {
		per_inst_cap = 32;
	} else if (src_len >= 512 * 1024) {
		per_inst_cap = 16;
	} else {
		return (B_TRUE);
	}

	*cap = per_inst_cap * (int)cap_instances;
	return (B_TRUE);
}

static boolean_t
qat_dc_profile_throughput_cap(void)
{
	return ((strcmp(zfs_qat_dc_profile, "throughput") == 0 ||
	    strcmp(zfs_qat_dc_profile, "offload") == 0) &&
	    zfs_qat_dc_profile_recordsize >= 1024 * 1024);
}

static boolean_t
qat_dc_async_effective_cap(int src_len, int *cap)
{
	*cap = qat_dc_effective_async_max_inflight();

	if (strcmp(zfs_qat_dc_async_cap_policy, "profile") == 0) {
		return (qat_dc_async_recordsize_cap(src_len, cap,
		    qat_dc_profile_throughput_cap()));
	}

	if (strcmp(zfs_qat_dc_async_cap_policy, "recordsize") == 0)
		return (qat_dc_async_recordsize_cap(src_len, cap, B_FALSE));

	if (strcmp(zfs_qat_dc_async_cap_policy, "throughput") == 0)
		return (qat_dc_async_recordsize_cap(src_len, cap, B_TRUE));

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

static void qat_dc_inflight_exit(qat_compress_dir_t dir);
static void qat_dc_async_inflight_exit(void);

static void
qat_dc_callback(void *p_callback, CpaStatus status)
{
	qat_dc_callback_ctx_t *ctx = p_callback;
	qat_dc_async_t *req;
	hrtime_t end;

	if (ctx == NULL || ctx->magic != QAT_DC_CALLBACK_MAGIC)
		return;

	if (ctx->type == QAT_DC_CALLBACK_SYNC) {
		complete(ctx->u.sync);
		return;
	}

	req = ctx->u.async;
	req->callback_status = status;
	end = gethrtime();
	qat_dc_inflight_exit(QAT_COMPRESS);
	qat_dc_async_inflight_exit();
	QAT_STAT_ADD_TIME(dc_compress_wait_ns, req->submit_end, end);
	QAT_STAT_BUMP(dc_compress_async_completions);
	membar_producer();
	atomic_swap_32((uint32_t *)&req->complete, 1);
	if (req->armed && req->resume != NULL) {
		QAT_STAT_BUMP(dc_compress_async_resumes);
		req->resume(req->resume_arg);
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
qat_dc_inflight_enter(qat_compress_dir_t dir)
{
	uint64_t inflight;

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
}

static void
qat_dc_inflight_exit(qat_compress_dir_t dir)
{
	if (dir == QAT_COMPRESS) {
		(void) atomic_dec_64_nv(
		    &qat_stats.dc_compress_inflight.value.ui64);
	} else {
		(void) atomic_dec_64_nv(
		    &qat_stats.dc_decompress_inflight.value.ui64);
	}
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

static boolean_t
qat_dc_try_coalesce_src(char **src, int src_len, void **coalesced_src)
{
	CpaStatus status;
	hrtime_t start;
	hrtime_t end;

	if (!qat_dc_effective_coalesce_src())
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

	if (!qat_dc_effective_coalesce_dst())
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
		QAT_PHYS_CONTIG_FREE(slot->coalesced_dst);
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

	for (Cpa16U i = 0; i < num_inst; i++) {
		cpaDcStopInstance(dc_inst_handles[i]);
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
	CpaDcRqResults dc_results = {.checksum = 1};
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
	struct completion complete;
	qat_dc_callback_ctx_t callback_ctx;
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
	boolean_t local_add_alloc = B_FALSE;

	/*
	 * We increment num_src_buf and num_dst_buf by 2 to allow
	 * us to handle non page-aligned buffer addresses and buffers
	 * whose sizes are not divisible by PAGE_SIZE.
	 */
	src_coalesced = (dir == QAT_COMPRESS &&
	    qat_dc_try_coalesce_src(&src, src_len, &coalesced_src));
	dst_coalesce_requested = (dir == QAT_COMPRESS &&
	    qat_dc_effective_coalesce_dst());
	dst_coalesced = B_FALSE;

	num_src_buf = src_coalesced ? 1 : ((src_len >> PAGE_SHIFT) + 2);
	num_dst_buf = dst_coalesce_requested ? 1 :
	    ((dst_len >> PAGE_SHIFT) + 2);
	num_add_buf = dst_coalesce_requested ? 0 :
	    ((add_len >> PAGE_SHIFT) + 2);

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
		} else {
			if (buffer_slot != NULL) {
				qat_dc_buffer_pool_release(i, buffer_slot);
				buffer_slot = NULL;
			}

			num_dst_buf = (dst_len >> PAGE_SHIFT) + 2;
			num_add_buf = (add_len >> PAGE_SHIFT) + 2;
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
		add = zio_data_buf_alloc(add_len);
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_alloc_ns,
		    scratch_start, scratch_end);
		if (add == NULL)
			goto fail;
		local_add_alloc = B_TRUE;
	}

	if (num_src_buf > QAT_DC_STACK_MAX_PAGES) {
		in_pages_size = num_src_buf * sizeof (*in_pages);
		in_pages = kmem_alloc(in_pages_size, KM_SLEEP);
		if (in_pages == NULL)
			goto fail;
	}

	if (num_dst_buf > QAT_DC_STACK_MAX_PAGES) {
		out_pages_size = num_dst_buf * sizeof (*out_pages);
		out_pages = kmem_alloc(out_pages_size, KM_SLEEP);
		if (out_pages == NULL)
			goto fail;
	}

	if (add_len > 0 && !dst_coalesced) {
		scratch_pages_size = num_add_buf * sizeof (*scratch_pages);
		scratch_pages = kmem_alloc(scratch_pages_size, KM_SLEEP);
		if (scratch_pages == NULL)
			goto fail;
	}

	if (buffer_slot != NULL) {
		buffer_meta_src = buffer_slot->buffer_meta_src;
		buffer_meta_dst = buffer_slot->buffer_meta_dst;
		buf_list_src = buffer_slot->buf_list_src;
		buf_list_dst = buffer_slot->buf_list_dst;
	} else {
		cpaDcBufferListGetMetaSize(dc_inst_handle, num_src_buf,
		    &buffer_meta_size);
		status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_src,
		    buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		cpaDcBufferListGetMetaSize(dc_inst_handle,
		    num_dst_buf + num_add_buf, &buffer_meta_size);
		status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_dst,
		    buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		status = QAT_PHYS_CONTIG_ALLOC(&buf_list_src,
		    src_buffer_list_mem_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		status = QAT_PHYS_CONTIG_ALLOC(&buf_list_dst,
		    dst_buffer_list_mem_size);
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

			bytes_left -= flat_buf_dst->dataLenInBytes;
			data += flat_buf_dst->dataLenInBytes;
			flat_buf_dst++;
			buf_list_dst->numBuffers++;
			page_num++;
		}
		add_pages = page_num;
	}

	init_completion(&complete);
	callback_ctx.magic = QAT_DC_CALLBACK_MAGIC;
	callback_ctx.type = QAT_DC_CALLBACK_SYNC;
	callback_ctx.u.sync = &complete;

	if (dir == QAT_COMPRESS) {
		QAT_STAT_BUMP(comp_requests);
		QAT_STAT_INCR(comp_total_in_bytes, src_len);
		qat_dc_record_compress_shape(buf_list_src->numBuffers,
		    dst_coalesced ? buf_list_dst->numBuffers : dst_pages,
		    add_pages);

		cpaDcGenerateHeader(session_handle,
		    buf_list_dst->pBuffers, &hdr_sz);
		buf_list_dst->pBuffers->pData += hdr_sz;
		buf_list_dst->pBuffers->dataLenInBytes -= hdr_sz;
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_setup_ns, op_start, phase_end);
		phase_start = gethrtime();
		qat_dc_inflight_enter(dir);
		status = cpaDcCompressData(
		    dc_inst_handle, session_handle,
		    buf_list_src, buf_list_dst,
		    &dc_results, CPA_DC_FLUSH_FINAL,
		    &callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_submit_ns, phase_start, phase_end);
		if (status != CPA_STATUS_SUCCESS) {
			qat_dc_inflight_exit(dir);
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		phase_start = gethrtime();
		wait_for_completion(&complete);
		phase_end = gethrtime();
		qat_dc_inflight_exit(dir);
		QAT_STAT_ADD_TIME(dc_compress_wait_ns, phase_start, phase_end);

		if (dc_results.status != CPA_STATUS_SUCCESS) {
			if (dc_results.status == CPA_DC_OVERFLOW)
				QAT_STAT_BUMP(dc_compress_overflows);
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		compressed_sz = dc_results.produced;
		if (compressed_sz + hdr_sz + ZLIB_FOOT_SZ > dst_len) {
			QAT_STAT_BUMP(dc_compress_incompressible);
			status = CPA_STATUS_INCOMPRESSIBLE;
			goto fail;
		}

		/* get adler32 checksum and append footer */
		*(Cpa32U*)(dst + hdr_sz + compressed_sz) =
		    BSWAP_32(dc_results.checksum);

		*c_len = hdr_sz + compressed_sz + ZLIB_FOOT_SZ;
		if (dst_coalesced) {
			free_start = gethrtime();
			memcpy(orig_dst, dst, *c_len);
			free_end = gethrtime();
			QAT_STAT_ADD_TIME(dc_compress_dst_coalesce_copy_ns,
			    free_start, free_end);
			QAT_STAT_INCR(dc_compress_dst_coalesce_copy_bytes,
			    *c_len);
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
		qat_dc_inflight_enter(dir);
		status = cpaDcDecompressData(dc_inst_handle, session_handle,
		    buf_list_src, buf_list_dst, &dc_results, CPA_DC_FLUSH_FINAL,
		    &callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_decompress_submit_ns, phase_start, phase_end);

		if (CPA_STATUS_SUCCESS != status) {
			qat_dc_inflight_exit(dir);
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		/* we now wait until the completion of the operation. */
		phase_start = gethrtime();
		wait_for_completion(&complete);
		phase_end = gethrtime();
		qat_dc_inflight_exit(dir);
		QAT_STAT_ADD_TIME(dc_decompress_wait_ns, phase_start, phase_end);

		if (dc_results.status != CPA_STATUS_SUCCESS) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}

		/* verify adler checksum */
		adler32 = *(Cpa32U *)(src + dc_results.consumed + ZLIB_HEAD_SZ);
		if (adler32 != BSWAP_32(dc_results.checksum)) {
			status = CPA_STATUS_FAIL;
			goto fail;
		}
		*c_len = dc_results.produced;
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

	if (buffer_slot != NULL) {
		qat_dc_buffer_pool_release(i, buffer_slot);
	} else {
		QAT_PHYS_CONTIG_FREE(buffer_meta_src);
		QAT_PHYS_CONTIG_FREE(buffer_meta_dst);
		QAT_PHYS_CONTIG_FREE(buf_list_src);
		QAT_PHYS_CONTIG_FREE(buf_list_dst);
	}

	if (in_pages != in_pages_stack && in_pages != NULL)
		kmem_free(in_pages, in_pages_size);

	if (out_pages != out_pages_stack && out_pages != NULL)
		kmem_free(out_pages, out_pages_size);

	if (scratch_pages != NULL)
		kmem_free(scratch_pages, scratch_pages_size);

	if (local_add_alloc && add != NULL) {
		scratch_start = gethrtime();
		zio_data_buf_free(add, add_len);
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_free_ns, scratch_start,
		    scratch_end);
	}

	if (coalesced_src != NULL) {
		free_start = gethrtime();
		QAT_PHYS_CONTIG_FREE(coalesced_src);
		free_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_coalesce_free_ns, free_start,
		    free_end);
	}

	if (coalesced_dst != NULL) {
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
		scratch_start = gethrtime();
		if (add_len > 0 && !qat_dc_effective_coalesce_dst())
			add = zio_data_buf_alloc(add_len);
		scratch_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_alloc_ns, scratch_start,
		    scratch_end);
	}

	ret = qat_compress_impl(dir, src, src_len, dst,
	    dst_len, add, add_len, c_len);

	if (dir == QAT_COMPRESS) {
		scratch_start = gethrtime();
		if (add != NULL)
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
	hrtime_t start;
	hrtime_t end;
	hrtime_t free_start;
	hrtime_t free_end;

	if (req == NULL)
		return;

	start = gethrtime();

	for (Cpa32U page_num = 0; page_num < req->src_pages; page_num++)
		kunmap(req->in_pages[page_num]);

	for (Cpa32U page_num = 0; page_num < req->dst_pages; page_num++)
		kunmap(req->out_pages[page_num]);

	for (Cpa32U page_num = 0; page_num < req->add_pages; page_num++)
		kunmap(req->scratch_pages[page_num]);

	if (req->buffer_slot != NULL) {
		qat_dc_buffer_pool_release(req->inst, req->buffer_slot);
	} else {
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_src);
		QAT_PHYS_CONTIG_FREE(req->buffer_meta_dst);
		QAT_PHYS_CONTIG_FREE(req->buf_list_src);
		QAT_PHYS_CONTIG_FREE(req->buf_list_dst);
	}

	if (req->in_pages != NULL)
		kmem_free(req->in_pages, req->in_pages_size);

	if (req->out_pages != NULL)
		kmem_free(req->out_pages, req->out_pages_size);

	if (req->scratch_pages != NULL)
		kmem_free(req->scratch_pages, req->scratch_pages_size);

	if (req->add != NULL)
		zio_data_buf_free(req->add, req->add_len);

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

	kmem_free(req, sizeof (*req));
}

qat_dc_async_t *
qat_dc_compress_async_submit(char *src, int src_len, char *dst, int dst_len,
    void (*resume)(void *), void *resume_arg)
{
	qat_dc_async_t *req = NULL;
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
	CpaStatus status = CPA_STATUS_FAIL;
	char *data;
	struct page *page;
	hrtime_t op_start = gethrtime();
	hrtime_t phase_start;
	hrtime_t phase_end;
	int attempt = 0;
	int retry_limit;
	boolean_t async_inflight = B_FALSE;
	boolean_t src_coalesced;
	boolean_t dst_coalesced = B_FALSE;
	boolean_t dst_coalesce_requested;

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

	req = kmem_zalloc(sizeof (*req), KM_SLEEP);
	req->src = src;
	req->dst = dst;
	req->orig_dst = dst;
	req->src_len = src_len;
	req->dst_len = dst_len;
	req->resume = resume;
	req->resume_arg = resume_arg;
	req->dc_results.checksum = 1;
	req->submit_status = CPA_STATUS_FAIL;

	req->add_len = qat_dc_compress_scratch_len(src_len, dst_len);
	src_coalesced = qat_dc_try_coalesce_src(&req->src, req->src_len,
	    &req->coalesced_src);
	dst_coalesce_requested = qat_dc_effective_coalesce_dst();

	num_src_buf = src_coalesced ? 1 : ((src_len >> PAGE_SHIFT) + 2);
	num_dst_buf = dst_coalesce_requested ? 1 :
	    ((dst_len >> PAGE_SHIFT) + 2);
	num_add_buf = dst_coalesce_requested ? 0 :
	    ((req->add_len >> PAGE_SHIFT) + 2);

	if (num_src_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
	    num_add_buf > QAT_DC_ABS_MAX_PAGES)
		goto fail;

	req->inst = (Cpa32U)atomic_inc_32_nv(&inst_num) % num_inst;
	dc_inst_handle = dc_inst_handles[req->inst];
	session_handle = session_handles[req->inst];

	req->buffer_slot = qat_dc_buffer_pool_acquire(req->inst, num_src_buf,
	    num_dst_buf + num_add_buf);

	if (dst_coalesce_requested) {
		dst_coalesced = qat_dc_try_coalesce_dst(&req->dst, dst_len,
		    req->add_len, req->buffer_slot, &req->coalesced_dst);
		if (dst_coalesced) {
			coalesced_dst_len = (Cpa32U)(dst_len + req->add_len);
		} else {
			if (req->buffer_slot != NULL) {
				qat_dc_buffer_pool_release(req->inst,
				    req->buffer_slot);
				req->buffer_slot = NULL;
			}

			num_dst_buf = (dst_len >> PAGE_SHIFT) + 2;
			num_add_buf = (req->add_len >> PAGE_SHIFT) + 2;
			if (num_dst_buf > QAT_DC_ABS_MAX_PAGES ||
			    num_add_buf > QAT_DC_ABS_MAX_PAGES)
				goto fail;

			req->buffer_slot = qat_dc_buffer_pool_acquire(
			    req->inst, num_src_buf, num_dst_buf + num_add_buf);
		}
	}

	if (req->add_len > 0 && !dst_coalesced) {
		phase_start = gethrtime();
		req->add = zio_data_buf_alloc(req->add_len);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_scratch_alloc_ns, phase_start,
		    phase_end);
		if (req->add == NULL)
			goto fail;
	}

	src_buffer_list_mem_size = sizeof (CpaBufferList) +
	    (num_src_buf * sizeof (CpaFlatBuffer));
	dst_buffer_list_mem_size = sizeof (CpaBufferList) +
	    ((num_dst_buf + num_add_buf) * sizeof (CpaFlatBuffer));

	req->in_pages_size = num_src_buf * sizeof (*req->in_pages);
	req->out_pages_size = num_dst_buf * sizeof (*req->out_pages);
	req->scratch_pages_size = num_add_buf * sizeof (*req->scratch_pages);
	req->in_pages = kmem_zalloc(req->in_pages_size, KM_SLEEP);
	req->out_pages = kmem_zalloc(req->out_pages_size, KM_SLEEP);
	if (num_add_buf > 0)
		req->scratch_pages = kmem_zalloc(req->scratch_pages_size,
		    KM_SLEEP);

	if (req->buffer_slot != NULL) {
		buffer_meta_src = req->buffer_slot->buffer_meta_src;
		buffer_meta_dst = req->buffer_slot->buffer_meta_dst;
		req->buf_list_src = req->buffer_slot->buf_list_src;
		req->buf_list_dst = req->buffer_slot->buf_list_dst;
	} else {
		status = cpaDcBufferListGetMetaSize(dc_inst_handle, num_src_buf,
		    &buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
		status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_src,
		    buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
		req->buffer_meta_src = buffer_meta_src;

		status = cpaDcBufferListGetMetaSize(dc_inst_handle,
		    num_dst_buf + num_add_buf, &buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
		status = QAT_PHYS_CONTIG_ALLOC(&buffer_meta_dst,
		    buffer_meta_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;
		req->buffer_meta_dst = buffer_meta_dst;

		status = QAT_PHYS_CONTIG_ALLOC(&req->buf_list_src,
		    src_buffer_list_mem_size);
		if (status != CPA_STATUS_SUCCESS)
			goto fail;

		status = QAT_PHYS_CONTIG_ALLOC(&req->buf_list_dst,
		    dst_buffer_list_mem_size);
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
		qat_dc_inflight_enter(QAT_COMPRESS);
		status = cpaDcCompressData(dc_inst_handle, session_handle,
		    req->buf_list_src, req->buf_list_dst, &req->dc_results,
		    CPA_DC_FLUSH_FINAL, &req->callback_ctx);
		phase_end = gethrtime();
		QAT_STAT_ADD_TIME(dc_compress_submit_ns, phase_start,
		    phase_end);
		if (status == CPA_STATUS_SUCCESS)
			break;

		qat_dc_inflight_exit(QAT_COMPRESS);
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
	if (req == NULL)
		return;

	membar_producer();
	atomic_swap_32((uint32_t *)&req->armed, 1);
	if (req->complete && req->resume != NULL) {
		QAT_STAT_BUMP(dc_compress_async_resumes);
		req->resume(req->resume_arg);
	}
}

boolean_t
qat_dc_compress_async_complete(qat_dc_async_t *req)
{
	if (req == NULL)
		return (B_FALSE);

	membar_consumer();
	return (req->complete != 0);
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
		    qat_dc_profile_level(zfs_qat_dc_ratio_profile)) {
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
		    zfs_qat_dc_ratio_profile), &huff_type)) {
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
param_set_qat_dc_max_buf_size(const char *val, zfs_kernel_param_t *kp)
{
	int old_value;
	unsigned int new_value;
	char **pvalue = kp->arg;
	int ret;

	if (qat_dc_param_profile(val)) {
		if (qat_dc_init_done &&
		    qat_dc_effective_max_buf_size() !=
		    zfs_qat_dc_profile_recordsize) {
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
	    qat_dc_profile_level(new_value) != qat_dc_effective_level()) {
		return (-EBUSY);
	}

	if (qat_dc_init_done &&
	    strcmp(zfs_qat_cpa_dc_hufftype, "profile") == 0 &&
	    strcmp(qat_dc_profile_hufftype(new_value),
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
	    *pvalue != old_value) {
		*pvalue = old_value;
		return (-EBUSY);
	}

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

module_param_call(zfs_qat_dc_max_buf_size, param_set_qat_dc_max_buf_size,
    param_get_qat_dc_max_buf_size, &zfs_qat_dc_max_buf_size, 0644);
MODULE_PARM_DESC(zfs_qat_dc_max_buf_size,
    "Maximum QAT compression buffer size");

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

module_param_call(zfs_qat_dc_async, param_set_qat_dc_async,
    param_get_qat_dc_async, &zfs_qat_dc_async, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async,
    "Enable/Disable experimental asynchronous QAT compression: profile, "
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
    "Maximum in-flight experimental asynchronous QAT compression requests: "
    "profile or integer");

module_param_call(zfs_qat_dc_async_cap_policy,
    param_set_qat_dc_async_cap_policy, param_get_charp,
    &zfs_qat_dc_async_cap_policy, 0644);
MODULE_PARM_DESC(zfs_qat_dc_async_cap_policy,
    "QAT async cap policy: profile, fixed, recordsize, or throughput");

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

#endif
