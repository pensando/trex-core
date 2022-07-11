/*
  TRex team
  Cisco Systems, Inc.
*/

/*
  Copyright (c) 2015-2017 Cisco Systems, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "trex_driver_ionic.h"
#include "trex_driver_defines.h"

CTRexExtendedDriverBaseIonic::CTRexExtendedDriverBaseIonic() {
    m_cap = tdCAP_ALL | TREX_DRV_CAP_MAC_ADDR_CHG ;
    for ( int i=0; i<TREX_MAX_PORTS; i++ ) {
        m_port_xstats[i] = {0};
    }
}

TRexPortAttr* CTRexExtendedDriverBaseIonic::create_port_attr(tvpid_t tvpid,repid_t repid) {
    return new DpdkTRexPortAttr(tvpid, repid, false, false, true, false, true);
}

bool CTRexExtendedDriverBaseIonic::is_support_for_rx_scatter_gather(){
    return false;
}


int CTRexExtendedDriverBaseIonic::get_min_sample_rate(void){
    return (RX_CHECK_MIX_SAMPLE_RATE);
}


void CTRexExtendedDriverBaseIonic::clear_extended_stats(CPhyEthIF * _if){
    repid_t repid=_if->get_repid();
    rte_eth_stats_reset(repid);
}

void CTRexExtendedDriverBaseIonic::update_configuration(port_cfg_t * cfg){
    cfg->m_tx_conf.tx_thresh.pthresh = TX_PTHRESH;
    cfg->m_tx_conf.tx_thresh.hthresh = TX_HTHRESH;
    cfg->m_tx_conf.tx_thresh.wthresh = TX_WTHRESH;
    cfg->m_port_conf.fdir_conf.mode = RTE_FDIR_MODE_PERFECT;
    cfg->m_port_conf.fdir_conf.pballoc = RTE_FDIR_PBALLOC_64K;
    cfg->m_port_conf.fdir_conf.status = RTE_FDIR_NO_REPORT_STATUS;
}

void CTRexExtendedDriverBaseIonic::reset_rx_stats(CPhyEthIF * _if, uint32_t *stats, int min, int len) {
    for (int i =0; i < len; i++) {
        stats[i] = 0;
    }
}

int CTRexExtendedDriverBaseIonic::get_rx_stats(CPhyEthIF * _if, uint32_t *pkts, uint32_t *prev_pkts
                                             ,uint32_t *bytes, uint32_t *prev_bytes, int min, int max) {
    /* not supported yet */
    return 0;
}

int CTRexExtendedDriverBaseIonic::dump_fdir_global_stats(CPhyEthIF * _if, FILE *fd)
{
    return(0);
}

bool CTRexExtendedDriverBaseIonic::get_extended_stats(CPhyEthIF * _if, CPhyEthIFStats *stats) {
    enum {
        rx_good_packets,
        tx_good_packets,
        rx_good_bytes,
        tx_good_bytes,
        rx_missed_errors,
        rx_errors,
        tx_errors,
        rx_mbuf_allocation_errors,
        COUNT
    };

    enum {
        rx_wqe_err,
        rx_port_unicast_packets,
        rx_port_unicast_bytes,
        tx_port_unicast_packets,
        tx_port_unicast_bytes,
        rx_port_multicast_packets,
        rx_port_multicast_bytes,
        tx_port_multicast_packets,
        tx_port_multicast_bytes,
        rx_port_broadcast_packets,
        rx_port_broadcast_bytes,
        tx_port_broadcast_packets,
        tx_port_broadcast_bytes,
        tx_packets_phy,
        rx_packets_phy,
        rx_crc_errors_phy,
        tx_bytes_phy,
        rx_bytes_phy,
        rx_in_range_len_errors_phy,
        rx_symbol_err_phy,
        rx_discards_phy,
        tx_discards_phy,
        tx_errors_phy,
        rx_out_of_buffer,
        //tx_pp_missed_interrupt_error,
        //tx_pp_rearm_queue_errors,
        //tx_pp_clock_queue_errors,    
        //tx_pp_timestamp_past_errors,
        //tx_pp_timestamp_future_error, 
        //tx_pp_jitter,
        //tx_pp_wander,
        //tx_pp_sync_lost,
        XCOUNT
    };

    uint16_t repid = _if->get_repid();
    struct rte_eth_stats rte_stats;

    /* fetch stats */
    assert(rte_eth_stats_get(repid, &rte_stats) == 0);

    stats->opackets = rte_stats.opackets ;
    stats->ipackets = rte_stats.ipackets;

    stats->obytes = rte_stats.obytes;
    stats->ibytes = rte_stats.ibytes;
    
    stats->rx_nombuf = rte_stats.rx_nombuf;
    stats->ierrors = rte_stats.ierrors + rte_stats.imissed + rte_stats.rx_nombuf;
    stats->oerrors = rte_stats.oerrors;
    return true;
}

int CTRexExtendedDriverBaseIonic::wait_for_stable_link(){
    delay(20);
    return (0);
}

CFlowStatParser *CTRexExtendedDriverBaseIonic::get_flow_stat_parser() {
    CFlowStatParser *parser = new CFlowStatParser(CFlowStatParser::FLOW_STAT_PARSER_MODE_HW);
    assert (parser);
    return parser;
}

void CTRexExtendedDriverBaseIonic::get_rx_stat_capabilities(uint16_t &flags, uint16_t &num_counters, uint16_t &base_ip_id) {
    flags = TrexPlatformApi::IF_STAT_IPV4_ID | TrexPlatformApi::IF_STAT_RX_BYTES_COUNT
        | TrexPlatformApi::IF_STAT_PAYLOAD;
    num_counters = 127; //With MAX_FLOW_STATS we saw packet failures in rx_test. Need to check.
    base_ip_id = IP_ID_RESERVE_BASE;
}

