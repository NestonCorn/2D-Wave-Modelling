#pragma once
#include <atomic>
#include <utility>
#include <vector>

// Expanding-box region returned by Eikonal::getBox (getEikonalBox in the HTML).
// hasRows == false means "no per-row bounds" (xMinZ/xMaxZ were null in JS).
struct EikBox {
    int min_i = 0, max_i = 0, min_j = 0, max_j = 0;
    bool hasRows = false;
    std::vector<int> xMinZ, xMaxZ;
};

// First-arrival travel-time solver (4-sweep fast sweeping) used to restrict the
// FDTD update to the region the wavefront can have reached.
//
// The HTML stored, for every time bucket, a full copy of the cumulative box
// (num_steps * H_sim * 2 ints, which explodes on big grids). Here each row keeps
// prefix/suffix minima of the arrival bucket so the same cumulative box is found
// with two binary searches per row; memory is O(W*H) and results are identical.
class Eikonal {
public:
    // srcCells: (si, sj) grid cells of the active shots.
    void compute(const std::vector<float>& vsq, int W_sim, int H_sim, double SCALE, double dt,
                 const std::vector<std::pair<int, int>>& srcCells, std::atomic<int>* progressPct = nullptr);
    void clear() { valid_ = false; }
    bool valid() const { return valid_; }

    void getBox(double t_sec, int fd_order, bool freeSurface, int sj_surf, EikBox& out) const;

private:
    bool valid_ = false;
    int W_ = 0, H_ = 0, numSteps_ = 0;
    double bucket_ = 1.0;
    std::vector<int> leftMin_, rightMin_, rowMin_;
};
