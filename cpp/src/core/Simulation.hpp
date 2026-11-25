#pragma once
#include <atomic>
#include <cmath>
#include <future>
#include <string>
#include <vector>
#include "CpuSolver.hpp"
#include "Eikonal.hpp"
#include "Geology.hpp"
#include "GpuSolver.hpp"
#include "Material.hpp"
#include "ModelImport.hpp"
#include "Params.hpp"
#include "Segy.hpp"
#include "Wavelet.hpp"

// t0: firing step. x: source position (m) at the firing instant. The whole source time
// function is injected there: at low frequency it lasts seconds, and following the towed
// source from cell to cell put a step into it at every cell crossing (grid-scale noise).
struct Shot { bool main; double t0; double x; };

enum class BadgeKind { Init, Gpu, ForcedCpu, Warning, Cpu };
struct Badge {
    BadgeKind kind = BadgeKind::Init;
    std::string label;     // text after the icon
    std::string reason;    // second line under the badge
};

struct NumericalStatus {
    double dt_max, cfl_ratio, ppw;
    int min_ppw;
};

// Simulation controller: the global state and event handlers of the HTML page
// (startMode/stopMode/loop/cpuLoop/gpuLoop/fireShot/update* handlers) minus the drawing.
// Everything runs on the GUI thread except the CPU solver threads and the eikonal task.
class Simulation {
public:
    Simulation();
    ~Simulation();

    // Requires a current GL context (for the wave texture and the GPU engine).
    void init(bool glAvailable);
    // Called once per frame. now = seconds since start, frameDt = real seconds since last call.
    void update(double now, double frameDt);

    // ── Toolbar actions ──
    void startMode(Mode m);
    void stopMode();
    void togglePause();
    void toggleComputeEngine();
    void toggleSismoOnly() { p.sismo_only = !p.sismo_only; }
    void toggleExpandingBox() { p.expanding_box = !p.expanding_box; }
    void toggleFreeSurface();
    void toggleObs();

    // ── Setting handlers (call after changing the matching field in p) ──
    void onFdOrder();
    void onGridSize(double now);        // debounced 100 ms like setGridSize()
    void onTimeStep();
    void onNpml();
    void onCableLen();
    void onReceiverSpacing(bool forceReset = true);
    void onShipX(double x);
    void onSourceOffset();
    void onGeology();
    void onDomainWidth(double now);
    void onDomainHeight(double now);
    void onObsSpacing();

    // ── Imported subsurface model ──
    // vel (m/s) is required; rho (g/cm^3) may be empty: the density then follows the
    // velocity (Gardner). z = 0 of the model is the sea surface; the domain takes the
    // model's size. Fails (and changes nothing) on an unusable model.
    bool loadModel(GridModel vel, GridModel rho, std::string& err);
    void clearModel();   // back to the built-in flat-layer model
    bool hasModel() const { return !modelVel_.empty(); }
    const GridModel& modelVelocity() const { return modelVel_; }
    const GridModel& modelDensity() const { return modelRho_; }
    // Seafloor depth (m from the top of the domain) at x: OBS placement, drawing, SEG-Y
    double seafloorDepth(double x) const;

    // ── SEG-Y live recording ──
    bool segyRecording() const { return segyRecording_; }
    int segyShotsSaved() const { return segyShotNum_; }
    void segyStart(const std::string& rootFolder);   // UI already picked the folder
    void segyStop(double now);
    const std::string& segyButtonLabel(double now);

    // ── Line simulation: every shot of a line into one SEG-Y per receiver type ──
    // Station k: the main source fires at x = firstX + k * interval, with every current
    // parameter, the ship moving at the set speed through the record (the streamer moves,
    // each source stays where it fired). Each station starts from a quiet wavefield.
    //   Trad  one shot, one record.
    //   SLO   the source vessel (src_offset ahead) fires 25 steps later: one blended record.
    //   CLO   the source vessel fires when the main record ends: a second record of the
    //         recording length (streamer -> line_streamer_src.segy, OBS -> line_obs.segy).
    // Gathers are appended to line_streamer.segy / line_obs.segy in a new folder under
    // `folder`. Runs batch by batch from update(), so the UI stays live.
    struct LineSettings {
        double firstX = 0, interval = 25;
        int nShots = 1;
        Mode mode = Mode::Trad;
        std::string folder;
    };
    bool startLine(const LineSettings& s, std::string& err);
    void cancelLine();
    bool lineActive() const { return line_.active; }
    int lineShotsDone() const { return line_.shot; }
    int lineShots() const { return line_.n; }
    double lineFirstX() const { return line_.firstX; }
    bool gliding() const { return glide_.active; }   // between a finished record and the next shot
    Mode lineMode() const { return line_.mode; }
    double lineProgress() const;   // 0..1 over the whole line
    const std::string& lineFolder() const { return line_.dir; }

    // ── Parameter files ──
    std::string configJson() const;
    bool applyConfigJson(const std::string& text, std::string& err);

    NumericalStatus numericalStatus() const;
    // Expanding box for drawing (getEikonalBox(cycle_t * dt_phy) with the toggles applied)
    bool drawBox(EikBox& out) const;
    double dtPhy() const { return (SCALE / 6000.0) * p.dt; }
    // Length of the record in time steps. Sample g of a trace is the mean of the steps
    // t_local in [g k, g k + k - 1] (k = sismoStepsPerSample(), the smallest that fits the
    // record in kSismoStride samples): every sample spans the same time, so the wavelet is
    // sampled at evenly spaced times on every trace.
    double sismoSteps() const { return std::floor((p.sismo_ms * 6.0) / (SCALE * p.dt)); }
    int sismoStepsPerSample() const { return std::max(1, (int)std::ceil(sismoSteps() / kSismoStride)); }
    int sismoSamples() const {
        return std::clamp((int)std::ceil(sismoSteps() / sismoStepsPerSample()), 1, kSismoStride);
    }
    int sjSurf() const { return (int)std::floor(OCEAN_Y / SCALE); }

    // Alerts raised by actions: problems the user must read (UI shows them in a modal, then clears)
    std::vector<std::string> alerts;
    // Confirmations (finished line, ...): the UI shows them briefly and clears them
    std::vector<std::string> notices;

    // ── State read by the UI ──
    SimParams p;
    double SCALE = 4.0;
    int W_sim = 0, H_sim = 0;
    double OCEAN_Y = 80.0;
    int N_REC = 57, N_OBS = 24;
    double sxShip = 370.0, sxSrc = 720.0;
    GeoLayout layout;
    std::vector<float> vsq;              // (v / 6000)^2 on the grid
    std::vector<float> rho;              // density on the grid, g/cm^3
    unsigned modelTex = 0;               // velocity image of an imported model (scene background)
    std::vector<float> sismo;            // last finished batch (display + SEG-Y)

    Mode mode = Mode::None, lastMode = Mode::None;
    double cycle_t = 0;
    bool paused = false, instability = false;
    int shotsMain = 0, shotsSrc = 0;
    std::vector<Shot> shots;
    double elapsedSec = 0;

    unsigned waveTex = 0;                // RGBA8 W_sim x H_sim
    bool gpuReady = false, userForceCPU = false;
    Badge badge;
    std::vector<ThreadStat> threadStats; // EMA of per-thread compute/wait (CPU bars)
    int cpuThreads() const { return cpu_.threadCount(); }
    bool usingGpu() const { return gpuReady; }

    bool eikonalBusy() const { return eikBusy_; }
    int eikonalProgress() const { return eikProgress_.load(); }

    // Tests
    CpuSolver& cpuSolver() { return cpu_; }
    GpuSolver& gpuSolver() { return gpu_; }

private:
    bool boxFor(double t_sec, EikBox& out) const;
    void regridNow();
    void rebuildEngines();           // CPU pool + GPU (N_REC / N_OBS / grid changed)
    void initGPU();
    void updateCpml();
    void computeObsPositions();
    void fillModel();                // vsq, rho, material and layout for the current geology and grid
    void pushMaterial();             // material + CPML to both engines
    void createModelTexture();
    void clearAllFields();           // P/V/psi on both engines + texture
    void clearSismoAll();
    void clearWaveTexture();
    void createWaveTexture();
    void waitInFlight(bool apply);
    void completeBatch();
    void loopIteration();
    void submitBatch(int steps);
    void fireShot(bool main, double delay);
    void startEikonal();
    void finishEikonal();
    void waitEikonal();
    void segySnapshotAndSave();
    void beginLineShot();
    void lineIteration();
    void finishLine(bool ok, const std::string& why);
    void checkInstabilityFromSismo();
    void setInstability();

    CpuSolver cpu_;
    GpuSolver gpu_;
    bool glAvailable_ = false, gpuCapable_ = false;
    std::string gpuRenderer_, gpuReason_;
    long long maxSsbo_ = 0;

    std::vector<float> bx_, ax_, bz_, az_;
    GridModel modelVel_, modelRho_;
    Material mat_;
    double vmaxNorm_ = 0.68, vminMs_ = 1500.0;   // model extremes (CPML damping, CFL / dispersion status)
    std::vector<float> obsX_, obsY_;
    double accumulated_ = 0;
    double simTime_ = 0;             // simulated seconds since the cycle start (expanding-box clock; survives dt changes)

    // In-flight batch
    struct InFlight {
        bool active = false, gpu = false;
        int steps = 0;
        double dtPhy = 0;
        double sxShip = 0, sxSrc = 0;
        BatchJob job;
    } inflight_;
    std::vector<StepData> stepData_;
    SourcePulse pulse_;              // injected source time function (rebuilt when wavelet / f change)
    EikBox batchBox_;
    int pendingSteps_ = 0;           // batch waiting for the eikonal solve

    // Eikonal
    Eikonal eik_;
    bool eikDirty_ = false, eikBusy_ = false;
    std::future<void> eikFuture_;
    std::atomic<int> eikProgress_{0};

    // Debounced regrid
    bool regridPending_ = false;
    double regridAt_ = 0;

    // SEG-Y
    bool segyRecording_ = false, segyShotSaved_ = false;
    std::string segyDir_;
    int segyShotNum_ = 0;
    std::string segyLabel_;
    double segyLabelRevertAt_ = -1;

    std::vector<uint32_t> zeroTex_;

    // Line simulation
    struct LineJob {
        bool active = false;
        int shot = 0, n = 0;            // shots finished, shots planned
        double firstX = 0, interval = 25;
        Mode mode = Mode::Trad;
        int phase = 0;                  // CLO: 0 main record, 1 source-vessel record
        double shotShipX = 0;           // ship x when the current record's shot fired
        std::vector<float> obsMain;     // CLO: OBS gather at the end of the main record
        double savedShipX = 0;
        std::string dir;
        SegyLineWriter streamer, streamerSrc, obs;
    } line_;
    bool appendLineGathers(bool srcVessel, std::string& err);

    // Skipping the idle part of a live shot cycle: the record is complete but the ship has not
    // yet sailed one shot interval. The wave field is cleared and the ship glides the rest of
    // the way in kGlideSec of real time; the cycle then continues at `target` (next shot, or
    // the CLO source vessel's shot) exactly where sailing in real time would have reached it.
    struct Glide {
        bool active = false;
        double t0 = 0, fromX = 0, toX = 0, target = 0;
        bool fireSrc = false;           // CLO: the source vessel fires at the end
    } glide_;
    double now_ = 0;
    bool glideStep();                   // true while gliding (no wave steps this frame)
    double cloSwitchStep(double mps) const;   // CLO: step at which the source vessel fires
};
