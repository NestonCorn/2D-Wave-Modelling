#pragma once
#include <functional>
#include <string>
#include <vector>
#include "Ui.hpp"
#include "core/ModelCanvas.hpp"

namespace ui {

// Drawing window for a velocity model: layer boundaries, lines, brush and fill on a raster
// whose colours are layers with a velocity each. The drawing stays between openings.
class ModelEditor {
public:
    void open(Simulation& sim, double now);
    // Call every frame (draws the modal window while it is open)
    void frame(Simulation& sim, double now, const std::function<void(const std::string&)>& notify,
               const std::function<void(const std::string&)>& alert);

private:
    enum Tool { Boundary = 0, Line = 1, Brush = 2, Bucket = 3 };

    void tools(Simulation& sim, const std::function<void(const std::string&)>& notify, const std::function<void(const std::string&)>& alert);
    void canvasPane(ImVec2 size);
    void finishPoints();
    void resizeToModel();
    void upload();

    ModelCanvas cv_;
    bool init_ = false, wantOpen_ = false, close_ = false;
    double openedAt_ = -1;
    float widthM_ = 1200, depthM_ = 400;
    int tool_ = Boundary, sel_ = 1;
    float radius_ = 3;
    std::vector<ModelCanvas::Pt> pts_;
    bool painting_ = false;
    float lastX_ = 0, lastY_ = 0;
    unsigned tex_ = 0;
    int texW_ = 0, texH_ = 0;
    bool dirty_ = true;
    std::vector<uint32_t> rgba_;
    std::string hover_;
};

} // namespace ui
