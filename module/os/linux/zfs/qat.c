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
#include <sys/zfs_context.h>
#include <sys/qat.h>

qat_stats_t qat_stats = {
	{ "comp_requests",			KSTAT_DATA_UINT64 },
	{ "comp_total_in_bytes",		KSTAT_DATA_UINT64 },
	{ "comp_total_out_bytes",		KSTAT_DATA_UINT64 },
	{ "decomp_requests",			KSTAT_DATA_UINT64 },
	{ "decomp_total_in_bytes",		KSTAT_DATA_UINT64 },
	{ "decomp_total_out_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_fails",				KSTAT_DATA_UINT64 },
	{ "dc_instances",			KSTAT_DATA_UINT64 },
	{ "dc_buffer_reuse_hits",		KSTAT_DATA_UINT64 },
	{ "dc_buffer_reuse_misses",		KSTAT_DATA_UINT64 },
	{ "dc_compress_bound_requests",		KSTAT_DATA_UINT64 },
	{ "dc_compress_bound_fails",		KSTAT_DATA_UINT64 },
	{ "dc_compress_bound_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_bound_total_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_total_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_scratch_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_scratch_saved_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_overflows",		KSTAT_DATA_UINT64 },
	{ "dc_compress_incompressible",		KSTAT_DATA_UINT64 },
	{ "dc_compress_src_buffers",		KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_buffers",		KSTAT_DATA_UINT64 },
	{ "dc_compress_add_buffers",		KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_total_buffers",	KSTAT_DATA_UINT64 },
	{ "dc_compress_src_buffers_max",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_buffers_max",	KSTAT_DATA_UINT64 },
	{ "dc_compress_add_buffers_max",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_total_buffers_max",	KSTAT_DATA_UINT64 },
	{ "dc_compress_src_buf_unaligned_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_src_buf_len_not_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_src_first_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_src_last_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_buf_unaligned_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_buf_len_not_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_first_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_last_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_add_buf_unaligned_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_add_buf_len_not_64",	KSTAT_DATA_UINT64 },
	{ "dc_compress_add_first_bytes",	KSTAT_DATA_UINT64 },
	{ "dc_compress_add_last_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_sync_submits",		KSTAT_DATA_UINT64 },
	{ "dc_compress_sync_completions",	KSTAT_DATA_UINT64 },
	{ "dc_compress_sync_fallbacks",		KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_stack_src",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_heap_src",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_slot_src",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_stack_dst",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_heap_dst",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_slot_dst",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_stack_scratch", KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_heap_scratch", KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_slot_scratch", KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_alloc_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_page_array_free_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_buffer_list_alloc_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_buffer_list_free_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_req_alloc_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_req_free_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_requests",	KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_success",	KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_fails",		KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_bytes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_alloc_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_copy_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_coalesce_free_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_requests",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_success",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_fails",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_reuse_hits", KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_reuse_misses", KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_alloc_bytes", KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_copy_bytes", KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_alloc_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_copy_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_dst_coalesce_free_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_requests", KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_success", KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_fails",	KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_copy_bytes", KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_retained", KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_retained_bytes", KSTAT_DATA_UINT64 },
	{ "dc_compress_quarantine_dst_retained_released", KSTAT_DATA_UINT64 },
	{ "dc_compress_scratch_alloc_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_scratch_free_ns",	KSTAT_DATA_UINT64 },
	{ "dc_compress_setup_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_submit_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_wait_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_cleanup_ns",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_setup_ns",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_submit_ns",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_wait_ns",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_cleanup_ns",		KSTAT_DATA_UINT64 },
	{ "dc_compress_inflight",		KSTAT_DATA_UINT64 },
	{ "dc_compress_inflight_max",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_inflight",		KSTAT_DATA_UINT64 },
	{ "dc_decompress_inflight_max",		KSTAT_DATA_UINT64 },
	{ "dc_poll_calls",			KSTAT_DATA_UINT64 },
	{ "dc_poll_success",			KSTAT_DATA_UINT64 },
	{ "dc_poll_retries",			KSTAT_DATA_UINT64 },
	{ "dc_poll_fails",			KSTAT_DATA_UINT64 },
	{ "dc_poll_ns",			KSTAT_DATA_UINT64 },
	{ "dc_watchdog_checks",		KSTAT_DATA_UINT64 },
	{ "dc_watchdog_stalls",		KSTAT_DATA_UINT64 },
	{ "dc_watchdog_runtime_disables",	KSTAT_DATA_UINT64 },
	{ "dc_watchdog_last_progress_ns",	KSTAT_DATA_UINT64 },
	{ "dc_watchdog_last_stall_ns",		KSTAT_DATA_UINT64 },
	{ "dc_watchdog_health",		KSTAT_DATA_UINT64 },
	{ "dc_watchdog_request_timeouts",	KSTAT_DATA_UINT64 },
	{ "dc_watchdog_request_recoveries",	KSTAT_DATA_UINT64 },
	{ "dc_watchdog_request_unrecoverable",	KSTAT_DATA_UINT64 },
	{ "dc_watchdog_late_completions",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_submits",		KSTAT_DATA_UINT64 },
	{ "dc_compress_async_submit_fails",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_completions",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_resumes",		KSTAT_DATA_UINT64 },
	{ "dc_compress_async_fallbacks",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_cancels",		KSTAT_DATA_UINT64 },
	{ "dc_compress_async_submit_retries",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_retry_success",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_fail_retry",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_fail_resource",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_fail_other",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_inflight",		KSTAT_DATA_UINT64 },
	{ "dc_compress_async_inflight_max",	KSTAT_DATA_UINT64 },
	{ "dc_compress_async_cap_skips",	KSTAT_DATA_UINT64 },
	{ "encrypt_requests",			KSTAT_DATA_UINT64 },
	{ "encrypt_total_in_bytes",		KSTAT_DATA_UINT64 },
	{ "encrypt_total_out_bytes",		KSTAT_DATA_UINT64 },
	{ "decrypt_requests",			KSTAT_DATA_UINT64 },
	{ "decrypt_total_in_bytes",		KSTAT_DATA_UINT64 },
	{ "decrypt_total_out_bytes",		KSTAT_DATA_UINT64 },
	{ "crypt_fails",			KSTAT_DATA_UINT64 },
	{ "cksum_requests",			KSTAT_DATA_UINT64 },
	{ "cksum_total_in_bytes",		KSTAT_DATA_UINT64 },
	{ "cksum_fails",			KSTAT_DATA_UINT64 },
};

static kstat_t *qat_ksp = NULL;

CpaStatus
qat_mem_alloc_contig(void **pp_mem_addr, Cpa32U size_bytes)
{
	*pp_mem_addr = kmalloc(size_bytes, GFP_KERNEL);
	if (*pp_mem_addr == NULL)
		return (CPA_STATUS_RESOURCE);
	return (CPA_STATUS_SUCCESS);
}

void
qat_mem_free_contig(void **pp_mem_addr)
{
	if (*pp_mem_addr != NULL) {
		kfree(*pp_mem_addr);
		*pp_mem_addr = NULL;
	}
}

int
qat_init(void)
{
	qat_ksp = kstat_create("zfs", 0, "qat", "misc",
	    KSTAT_TYPE_NAMED, sizeof (qat_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (qat_ksp != NULL) {
		qat_ksp->ks_data = &qat_stats;
		kstat_install(qat_ksp);
	}

	/*
	 * Just set the disable flag when qat init failed, qat can be
	 * turned on again in post-process after zfs module is loaded, e.g.:
	 * echo 0 > /sys/module/zfs/parameters/zfs_qat_compress_disable
	 */
	if (qat_dc_init() != 0)
		zfs_qat_compress_disable = 1;

	if (qat_cy_init() != 0) {
		zfs_qat_checksum_disable = 1;
		zfs_qat_encrypt_disable = 1;
	}

	return (0);
}

void
qat_fini(void)
{
	if (qat_ksp != NULL) {
		kstat_delete(qat_ksp);
		qat_ksp = NULL;
	}

	qat_cy_fini();
	qat_dc_fini();
}

#endif
