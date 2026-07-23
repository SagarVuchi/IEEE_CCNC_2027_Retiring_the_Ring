#!/usr/bin/env bash
# compile_l2k44_bench.sh
# ======================
# Builds htsim_tcp_l2k44_bench using ffapp_l2k44.cpp (no ffapp.cpp).
# Routes file compiled as separate translation unit for fast compilation.
set -e

CLOS=~/TopoOpt_copy1/topoopt_fresh/simulation/FlexNetPacket/src/clos
DC=${CLOS}/datacenter
FBUF=~/TopoOpt_copy1/topoopt_fresh/simulation/FlexNet/fbuf2/include
FLAGS="-Wall -std=c++17 -O3 -I${FBUF} -I${CLOS} -I${DC}"

# ── Check generated schedule files exist ──────────────────────────────────────
for f in l2k44_128_routes_generated.cpp l2k44_128_schedule_generated.cpp; do
    if [ ! -f "${DC}/${f}" ]; then
        echo "ERROR: ${f} missing. Run: python3 solve_bfb_l2k44_128_v3.py"
        exit 1
    fi
done

# ── Build htsim core library ──────────────────────────────────────────────────
echo "[L2K44-Bench] Building htsim core library..."
cd "${CLOS}"
make lib -j4

cd "${DC}"

# ── Compile units ─────────────────────────────────────────────────────────────
echo "[L2K44-Bench] Compiling l2k44_128_routes_generated.cpp ..."
g++ ${FLAGS} -c l2k44_128_routes_generated.cpp -o l2k44_bench_routes.o

echo "[L2K44-Bench] Compiling l2k44_topology.cpp ..."
g++ ${FLAGS} -c l2k44_topology.cpp -o l2k44_bench_topology.o

echo "[L2K44-Bench] Compiling ffapp_l2k44.cpp ..."
g++ ${FLAGS} -c ffapp_l2k44.cpp -o l2k44_bench_ffapp.o

echo "[L2K44-Bench] Compiling firstfit.cpp ..."
g++ ${FLAGS} -c firstfit.cpp -o l2k44_bench_firstfit.o

echo "[L2K44-Bench] Compiling main_tcp_l2k44_bench.cpp ..."
g++ ${FLAGS} -c main_tcp_l2k44_bench.cpp -o l2k44_bench_main.o

# ── Link ──────────────────────────────────────────────────────────────────────
echo "[L2K44-Bench] Linking htsim_tcp_l2k44_bench ..."
g++ ${FLAGS} \
    l2k44_bench_firstfit.o  \
    l2k44_bench_main.o      \
    l2k44_bench_topology.o  \
    l2k44_bench_routes.o    \
    l2k44_bench_ffapp.o     \
    "${CLOS}/eventlist.o"     \
    "${CLOS}/tcppacket.o"     \
    "${CLOS}/pipe.o"          \
    "${CLOS}/queue.o"         \
    "${CLOS}/ecnqueue.o"      \
    "${CLOS}/tcp.o"           \
    "${CLOS}/dctcp.o"         \
    "${CLOS}/mtcp.o"          \
    "${CLOS}/loggers.o"       \
    "${CLOS}/logfile.o"       \
    "${CLOS}/clock.o"         \
    "${CLOS}/config.o"        \
    "${CLOS}/network.o"       \
    "${CLOS}/qcn.o"           \
    "${CLOS}/exoqueue.o"      \
    "${CLOS}/randomqueue.o"   \
    "${CLOS}/cbr.o"           \
    "${CLOS}/cbrpacket.o"     \
    "${CLOS}/sent_packets.o"  \
    "${CLOS}/ndp.o"           \
    "${CLOS}/ndppacket.o"     \
    "${CLOS}/eth_pause_packet.o" \
    "${CLOS}/compositequeue.o" \
    "${CLOS}/prioqueue.o"     \
    "${CLOS}/cpqueue.o"       \
    "${CLOS}/compositeprioqueue.o" \
    "${CLOS}/switch.o"        \
    "${CLOS}/fairpullqueue.o" \
    "${CLOS}/route.o"         \
    "${CLOS}/dyn_net_sch.o"   \
    "${CLOS}/queue_lossless.o" \
    "${CLOS}/queue_lossless_input.o" \
    "${CLOS}/queue_lossless_output.o" \
    -o htsim_tcp_l2k44_bench

echo ""
echo "[L2K44-Bench] Build complete: ${DC}/htsim_tcp_l2k44_bench"
echo ""
echo "Quick test:"
echo "  ./htsim_tcp_l2k44_bench -arsize 4194304 -speed 200000 -ssthresh 15 -q 200 -rto 10 -simtime 3600.1 -ofile /dev/null"
