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

#ifndef	_SYS_QAT_H
#define	_SYS_QAT_H

typedef enum qat_compress_dir {
	QAT_DECOMPRESS = 0,
	QAT_COMPRESS = 1,
} qat_compress_dir_t;

typedef enum qat_encrypt_dir {
	QAT_DECRYPT = 0,
	QAT_ENCRYPT = 1,
} qat_encrypt_dir_t;

typedef struct qat_dc_async qat_dc_async_t;

#if defined(_KERNEL) && defined(HAVE_QAT)
#include <sys/zio.h>
#include <sys/crypto/api.h>
#include "cpa.h"
#include "dc/cpa_dc.h"
#include "lac/cpa_cy_sym.h"

/*
 * QAT hardware can process outside these sizes, but these bounds avoid
 * cases where offload setup costs or failures dominate observed benefit.
 */
#define	QAT_MIN_BUF_SIZE	(4*1024)
#define	QAT_MAX_BUF_SIZE	(128*1024)

/*
 * Compression uses a higher minimum than crypto/checksum. Phase 4 testing
 * on dh895xcc with QAT 4.28 showed 4 KiB gzip records produced QAT DC
 * failures, while 8 KiB and larger records did not.
 */
#define	QAT_DC_MIN_BUF_SIZE	(8*1024)
#define	QAT_DC_DEFAULT_MAX_BUF_SIZE	QAT_MAX_BUF_SIZE
#define	QAT_DC_ABS_MAX_BUF_SIZE		(1024*1024)

/*
 * Used for QAT kstat.
 */
typedef struct qat_stats {
	/*
	 * Number of jobs submitted to QAT compression engine.
	 */
	kstat_named_t comp_requests;
	/*
	 * Total bytes sent to QAT compression engine.
	 */
	kstat_named_t comp_total_in_bytes;
	/*
	 * Total bytes output from QAT compression engine.
	 */
	kstat_named_t comp_total_out_bytes;
	/*
	 * Number of jobs submitted to QAT de-compression engine.
	 */
	kstat_named_t decomp_requests;
	/*
	 * Total bytes sent to QAT de-compression engine.
	 */
	kstat_named_t decomp_total_in_bytes;
	/*
	 * Total bytes output from QAT de-compression engine.
	 */
	kstat_named_t decomp_total_out_bytes;
	/*
	 * Number of fails in the QAT compression / decompression engine.
	 * Note: when a QAT error happens, it doesn't necessarily indicate a
	 * critical hardware issue. Sometimes it is because the output buffer
	 * is not big enough. The compression job will be transferred to the
	 * gzip software implementation so the functionality of ZFS is not
	 * impacted.
	 */
	kstat_named_t dc_fails;
	/*
	 * Number of QAT compression requests that reused preallocated
	 * per-instance buffer-list metadata.
	 */
	kstat_named_t dc_buffer_reuse_hits;
	/*
	 * Number of QAT compression requests that fell back to per-request
	 * buffer-list metadata allocation.
	 */
	kstat_named_t dc_buffer_reuse_misses;
	/*
	 * QAT deflate bound and output sizing counters.
	 */
	kstat_named_t dc_compress_bound_requests;
	kstat_named_t dc_compress_bound_fails;
	kstat_named_t dc_compress_bound_ns;
	kstat_named_t dc_compress_bound_total_bytes;
	kstat_named_t dc_compress_dst_total_bytes;
	kstat_named_t dc_compress_scratch_bytes;
	kstat_named_t dc_compress_scratch_saved_bytes;
	kstat_named_t dc_compress_overflows;
	kstat_named_t dc_compress_incompressible;
	/*
	 * QAT compression buffer-list shape counters.
	 */
	kstat_named_t dc_compress_src_buffers;
	kstat_named_t dc_compress_dst_buffers;
	kstat_named_t dc_compress_add_buffers;
	kstat_named_t dc_compress_dst_total_buffers;
	kstat_named_t dc_compress_src_buffers_max;
	kstat_named_t dc_compress_dst_buffers_max;
	kstat_named_t dc_compress_add_buffers_max;
	kstat_named_t dc_compress_dst_total_buffers_max;
	/*
	 * Experimental source-buffer coalescing counters.
	 */
	kstat_named_t dc_compress_coalesce_requests;
	kstat_named_t dc_compress_coalesce_success;
	kstat_named_t dc_compress_coalesce_fails;
	kstat_named_t dc_compress_coalesce_bytes;
	kstat_named_t dc_compress_coalesce_alloc_ns;
	kstat_named_t dc_compress_coalesce_copy_ns;
	kstat_named_t dc_compress_coalesce_free_ns;
	/*
	 * Experimental destination-buffer coalescing counters.
	 */
	kstat_named_t dc_compress_dst_coalesce_requests;
	kstat_named_t dc_compress_dst_coalesce_success;
	kstat_named_t dc_compress_dst_coalesce_fails;
	kstat_named_t dc_compress_dst_coalesce_reuse_hits;
	kstat_named_t dc_compress_dst_coalesce_reuse_misses;
	kstat_named_t dc_compress_dst_coalesce_alloc_bytes;
	kstat_named_t dc_compress_dst_coalesce_copy_bytes;
	kstat_named_t dc_compress_dst_coalesce_alloc_ns;
	kstat_named_t dc_compress_dst_coalesce_copy_ns;
	kstat_named_t dc_compress_dst_coalesce_free_ns;
	/*
	 * Cumulative nanoseconds spent allocating and freeing QAT compression
	 * scratch buffers.
	 */
	kstat_named_t dc_compress_scratch_alloc_ns;
	kstat_named_t dc_compress_scratch_free_ns;
	/*
	 * Cumulative nanoseconds spent in QAT compression setup, submit, wait,
	 * and cleanup phases.
	 */
	kstat_named_t dc_compress_setup_ns;
	kstat_named_t dc_compress_submit_ns;
	kstat_named_t dc_compress_wait_ns;
	kstat_named_t dc_compress_cleanup_ns;
	/*
	 * Cumulative nanoseconds spent in QAT decompression setup, submit, wait,
	 * and cleanup phases.
	 */
	kstat_named_t dc_decompress_setup_ns;
	kstat_named_t dc_decompress_submit_ns;
	kstat_named_t dc_decompress_wait_ns;
	kstat_named_t dc_decompress_cleanup_ns;
	/*
	 * Current and peak in-flight QAT data-compression requests.
	 */
	kstat_named_t dc_compress_inflight;
	kstat_named_t dc_compress_inflight_max;
	kstat_named_t dc_decompress_inflight;
	kstat_named_t dc_decompress_inflight_max;
	/*
	 * Experimental async QAT compression counters.
	 */
	kstat_named_t dc_compress_async_submits;
	kstat_named_t dc_compress_async_submit_fails;
	kstat_named_t dc_compress_async_completions;
	kstat_named_t dc_compress_async_resumes;
	kstat_named_t dc_compress_async_fallbacks;
	kstat_named_t dc_compress_async_cancels;
	kstat_named_t dc_compress_async_submit_retries;
	kstat_named_t dc_compress_async_retry_success;
	kstat_named_t dc_compress_async_fail_retry;
	kstat_named_t dc_compress_async_fail_resource;
	kstat_named_t dc_compress_async_fail_other;
	kstat_named_t dc_compress_async_inflight;
	kstat_named_t dc_compress_async_inflight_max;
	kstat_named_t dc_compress_async_cap_skips;

	/*
	 * Number of jobs submitted to QAT encryption engine.
	 */
	kstat_named_t encrypt_requests;
	/*
	 * Total bytes sent to QAT encryption engine.
	 */
	kstat_named_t encrypt_total_in_bytes;
	/*
	 * Total bytes output from QAT encryption engine.
	 */
	kstat_named_t encrypt_total_out_bytes;
	/*
	 * Number of jobs submitted to QAT decryption engine.
	 */
	kstat_named_t decrypt_requests;
	/*
	 * Total bytes sent to QAT decryption engine.
	 */
	kstat_named_t decrypt_total_in_bytes;
	/*
	 * Total bytes output from QAT decryption engine.
	 */
	kstat_named_t decrypt_total_out_bytes;
	/*
	 * Number of fails in the QAT encryption / decryption engine.
	 * Note: when a QAT error happens, it doesn't necessarily indicate a
	 * critical hardware issue. The encryption job will be transferred
	 * to the software implementation so the functionality of ZFS is
	 * not impacted.
	 */
	kstat_named_t crypt_fails;

	/*
	 * Number of jobs submitted to QAT checksum engine.
	 */
	kstat_named_t cksum_requests;
	/*
	 * Total bytes sent to QAT checksum engine.
	 */
	kstat_named_t cksum_total_in_bytes;
	/*
	 * Number of fails in the QAT checksum engine.
	 * Note: when a QAT error happens, it doesn't necessarily indicate a
	 * critical hardware issue. The checksum job will be transferred to the
	 * software implementation so the functionality of ZFS is not impacted.
	 */
	kstat_named_t cksum_fails;
} qat_stats_t;

#define	QAT_STAT_INCR(stat, val) \
	atomic_add_64(&qat_stats.stat.value.ui64, (val))
#define	QAT_STAT_BUMP(stat) \
	QAT_STAT_INCR(stat, 1)
#define	QAT_STAT_ADD_TIME(stat, start, end) \
	do { \
		uint64_t qat_stat_start__ = (uint64_t)(start); \
		uint64_t qat_stat_end__ = (uint64_t)(end); \
		if (qat_stat_end__ > qat_stat_start__) \
			QAT_STAT_INCR(stat, \
			    qat_stat_end__ - qat_stat_start__); \
	} while (0)

extern qat_stats_t qat_stats;
extern int zfs_qat_compress_disable;
extern int zfs_qat_decompress_disable;
extern int zfs_qat_cpa_dc_level;
extern char *zfs_qat_cpa_dc_hufftype;
extern int zfs_qat_dc_max_buf_size;
extern int zfs_qat_dc_max_instances;
extern int zfs_qat_dc_coalesce_src;
extern int zfs_qat_dc_coalesce_dst;
extern int zfs_qat_dc_async;
extern int zfs_qat_dc_async_submit_retries;
extern int zfs_qat_dc_async_retry_us;
extern int zfs_qat_dc_async_max_inflight;
extern char *zfs_qat_dc_async_cap_policy;
extern int zfs_qat_checksum_disable;
extern int zfs_qat_encrypt_disable;
extern int zfs_qat_cy_max_instances;

/* inlined for performance */
static inline struct page *
qat_mem_to_page(void *addr)
{
	if (!is_vmalloc_addr(addr))
		return (virt_to_page(addr));

	return (vmalloc_to_page(addr));
}

CpaStatus qat_mem_alloc_contig(void **pp_mem_addr, Cpa32U size_bytes);
void qat_mem_free_contig(void **pp_mem_addr);
#define	QAT_PHYS_CONTIG_ALLOC(pp_mem_addr, size_bytes)	\
	qat_mem_alloc_contig((void *)(pp_mem_addr), (size_bytes))
#define	QAT_PHYS_CONTIG_FREE(p_mem_addr)	\
	qat_mem_free_contig((void *)&(p_mem_addr))

extern int qat_dc_init(void);
extern void qat_dc_fini(void);
extern int qat_cy_init(void);
extern void qat_cy_fini(void);
extern int qat_init(void);
extern void qat_fini(void);

/* fake CpaStatus used to indicate data was not compressible */
#define	CPA_STATUS_INCOMPRESSIBLE		(-127)

extern boolean_t qat_dc_compress_use_accel(size_t s_len);
extern boolean_t qat_dc_decompress_use_accel(size_t s_len);
extern boolean_t qat_crypt_use_accel(size_t s_len);
extern boolean_t qat_checksum_use_accel(size_t s_len);
extern int qat_compress(qat_compress_dir_t dir, char *src, int src_len,
    char *dst, int dst_len, size_t *c_len);
extern boolean_t qat_dc_compress_async_enabled(void);
extern qat_dc_async_t *qat_dc_compress_async_submit(char *src, int src_len,
    char *dst, int dst_len, void (*resume)(void *), void *resume_arg);
extern void qat_dc_compress_async_arm(qat_dc_async_t *req);
extern boolean_t qat_dc_compress_async_complete(qat_dc_async_t *req);
extern int qat_dc_compress_async_finish(qat_dc_async_t *req, size_t *c_len);
extern void qat_dc_compress_async_cancel(qat_dc_async_t *req);
extern int qat_crypt(qat_encrypt_dir_t dir, uint8_t *src_buf, uint8_t *dst_buf,
    uint8_t *aad_buf, uint32_t aad_len, uint8_t *iv_buf, uint8_t *digest_buf,
    crypto_key_t *key, uint64_t crypt, uint32_t enc_len);
extern int qat_checksum(uint64_t cksum, uint8_t *buf, uint64_t size,
    zio_cksum_t *zcp);
#else
#define	CPA_STATUS_SUCCESS			0
#define	CPA_STATUS_FAIL			(-1)
#define	CPA_STATUS_INCOMPRESSIBLE		(-127)
#define	qat_init()
#define	qat_fini()
#define	qat_dc_compress_use_accel(s_len)	((void) sizeof (s_len), 0)
#define	qat_dc_decompress_use_accel(s_len)	((void) sizeof (s_len), 0)
#define	qat_crypt_use_accel(s_len)		((void) sizeof (s_len), 0)
#define	qat_checksum_use_accel(s_len)		((void) sizeof (s_len), 0)
#define	qat_compress(dir, s, sl, d, dl, cl)			\
	((void) sizeof (dir), (void) sizeof (s), (void) sizeof (sl), \
	    (void) sizeof (d), (void) sizeof (dl), (void) sizeof (cl), 0)
#define	qat_dc_compress_async_enabled()				(0)
#define	qat_dc_compress_async_submit(s, sl, d, dl, r, a)	\
	((void) sizeof (s), (void) sizeof (sl), (void) sizeof (d), \
	    (void) sizeof (dl), (void) sizeof (r), (void) sizeof (a), \
	    (qat_dc_async_t *)NULL)
#define	qat_dc_compress_async_arm(req)				\
	((void) sizeof (req))
#define	qat_dc_compress_async_complete(req)			\
	((void) sizeof (req), 0)
#define	qat_dc_compress_async_finish(req, c_len)			\
	((void) sizeof (req), (void) sizeof (c_len), CPA_STATUS_FAIL)
#define	qat_dc_compress_async_cancel(req)			\
	((void) sizeof (req))
#define	qat_crypt(dir, s, d, a, al, i, db, k, c, el)		\
	((void) sizeof (dir), (void) sizeof (s), (void) sizeof (d), \
	    (void) sizeof (a),  (void) sizeof (al), (void) sizeof (i), \
	    (void) sizeof (db), (void) sizeof (k), (void) sizeof (c), \
	    (void) sizeof (el), 0)
#define	qat_checksum(c, buf, s, z)				\
	((void) sizeof (c), (void) sizeof (buf), (void) sizeof (s), \
	    (void) sizeof (z), 0)
#endif

#endif /* _SYS_QAT_H */
