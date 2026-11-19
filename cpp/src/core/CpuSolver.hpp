#pragma once
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "SolverTypes.hpp"

// Multi-threaded CPU FDTD engine (port of worker.js).
// A persistent thread pool advances the staggered-grid acoustic wave equation
// with CPML; kernels are templated on the FD order so each stencil is fully
// unrolled and vectorised. Batches run asynchronously: submit() returns at once
// and the UI polls done().
class CpuSolver {
public:
    CpuSolver() = default;
    ~CpuSolver();

    void init(int W_sim, int H_sim, int nThreads, size_t sismoSize);
    int threadCount() const { return nThreads_; }
    int width() const { return W_; }
    int height() const { return H_; }

    void setVsq(const std::vector<float>& vsq);
    void setCpml(const std::vector<float>& bx, const std::vector<float>& ax,
                 const std::vector<float>& bz, const std::vector<float>& az);
    void setCpmlZ(const std::vector<float>& bz, const std::vector<float>& az);

    // These must only be called while no batch is running.
    void clearFields();     // P, Vx, Vz
    void clearPsi();        // CPML memory variables
    void clearSismo();
    void clearPixels();

    void submit(const BatchJob& job);
    bool busy() const { return busy_.load(std::memory_order_acquire); }
    void wait();

    // Results of the last finished batch
    const std::vector<float>& sismo() const { return sismo_; }
    const std::vector<uint32_t>& pixels() const { return pixels_; }
    bool instabilityDetected() const { return unstable_.load(); }
    void resetInstability() { unstable_ = false; }
    const std::vector<ThreadStat>& lastStats() const { return stats_; }
    // Temporal blocking (pipelined wavefront) instead of one barrier per phase
    void setPipelined(bool on) { usePipeline_ = on; }

    // Direct field access for tests
    const std::vector<float>& P() const { return P_; }

private:
    void shutdown();
    void workerMain(int t);
    template <int ORD> void runJob(int t);         // barrier per phase, rows split across threads
    template <int ORD> void runPipelined(int t);   // temporal blocking: one step per thread, wavefront
    void serialPhase(int s);
    void serialRows(int s, int ra, int rb);
    void pixelPhase(int t);

    int W_ = 0, H_ = 0, nThreads_ = 0;
    std::vector<float> P_, Vx_, Vz_, Vsq_, psiVx_, psiVz_, psiPx_, psiPz_;
    std::vector<float> bx_, ax_, bz_, az_;
    int xi0_ = 0, xi1_ = -1;   // contiguous columns with ax == 0 (no x-CPML)
    std::vector<float> sismo_;
    std::vector<uint32_t> pixels_;

    // Current job (owned copies of the caller's arrays)
    BatchJob job_;
    std::vector<StepData> steps_;
    std::vector<int> rowMin_, rowMax_;
    std::vector<float> obsX_, obsY_;   // OBS positions in grid cells

    std::vector<std::thread> threads_;
    std::unique_ptr<std::barrier<>> barrier_;
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t gen_ = 0;
    bool quit_ = false;
    std::atomic<int> remaining_{0};
    std::atomic<bool> busy_{false};
    std::atomic<bool> unstable_{false};
    std::vector<ThreadStat> stats_;
    std::unique_ptr<std::atomic<int64_t>[]> prog_;   // per thread: step * (H + 1) + pressure front
    bool usePipeline_ = true, pipelined_ = true;
    static constexpr int kPipeRows = 8;
};
