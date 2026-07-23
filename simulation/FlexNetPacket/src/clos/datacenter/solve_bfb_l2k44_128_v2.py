#!/usr/bin/env python3
"""
solve_bfb_l2k44_128_v2.py
==========================
Computes BFB AllReduce schedule for L²(K₄,₄) — 128-node, degree-4 topology.

Construction (directed throughout, per paper §3.1):
  K₄,₄ directed: parts {0..3} and {4..7}, edges in both directions → 32 directed edges
  L(K₄,₄): 32 nodes (one per directed edge of K₄,₄), out-degree 4
             node uv -> vw for each (v,w) in K₄,₄, excluding self-loops
  L²(K₄,₄): 128 nodes (one per directed edge of L(K₄,₄)), out-degree 4
              node (uv)(vw) -> (vw)(wx) for each (vw,wx) in L(K₄,₄)

Properties (from paper Table 7):
  N=128, degree=4, diameter D(G)=4
  T_L = 4*alpha, T_B = 1.031*M/B
  Reverse-symmetric: G ≅ G^T (so RS derived from AG on G^T)

Note on bidirectionality:
  The directed graph is used for BFB schedule computation.
  The simulator's init_network() already creates queues in both directions
  for ACK paths, so no changes needed to the topology file.

Outputs:
  l2k44_128_routes_generated.cpp
  l2k44_128_schedule_generated.cpp

Run:
  pip install scipy --break-system-packages
  python3 solve_bfb_l2k44_128_v2.py
"""

import sys
from collections import defaultdict, deque

try:
    from scipy.optimize import linprog
except ImportError:
    sys.exit("ERROR: pip install scipy --break-system-packages")

# ---------------------------------------------------------------------------
# Build L²(K₄,₄) — directed construction per paper
# ---------------------------------------------------------------------------

def build_l2k44():
    # Step 1: Directed edges of K₄,₄
    # Left part {0,1,2,3}, Right part {4,5,6,7}
    # Every left->right and right->left directed edge
    edges_K44 = []
    for a in range(4):
        for b in range(4):
            edges_K44.append((a, 4 + b))   # left -> right
    for b in range(4):
        for a in range(4):
            edges_K44.append((4 + b, a))   # right -> left
    # 32 directed edges total
    assert len(edges_K44) == 32

    N_LK44 = 32

    # Step 2: Build directed L(K₄,₄)
    # Node e1 in L(K₄,₄) represents directed edge e1 in K₄,₄
    # Edge e1 -> e2 if head(e1) == tail(e2) and e1 != e2
    adj_LK44 = [[] for _ in range(N_LK44)]
    for e1 in range(N_LK44):
        head_e1 = edges_K44[e1][1]
        for e2 in range(N_LK44):
            if e1 != e2 and edges_K44[e2][0] == head_e1:
                adj_LK44[e1].append(e2)

    # Each node in directed L(K₄,₄) has out-degree 4
    # (head of e1 has 4 outgoing edges in K₄,₄, all become neighbours except e1 itself
    #  but since K₄,₄ is bipartite, the reverse edge (head->tail) is NOT among them)
    for i in range(N_LK44):
        assert len(adj_LK44[i]) == 4, \
            f"L(K₄,₄) node {i} has out-degree {len(adj_LK44[i])}, expected 4"

    # Step 3: Directed edges of L(K₄,₄)
    edges_L2 = []
    edge_index = {}
    for u in range(N_LK44):
        for v in adj_LK44[u]:
            edge_index[(u, v)] = len(edges_L2)
            edges_L2.append((u, v))

    N = len(edges_L2)
    assert N == 128, f"Expected 128 nodes in L²(K₄,₄), got {N}"

    # Step 4: Build directed L²(K₄,₄)
    # Node i represents directed edge i in L(K₄,₄)
    # Edge i -> j if head(edge_i) == tail(edge_j) and i != j
    adj = defaultdict(list)
    for i, (u, v) in enumerate(edges_L2):
        for w in adj_LK44[v]:
            j = edge_index[(v, w)]
            if j != i:
                adj[i].append(j)

    # Each node has out-degree 4
    for i in range(N):
        assert len(adj[i]) == 4, \
            f"L²(K₄,₄) node {i} has out-degree {len(adj[i])}, expected 4"

    return adj, N

# ---------------------------------------------------------------------------
# BFS utilities
# ---------------------------------------------------------------------------

def bfs_dist(adj, src, N):
    d = {src: 0}
    q = deque([src])
    while q:
        u = q.popleft()
        for v in adj[u]:
            if v not in d:
                d[v] = d[u] + 1
                q.append(v)
    assert len(d) == N, \
        f"Graph not connected from {src}: reached {len(d)}/{N} nodes"
    return d

def all_dists(adj, N):
    return {u: bfs_dist(adj, u, N) for u in range(N)}

def bfs_single_path(adj, src, N):
    """BFS shortest path tree from src using given adjacency."""
    parent = {src: None}
    q = deque([src])
    while q:
        u = q.popleft()
        for v in adj[u]:
            if v not in parent:
                parent[v] = u
                q.append(v)
    paths = {}
    for dst in range(N):
        if dst == src:
            continue
        path, cur = [], dst
        while cur is not None:
            path.append(cur)
            cur = parent.get(cur)
        path.reverse()
        if path and path[0] == src:
            paths[dst] = path
    return paths

def transpose(adj, N):
    """Reverse all edges."""
    t = defaultdict(list)
    for u in range(N):
        for v in adj[u]:
            if u not in t[v]:
                t[v].append(u)
    return t

def bidir(adj, N):
    """Make bidirectional — used only for route computation so ACKs work."""
    b = defaultdict(list)
    for u in range(N):
        for v in adj[u]:
            if v not in b[u]: b[u].append(v)
            if u not in b[v]: b[v].append(u)
    return b

# ---------------------------------------------------------------------------
# BFB AllGather LP solver
# Same formulation as solve_bfb_distreg32.py
# ---------------------------------------------------------------------------

def solve_bfb_ag(adj, N):
    """
    Compute BFB AllGather schedule via LP.

    At each step t (1..D), for each destination u:
      - origins: nodes v with dist(v->u) == t
      - eligible in-neighbours: nodes w with edge w->u and dist(v->w) == t-1
      - LP minimises max load on any single in-link to u
        subject to every origin's shard being fully delivered
    """
    df = all_dists(adj, N)
    D  = max(max(d.values()) for d in df.values())
    print(f"  Diameter = {D}")

    in_nbr = defaultdict(list)
    for u in range(N):
        for v in adj[u]:
            in_nbr[v].append(u)

    steps     = [[] for _ in range(D)]
    all_fracs = []

    for t in range(1, D + 1):
        print(f"  Solving LP step t={t} ...")
        for u in range(N):
            origins = [v for v in range(N)
                       if v != u and df[v].get(u) == t]
            if not origins:
                continue

            elig = {v: [w for w in in_nbr[u]
                        if df[v].get(w) == t - 1]
                    for v in origins}

            vw = [(v, w) for v in origins for w in elig[v]]
            if not vw:
                continue

            nvar  = len(vw) + 1
            U_idx = nvar - 1
            idx   = {pair: i for i, pair in enumerate(vw)}

            # Objective: minimise U
            c_obj        = [0.0] * nvar
            c_obj[U_idx] = 1.0

            # Equality: each origin fully delivered
            A_eq, b_eq = [], []
            for v in origins:
                row = [0.0] * nvar
                for w in elig[v]:
                    row[idx[(v, w)]] = 1.0
                A_eq.append(row)
                b_eq.append(1.0)

            # Inequality: load on each in-link <= U
            A_ub, b_ub = [], []
            for w in in_nbr[u]:
                row = [0.0] * nvar
                for v in origins:
                    if (v, w) in idx:
                        row[idx[(v, w)]] = 1.0
                row[U_idx] = -1.0
                A_ub.append(row)
                b_ub.append(0.0)

            bounds = [(0.0, 1.0)] * (nvar - 1) + [(0.0, None)]

            res = linprog(c_obj, A_ub=A_ub, b_ub=b_ub,
                          A_eq=A_eq, b_eq=b_eq,
                          bounds=bounds, method='highs')

            if not res.success:
                print(f"    WARNING: LP failed u={u} t={t}: {res.message}")
                for v in origins:
                    for w in elig[v]:
                        frac = 1.0 / len(elig[v])
                        steps[t - 1].append((w, u, v, frac))
                        all_fracs.append(frac)
                continue

            x = res.x
            for (v, w), i in idx.items():
                frac = x[i]
                if frac > 1e-6:
                    steps[t - 1].append((w, u, v, frac))
                    all_fracs.append(frac)

    # Find minimum P for clean discretisation (error < 1%)
    P = 4
    for candidate in [4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256]:
        err = max((abs(round(candidate * f) - candidate * f)
                   for f in all_fracs), default=0.0)
        if err < 0.01:
            P = candidate
            break

    print(f"  Discretisation: P={P}")

    int_steps = []
    for step in steps:
        int_step = []
        for (w, u, v, frac) in step:
            chunks = max(1, round(P * frac))
            int_step.append((w, u, v, chunks))
        int_steps.append(int_step)

    return int_steps, P, D


def derive_rs_from_ag(ag_steps):
    """
    RS on G = reverse of AG on G^T.
    For reverse-symmetric graphs (G ≅ G^T), this gives the correct RS schedule.
    """
    rs = []
    for t in range(len(ag_steps) - 1, -1, -1):
        rs.append([(rcv, snd, orig, chunks)
                   for (snd, rcv, orig, chunks) in ag_steps[t]])
    return rs

# ---------------------------------------------------------------------------
# C++ emitters
# ---------------------------------------------------------------------------

def emit_routes(routes, N):
    lines = [
        "// l2k44_128_routes_generated.cpp",
        "// Auto-generated by solve_bfb_l2k44_128_v2.py — DO NOT EDIT",
        "",
    ]
    for src in range(N):
        for dst, path in routes[src].items():
            key      = src * N + dst
            path_str = "{" + ", ".join(str(n) for n in path) + "}"
            lines.append(f"_routes[{key}ULL].push_back({path_str});")
    lines.append("")
    return "\n".join(lines) + "\n"


def emit_schedule(ag_G, P_ag_G, rs_G, P_rs_G,
                  ag_GT, P_ag_GT, rs_GT, P_rs_GT, D):
    lines = [
        "// l2k44_128_schedule_generated.cpp",
        "// Auto-generated by solve_bfb_l2k44_128_v2.py — DO NOT EDIT",
        f"// L²(K₄,₄): N=128, d=4, diameter={D}, T_B=1.031*M/B",
        "",
        f"_bfb_ag_total_chunks_on_G  = {P_ag_G};",
        f"_bfb_rs_total_chunks_on_G  = {P_rs_G};",
        f"_bfb_ag_total_chunks_on_GT = {P_ag_GT};",
        f"_bfb_rs_total_chunks_on_GT = {P_rs_GT};",
        "",
        f"_bfb_ag_steps_on_G.resize({len(ag_G)});",
        f"_bfb_rs_steps_on_G.resize({len(rs_G)});",
        f"_bfb_ag_steps_on_GT.resize({len(ag_GT)});",
        f"_bfb_rs_steps_on_GT.resize({len(rs_GT)});",
        "",
    ]

    def emit_array(name, steps):
        lines.append(f"// {name}")
        for t, step in enumerate(steps):
            for (snd, rcv, orig, chunks) in step:
                lines.append(
                    f"_{name}[{t}].push_back("
                    f"{{/*sender=*/{snd},/*receiver=*/{rcv},"
                    f"/*origin=*/{orig},/*chunks=*/{chunks}}});")
        lines.append("")

    emit_array("bfb_ag_steps_on_G",  ag_G)
    emit_array("bfb_rs_steps_on_G",  rs_G)
    emit_array("bfb_ag_steps_on_GT", ag_GT)
    emit_array("bfb_rs_steps_on_GT", rs_GT)

    return "\n".join(lines) + "\n"

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("Building L²(K₄,₄) ...")
    adj_G, N = build_l2k44()
    DEG      = 4
    L2K44_D  = 4

    # Verify connectivity and diameter on directed graph
    dist0 = bfs_dist(adj_G, 0, N)
    diam  = max(dist0.values())
    print(f"  G (directed): {N} nodes, out-degree {DEG}, diameter {diam}")
    assert diam == L2K44_D, f"Expected diameter {L2K44_D}, got {diam}"

    # Transpose for RS phase (G is reverse-symmetric so G^T has same diameter)
    adj_GT = transpose(adj_G, N)
    dist0_T = bfs_dist(adj_GT, 0, N)
    diam_T  = max(dist0_T.values())
    print(f"  G^T (directed): {N} nodes, diameter {diam_T}")

    # Bidirectional version for route computation only (ensures ACK paths exist)
    adj_B = bidir(adj_G, N)

    print("\nComputing BFS routes (bidirectional for ACK paths) ...")
    routes   = {src: bfs_single_path(adj_B, src, N) for src in range(N)}
    n_routes = sum(len(v) for v in routes.values())
    assert n_routes == N * (N - 1), \
        f"Expected {N*(N-1)} routes, got {n_routes}"
    print(f"  {n_routes} paths computed ✓")

    print("\nSolving BFB AllGather LP on G ...")
    ag_G, P_ag_G, D = solve_bfb_ag(adj_G, N)
    print(f"  AG(G):  steps={len(ag_G)}, txns={[len(s) for s in ag_G]}, P={P_ag_G}")

    print("\nSolving BFB AllGather LP on G^T ...")
    ag_GT, P_ag_GT, _ = solve_bfb_ag(adj_GT, N)
    print(f"  AG(GT): steps={len(ag_GT)}, txns={[len(s) for s in ag_GT]}, P={P_ag_GT}")

    # RS on G = reverse of AG on G^T, and vice versa
    rs_G    = derive_rs_from_ag(ag_GT)
    P_rs_G  = P_ag_GT
    rs_GT   = derive_rs_from_ag(ag_G)
    P_rs_GT = P_ag_G

    print(f"\n  RS(G):  txns={[len(s) for s in rs_G]} (derived from AG on G^T)")
    print(f"  RS(GT): txns={[len(s) for s in rs_GT]} (derived from AG on G)")

    print("\nWriting l2k44_128_routes_generated.cpp ...")
    code = emit_routes(routes, N)
    with open("l2k44_128_routes_generated.cpp", "w") as f:
        f.write(code)
    print(f"  {len(code.splitlines())} lines written")

    print("Writing l2k44_128_schedule_generated.cpp ...")
    code = emit_schedule(ag_G,  P_ag_G,  rs_G,  P_rs_G,
                         ag_GT, P_ag_GT, rs_GT, P_rs_GT, D)
    with open("l2k44_128_schedule_generated.cpp", "w") as f:
        f.write(code)
    print(f"  {len(code.splitlines())} lines written")

    # Theoretical bandwidth check
    M           = 1073741824   # 1 GB
    B           = 200e9 / 8    # 200 Gbps in bytes/s
    T_bw_paper  = 1.031 * M / B
    print(f"\n=== Theoretical predictions (M=1GB, B=200Gbps) ===")
    print(f"  T_B paper (L²(K₄,₄)) = {T_bw_paper*1e3:.3f} ms")

    shard      = M / N
    step_loads = []
    for t, step in enumerate(rs_G):
        unique_links = {}
        for (snd, rcv, orig, chunks) in step:
            k = (snd, rcv)
            unique_links[k] = unique_links.get(k, 0) + chunks
        max_chunks = max(unique_links.values()) if unique_links else 0
        P          = P_rs_G
        max_bytes  = max_chunks * shard / P
        step_time  = max_bytes / B
        step_loads.append(step_time * 1e3)
        print(f"  RS step {t}: max_link_load={max_chunks}/{P} "
              f"= {max_bytes/1e6:.2f} MB → {step_time*1e3:.3f} ms")

    rs_bound = sum(step_loads)
    print(f"  RS bound            = {rs_bound:.3f} ms")
    print(f"  Total AllReduce BW  = {2*rs_bound:.3f} ms")
    print(f"  T_B* paper optimal  = {T_bw_paper*1e3:.3f} ms")
    print(f"\nDone.")


if __name__ == "__main__":
    main()
