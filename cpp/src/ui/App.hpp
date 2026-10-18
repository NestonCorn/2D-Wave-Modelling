#pragma once
#include <string>
#include "Ui.hpp"

namespace ui {

// The whole window, top to bottom, mirroring SLO_CLO_Interactive.html.
class App {
public:
    explicit App(Simulation& sim) : sim_(sim) {}
    void frame(double now);

private:
    float header();                 // returns its height
    void toolbar(double now);
    void tabs(double now);
    void tabPhysics(double now);
    void tabGeometry();
    void tabGeology(double now);
    void tabRecord();
    void cpuPanel();
    void legendBar();
    void statusBar();
    void canvasSection();
    void gatherSection();
    void footerLegend();
    void eikonalPopup();
    void alertsModal();
    void applyDeferred();

    void alert(const std::string& msg) { sim_.alerts.push_back(msg); }
    void saveLocal();
    void loadLocal();
    void saveFile();
    void loadFile();
    void segyButton(double now);

    Simulation& sim_;
    GatherView gv_;
    int overlay_ = 0;
    // Heavy handlers are applied when the slider is released (the HTML respawned
    // every worker on each input event)
    bool pendingObs_ = false, pendingNpml_ = false;
};

} // namespace ui
