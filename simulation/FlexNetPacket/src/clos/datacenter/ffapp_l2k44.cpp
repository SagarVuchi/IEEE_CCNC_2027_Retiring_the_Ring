// -*- c-basic-offset: 4; tab-width: 8; indent-tabs-mode: t -*-
/*
 * ffapp_l2k44.cpp
 * ===============
 * Self-contained implementation for L²(K₄,₄) BFB AllReduce microbenchmark.
 *
 * Contains:
 *   1. Minimal FFApplication stub  — only the fields/methods used by the bench
 *   2. Minimal FFTask stub         — only what FFLK44_128BFBAllreduceTask needs
 *   3. FFLK44_128BFBAllreduceTask  — full BFB AllReduce implementation
 *
 * Does NOT contain:
 *   - load_taskgraph_flatbuf / load_taskgraph_json
 *   - FFRingAllreduce, FFPSAllreduce, FFDPSAllreduce, etc.
 *   - Any other topology's AllReduce
 *
 * Compile with compile_l2k44_bench.sh (links against htsim core only).
 */

#include "ffapp.h"
#include "dctcp.h"
#include <numeric>
#include "datacenter/ffapp_bfb_l2k44_tg.h"
#include "datacenter/l2k44_topology.h"
#include <iostream>
#include <map>

// ============================================================================
// Static members
// ============================================================================

int FFApplication::total_apps    = 0;
int FFApplication::finished_apps = 0;

// ============================================================================
// FFApplication — minimal stub constructor
// Only initialises fields that FFLK44_128BFBAllreduceTask touches.
// ============================================================================

FFApplication::FFApplication(Topology* top, int ss, ofstream* _fstream_out,
                             TcpRtxTimerScanner& rtx, EventList& evl,
                             FFAllReduceStrategy ars)
    : topology(top),
      ssthresh(ss),
      eventlist(evl),
      fstream_out(_fstream_out),
      tcpRtxScanner(rtx),
      allreduce_strategy(ars),
      final_finish_time(0),
      first_iter_time(0),
      n_finished_tasks(0),
      finished_once(false),
      fancy_ring(false),
      bfb_rto_ms(1.0),
      ngpupernode(1),
      nnodes(L2K44_N),
      nswitches(0),
      cwnd(0),
      pull_rate(0.0)
{
    FFApplication::total_apps++;
    gpus.resize(L2K44_N);
    std::iota(gpus.begin(), gpus.end(), 0);
}

// Second constructor (with explicit gpus vector) — required by ffapp.h
FFApplication::FFApplication(Topology* top, int ss, ofstream* _fstream_out,
                             std::vector<int> gpus_,
                             TcpRtxTimerScanner& rtx, EventList& evl,
                             FFAllReduceStrategy ars)
    : topology(top),
      ssthresh(ss),
      eventlist(evl),
      fstream_out(_fstream_out),
      gpus(gpus_),
      tcpRtxScanner(rtx),
      allreduce_strategy(ars),
      final_finish_time(0),
      first_iter_time(0),
      n_finished_tasks(0),
      finished_once(false),
      fancy_ring(false),
      bfb_rto_ms(1.0),
      ngpupernode(1),
      nnodes(L2K44_N),
      nswitches(0),
      cwnd(0),
      pull_rate(0.0)
{
    FFApplication::total_apps++;
}

FFApplication::~FFApplication()
{
    for (auto& kv : tasks)   delete kv.second;
    for (auto& kv : devices) delete kv.second;
}

// Stubs for methods declared in ffapp.h but not needed by the bench
void FFApplication::load_taskgraph_json(std::string&)   { assert(false && "not used in bench"); }
void FFApplication::load_taskgraph_flatbuf(std::string&){ assert(false && "not used in bench"); }
void FFApplication::start_init_tasks()                  { assert(false && "not used in bench"); }
void FFApplication::reset_and_restart()                 { assert(false && "not used in bench"); }

std::vector<int> FFApplication::choose_gpus(
    std::unordered_set<int>& candidates, int n)
{
    assert(false && "not used in bench");
    return {};
}

// ============================================================================
// FFTask — minimal implementation
// Only the methods called from FFLK44_128BFBAllreduceTask::cleanup()
// ============================================================================

FFTask::FFTask(FFApplication* app, FFTaskType t)
    : ffapp(app), EventSource(app->eventlist, "FFTask"),
      type(t), state(TASK_NOT_READY), device(nullptr),
      counter(0), xfersize(0), src_node(-1), dst_node(-1),
      ready_time(0), run_time(0), start_time(0), finish_time(0)
{}

// Stub constructors required by the linker (header declares them)
FFTask::FFTask(FFApplication* app, std::string type_str,
               FFDevice* dev, uint64_t sz, float rt)
    : ffapp(app), EventSource(app->eventlist, "FFTask"),
      type(TASK_COMM), state(TASK_NOT_READY), device(dev),
      counter(0), xfersize(sz), src_node(-1), dst_node(-1),
      ready_time(0), run_time((simtime_picosec)(rt * 1e9)),
      start_time(0), finish_time(0)
{ (void)type_str; }

FFTask::FFTask(FFApplication* app, FlatBufTaskGraph::SimTaskType,
               FFDevice* dev, uint64_t sz, float rt)
    : ffapp(app), EventSource(app->eventlist, "FFTask"),
      type(TASK_COMM), state(TASK_NOT_READY), device(dev),
      counter(0), xfersize(sz), src_node(-1), dst_node(-1),
      ready_time(0), run_time((simtime_picosec)(rt * 1e9)),
      start_time(0), finish_time(0)
{}

void FFTask::doNextEvent() {}

// cleanup() — called by FFLK44_128BFBAllreduceTask when AllReduce completes.
// Records finish time, increments counters, signals simulation end.
void FFTask::cleanup()
{
    state = TASK_FINISHED;
    ffapp->n_finished_tasks++;

    if (ffapp->final_finish_time < finish_time)
        ffapp->final_finish_time = finish_time;

    // Notify successors (none in bench mode, but kept for correctness)
    for (uint64_t nid : next_tasks) {
        FFTask* t = ffapp->tasks[nid];
        if (--t->counter == 0) {
            t->ready_time = finish_time;
            t->state = TASK_READY;
            eventlist().sourceIsPending(*t, t->ready_time);
        }
    }

    // Signal simulation end when all tasks complete
    if (ffapp->n_finished_tasks == ffapp->tasks.size()) {
        if (!ffapp->finished_once) {
            ffapp->finished_once  = true;
            ffapp->first_iter_time = ffapp->final_finish_time;
            FFApplication::finished_apps++;
        }
        if (FFApplication::finished_apps == FFApplication::total_apps)
            eventlist().setEndtime(eventlist().now());
    }
}

// Stubs for methods declared in ffapp.h but unreachable in bench mode
void FFTask::taskstart()       {}
void FFTask::start_flow()      {}
void FFTask::execute_compute() {}
void FFTask::add_nextask(FFTask*) {}

// taskfinish callback (required by linker)
void taskfinish(void* task)
{
    FFTask* t = static_cast<FFTask*>(task);
    t->finish_time = t->eventlist().now();
    t->cleanup();
}

// FFDevice stub constructors (required by linker)
FFDevice::FFDevice(FFApplication* app, std::string type_str, float bw,
                   int nid, int gid, int fn, int tn, int fg, int tg)
    : ffapp(app), node_id(nid), gpu_id(gid), bandwidth(bw * 8 * 1000),
      type(DEVICE_GPU), state(DEVICE_IDLE),
      from_gpu(fg), to_gpu(tg), from_node(fn), to_node(tn), busy_up_to(0)
{ (void)type_str; }

FFDevice::FFDevice(FFApplication* app, FlatBufTaskGraph::DeviceType,
                   uint64_t nid, uint64_t prop, uint64_t bw)
    : ffapp(app), node_id((int)nid), gpu_id(0),
      bandwidth((float)bw * 8 * 1000),
      type(DEVICE_GPU), state(DEVICE_IDLE),
      from_gpu(0), to_gpu(0), from_node(0), to_node(0), busy_up_to(0)
{ (void)prop; }

// ============================================================================
// FFLK44_128BFBAllreduceTask — full implementation
// ============================================================================

void ar_finish_l2k44_bfb_task(void* arinfo)
{
    auto* f = static_cast<FFLK44_128BFBTaskFlow*>(arinfo);
    f->ar->step_done(f->step, f->phase);
    delete f;
}

FFLK44_128BFBAllreduceTask::FFLK44_128BFBAllreduceTask(
        FFApplication*        ffapp,
        L2K44Topology*        top,
        std::vector<uint64_t> ng,
        uint64_t              sz,
        double                rt)
    : FFTask(ffapp, FFTask::TASK_ALLREDUCE),
      _topology(top),
      node_group(ng),
      operator_size(sz > 0 ? sz : 1ULL),
      current_step(0), current_phase(0),
      flows_in_step(0), finished_in_step(0)
{
    run_time = (simtime_picosec)(rt * 1e9);
    xfersize = sz;
}

void FFLK44_128BFBAllreduceTask::doNextEvent()
{
    start_time = ready_time;
    state      = FFTask::TASK_RUNNING;

    uint64_t N = (uint64_t)_topology->no_of_nodes();  // 128
    if (operator_size == 0) operator_size = 1;
    if (operator_size < 9000ULL * N) {
        operator_size = (uint64_t)(operator_size * (2.0 * (N - 1) / (double)N));
        if (operator_size == 0) operator_size = 1;
    }

    std::cerr << "[L2K44BFB] Starting AllReduce"
              << " N="            << N
              << " operator_size=" << operator_size
              << " bytes (" << operator_size / (1024.0 * 1024.0) << " MB)"
              << " D="            << _topology->bfb_num_steps() << "\n";

    current_step = current_phase = flows_in_step = finished_in_step = 0;
    launch_step();
}

void FFLK44_128BFBAllreduceTask::launch_step()
{
    const std::vector<L2K44BFBTransmission>* txs =
        (current_phase == 0)
        ? &_topology->bfb_rs_step_on_G(current_step)
        : &_topology->bfb_ag_step_on_G(current_step);

    flows_in_step    = 0;
    finished_in_step = 0;

    if (txs->empty()) {
        advance();
        return;
    }

    int P = (current_phase == 0)
            ? _topology->bfb_rs_total_chunks_on_G()
            : _topology->bfb_ag_total_chunks_on_G();
    if (P <= 0) P = 1;

    uint64_t N     = (uint64_t)_topology->no_of_nodes();  // 128
    uint64_t shard = operator_size / N;
    if (shard == 0) shard = 1;

    // Aggregate all schedule entries onto one TCP flow per (sender, receiver)
    std::map<std::pair<int,int>, uint64_t> link_bytes;
    for (const auto& tx : *txs) {
        uint64_t bytes;
        if (shard < (uint64_t)P * 64) {
            bytes = shard;
        } else {
            bytes = std::max((uint64_t)1,
                             shard * (uint64_t)tx.chunks / (uint64_t)P);
        }
        link_bytes[{tx.sender, tx.receiver}] += bytes;
    }

    simtime_picosec t0 = (current_step == 0 && current_phase == 0)
                         ? start_time
                         : eventlist().now();

    for (const auto& [link, total_bytes] : link_bytes) {
        if (start_bfb_flow(link.first, link.second, total_bytes, t0))
            flows_in_step++;
    }

    if (flows_in_step == 0) {
        advance();
    } else {
        std::cerr << "[L2K44BFB] phase=" << current_phase
                  << " step="    << current_step
                  << " launched=" << flows_in_step << " flows"
                  << " (from "   << txs->size() << " schedule entries)\n";
    }
}

bool FFLK44_128BFBAllreduceTask::start_bfb_flow(int snd, int rcv,
                                                  uint64_t bytes,
                                                  simtime_picosec t0)
{
    auto* f = new FFLK44_128BFBTaskFlow{this, current_step, current_phase};

    DCTCPSrc* fs = new DCTCPSrc(
        nullptr, nullptr, ffapp->fstream_out,
        eventlist(), snd, rcv,
        ar_finish_l2k44_bfb_task, static_cast<void*>(f));
    TcpSink* snk = new TcpSink();

    fs->set_flowsize(bytes);
    fs->set_ssthresh(ffapp->ssthresh * Packet::data_packet_size());
    fs->_rto = timeFromMs(ffapp->bfb_rto_ms);
    ffapp->tcpRtxScanner.registerTcp(*fs);

    auto* sp = _topology->get_paths(snd, rcv);
    if (!sp || sp->empty()) {
        delete sp; delete f; delete snk; delete fs;
        return false;
    }
    Route* ro = new Route(*(sp->at(rand() % sp->size())));
    ro->push_back(snk);
    delete sp;

    auto* dp = _topology->get_paths(rcv, snd);
    if (!dp || dp->empty()) {
        delete dp; delete f; delete snk; delete ro; delete fs;
        return false;
    }
    Route* ri = new Route(*(dp->at(rand() % dp->size())));
    ri->push_back(fs);
    delete dp;

    fs->connect(*ro, *ri, *snk, t0);
    return true;
}

void FFLK44_128BFBAllreduceTask::step_done(int step, int phase)
{
    if (step != current_step || phase != current_phase) return;
    if (++finished_in_step == flows_in_step)
        advance();
}

void FFLK44_128BFBAllreduceTask::advance()
{
    const int D = _topology->bfb_num_steps();  // must be 4

    if (current_step < D - 1) {
        current_step++;
        launch_step();
    } else if (current_phase == 0) {
        current_phase = 1;
        current_step  = 0;
        std::cerr << "[L2K44BFB] RS complete, starting AG\n";
        launch_step();
    } else {
        finish_time = eventlist().now();
        std::cerr << "[L2K44BFB] AllReduce complete at "
                  << timeAsMs(finish_time) << " ms\n";
        cleanup();
    }
}
