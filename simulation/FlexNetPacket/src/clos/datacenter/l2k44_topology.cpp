// l2k44_topology.cpp
#include "l2k44_topology.h"
#include "ecnqueue.h"
#include "randomqueue.h"
#include "compositequeue.h"
#include <cassert>
#include <iostream>
#include <algorithm>
#include <deque>

extern uint32_t RTT;
extern uint32_t SPEED;
extern uint32_t ECNTHRESH;

L2K44Topology::L2K44Topology(int no_of_nodes, mem_b queuesize,
                   Logfile *lg, EventList *ev,
                   FirstFit *fit, queue_type q)
    : _no_of_nodes(no_of_nodes), _queuesize(queuesize),
      logfile(lg), eventlist(ev), ff(fit), qt(q)
{
    assert(no_of_nodes == 128);
    queues.assign(128, std::vector<Queue*>(128, nullptr));
    pipes .assign(128, std::vector<Pipe* >(128, nullptr));

    build_graph();
    load_bfb_routes();
    load_bfb_schedule();
    init_network();

    std::cerr << "[L2K44Topology] ready: N=128"
              << " bfb_steps=" << bfb_num_steps()
              << " P_RS_G=" << _bfb_rs_total_chunks_on_G << "\n";
}

L2K44Topology::~L2K44Topology() {
    for (int u=0;u<128;u++) for (int v=0;v<128;v++) {
        delete queues[u][v]; delete pipes[u][v];
    }
}

void L2K44Topology::build_graph() {
    // Build K4,4 edges, then L(K4,4), then L^2(K4,4)
    std::vector<std::pair<int,int>> eK44;
    for (int a=0;a<4;a++) for (int b=0;b<4;b++) eK44.push_back({a,4+b});
    for (int b=0;b<4;b++) for (int a=0;a<4;a++) eK44.push_back({4+b,a});
    const int NLK=32;
    std::vector<std::vector<int>> aLK(NLK);
    for (int e1=0;e1<NLK;e1++) { int h=eK44[e1].second; for (int e2=0;e2<NLK;e2++) if(e1!=e2&&eK44[e2].first==h) aLK[e1].push_back(e2); }
    std::vector<std::pair<int,int>> eL2; std::map<std::pair<int,int>,int> ei;
    for (int u=0;u<NLK;u++) for (int v:aLK[u]) { ei[{u,v}]=(int)eL2.size(); eL2.push_back({u,v}); }
    assert((int)eL2.size()==128);
    _adj.assign(128,{});
    for (int i=0;i<128;i++) { int v=eL2[i].second; for (int w:aLK[v]) _adj[i].push_back(ei[{v,w}]); }
    std::cerr << "[L2K44Topology] Graph built: 128 nodes\n";
}

// void L2K44Topology::load_bfb_routes() {
// #include "l2k44_128_routes_generated.cpp"
//     std::cerr << "[L2K44Topology] Routes loaded\n";
// }

// NEW — replace with this:
extern void load_l2k44_routes(
    std::unordered_map<uint64_t, std::vector<std::vector<int>>>& _routes);

void L2K44Topology::load_bfb_routes() {
    load_l2k44_routes(_routes);
    std::cerr << "[L2K44Topology] Routes loaded\n";
}

void L2K44Topology::load_bfb_schedule() {
#include "l2k44_128_schedule_generated.cpp"
    std::cerr << "[L2K44Topology] Schedule loaded\n";
}

void L2K44Topology::init_network() {
    linkspeed_bps speed = (linkspeed_bps)SPEED * 1000000ULL;
    for (int u=0;u<128;u++) for (int v:_adj[u]) {
        if (!queues[u][v]) {
            queues[u][v] = new Queue(speed, _queuesize, *eventlist, nullptr);
            pipes [u][v] = new Pipe(timeFromUs(1.0), *eventlist);
            queues[u][v]->setName("Q-"+std::to_string(u)+"->"+std::to_string(v));
            pipes [u][v]->setName("P-"+std::to_string(u)+"->"+std::to_string(v));
        }
        // reverse for ACKs
        if (!queues[v][u]) {
            queues[v][u] = new Queue(speed, _queuesize, *eventlist, nullptr);
            pipes [v][u] = new Pipe(timeFromUs(1.0), *eventlist);
            queues[v][u]->setName("Q-"+std::to_string(v)+"->"+std::to_string(u));
            pipes [v][u]->setName("P-"+std::to_string(v)+"->"+std::to_string(u));
        }
    }
}

std::vector<const Route*>* L2K44Topology::get_paths(int src, int dst) {
    auto* v = new std::vector<const Route*>();
    if (src == dst) return v;
    uint64_t key = (uint64_t)src * 128 + dst;
    auto it = _routes.find(key);
    if (it == _routes.end() || it->second.empty()) {
        std::cerr << "[L2K44Topology] no route " << src << "->" << dst << "\n";
        return v;
    }
    for (const auto& path : it->second) {
        Route* r = new Route();
        for (int k=0; k+1<(int)path.size(); k++) {
            int u=path[k], w=path[k+1];
            r->push_back(queues[u][w]);
            r->push_back(pipes [u][w]);
        }
        v->push_back(r);
    }
    return v;
}

std::vector<int>* L2K44Topology::get_neighbours(int src) {
    return new std::vector<int>(_adj[src]);
}
