/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 * Copyright(c) 2019-2021 Pensando Systems, Inc. All rights reserved.
 */

#ifndef _IONIC_SIM_H_
#define _IONIC_SIM_H_

#ifdef DPDK_SIM
#include "lib/dpdk/sim/sim.hpp"
#include <rte_bus_vdev.h>

#define DPDK_SIM_UNUSED __rte_unused

#define DPDK_SIM_INIT() {						\
	dpdk_sim_init();						\
}

#define DPDK_SIM_BARS_INIT(_dev, _bars, _num_bars) {			\
	const char *_name = rte_vdev_device_name(_dev);			\
	uint64_t _v = dpdk_sim_get_bar_addr(_name);			\
	for (int _i = 0; _i < _num_bars; _i++)				\
		(_bars)->bar[_i].vaddr = (void *)_v;			\
}

#define DPDK_SIM_DESC_ALLOC(dst, src, size) {				\
	dst = dpdk_sim_desc_alloc(src, size);				\
}

#define DPDK_SIM_DESC_RD(q_type, q_base, index, q_desc) {		\
	q_type *q_pbase = (q_type *)q_base;				\
	dpdk_sim_read_mem(q_desc,					\
			(uint64_t)&q_pbase[index], sizeof(*q_desc));	\
}

#define DPDK_SIM_DESC_WR(q_type, q_base, index, q_desc) {		\
	q_type *q_pbase = (q_type *)q_base;				\
	dpdk_sim_write_mem(q_desc,					\
			(uint64_t)&q_pbase[index], sizeof(*q_desc));	\
}

#define DPDK_SIM_MEM_RD(src_dst, pa, size) {				\
	dpdk_sim_read_mem(src_dst, (uint64_t)pa, size);			\
}

#define DPDK_SIM_MEM_WR(src_dst, pa, size) {				\
	dpdk_sim_write_mem(src_dst, (uint64_t)pa, size);		\
}

#define DPDK_SIM_MBUF_INIT(ndescs) dpdk_sim_mbuf_init(ndescs)

#define DPDK_SIM_MBUF_RD(buf_addr, buf_len) {				\
	dpdk_sim_mbuf_read(buf_addr, 0, buf_len);			\
	dpdk_sim_mbuf_free(buf_addr);					\
}

#define DPDK_SIM_MBUF_WR(buf_addr, buf_len, addr) {			\
	addr = dpdk_sim_mbuf_alloc(buf_addr, buf_len);			\
	dpdk_sim_write_mem(buf_addr, addr, buf_len);			\
}

/**
 * Ethernet PMD specific
 */
#define DPDK_SIM_RX(_rxm, addr, frame) {				\
	void *_maddr = (void *)_rxm->buf_addr;				\
	uint32_t _len = frame + RTE_PKTMBUF_HEADROOM;			\
	addr = dpdk_sim_mbuf_alloc(_maddr, _len);			\
}

#define DPDK_SIM_RX_DONE(_rxm) {					\
	void *_maddr = (void *)_rxm->buf_addr;				\
	dpdk_sim_mbuf_read(_maddr, _rxm->data_off, _rxm->data_len);	\
	dpdk_sim_mbuf_free(_maddr);					\
}

#define DPDK_SIM_TX(_txm, addr) {					\
	void *_maddr = rte_pktmbuf_mtod(_txm, void *);			\
	uint32_t _len = _txm->data_len + RTE_PKTMBUF_HEADROOM;		\
	addr = dpdk_sim_mbuf_alloc(_txm->buf_addr, _len);		\
	dpdk_sim_write_mem(_maddr, addr, _txm->data_len);		\
}

#define DPDK_SIM_TX_DONE(_txm) {					\
	dpdk_sim_mbuf_free(_txm->buf_addr);				\
}

#define DPDK_SIM_FILL_LINK_STATE(_link) {				\
	_link.link_status = ETH_LINK_UP;				\
	_link.link_duplex = ETH_LINK_FULL_DUPLEX;			\
	_link.link_speed = ETH_SPEED_NUM_NONE;				\
}

#else

#define DPDK_SIM_UNUSED
#define DPDK_SIM_INIT(...) { }
#define DPDK_SIM_BARS_INIT(...) { }
#define DPDK_SIM_DESC_ALLOC(...) { }
#define DPDK_SIM_DESC_RD(...) { }
#define DPDK_SIM_DESC_WR(...) { }
#define DPDK_SIM_MEM_RD(...) { }
#define DPDK_SIM_MEM_WR(...) { }
#define DPDK_SIM_MBUF_INIT(...) { }
#define DPDK_SIM_MBUF_RD(...) { }
#define DPDK_SIM_MBUF_WR(...) { }
#define DPDK_SIM_RX(...) { }
#define DPDK_SIM_RX_DONE(...) { }
#define DPDK_SIM_TX(...) { }
#define DPDK_SIM_TX_DONE(...) { }
#define DPDK_SIM_FILL_LINK_STATE(...) { }

#endif /* DPDK_SIM */

#endif /* _IONIC_SIM_H_ */
