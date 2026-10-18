// Advanced Geophysical Core – FDTD Solver (native C++ port of SLO_CLO_Interactive.html)
#ifdef _WIN32
#include <windows.h>   // before glad, so APIENTRY is defined once
#endif
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <cstdio>
#include <filesystem>
#include "core/Simulation.hpp"
#include "ui/App.hpp"

static void fatal(const char* msg) {
#ifdef _WIN32
    MessageBoxA(nullptr, msg, "SeismicFDTD", MB_ICONERROR);
#else
    std::fprintf(stderr, "%s\n", msg);
#endif
}

int main(int argc, char** argv) {
    // Run next to the executable so assets/ is found
    std::error_code ec;
    if (argc > 0) std::filesystem::current_path(std::filesystem::path(argv[0]).parent_path(), ec);

    if (!glfwInit()) { fatal("GLFW başlatılamadı."); return 1; }

    // Prefer OpenGL 4.3 core (compute shaders); fall back to 3.3 for CPU-only rendering
    bool gl43 = true;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* win = glfwCreateWindow(1500, 1000, "Advanced Geophysical Core – FDTD Solver", nullptr, nullptr);
    if (!win) {
        gl43 = false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        win = glfwCreateWindow(1500, 1000, "Advanced Geophysical Core – FDTD Solver", nullptr, nullptr);
    }
    if (!win) { fatal("OpenGL 3.3 penceresi oluşturulamadı. Ekran kartı sürücüsünü güncelleyin."); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) { fatal("OpenGL fonksiyonları yüklenemedi."); return 1; }
    gl43 = gl43 && GLAD_GL_VERSION_4_3;

    float sx = 1, sy = 1;
    glfwGetWindowContentScale(win, &sx, &sy);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    ui::loadFonts(sx);
    ui::applyTheme(sx);

    {
        Simulation sim;
        sim.init(true);
        ui::App app(sim);

        double last = glfwGetTime();
        while (!glfwWindowShouldClose(win)) {
            glfwPollEvents();
            if (glfwGetWindowAttrib(win, GLFW_ICONIFIED)) {
                // Keep the physics running while minimised (setTimeout path in the HTML)
                double now = glfwGetTime();
                sim.update(now, now - last);
                last = now;
                glfwWaitEventsTimeout(0.016);
                continue;
            }
            const double now = glfwGetTime();
            sim.update(now, now - last);
            last = now;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            app.frame(now);
            ImGui::Render();

            int fbw, fbh;
            glfwGetFramebufferSize(win, &fbw, &fbh);
            glViewport(0, 0, fbw, fbh);
            glClearColor(1.0f / 255, 4.0f / 255, 8.0f / 255, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(win);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    (void)gl43;
    return 0;
}
