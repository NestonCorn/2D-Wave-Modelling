#include "GpuSolver.hpp"
#include <glad/glad.h>
#include <algorithm>
#include <cstring>
#include "Params.hpp"
#include "Shaders.hpp"

namespace {
enum Binding { B_P = 0, B_VX, B_VZ, B_VSQ, B_PSIVX, B_PSIVZ, B_PSIPX, B_PSIPZ, B_CPML, B_SISMO, B_STEPS, B_OBS };

struct ParamsUBO {   // std140, all scalars → tightly packed
    int W, H, boxX0, boxY0;
    int boxX1, boxY1, sjSurf, nRec;
    int nObs, freeSurf; float dt, scale;
    float cable, gain; int GH_, pad;
};
static_assert(sizeof(ParamsUBO) == 64, "UBO layout");

void zeroBuffer(unsigned b) {
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, nullptr);
}
} // namespace

GpuSolver::~GpuSolver() { destroy(); }

bool GpuSolver::queryCapabilities(std::string& renderer, std::string& reason, long long& maxSsbo) {
    const char* r = (const char*)glGetString(GL_RENDERER);
    renderer = r ? r : "GPU";
    // Trim vendor noise: "Intel(R) Graphics" stays short enough for the badge
    if (!GLAD_GL_VERSION_4_3) {
        reason = "Ekran kartı sürücüsü OpenGL 4.3 compute shader desteklemiyor.";
        return false;
    }
    GLint blocks = 0;
    glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &blocks);
    if (blocks < 12) {
        reason = "GPU compute shader başına yeterli storage buffer sunmuyor (" + std::to_string(blocks) + " < 12).";
        return false;
    }
    GLint64 sz = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &sz);
    maxSsbo = sz;
    return true;
}

void GpuSolver::destroy() {
    if (fence_) { glDeleteSync((GLsync)fence_); fence_ = nullptr; }
    for (auto& [o, p] : programs_)
        for (unsigned prog : {p.vel, p.pres, p.surf, p.src, p.sismo, p.pix})
            if (prog) glDeleteProgram(prog);
    programs_.clear();
    if (buf_[0]) glDeleteBuffers(12, buf_);
    std::memset(buf_, 0, sizeof(buf_));
    if (ubo_) glDeleteBuffers(1, &ubo_);
    if (readback_) glDeleteBuffers(1, &readback_);
    ubo_ = readback_ = 0;
    ready_ = false;
}

bool GpuSolver::init(int W, int H, int nRec, int nObs, std::string& err) {
    destroy();
    W_ = W; H_ = H; nRec_ = nRec; nObs_ = nObs;
    const size_t N = (size_t)W * H;
    sismoSize_ = (size_t)(nRec * 2 + nObs) * GH;
    sismo_.assign(sismoSize_, 0.0f);

    glGenBuffers(12, buf_);
    auto alloc = [&](int b, size_t bytes) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[b]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, std::max<size_t>(bytes, 16), nullptr, GL_DYNAMIC_DRAW);
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32F, GL_RED, GL_FLOAT, nullptr);
    };
    for (int b : {B_P, B_VX, B_VZ, B_VSQ, B_PSIVX, B_PSIVZ, B_PSIPX, B_PSIPZ}) alloc(b, N * 4);
    alloc(B_CPML, (size_t)(2 * W + 2 * H) * 4);
    alloc(B_SISMO, sismoSize_ * 4);
    alloc(B_STEPS, (size_t)MAX_STEPS * sizeof(StepData));
    alloc(B_OBS, (size_t)std::max(1, nObs) * 8);

    glGenBuffers(1, &ubo_);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ParamsUBO), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &readback_);
    glBindBuffer(GL_COPY_WRITE_BUFFER, readback_);
    glBufferData(GL_COPY_WRITE_BUFFER, (sismoSize_ + (size_t)W) * 4, nullptr, GL_STREAM_READ);   // sismo + probe row
    probe_.assign(W, 0.0f);
    probeValid_ = false;

    cpmlHost_.assign(2 * W + 2 * H, 0.0f);
    GLenum e = glGetError();
    if (e == GL_OUT_OF_MEMORY) { err = "GPU belleği yetersiz (GL_OUT_OF_MEMORY)."; destroy(); return false; }
    ready_ = true;
    return true;
}

unsigned GpuSolver::makeProgram(int order, const char* body, std::string& err) {
    std::string src = "#version 430\n#define FD_ORDER " + std::to_string(order) + "\n" + shaders::kHeader + body;
    const char* s = src.c_str();
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        err = std::string("Shader derleme hatası: ") + log;
        glDeleteShader(sh);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, sh);
    glLinkProgram(p);
    glDeleteShader(sh);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        err = std::string("Shader bağlama hatası: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

bool GpuSolver::ensurePrograms(int order, std::string& err) {
    if (programs_.count(order)) return true;
    Programs p;
    p.vel = makeProgram(order, shaders::kVelocity, err);
    p.pres = p.vel ? makeProgram(order, shaders::kPressure, err) : 0;
    p.surf = p.pres ? makeProgram(order, shaders::kFreeSurface, err) : 0;
    p.src = p.surf ? makeProgram(order, shaders::kSource, err) : 0;
    p.sismo = p.src ? makeProgram(order, shaders::kSismo, err) : 0;
    p.pix = p.sismo ? makeProgram(order, shaders::kPixels, err) : 0;
    if (!p.pix) {
        for (unsigned prog : {p.vel, p.pres, p.surf, p.src, p.sismo}) if (prog) glDeleteProgram(prog);
        return false;
    }
    unsigned progs[5] = {p.vel, p.pres, p.surf, p.src, p.sismo};
    for (int k = 0; k < 5; k++) p.locStep[k] = glGetUniformLocation(progs[k], "uStep");
    programs_[order] = p;
    return true;
}

void GpuSolver::setVsq(const std::vector<float>& vsq) {
    wait();
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_VSQ]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, vsq.size() * 4, vsq.data());
}

void GpuSolver::setCpml(const std::vector<float>& bx, const std::vector<float>& ax,
                        const std::vector<float>& bz, const std::vector<float>& az) {
    wait();
    std::copy(bx.begin(), bx.end(), cpmlHost_.begin());
    std::copy(ax.begin(), ax.end(), cpmlHost_.begin() + W_);
    std::copy(bz.begin(), bz.end(), cpmlHost_.begin() + 2 * W_);
    std::copy(az.begin(), az.end(), cpmlHost_.begin() + 2 * W_ + H_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_CPML]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, cpmlHost_.size() * 4, cpmlHost_.data());
}

void GpuSolver::setCpmlZ(const std::vector<float>& bz, const std::vector<float>& az) {
    wait();
    std::copy(bz.begin(), bz.end(), cpmlHost_.begin() + 2 * W_);
    std::copy(az.begin(), az.end(), cpmlHost_.begin() + 2 * W_ + H_);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_CPML]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, cpmlHost_.size() * 4, cpmlHost_.data());
}

void GpuSolver::setObs(const std::vector<float>& obsX, const std::vector<float>& obsY) {
    wait();
    std::vector<float> v(2 * std::max(1, nObs_), 0.0f);
    for (int i = 0; i < nObs_ && i < (int)obsX.size(); i++) { v[2 * i] = obsX[i]; v[2 * i + 1] = obsY[i]; }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_OBS]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, v.size() * 4, v.data());
}

void GpuSolver::clearFields() { wait(); for (int b : {B_P, B_VX, B_VZ}) zeroBuffer(buf_[b]); }
void GpuSolver::clearPsi() { wait(); for (int b : {B_PSIVX, B_PSIVZ, B_PSIPX, B_PSIPZ}) zeroBuffer(buf_[b]); }
void GpuSolver::clearSismo() {
    wait();
    zeroBuffer(buf_[B_SISMO]);
    std::fill(sismo_.begin(), sismo_.end(), 0.0f);
}

void GpuSolver::submit(const BatchJob& J, unsigned waveTex) {
    wait();
    const Programs& pr = programs_.at(J.fd_order);
    const int steps = std::min(J.steps, MAX_STEPS);

    ParamsUBO u{};
    u.W = W_; u.H = H_;
    u.boxX0 = J.bxmin; u.boxY0 = J.bymin; u.boxX1 = J.bxmax; u.boxY1 = J.bymax;
    u.sjSurf = J.sj_surf; u.nRec = nRec_; u.nObs = nObs_;
    u.freeSurf = J.freeSurface ? 1 : 0; u.dt = J.dt; u.scale = J.SCALE;
    u.cable = J.cable_len; u.gain = J.gainNorm; u.GH_ = GH;
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(u), &u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_STEPS]);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (size_t)steps * sizeof(StepData), J.stepData);
    for (int b = 0; b < 12; b++) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, b, buf_[b]);

    const int x0 = std::max(0, J.bxmin), y0 = std::max(0, J.bymin);
    const int boxW = std::max(0, J.bxmax - x0 + 1), boxH = std::max(0, J.bymax - y0 + 1);
    const GLuint wx = (boxW + 15) / 16, wy = (boxH + 15) / 16;
    const int surfH = std::max(0, std::min(J.bymax, J.sj_surf + 1) - y0);
    const GLuint wySurf = (surfH + 15) / 16;
    const GLuint wxRec = (nRec_ + nObs_ + 127) / 128;

    for (int s = 0; s < steps && wx > 0 && wy > 0; s++) {
        const StepData& sd = J.stepData[s];
        glUseProgram(pr.vel);  glUniform1i(pr.locStep[0], s); glDispatchCompute(wx, wy, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        glUseProgram(pr.pres); glUniform1i(pr.locStep[1], s); glDispatchCompute(wx, wy, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        if (J.freeSurface && wySurf > 0) {
            glUseProgram(pr.surf); glUniform1i(pr.locStep[2], s); glDispatchCompute(wx, wySurf, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if (sd.act0 > 0.5f || sd.act1 > 0.5f) {
            glUseProgram(pr.src); glUniform1i(pr.locStep[3], s); glDispatchCompute(1, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if (sd.rec_valid > 0.5f && wxRec > 0) {
            glUseProgram(pr.sismo); glUniform1i(pr.locStep[4], s); glDispatchCompute(wxRec, 1, 1);
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        }
        if ((s & 31) == 31) glFlush();   // keep command packets short (avoids driver TDR on big grids)
    }

    if (J.renderPixels && waveTex && wx > 0 && wy > 0) {
        glBindImageTexture(0, waveTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUseProgram(pr.pix);
        glDispatchCompute(wx, wy, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    }
    glUseProgram(0);

    // Async sismogram readback: copy now, read when the fence signals
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_COPY_READ_BUFFER, buf_[B_SISMO]);
    glBindBuffer(GL_COPY_WRITE_BUFFER, readback_);
    if (sismoSize_) glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, sismoSize_ * 4);
    probeValid_ = J.probeRow >= 0 && J.probeRow < H_;
    if (probeValid_) {
        glBindBuffer(GL_COPY_READ_BUFFER, buf_[B_P]);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)J.probeRow * W_ * 4,
                            (GLintptr)sismoSize_ * 4, (GLsizeiptr)W_ * 4);
    }
    fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

void GpuSolver::finishReadback() {
    glDeleteSync((GLsync)fence_);
    fence_ = nullptr;
    glBindBuffer(GL_COPY_WRITE_BUFFER, readback_);
    if (sismoSize_) glGetBufferSubData(GL_COPY_WRITE_BUFFER, 0, sismoSize_ * 4, sismo_.data());
    unstable_ = false;
    if (probeValid_) {
        glGetBufferSubData(GL_COPY_WRITE_BUFFER, (GLintptr)sismoSize_ * 4, (GLsizeiptr)W_ * 4, probe_.data());
        for (float v : probe_) {
            uint32_t bits; std::memcpy(&bits, &v, 4);
            if ((bits & 0x7f800000u) == 0x7f800000u || (bits & 0x7fffffffu) > 0x47c35000u) { unstable_ = true; break; }   // NaN/Inf or |v| > 1e5
        }
    }
}

bool GpuSolver::poll() {
    if (!fence_) return false;
    GLenum r = glClientWaitSync((GLsync)fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) { finishReadback(); return true; }
    return false;
}

void GpuSolver::wait() {
    if (!fence_) return;
    while (true) {
        GLenum r = glClientWaitSync((GLsync)fence_, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000);
        if (r != GL_TIMEOUT_EXPIRED) break;
    }
    finishReadback();
}

void GpuSolver::readP(std::vector<float>& out) {
    wait();
    out.resize((size_t)W_ * H_);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf_[B_P]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, out.size() * 4, out.data());
}
