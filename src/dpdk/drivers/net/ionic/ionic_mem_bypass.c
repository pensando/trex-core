/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 * Copyright(c) 2019-2020 Pensando Systems, Inc. All rights reserved.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>

#include <rte_errno.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_memory.h>
#include <rte_eal.h>
#include <rte_ethdev_driver.h>
#include <rte_malloc.h>
#include <rte_dev.h>
#include <rte_mbuf_pool_ops.h>

#include "ionic.h"
#include "ionic_logs.h"
#include "ionic_dev.h"

#define CAPMEM_RANGES_MAX	8
#define CAPMEM_DEV		"/dev/capmem"
#define CAPMEM_IOCTL		0xcc

struct capmem_range {
	uint64_t start;
	uint64_t len;
	int      type;
};

struct capmem_ranges_args {
	struct capmem_range *range;
	int nranges;
};

#define CAPMEM_GET_NRANGES   _IOR(CAPMEM_IOCTL, 1, int)
#define CAPMEM_GET_RANGES    _IOWR(CAPMEM_IOCTL, 2, struct capmem_ranges_args)

#define BYPASS_HEAP_NAME	"bypass"

struct bypass_pool {
	struct rte_mempool *mp;
	struct rte_mempool *base_mp;
        struct rte_pktmbuf_extmem *extmem;
	struct bypass_pool *next;
};

struct bypass_range {
	struct capmem_range *range;
	size_t   map_sz;
	off_t    map_off;
	void    *map_va;
	uint8_t  range_idx;
	uint8_t  pool_cnt;

	struct bypass_pool *pools;
};

static struct capmem_range capmem_ranges[CAPMEM_RANGES_MAX];
static struct bypass_range bypass_info;

/*
 * Allocate enough memzones to track the requested number of mbufs.
 */
static int
ionic_mem_setup_extbuf(char *pool_name, uint32_t nb_mbufs, uint16_t mbuf_sz,
		uint32_t bypass_socket_id, struct rte_pktmbuf_extmem **ext_mem)
{
	struct rte_pktmbuf_extmem *xmem;
	uint32_t ext_num = 0, zone_num, elt_num;
	uint16_t elt_size;

	IONIC_PRINT_CALL();

	elt_size = RTE_ALIGN_CEIL(mbuf_sz, IONIC_EXTBUF_ALIGN);
	elt_num = RTE_PGSIZE_2M / elt_size;
	zone_num = (nb_mbufs + elt_num - 1) / elt_num;

	xmem = rte_calloc("ionic", zone_num, sizeof(struct rte_pktmbuf_extmem),
			rte_mem_page_size());
	if (xmem == NULL) {
		IONIC_PRINT(ERR, "Cannot alloc external buffer descriptors");
		goto out;
	}

	for (ext_num = 0; ext_num < zone_num; ext_num++) {
		struct rte_pktmbuf_extmem *xseg = xmem + ext_num;
		const struct rte_memzone *mz;
		char mz_name[RTE_MEMZONE_NAMESIZE];
		int ret;

		ret = snprintf(mz_name, sizeof(mz_name),
			RTE_MEMPOOL_MZ_FORMAT "_xb_%u_%u",
			pool_name, bypass_info.pool_cnt, ext_num);
		if (ret < 0 || ret >= (int)sizeof(mz_name)) {
			ext_num = 0;
			IONIC_PRINT(ERR, "Memory zone name too long");
			break;
		}

		mz = rte_memzone_reserve_aligned(mz_name, RTE_PGSIZE_2M,
						bypass_socket_id,
						RTE_MEMZONE_IOVA_CONTIG,
						128);
		if (mz == NULL) {
			IONIC_PRINT(ERR,
				"Memory zone reserve extnum %u/%u failed (%d)",
				ext_num, zone_num, rte_errno);
			ext_num = 0;
			break;
		}

		xseg->buf_ptr = mz->addr;
		xseg->buf_iova = mz->iova;
		xseg->buf_len = RTE_PGSIZE_2M;
		xseg->elt_size = elt_size;

		assert(mz->addr >= bypass_info.map_va);
		assert(mz->addr <= RTE_PTR_ADD(bypass_info.map_va,
						bypass_info.map_sz));
		assert(mz->iova >= (rte_iova_t)bypass_info.range->start);
		assert(mz->iova <= (rte_iova_t)bypass_info.range->start +
						bypass_info.range->len);
	}

out:
	if (ext_num == 0 && xmem != NULL) {
		/*
		 * Callers must exit on external buffer creation
		 * error, so there is no need to free any memzones.
		 */
		rte_free(xmem);
		xmem = NULL;
	}
	*ext_mem = xmem;

	return ext_num;
}

/*
 * Create a bypass mempool mirroring the base_mp, but with mbufs pulled
 * from the device's bypass (uncached) memory.
 */
static struct bypass_pool *
ionic_mem_setup_mempool(uint32_t socket_id, uint32_t seg_sz,
		struct rte_mempool *base_mp)
{
	struct bypass_pool *bpool = NULL;
	char pool_name[RTE_MEMPOOL_NAMESIZE];
	const char *ops_name;
	int bypass_socket_id, ext_num, ret;

	IONIC_PRINT(DEBUG, "Bypass skt_id %u seg_sz %u", socket_id, seg_sz);

	if ((base_mp->flags & MEMPOOL_F_SP_PUT) &&
	    (base_mp->flags & MEMPOOL_F_SC_GET))
		ops_name = "ring_sp_sc";
	else if (base_mp->flags & MEMPOOL_F_SP_PUT)
		ops_name = "ring_sp_mc";
	else if (base_mp->flags & MEMPOOL_F_SC_GET)
		ops_name = "ring_mp_sc";
	else
		ops_name = "ring_mp_mc";
	ret = rte_mbuf_set_user_mempool_ops(ops_name);
	if (ret) {
		IONIC_PRINT(ERR, "Failed to set mempool ops %s (%d)",
			    ops_name, ret);
		goto err;
	}

	bpool = rte_zmalloc_socket("ionic", sizeof(*bpool),
				RTE_CACHE_LINE_SIZE, socket_id);
	if (bpool == NULL) {
		IONIC_PRINT(ERR, "Failed to allocate bypass_pool");
		goto err;
	}

	bypass_socket_id = rte_malloc_heap_get_socket(BYPASS_HEAP_NAME);

	ext_num = ionic_mem_setup_extbuf(base_mp->name, base_mp->size,
					seg_sz,
					bypass_socket_id,
					&bpool->extmem);
	if (bpool->extmem == NULL) {
		IONIC_PRINT(ERR, "Failed to create external mbuf descriptors");
		goto err_free_pool;
	}

	snprintf(pool_name, RTE_MEMPOOL_NAMESIZE,
		"mbext_pool_skt_%u_%u", socket_id, bypass_info.pool_cnt);

	bpool->mp = rte_pktmbuf_pool_create_extbuf(pool_name,
					base_mp->size, base_mp->cache_size,
					0, seg_sz, socket_id,
					bpool->extmem, ext_num);
	if (bpool->mp == NULL) {
		IONIC_PRINT(ERR, "Failed to create external mbuf pool %s (%d)",
			    pool_name, rte_errno);
		goto err_free_extmem;
	}

	bpool->base_mp = base_mp;
	IONIC_PRINT(DEBUG, "Created bypass pool %s %p to shadow mpool %p",
		pool_name, bpool->mp, bpool->base_mp);

	return bpool;

err_free_extmem:
	rte_free(bpool->extmem);
err_free_pool:
	rte_free(bpool);
err:
	return NULL;
}

/*
 * Find (or create if necessary) a bypass mempool for the given socket/mpool.
 */
struct rte_mempool *
ionic_mem_bypass_mpool(uint32_t socket_id, uint32_t seg_sz,
		struct rte_mempool *base_mp)
{
	struct bypass_pool *bpool = bypass_info.pools;

	while (bpool) {
		if (bpool->base_mp == base_mp) {
			IONIC_PRINT(DEBUG, "Bypass pool %p shadows mpool %p",
				    bpool->mp, base_mp);
			return bpool->mp;
		}

		bpool = bpool->next;
	}

	bpool = ionic_mem_setup_mempool(socket_id, seg_sz, base_mp);
	if (bpool) {
		bpool->next = bypass_info.pools;
		bypass_info.pools = bpool;
		bypass_info.pool_cnt++;

		return bpool->mp;
	}

	return NULL;
}

/*
 * Use the capmem virtual device to read out the existing memory ranges,
 * and then mmap() the selected one if requested.
 */
static int
ionic_mem_read_capmem(struct ionic_adapter *adapter, bool do_map)
{
	struct capmem_ranges_args args;
	struct capmem_range *range;
	uint64_t map_sz;
	off_t map_off;
	void *map_va;
	int fd, ret, i;

	IONIC_PRINT_CALL();

	fd = open(CAPMEM_DEV, O_RDWR | O_SYNC);
	if (fd < 0) {
		ret = -errno;
		IONIC_PRINT(ERR, "Failed to open %s (%d)", CAPMEM_DEV, ret);
		return ret;
	}

	ret = ioctl(fd, CAPMEM_GET_NRANGES, &args.nranges);
	if (ret < 0) {
		IONIC_PRINT(ERR, "CAPMEM_GET_NRANGES failed (%d)", ret);
		goto out;
	}

	if (args.nranges <= 0) {
		IONIC_PRINT(ERR, "CAPMEM_GET_NRANGES: Invalid nranges %d",
			args.nranges);
		ret = -ERANGE;
		goto out;
	}

	args.nranges = RTE_MIN(args.nranges, CAPMEM_RANGES_MAX);
	args.range = capmem_ranges;

	ret = ioctl(fd, CAPMEM_GET_RANGES, &args);
	if (ret < 0) {
		IONIC_PRINT(ERR, "CAPMEM_GET_RANGES failed (%d)", ret);
		goto out;
	}

	for (i = 0; i < args.nranges; i++)
		IONIC_PRINT(INFO, "CAPMEM_RANGE %d: %#jx %#jx %d", i,
			    capmem_ranges[i].start,
			    capmem_ranges[i].len,
			    capmem_ranges[i].type);

	if (!do_map)
		goto out;

	if (adapter->bypass_range_idx >= CAPMEM_RANGES_MAX) {
		ret = -ERANGE;
		IONIC_PRINT(ERR, "Requested range %u is above limit %u",
			adapter->bypass_range_idx, CAPMEM_RANGES_MAX - 1);
		goto out;
	}

	range = &capmem_ranges[adapter->bypass_range_idx];
	if (range->len == 0) {
		ret = -EINVAL;
		IONIC_PRINT(ERR, "Requested range %u does not exist",
			adapter->bypass_range_idx);
		goto out;
	}

	map_off = range->start & ~(getpagesize() - 1);
	map_sz = range->start + range->len - map_off;

	map_va = mmap(NULL, map_sz, PROT_READ | PROT_WRITE, MAP_SHARED,
				fd, map_off);
	if (map_va == MAP_FAILED) {
		ret = -errno;
		IONIC_PRINT(ERR, "Failed to map bypass region at %#jx (%d)",
			range->start, ret);
		goto out;
	}

	bypass_info.range = range;
	bypass_info.map_sz = map_sz;
	bypass_info.map_off = map_off;
	bypass_info.map_va = map_va;
	IONIC_PRINT(DEBUG, "Bypass range %u iova %#lx sz %#lx va %p",
		adapter->bypass_range_idx, range->start, map_sz, map_va);
	ret = 0;

out:
	if (close(fd))
		IONIC_PRINT(WARNING, "Failed to close %s (%d)",
			CAPMEM_DEV, -errno);
	return ret;
}

int
ionic_mem_setup_bypass(struct ionic_adapter *adapter)
{
	rte_iova_t iova, *iova_table;
	size_t n_pages, offset;
	uint64_t page_sz;
	uint32_t cur_page;
	void *cur;
	int ret;
	bool do_map;

	if (bypass_info.range_idx != 0 &&
	    adapter->bypass_range_idx != 0) {
		if (bypass_info.range_idx == adapter->bypass_range_idx) {
			IONIC_PRINT(DEBUG, "Already done");
			return 0;
		}

		IONIC_PRINT(ERR, "Request to map range %u conflicts with "
			"existing map of range %u",
			adapter->bypass_range_idx, bypass_info.range_idx);
		return -EEXIST;
	}

	IONIC_PRINT_CALL();

	/*
	 * Bypass will be enabled if a valid memory range is selected.
	 * If bypass is not enabled, log the capmem ranges at debug level
	 * and then return without mapping anything.
	 * (This allows the client to probe for the current memory ranges.)
	 */
	do_map = (adapter->bypass_range_idx != 0);

	ret = ionic_mem_read_capmem(adapter, do_map);
	if (!do_map)
		return 0;
	if (ret)
		return ret;

	page_sz = getpagesize();
	n_pages = bypass_info.map_sz / page_sz;

	iova_table = rte_calloc("ionic", n_pages, sizeof(*iova_table), 0);
	if (iova_table == NULL) {
		ret = -ENOMEM;
		IONIC_PRINT(ERR, "Cannot allocate memory for iova addresses");
		goto err_unmap;
	}

	for (cur_page = 0; cur_page < n_pages; cur_page++) {
		offset = page_sz * cur_page;
		cur = RTE_PTR_ADD(bypass_info.map_va, offset);

		*(volatile char *)cur = 0;

		iova = bypass_info.map_off + offset;
		iova_table[cur_page] = iova;
	}

	if (rte_malloc_heap_create(BYPASS_HEAP_NAME) < 0) {
		ret = -rte_errno;
		IONIC_PRINT(ERR, "Cannot create heap (%d)", ret);
		goto err_free;
	}

	ret = rte_malloc_heap_memory_add(BYPASS_HEAP_NAME,
				bypass_info.map_va, bypass_info.range->len,
				iova_table, n_pages, page_sz);
	if (ret < 0) {
		ret = -rte_errno;
		IONIC_PRINT(ERR, "Cannot add memory to heap (%d)", ret);
		goto err_free;
	}

	rte_free(iova_table);
	bypass_info.range_idx = adapter->bypass_range_idx;

	return 0;

err_free:
	rte_free(iova_table);
err_unmap:
	munmap(bypass_info.map_va, bypass_info.map_sz);
	return ret;
}
