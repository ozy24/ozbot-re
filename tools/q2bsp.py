#!/usr/bin/env python3
"""
Minimal Quake 2 BSP (IBSP v38) reader: point contents, sampled line of sight,
and cluster PVS.  Enough geometry for aim/visibility studies over demos and bot
telemetry without an engine (tools/q2prox_profile.py).

    from q2bsp import BSP
    b = BSP("engine/baseq2/maps/q2duel5.bsp")
    b.contents(p) & 1        # CONTENTS_SOLID
    b.los(eye, target)       # True if no solid sample between them (4u steps)
    b.pvs(a, b2)             # True if b2's cluster is in a's PVS -- the engine's
                             # "would this entity be sent" test, which is what a
                             # .dm2 inherently filters on

Usage:  python q2bsp.py <map.bsp> [x y z]   -- prints sizes / contents at a point
"""

import math
import struct
import sys

CONTENTS_SOLID = 1
LUMP_PLANES, LUMP_VISIBILITY, LUMP_NODES, LUMP_LEAFS = 1, 3, 4, 8


class BSP:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"IBSP":
            raise ValueError(f"{path}: not an IBSP file")
        lumps = [struct.unpack_from("<ii", d, 8 + 8 * i) for i in range(19)]
        po, pl = lumps[LUMP_PLANES]
        self.planes = [struct.unpack_from("<ffffi", d, po + 20 * i) for i in range(pl // 20)]
        no, nl = lumps[LUMP_NODES]
        self.nodes = [struct.unpack_from("<iii", d, no + 28 * i) for i in range(nl // 28)]
        lo, ll = lumps[LUMP_LEAFS]
        leafs = [struct.unpack_from("<ih", d, lo + 28 * i) for i in range(ll // 28)]
        self.leaf_contents = [c for c, _ in leafs]
        self.leaf_cluster = [cl for _, cl in leafs]
        vo, vl = lumps[LUMP_VISIBILITY]
        self._vis = d[vo:vo + vl]
        self.numclusters = struct.unpack_from("<i", self._vis, 0)[0] if vl else 0
        self._pvs_cache = {}

    def leaf(self, p):
        n = 0
        while n >= 0:
            pi, c0, c1 = self.nodes[n]
            nx, ny, nz, dist, _ = self.planes[pi]
            n = c0 if nx * p[0] + ny * p[1] + nz * p[2] - dist >= 0 else c1
        return -n - 1

    def contents(self, p):
        return self.leaf_contents[self.leaf(p)]

    def los(self, a, b, step=4.0):
        L = math.dist(a, b)
        k = max(1, int(L / step))
        for i in range(1, k):
            t = i / k
            q = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)
            if self.contents(q) & CONTENTS_SOLID:
                return False
        return True

    def _cluster_pvs(self, c):
        """Decompress cluster c's PVS row (zero-run RLE) into a set of clusters."""
        s = self._pvs_cache.get(c)
        if s is not None:
            return s
        ofs = struct.unpack_from("<i", self._vis, 4 + 8 * c)[0]
        s, cl, i = set(), 0, ofs
        while cl < self.numclusters:
            byte = self._vis[i]
            i += 1
            if byte == 0:
                cl += 8 * self._vis[i]
                i += 1
                continue
            for bit in range(8):
                if byte & (1 << bit) and cl + bit < self.numclusters:
                    s.add(cl + bit)
            cl += 8
        self._pvs_cache[c] = s
        return s

    def pvs(self, a, b):
        if not self.numclusters:
            return True
        ca, cb = self.leaf_cluster[self.leaf(a)], self.leaf_cluster[self.leaf(b)]
        if ca < 0 or cb < 0:
            return False
        return cb in self._cluster_pvs(ca)

    def pvs_box(self, eye, origin, mins=(-16, -16, -24), maxs=(16, 16, 32)):
        """Server-style entity PVS: the entity is sent if ANY cluster its bbox
        touches is visible from the viewer's cluster (SV_BuildClientFrame tests
        the entity's clusternums, not its origin).  Samples the 8 corners and
        the origin -- not exact, but the corners are what cross cluster planes."""
        if not self.numclusters:
            return True
        ca = self.leaf_cluster[self.leaf(eye)]
        if ca < 0:
            return False
        row = self._cluster_pvs(ca)
        for dx in (mins[0], maxs[0]):
            for dy in (mins[1], maxs[1]):
                for dz in (mins[2], maxs[2]):
                    cb = self.leaf_cluster[self.leaf((origin[0] + dx, origin[1] + dy, origin[2] + dz))]
                    if cb >= 0 and cb in row:
                        return True
        cb = self.leaf_cluster[self.leaf(origin)]
        return cb >= 0 and cb in row


if __name__ == "__main__":
    b = BSP(sys.argv[1])
    print(f"planes={len(b.planes)} nodes={len(b.nodes)} leafs={len(b.leaf_contents)} "
          f"clusters={b.numclusters}")
    if len(sys.argv) >= 5:
        p = tuple(float(v) for v in sys.argv[2:5])
        print(f"contents at {p}: {b.contents(p)} (solid={bool(b.contents(p) & 1)}) "
              f"cluster={b.leaf_cluster[b.leaf(p)]}")
