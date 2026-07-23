// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
/*
 * main_tcp_l2k44_bench.cpp
 * =========================
 * Pure AllReduce microbenchmark for L²(K₄,₄), 128 nodes.
 * No task graph, no FFApplication workload loading.
 * Just: build topology → create AllReduce task → run → print time.
 *
 * Usage:
 *   ./htsim_tcp_l2k44_bench \
 *       -arsize 1073741824 \
 *       -speed 200000      \
 *       -ssthresh 15       \
 *       -q 200             \
 *       -rto 10.0          \
 *       -simtime 3600.1    \
 *       -ofile /dev/null
 *
 *   -arsize   <bytes>   AllReduce message size (total, across all 128 nodes)
 *   -speed    <Mbps>    Link speed (e.g. 100000 = 100 Gbps)
 *   -ssthresh <pkts>    TCP slow-start threshold in packets
 *   -q        <pkts>    Queue size in packets
 *   -rto      <ms>      TCP retransmission timeout in ms
 *   -simtime  <s>       Simulation time limit
 *   -ofile    <path>    Flow completion output file (use /dev/null for bench)
 */

#include "config.h"
#include <sstream>
#include <fstream>
#include <iostream>
#include <string.h>
#include <math.h>
#include "network.h"
#include "randomqueue.h"
#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "clock.h"
#include "tcp.h"
#include "mtcp.h"
#include "compositequeue.h"
#include "firstfit.h"
#include "topology.h"

#include "l2k44_topology.h"
#include "ffapp.h"
#include "ffapp_bfb_l2k44_tg.h"

uint32_t RTT       = 1000;
uint32_t SPEED     = 0;
uint32_t ECNTHRESH = 50;

std::ofstream fct_util_out;
FirstFit *ff = NULL;

#define DEFAULT_PACKET_SIZE 9000
#define DEFAULT_HEADER_SIZE 64
#define DEFAULT_QUEUE_SIZE  200

EventList eventlist;

string ntoa(double n) { stringstream s; s << n; return s.str(); }
string itoa(uint64_t n) { stringstream s; s << n; return s.str(); }

int main(int argc, char **argv)
{
    TcpPacket::set_packet_size(DEFAULT_PACKET_SIZE - DEFAULT_HEADER_SIZE);
    mem_b queuesize = DEFAULT_QUEUE_SIZE * DEFAULT_PACKET_SIZE;

    int      ssthresh = 15;
    double   simtime  = 3600.1;
    double   rto_ms   = 10.0;
    uint64_t arsize   = 0;

    int i = 1;
    while (i < argc) {
        if      (!strcmp(argv[i], "-arsize"))   { arsize   = (uint64_t)atoll(argv[++i]); }
        else if (!strcmp(argv[i], "-speed"))    { SPEED    = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-rtt"))      { RTT      = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-ssthresh")) { ssthresh = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-q"))        { queuesize = memFromPkt(atoi(argv[++i])); }
        else if (!strcmp(argv[i], "-ecnthresh")){ ECNTHRESH = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "-simtime"))  { simtime  = atof(argv[++i]); }
        else if (!strcmp(argv[i], "-rto"))      { rto_ms   = atof(argv[++i]); }
        else if (!strcmp(argv[i], "-ofile"))    { fct_util_out = std::ofstream(argv[++i]); }
        else {
            cerr << "Unknown argument: " << argv[i] << "\n";
            exit(1);
        }
        i++;
    }

    if (SPEED == 0) {
        cerr << "ERROR: -speed <Mbps> is required\n";
        exit(1);
    }
    if (arsize == 0) {
        cerr << "ERROR: -arsize <bytes> is required\n";
        exit(1);
    }

    cerr << "[L2K44-Bench] arsize="   << arsize
         << " (" << arsize / (1024.0*1024.0) << " MB)"
         << " speed="    << SPEED    << " Mbps"
         << " ssthresh=" << ssthresh << " pkts"
         << " q="        << (queuesize / DEFAULT_PACKET_SIZE) << " pkts"
         << " rto="      << rto_ms   << " ms\n";

    srand(13);
    eventlist.setEndtime(timeFromSec(simtime));
    Clock c(timeFromSec(5.0 / 100.0), eventlist);
    TcpRtxTimerScanner tcpRtxScanner(timeFromMs(1), eventlist);

    // Build L²(K₄,₄) topology — 128 nodes, degree 4, diameter 4
    L2K44Topology *top = new L2K44Topology(
        L2K44_N, queuesize,
        nullptr, &eventlist, ff, ECN);

    // Minimal FFApplication — no task graph, only provides
    // ssthresh / fstream_out / tcpRtxScanner / bfb_rto_ms
    FFApplication app(top, ssthresh, &fct_util_out,
                      tcpRtxScanner, eventlist,
                      FFApplication::FF_BFB_L2K44_AR);
    app.bfb_rto_ms  = rto_ms;
    app.ngpupernode = 1;
    app.nnodes      = L2K44_N;
    app.nswitches   = 0;

    // All 128 nodes participate
    std::vector<uint64_t> node_group;
    node_group.reserve(L2K44_N);
    for (int n = 0; n < L2K44_N; n++)
        node_group.push_back((uint64_t)n);
    app.gpus = std::vector<int>(node_group.begin(), node_group.end());

    // Create AllReduce task and schedule it at t=0
    FFLK44_128BFBAllreduceTask *ar = new FFLK44_128BFBAllreduceTask(
        &app, top, node_group, arsize, /*local_runtime=*/0.0);

    // Register with app so cleanup() can find it
    app.tasks[(uint64_t)(void*)ar] = ar;
    FFApplication::total_apps      = 1;
    FFApplication::finished_apps   = 0;

    eventlist.sourceIsPending(*ar, timeFromSec(0.0));

    cerr << "[L2K44-Bench] Simulation starting...\n";
    while (eventlist.doNextEvent()) {}

    cerr << "[L2K44-Bench] FinalFinish "
         << timeAsMs(app.final_finish_time) << " ms\n";

    // Also write to ofile so the sweep script can parse it
    fct_util_out << "FinalFinish " << app.final_finish_time << "\n";

    delete top;
    return 0;
}
