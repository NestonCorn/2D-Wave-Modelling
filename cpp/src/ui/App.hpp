#pragma once
#include <string>
#include <vector>
#include <array>
#include "Ui.hpp"
#include "core/ImageModel.hpp"
#include "ModelEditor.hpp"

namespace ui {

// The application window as a workstation: title bar, command bar, an icon rail of parameter
// groups with a panel showing one group, view tabs (model / recordings / side by side), and a
// status bar. Nothing scrolls except the parameter panel.
class App {
public:
    explicit App(Simulation& sim) : sim_(sim) {}
    void frame(double now);

private:
    // Regions
    void titleBar(ImVec2 o, ImVec2 size);
    void commandBar(ImVec2 o, ImVec2 size);
    void iconRail(ImVec2 o, ImVec2 size);
    void sidePanel(ImVec2 o, ImVec2 size, double now);
    void viewTabs(ImVec2 o, ImVec2 size);
    void views(ImVec2 o, ImVec2 size);
    void scenePane(ImVec2 o, ImVec2 size);
    void gatherPanes(ImVec2 o, ImVec2 size);
    void statusBar(ImVec2 o, ImVec2 size);

    // Parameter panel sections
    void sectionSource();
    void sectionMedium(double now);
    void sectionNumerics(double now);
    void sectionGeometry();
    void sectionObs();
    void sectionView();
    void sectionLine();
    void sectionFiles();
    void sectionCpu();

    // Overlays
    void instabilityCard(ImVec2 o, ImVec2 size);
    void toasts(ImVec2 bottomRight);
    void modelImportPopup();
    void alertsModal();
    void keyboard();
    void applyDeferred();

    // Actions
    void notify(const std::string& msg);                       // brief confirmation (toast)
    void alert(const std::string& msg) { sim_.alerts.push_back(msg); }   // problem to read (modal)
    void pickModelFile(ModelQuantity q);
    void pickImageModel();
    void sectionImageModel();
    void toggleSegy();
    void saveLocal();
    void loadLocal();
    void saveFile();
    void loadFile();
    void startOrPause();

    Simulation& sim_;
    GatherView gv_;
    double now_ = 0;
    float sideW_ = 360.0f;          // parameter panel width (logical px), rail excluded
    int group_ = 0;                 // parameter group shown in the panel
    bool panelOpen_ = true;         // clicking the selected rail icon hides the panel
    int view_ = 2;                  // 0 model, 1 recordings, 2 side by side
    float split_ = 0.56f;           // scene share of the view height
    int selMode_ = 0;               // 0 traditional, 1 SLO, 2 CLO (what Start runs)
    bool pendingObs_ = false, pendingNpml_ = false;

    struct Toast {
        std::string text;
        double t0;
    };
    std::vector<Toast> toasts_;
    double alertOpenedAt_ = -1, importOpenedAt_ = -1;

    // Model file being imported (popup)
    struct ModelImportState {
        bool open = false;
        ModelQuantity q = ModelQuantity::Velocity;
        std::string path, name;
        int format = 0;            // 0 SEG-Y, 1 raw float32, 2 text grid
        int nx = 0, nz = 0;        // raw: entered; SEG-Y / text: read from the file
        float dx = 10, dz = 10;
        int order = 0;             // raw: 0 z fastest (column order), 1 x fastest (row order)
        bool bigEndian = false;
        std::string probeError;
    } imp_;

    // Velocity model drawn as an image (gray level -> velocity)
    struct ImageModelState {
        bool loaded = false;
        std::string name;
        GrayImage img;
        std::array<float, 256> vel{};
        float widthM = 1200, depthM = 400;
    } imgModel_;
    ModelEditor editor_;   // drawing window for a model

    // Line simulation settings
    int lineShots_ = 10;
};

} // namespace ui
