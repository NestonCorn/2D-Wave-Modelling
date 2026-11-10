#pragma once
#include <map>
#include <string>
#include <vector>
#include "SolverTypes.hpp"

// OpenGL 4.3 compute-shader FDTD engine (port of gpu_engine.js).
// All calls must come from the thread that owns the GL context.
// submit() records a whole batch (5 dispatches per step), colours the wavefield
// directly into the display texture and starts an async sismogram readback;
// poll() returns true once the GPU has finished (fence based, no stall).
class GpuSolver {
public:
    ~GpuSolver();

    // Checks GL version / compute limits. Returns false with a Turkish reason.
    static bool queryCapabilities(std::string& renderer, std::string& reason, long long& maxSsboBytes);

    bool init(int W_sim, int H_sim, int N_REC, int N_OBS, std::string& err);
    void destroy();
    bool ready() const { return ready_; }
    bool matches(int W, int H, int nRec, int nObs) const {
        return ready_ && W == W_ && H == H_ && nRec == nRec_ && nObs == nObs_;
    }

    void setVsq(const std::vector<float>& vsq);
    void setCpml(const std::vector<float>& bx, const std::vector<float>& ax,
                 const std::vector<float>& bz, const std::vector<float>& az);
    void setCpmlZ(const std::vector<float>& bz, const std::vector<float>& az);
    void setObs(const std::vector<float>& obsX, const std::vector<float>& obsY);

    void clearFields();
    void clearPsi();
    void clearSismo();

    bool ensurePrograms(int fd_order, std::string& err);
    void submit(const BatchJob& job, unsigned waveTexture);
    bool busy() const { return fence_ != nullptr; }
    bool poll();          // non-blocking; true when the in-flight batch completed
    void wait();          // blocking

    const std::vector<float>& sismo() const { return sismo_; }
    bool instabilityDetected() const { return unstable_; }   // from the last batch's probe row
    void readP(std::vector<float>& out);   // tests only (blocking)

private:
    struct Programs { unsigned vel = 0, pres = 0, surf = 0, src = 0, sismo = 0, pix = 0; int locStep[5]{}; };
    unsigned makeProgram(int order, const char* body, std::string& err);
    void finishReadback();

    bool ready_ = false;
    int W_ = 0, H_ = 0, nRec_ = 0, nObs_ = 0;
    size_t sismoSize_ = 0;
    unsigned buf_[12]{};       // SSBOs by binding index
    unsigned ubo_ = 0, readback_ = 0;
    std::map<int, Programs> programs_;
    void* fence_ = nullptr;   // GLsync
    std::vector<float> sismo_;
    std::vector<float> cpmlHost_;
    std::vector<float> probe_;
    bool probeValid_ = false, unstable_ = false;
};
