#include "ui/App.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include "core/ColorManagement.h"
#include "core/Guide.h"
#include "core/Version.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/io/IONodes.h"
#include "nodes/matte/MatteNodes.h"
#include "nodes/transform/TransformNodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/ColorDisplay.h"
#include "ui/Eyedropper.h"
#include "ui/FileDialog.h"
#include "ui/GuideWindow.h"
#include "ui/Inspector.h"
#include "ui/UiScript.h"

namespace fs = std::filesystem;

static const char* kProjectFilter = "NodeLab project (*.nlproj)|*.nlproj|All files|*.*";

static bool isImageFile(const std::filesystem::path& p) { return isImageFile(pathToU8(p)); }
static const char* kDockName = "NodeLabDockSpace";

void dropCallback(GLFWwindow* w, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    for (int i = 0; i < count; ++i) app->drops_.emplace_back(paths[i]);
}

void closeCallback(GLFWwindow* w) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    glfwSetWindowShouldClose(w, GLFW_FALSE);
    app->requestAction(App::Pending::Quit);
}

// Per-user settings folder (%APPDATA%\NodeLab), created on demand.
static fs::path settingsDir() {
#ifdef _WIN32
    if (const wchar_t* appdata = _wgetenv(L"APPDATA")) {
        fs::path p = fs::path(appdata) / "NodeLab";
        std::error_code ec;
        fs::create_directories(p, ec);
        return p;
    }
#endif
    return fs::current_path();
}

App::App() = default;
App::~App() = default;

// ---------------------------------------------------------------- main loop

int App::run(const RunOptions& opt) {
    UiScript script;
    if (!opt.script.empty()) {
        std::string err;
        if (!script.load(opt.script, err)) {
            std::fprintf(stderr, "script: %s\n", err.c_str());
            return 1;
        }
    }
    automated_ = !opt.screenshot.empty() || script.active();
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Automated runs use a fixed size and never take focus from whatever the user is doing.
    glfwWindowHint(GLFW_MAXIMIZED, automated_ ? GLFW_FALSE : GLFW_TRUE);
    if (automated_) {
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }
    window_ = glfwCreateWindow(1600, 900, "NodeLab", nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "failed to create window (OpenGL 3.0 required)\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    glfwSetWindowUserPointer(window_, this);
    glfwSetDropCallback(window_, dropCallback);
    glfwSetWindowCloseCallback(window_, closeCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Panels can be dragged out of the main window into their own OS windows (not in test runs,
    // which need a deterministic single-window frame).
    if (!automated_) io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    // Layout persists per user; automated runs always start from the default layout.
    iniPath_ = pathToU8(settingsDir() / "layout.ini");
    bool haveLayout = false;
    if (automated_) {
        io.IniFilename = nullptr;
    } else {
        io.IniFilename = iniPath_.c_str();
        haveLayout = fs::exists(u8ToPath(iniPath_));
    }
    resetLayout_ = !haveLayout;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    const float dpi = std::max(1.0f, xscale);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 3.0f;
    style.ScaleAllSizes(dpi);
    style.Colors[ImGuiCol_WindowBg].w = 1.0f;  // opaque, also for floating viewports

    const char* uiFont = "C:/Windows/Fonts/segoeui.ttf";
    if (fs::exists(uiFont)) io.Fonts->AddFontFromFileTTF(uiFont, 17.0f * dpi);
    else io.FontGlobalScale = dpi;
    // Extra faces for the guide: bold, headings and code. Glyphs cover the guide's own text.
    {
        static ImVector<ImWchar> ranges;
        ImFontGlyphRangesBuilder rb;
        rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
        const std::string_view guide = guideMarkdown();
        rb.AddText(guide.data(), guide.data() + guide.size());
        rb.BuildRanges(&ranges);
        auto load = [&](const char* file, float size) -> ImFont* {
            return fs::exists(file) ? io.Fonts->AddFontFromFileTTF(file, size * dpi, nullptr, ranges.Data) : nullptr;
        };
        GuideFonts gf;
        gf.bold = load("C:/Windows/Fonts/segoeuib.ttf", 17.0f);
        gf.h1 = load("C:/Windows/Fonts/segoeuib.ttf", 30.0f);
        gf.h2 = load("C:/Windows/Fonts/segoeuib.ttf", 24.0f);
        gf.h3 = load("C:/Windows/Fonts/segoeuib.ttf", 20.0f);
        gf.code = load("C:/Windows/Fonts/consola.ttf", 16.0f);
        setGuideFonts(gf);
    }

    // While a script runs, OS input is not forwarded to ImGui so the real mouse can't interfere.
    ImGui_ImplGlfw_InitForOpenGL(window_, !script.active());
    ImGui_ImplOpenGL3_Init("#version 130");

    eval_ = std::make_unique<AsyncEvaluator>(cache_);
    auto main = std::make_unique<Viewer>();
    main->id = 0;
    viewers_.push_back(std::move(main));

    if (opt.project.empty() || !openProject(opt.project)) newProject();

    int frame = 0, settled = 0;
    while (!quit_) {
        glfwWaitEventsTimeout(eval_->busy() || evalDirty_ || automated_ ? 0.01 : 0.05);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        std::string shotPath;
        if (script.active()) shotPath = script.step(!eval_->busy() && !evalDirty_, quit_);
        ImGui::NewFrame();
        drawFrame();
        ImGui::Render();

        int fbw, fbh;
        glfwGetFramebufferSize(window_, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!opt.screenshot.empty() && !script.active()) {
            // Plain --screenshot: wait for evaluation to settle and layout to stabilize.
            ++frame;
            settled = (!eval_->busy() && !evalDirty_) ? settled + 1 : 0;
            if ((frame > 30 && settled > 10) || frame > 600) {
                shotPath = opt.screenshot;
                quit_ = true;
            }
        }
        if (!shotPath.empty() && !saveFramebuffer(shotPath)) std::fprintf(stderr, "screenshot failed\n");

        // Floating panels live in their own OS windows.
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }
        glfwSwapBuffers(window_);
    }

    eval_.reset();
    leftTex_.reset();
    viewers_.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

void App::drawFrame() {
    handleDrops();
    handleShortcuts();

    drawMainMenu();
    drawStatusBar();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dockId = ImHashStr(kDockName);
    if (resetLayout_) {
        buildDefaultLayout(dockId);
        resetLayout_ = false;
    }
    ImGui::DockSpaceOverViewport(dockId, vp);

    if (showOriginal_) drawOriginalWindow();
    if (showEditor_) drawEditorWindow();
    if (showInspector_) drawInspectorWindow();
    if (showResult_) drawViewerWindow(*viewers_[0], true);
    for (size_t i = 1; i < viewers_.size(); ++i) drawViewerWindow(*viewers_[i], false);
    drawGuideWindow();
    drawExportWindow();
    pollExport();
    std::erase_if(viewers_, [](const std::unique_ptr<Viewer>& v) { return v->id != 0 && !v->open; });

    drawUnsavedModal();
    drawConvertModal();

    // The selected node's on-image controls decide two extra things about the evaluation: a
    // selected Crop shows the whole frame (its rectangle is drawn instead, like Lightroom's crop
    // tool), and a selected mask is evaluated too, for the tinted mask overlay.
    Node* ov = overlayNode();
    NodePath ovPath;
    if (ov) ovPath = groupPath_, ovPath.push_back(ov->id);
    const bool wantMask = ov && NodeOverlay::isMask(*ov) && maskOverlay_;
    if (ovPath != overlayPath_ || wantMask != maskWanted_) {
        overlayPath_ = ovPath;
        maskWanted_ = wantMask;
        maskTex_.reset();
        evalDirty_ = true;
    }

    // Kick evaluation after the UI had a chance to change the graph this frame.
    const bool gesture = ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) || editor_.interacting();
    if (evalDirty_) {
        submittedTargets_.clear();
        std::vector<int> pins;
        for (auto& v : viewers_) {
            const bool followsPreview = v->id == 0 || v->pin.empty();
            submittedTargets_.push_back(followsPreview ? resultTarget() : v->pin);
            pins.push_back(followsPreview && pathValid(previewPath_) ? previewPin_ : 0);
        }
        submittedViewers_ = viewers_.size();
        if (maskWanted_) {
            submittedTargets_.push_back(overlayPath_);
            pins.push_back(0);
        }
        nlohmann::json gj;
        if (ov && ov->info().type == crop::kType) {
            const std::vector<nlohmann::json> saved = ov->params;
            ov->params[crop::Left] = 0.0f, ov->params[crop::Right] = 1.0f;
            ov->params[crop::Top] = 0.0f, ov->params[crop::Bottom] = 1.0f;
            ov->params[crop::Aspect] = 0;
            gj = graph_.toJson();
            ov->params = saved;
        } else {
            gj = graph_.toJson();
        }
        eval_->submit(std::move(gj), submittedTargets_, pins, !gesture);
        evalDirty_ = false;
    }
    // A drag just ended: its last value is queued behind an intermediate one; skip the latter.
    if (gestureWas_ && !gesture) eval_->preempt();
    gestureWas_ = gesture;
    updateTextures();
    updateTitle();

    // Snapshot for undo once the current gesture (drag, slider, text entry) has finished, so one
    // drag becomes one undo step.
    if (historyDirty_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !editor_.interacting()) {
        commitHistory();
        historyDirty_ = false;
    }
}

// ---------------------------------------------------------------- layout & panels

void App::buildDefaultLayout(unsigned dockIdU) {
    const ImGuiID dockId = dockIdU;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
    ImGuiID center = dockId;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.27f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.37f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Original", left);
    ImGui::DockBuilderDockWindow("###Result", right);
    ImGui::DockBuilderDockWindow("###NodeEditor", center);
    ImGui::DockBuilderDockWindow("###Inspector", bottom);
    for (size_t i = 1; i < viewers_.size(); ++i)
        ImGui::DockBuilderDockWindow(("###Viewer" + std::to_string(viewers_[i]->id)).c_str(), right);
    ImGui::DockBuilderFinish(dockId);
    showOriginal_ = showEditor_ = showInspector_ = showResult_ = true;
}

static constexpr ImGuiWindowFlags kCanvasFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

void App::drawOriginalWindow() {
    if (ImGui::Begin("Original###Original", &showOriginal_, kCanvasFlags)) {
        PickRequest pick{leftShown_.get()};
        drawImageView("##leftview", leftTex_, view_, "Drop an image here or use File > Import Image",
                      eyedropper().active() ? &pick : nullptr);
        finishPick(pick);
    }
    ImGui::End();
}

void App::drawEditorWindow() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    const bool visible = ImGui::Begin("Node Editor###NodeEditor", &showEditor_, kCanvasFlags);
    ImGui::PopStyleVar();
    if (visible) {
        // Breadcrumb: Root > Group > ...; click a level to go back up.
        if (!groupPath_.empty()) {
            if (ImGui::SmallButton("Root")) setGroupPath({});
            Graph* g = &graph_;
            for (size_t i = 0; i < groupPath_.size() && g; ++i) {
                auto* grp = dynamic_cast<GroupNode*>(g->find(groupPath_[i]));
                if (!grp) break;
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
                ImGui::PushID(int(i));
                if (ImGui::SmallButton(grp->name.c_str()) && i + 1 < groupPath_.size())
                    setGroupPath(std::vector<int>(groupPath_.begin(), groupPath_.begin() + i + 1));
                ImGui::PopID();
                g = &grp->inner();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("  (Tab to exit group)");
        }

        Graph& g = currentGraph();
        // The editor works on ids within the current graph; map the preview path in and out.
        int preview = 0;
        if (previewPath_.size() == groupPath_.size() + 1 &&
            std::equal(groupPath_.begin(), groupPath_.end(), previewPath_.begin()))
            preview = previewPath_.back();
        const int previewBefore = preview;
        // Timings are for the top-level graph; ids inside a group mean different nodes.
        editor_.setTimings(groupPath_.empty() ? nodeMs_ : std::unordered_map<int, double>{});
        NodeEditor::Result r = editor_.draw(g, selected_, preview, previewPin_);
        if (preview != previewBefore || r.previewChanged) {
            previewPath_.clear();
            if (preview) {
                previewPath_ = groupPath_;
                previewPath_.push_back(preview);
            }
            evalDirty_ = true;
        }
        if (r.evalChanged) evalDirty_ = true;
        if (r.docChanged) {
            modified_ = true;
            historyDirty_ = true;
        }
        if (r.openViewer) {
            NodePath pin = groupPath_;
            pin.push_back(r.openViewer);
            openViewer(std::move(pin));
        }
        if (r.enterGroup) enterGroup(r.enterGroup);
        else if (r.exitGroup) exitGroup();
    }
    ImGui::End();
}

void App::drawInspectorWindow() {
    if (ImGui::Begin("Inspector###Inspector", &showInspector_)) {
        Graph& g = currentGraph();
        Graph* parent = groupPath_.empty()
                            ? nullptr
                            : resolveGroupPath(graph_, std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
        if (drawInspector(g, selected_, currentGroupOwner(), parent)) markChanged(true);
    }
    ImGui::End();
}

void App::drawViewerWindow(Viewer& v, bool isMain) {
    std::string title;
    if (isMain) title = "Result###Result";
    else title = "Viewer " + std::to_string(v.id) + "###Viewer" + std::to_string(v.id);
    bool* open = isMain ? &showResult_ : &v.open;
    if (!isMain) {
        // New viewers float in the middle of the window (cascaded) until docked somewhere.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float step = 30.0f * float((v.id - 1) % 6);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f + step, vp->WorkPos.y + vp->WorkSize.y * 0.45f + step),
                                ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
    }
    if (ImGui::Begin(title.c_str(), open, kCanvasFlags)) {
        // Toolbar: what is shown, and pin controls for extra viewers.
        NodePath shown = isMain || v.pin.empty() ? resultTarget() : v.pin;
        std::string label = shown.empty() ? "(nothing)" : pathLabel(shown);
        if (isMain && !previewPath_.empty()) label = "Preview: " + label + "  (Ctrl+click it again to clear)";
        if (!isMain) {
            // Any node of the graph being edited, left to right, so a chain a -> b -> c reads in order.
            ImGui::SetNextItemWidth(std::min(240.0f, ImGui::GetContentRegionAvail().x * 0.5f));
            const std::string current = v.pin.empty() ? "Follow Result" : pathLabel(v.pin);
            if (ImGui::BeginCombo("##node", current.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Follow Result", v.pin.empty())) {
                    v.pin.clear();
                    evalDirty_ = true;
                }
                ImGui::Separator();
                std::vector<const Node*> nodes;
                for (const auto& [id, n] : currentGraph().nodes()) nodes.push_back(n.get());
                std::sort(nodes.begin(), nodes.end(), [](const Node* a, const Node* b) {
                    return a->x != b->x ? a->x < b->x : a->y < b->y;
                });
                for (const Node* n : nodes) {
                    NodePath p = groupPath_;
                    p.push_back(n->id);
                    ImGui::PushID(n->id);
                    if (ImGui::Selectable(n->title().c_str(), v.pin == p)) {
                        v.pin = std::move(p);
                        evalDirty_ = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Node shown in this viewer");
            ImGui::SameLine();
            if (ImGui::SmallButton("Pin Selected") && selected_) {
                v.pin = groupPath_;
                v.pin.push_back(selected_);
                evalDirty_ = true;
            }
            ImGui::SameLine();
            ImGui::Checkbox("Sync view", &v.sync);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom and pan together with Original and Result");
        }
        Node* ov = isMain ? overlayNode() : nullptr;
        if (isMain) {
            drawResultToolbar(ov);
            if (!previewPath_.empty()) {
                // Only worth mentioning when showing something other than the Output node.
                ImGui::SameLine();
                ImGui::TextDisabled("%s", label.c_str());
            }
        }
        const char* emptyMsg = !v.error.empty() ? v.error.c_str()
                               : shown.empty() ? "Add an Output node (right-click the canvas)"
                                               : "No output yet - connect this node's inputs";
        PickRequest pick{v.shown.get()};
        overlay_.set(ov, maskWanted_ && maskTex_.valid() ? &maskTex_ : nullptr);
        const ImVec2 viewMin = ImGui::GetCursorScreenPos();
        drawImageView(isMain ? "##result" : "##viewer", v.tex, isMain || v.sync ? view_ : v.view, emptyMsg,
                      eyedropper().active() ? &pick : nullptr, ov ? &overlay_ : nullptr);
        finishPick(pick);
        if (ov && overlay_.takeChanged()) markChanged(true);
        if (isMain && showHistogram_ && histogram_.valid) {
            // Top-right corner of the view, like Lightroom's histogram panel.
            const ImVec2 viewMax = ImGui::GetItemRectMax();
            const ImVec2 size(std::min(256.0f, viewMax.x - viewMin.x - 16.0f), 110.0f);
            if (size.x > 60.0f && viewMax.y - viewMin.y > size.y + 16.0f &&
                drawHistogram(ImGui::GetWindowDrawList(), ImVec2(viewMax.x - size.x - 8.0f, viewMin.y + 8.0f), size,
                              histogram_, clipping_)) {
                clipping_ = !clipping_;
                if (v.display) v.tex.upload(*v.display, clipping_);
            }
        }
    }
    ImGui::End();
}

Node* App::overlayNode() {
    Node* n = currentGraph().find(selected_);
    return n && NodeOverlay::supports(*n) ? n : nullptr;
}

void App::drawResultToolbar(Node* ov) {
    Viewer& v = *viewers_[0];
    // Hotkeys while the pointer is over the Result viewer (J and O as in Lightroom).
    const bool hover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !ImGui::GetIO().WantTextInput &&
                       !ImGui::GetIO().KeyCtrl;
    bool clipToggled = false;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_J, false)) clipping_ = !clipping_, clipToggled = true;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_O, false)) maskOverlay_ = !maskOverlay_;
    if (hover && ImGui::IsKeyPressed(ImGuiKey_H, false)) showHistogram_ = !showHistogram_;

    if (ImGui::Checkbox("Histogram", &showHistogram_) && showHistogram_ && v.display) histogram_.compute(*v.display);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show the histogram (H)");
    ImGui::SameLine();
    clipToggled |= ImGui::Checkbox("Clipping", &clipping_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show clipped highlights in red and crushed shadows in blue (J)");
    if (clipToggled && v.display) v.tex.upload(*v.display, clipping_);
    if (showHistogram_ && !histogram_.valid && v.display) histogram_.compute(*v.display);
    if (ov && NodeOverlay::isMask(*ov)) {
        ImGui::SameLine();
        ImGui::Checkbox("Mask Overlay", &maskOverlay_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tint the selected mask over the image (O)");
    }
    if (ov) {
        ImGui::SameLine();
        const char* hint = ov->info().type == crop::kType
                               ? "Drag the frame or its handles; drag outside to straighten"
                           : dynamic_cast<BrushMaskNode*>(ov) ? "Paint to add, Alt+paint to erase, [ ] brush size"
                                                              : "Drag the handles to shape the mask";
        ImGui::TextDisabled("%s", hint);
    }
}

void App::drawStatusBar() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)) {
        if (ImGui::BeginMenuBar()) {
            const Viewer& main = *viewers_[0];
            std::string st;
            if (main.tex.valid())
                st = std::to_string(main.tex.width()) + " x " + std::to_string(main.tex.height()) + " preview  |  " +
                     std::to_string(int(evalMs_)) + " ms";
            if (eval_->busy()) st += "  |  evaluating...";
            if (exporter_.busy()) {
                const Exporter::Progress pr = exporter_.progress();
                st += "  |  Export " + std::to_string(pr.done) + "/" + std::to_string(pr.total) + ": " + pr.stage;
            }
            if (!main.error.empty()) st += "  |  " + main.error;
            if (!status_.empty()) st += "  |  " + status_;
            if (eyedropper().active()) {
                const Node* n = currentGraph().find(eyedropper().node);
                st = "Eyedropper: click a pixel or drag a rectangle on an image";
                if (n && eyedropper().param < int(n->info().params.size()))
                    st += " for " + n->title() + " > " + n->info().params[eyedropper().param].name;
                st += "   (right-click or Esc cancels)";
            }
            ImGui::TextDisabled("%s", st.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- menus & shortcuts

// Blender's Render Properties > Color Management. The view settings only change how the viewers
// and exports show the result, so they don't re-evaluate the graph.
void App::drawColorMenu() {
    if (!ImGui::BeginMenu("Color")) return;
    ColorManagement& cm = graph_.colorManagement;
    ImGui::TextDisabled(cm.linear ? "Working space: Scene-Linear (Rec.709)" : "Working space: Legacy (sRGB-encoded)");
    ImGui::Separator();
    ImGui::BeginDisabled(!cm.linear);
    bool changed = false;
    ImGui::TextUnformatted("View Transform");
    for (int i = 0; i < 3; ++i)
        if (ImGui::RadioButton(colormgmt::kViewNames[i], &cm.view, i)) changed = true;
    ImGui::BeginDisabled(cm.view != ColorManagement::AgX);
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::Combo("Look", &cm.look, colormgmt::kLookNames, 3);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::DragFloat("Exposure", &cm.exposure, 0.01f, -10.0f, 10.0f, "%.2f");
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) cm.exposure = 0.0f, changed = true;
    ImGui::SetNextItemWidth(160);
    changed |= ImGui::DragFloat("Gamma", &cm.gamma, 0.005f, 0.01f, 5.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) cm.gamma = 1.0f, changed = true;
    ImGui::EndDisabled();
    if (changed) markChanged(false);
    ImGui::Separator();
    if (ImGui::MenuItem("Convert Project to Scene-Linear...", nullptr, false, !cm.linear))
        convertPrompt_ = true;
    ImGui::EndMenu();
}

void App::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) requestAction(Pending::New);
        if (ImGui::MenuItem("Open Project...", "Ctrl+O")) requestAction(Pending::Open);
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) saveProject(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Import Image...", "Ctrl+I"))
            if (auto p = openFileDialog("Import image", kImageFileFilter)) importImage(*p);
        if (ImGui::MenuItem("Export...", "Ctrl+E")) openExportWindow();
        if (ImGui::MenuItem("Write File Outputs", nullptr, false, !exporter_.busy())) startExport({{"", ""}}, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) requestAction(Pending::Quit);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        Graph& g = currentGraph();
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editor_.hasSelection()) && editor_.duplicateSelection(g))
            markChanged(true);
        int preview = 0;
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, editor_.hasSelection())) editor_.copySelection(g);
        if (ImGui::MenuItem("Paste", "Ctrl+V") && editor_.paste(g)) markChanged(true);
        if (ImGui::MenuItem("Delete (reconnect)", "Del / X", false, editor_.hasSelection()) && editor_.deleteSelection(g, preview, true))
            markChanged(true);
        if (ImGui::MenuItem("Delete", "Alt+Del", false, editor_.hasSelection()) && editor_.deleteSelection(g, preview, false))
            markChanged(true);
        if (ImGui::MenuItem("Mute", "M", false, editor_.hasSelection()) && editor_.toggleMute(g)) markChanged(true);
        if (ImGui::MenuItem("Collapse", "H", false, editor_.hasSelection()) && editor_.toggleCollapse(g)) markChanged(false);
        if (ImGui::MenuItem("Make Links", "F", false, editor_.hasSelection()) && editor_.makeLinks(g)) markChanged(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Group Selected", "Ctrl+G", false, editor_.hasSelection()) && editor_.groupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Ungroup", "Ctrl+Alt+G", false, editor_.selectedGroup(g) != 0) && editor_.ungroupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Edit Group", "Tab", false, editor_.selectedGroup(g) != 0)) enterGroup(editor_.selectedGroup(g));
        if (ImGui::MenuItem("Exit Group", "Tab", false, !groupPath_.empty())) exitGroup();
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "Ctrl+J") && editor_.frameSelection(g)) markChanged(false);
        if (ImGui::MenuItem("Remove from Frame", "Alt+P") && editor_.moveSelectionToFrame(g, 0)) markChanged(false);
        if (ImGui::MenuItem("Arrange Nodes", "Shift+P") && editor_.arrange(g)) markChanged(false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Original", nullptr, &showOriginal_);
        ImGui::MenuItem("Result", nullptr, &showResult_);
        ImGui::MenuItem("Node Editor", nullptr, &showEditor_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        if (ImGui::MenuItem("New Viewer")) {
            NodePath pin;
            if (selected_) {
                pin = groupPath_;
                pin.push_back(selected_);
            }
            openViewer(std::move(pin));
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) resetLayout_ = true;
        ImGui::Separator();
        ImGui::MenuItem("Node Timings", nullptr, &editor_.showTimings);
        if (ImGui::MenuItem("Frame All Nodes", "Home")) editor_.frameAll();
        if (ImGui::MenuItem("Reset Image Zoom", "double-click image")) view_.reset();
        if (ImGui::MenuItem("Clear Node Preview", nullptr, false, !previewPath_.empty())) {
            previewPath_.clear();
            evalDirty_ = true;
        }
        ImGui::EndMenu();
    }
    drawColorMenu();
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("NodeLab %s - node-based image manipulation", versionString().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Guide", "F1")) openGuide();
        if (const Node* n = currentGraph().find(selected_))
            if (ImGui::MenuItem(("Guide: " + n->info().displayName).c_str())) openGuide(n->info().displayName);
        ImGui::Separator();
        ImGui::TextUnformatted("Right-click canvas or Shift+A: add node   Drag pin to empty space: add connected node");
        ImGui::TextUnformatted("Drag empty space: pan   Wheel: zoom   Shift+drag: box select   Home / . : frame all / selected");
        ImGui::TextUnformatted("Ctrl+click node: preview (Ctrl+Shift+click: next output)   Del / X: delete and reconnect");
        ImGui::TextUnformatted("Ctrl+C / Ctrl+V   Ctrl+D / Shift+D: duplicate (and move)   G: move   H: collapse   M: mute");
        ImGui::TextUnformatted("F: make links   L / Shift+L: select upstream / downstream   F2: rename   Alt+drag: pull out");
        ImGui::TextUnformatted("Ctrl+right-drag: cut wires   Shift+right-drag: add reroutes");
        ImGui::TextUnformatted("Ctrl+G: group   Ctrl+Alt+G: ungroup   Tab: enter / exit group   Ctrl+J: frame   Alt+P: remove from frame");
        ImGui::TextUnformatted("Panels: drag a tab to dock it anywhere or pull it out into its own window");
        ImGui::Separator();
        ImGui::TextUnformatted("Wires: amber = Image, gray = Channel, blue = Number");
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void App::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_N)) requestAction(Pending::New);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) requestAction(Pending::Open);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveProject(false);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveProject(true);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I))
        if (auto p = openFileDialog("Import image", kImageFileFilter)) importImage(*p);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) openExportWindow();
    if (eyedropper().active() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) eyedropper().cancel();
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        // F1 opens the guide at the selected node's entry, like context help.
        const Node* n = currentGraph().find(selected_);
        openGuide(n ? n->info().displayName : std::string());
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
}

void App::handleDrops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (const auto& p : drops) {
        // With the Batch tab open, dropped images and folders become batch sources instead.
        if (showExport_ && exportTab_ == 1 && !exporter_.busy()) {
            std::vector<std::string> found;
            std::error_code ec;
            if (std::filesystem::is_directory(u8ToPath(p), ec)) {
                for (const auto& e : std::filesystem::directory_iterator(u8ToPath(p), ec))
                    if (e.is_regular_file() && isImageFile(e.path())) found.push_back(pathToU8(e.path()));
                std::sort(found.begin(), found.end());
            } else if (isImageFile(u8ToPath(p))) {
                found.push_back(p);
            }
            for (std::string& f : found)
                if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                    batchSources_.push_back(std::move(f));
            if (!found.empty()) continue;
        }
        std::string ext = pathToU8(u8ToPath(p).extension());
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".nlproj") {
            if (!modified_) openProject(p);
            else status_ = "Save or discard changes before opening a dropped project";
        } else {
            importImage(p);
        }
    }
}

// ---------------------------------------------------------------- groups

Graph& App::currentGraph() {
    if (Graph* g = resolveGroupPath(graph_, groupPath_)) return *g;
    groupPath_.clear();
    return graph_;
}

GroupNode* App::currentGroupOwner() {
    if (groupPath_.empty()) return nullptr;
    Graph* parent = resolveGroupPath(graph_, std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
    return parent ? dynamic_cast<GroupNode*>(parent->find(groupPath_.back())) : nullptr;
}

void App::setGroupPath(std::vector<int> path) {
    groupViews_[groupPath_] = editor_.viewState();
    const int leaving = groupPath_.size() > path.size() ? groupPath_[path.size()] : 0;
    groupPath_ = std::move(path);
    auto it = groupViews_.find(groupPath_);
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(it == groupViews_.end());
    if (it != groupViews_.end()) editor_.setViewState(it->second);
    if (leaving) editor_.select(leaving);  // going up: highlight the group we came out of
    selected_ = 0;
}

void App::enterGroup(int nodeId) {
    if (!dynamic_cast<GroupNode*>(currentGraph().find(nodeId))) return;
    auto p = groupPath_;
    p.push_back(nodeId);
    setGroupPath(p);
}

void App::exitGroup() {
    if (groupPath_.empty()) return;
    setGroupPath(std::vector<int>(groupPath_.begin(), groupPath_.end() - 1));
}

void App::finishPick(const PickRequest& pick) {
    Eyedropper& e = eyedropper();
    if (pick.cancelled) {
        e.cancel();
        return;
    }
    if (!pick.done) return;
    Node* n = currentGraph().find(e.node);
    if (n && e.param >= 0 && e.param < int(n->info().params.size())) {
        const ParamDesc& d = n->info().params[e.param];
        float c[3];
        for (int k = 0; k < 3; ++k) c[k] = std::clamp(pick.rgb[k], d.hardMin, d.hardMax);
        n->params[e.param] = nlohmann::json::array({c[0], c[1], c[2]});
        markChanged(true);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "Picked %.3f %.3f %.3f for %s", c[0], c[1], c[2], d.name.c_str());
        status_ = buf;
    }
    e.cancel();
}

void App::openViewer(NodePath pin) {
    auto v = std::make_unique<Viewer>();
    v->id = nextViewerId_++;
    v->pin = std::move(pin);
    viewers_.push_back(std::move(v));
    evalDirty_ = true;
}

std::string App::pathLabel(const NodePath& p) {
    std::string label;
    Graph* g = &graph_;
    for (size_t i = 0; i < p.size() && g; ++i) {
        Node* n = g->find(p[i]);
        if (!n) return "(missing)";
        if (!label.empty()) label += " > ";
        label += n->title();
        auto* grp = dynamic_cast<GroupNode*>(n);
        g = grp ? &grp->inner() : nullptr;
    }
    return label;
}

// ---------------------------------------------------------------- undo / redo (whole-graph snapshots)

void App::resetHistory() {
    undo_.clear();
    redo_.clear();
    committed_ = graph_.toJson();
    historyDirty_ = false;
}

bool App::commitHistory() {
    nlohmann::json cur = graph_.toJson();
    if (cur == committed_) return false;
    undo_.push_back(std::move(committed_));
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    committed_ = std::move(cur);
    redo_.clear();
    return true;
}

bool App::canUndo() const { return !undo_.empty() || historyDirty_; }

void App::restoreSnapshot(const nlohmann::json& j) {
    try {
        graph_.fromJson(j);
    } catch (const std::exception& e) {
        status_ = std::string("Undo failed: ") + e.what();
        return;
    }
    // Stay inside the current group if it still exists, otherwise back out to where it does.
    while (!groupPath_.empty() && !resolveGroupPath(graph_, groupPath_)) groupPath_.pop_back();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(false);
    if (!pathValid(previewPath_)) previewPath_.clear();
    selected_ = 0;
    modified_ = true;
    evalDirty_ = true;
    historyDirty_ = false;
}

void App::undo() {
    commitHistory();  // include any not-yet-snapshotted change so it is what gets undone
    if (undo_.empty()) return;
    redo_.push_back(std::move(committed_));
    committed_ = std::move(undo_.back());
    undo_.pop_back();
    restoreSnapshot(committed_);
    status_ = "Undo";
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(std::move(committed_));
    committed_ = std::move(redo_.back());
    redo_.pop_back();
    restoreSnapshot(committed_);
    status_ = "Redo";
}

// ---------------------------------------------------------------- unsaved-changes flow

void App::requestAction(Pending action) {
    if (modified_) {
        pending_ = action;
        openUnsavedModal_ = true;
    } else {
        performAction(action);
    }
}

void App::performAction(Pending action) {
    switch (action) {
        case Pending::New: newProject(); break;
        case Pending::Open:
            if (auto p = openFileDialog("Open project", kProjectFilter)) openProject(*p);
            break;
        case Pending::Quit: quit_ = true; break;
        case Pending::None: break;
    }
}

void App::drawUnsavedModal() {
    if (openUnsavedModal_) {
        ImGui::OpenPopup("Unsaved changes");
        openUnsavedModal_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("This project has unsaved changes.");
    ImGui::Spacing();
    if (ImGui::Button("Save")) {
        ImGui::CloseCurrentPopup();
        if (saveProject(false)) performAction(pending_);
        pending_ = Pending::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard")) {
        ImGui::CloseCurrentPopup();
        modified_ = false;
        performAction(pending_);
        pending_ = Pending::None;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        pending_ = Pending::None;
    }
    ImGui::EndPopup();
}

void App::drawConvertModal() {
    // Opened here, not inside the Color menu, so the ID stack matches BeginPopupModal's.
    if (convertPrompt_) {
        ImGui::OpenPopup("Convert to Scene-Linear");
        convertPrompt_ = false;
    }
    if (!ImGui::BeginPopupModal("Convert to Scene-Linear", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Images will load as linear light and nodes will work on linear values,");
    ImGui::TextUnformatted("with the view transform applied only for display and export.");
    ImGui::TextUnformatted("Nothing is added to the graph, so the result will look different:");
    ImGui::TextUnformatted("curves, levels and blends tuned on sRGB values may need adjusting.");
    ImGui::TextUnformatted("Ctrl+Z undoes the conversion.");
    ImGui::Spacing();
    if (ImGui::Button("Convert")) {
        ImGui::CloseCurrentPopup();
        graph_.colorManagement.linear = true;
        markChanged(true);
        status_ = "Converted to scene-linear";
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- project lifecycle

void App::newProject() {
    graph_.clear();
    graph_.colorManagement = ColorManagement::sceneLinear();
    Node* in = graph_.addNode(ImageInputNode::staticInfo().type, 40, 80);
    Node* out = graph_.addNode(OutputNode::staticInfo().type, 460, 80);
    graph_.connect(in->id, 0, out->id, 0);
    groupPath_.clear();
    groupViews_.clear();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(true);
    editor_.select(in->id);
    resetHistory();
    projectPath_.clear();
    previewPath_.clear();
    selected_ = 0;
    view_.reset();
    modified_ = false;
    evalDirty_ = true;
    status_ = "New project";
}

bool App::openProject(const std::string& path) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(path, g, ui, err)) {
        status_ = "Open failed: " + err;
        return false;
    }
    graph_ = std::move(g);
    groupPath_.clear();
    groupViews_.clear();
    eyedropper().cancel();  // its node id may mean something else now
    editor_.onGraphReplaced(true);
    projectPath_ = path;
    selected_ = 0;
    applyUiState(ui);
    resetHistory();
    modified_ = false;
    evalDirty_ = true;
    status_ = "Opened " + pathToU8(u8ToPath(path).filename());
    return true;
}

bool App::saveProject(bool saveAs) {
    std::string path = projectPath_;
    if (saveAs || path.empty()) {
        auto p = saveFileDialog("Save project", kProjectFilter, "nlproj");
        if (!p) return false;
        path = *p;
    }
    std::string err;
    if (!::saveProject(path, graph_, uiState(), err)) {
        status_ = "Save failed: " + err;
        return false;
    }
    projectPath_ = path;
    modified_ = false;
    status_ = "Saved " + pathToU8(u8ToPath(path).filename());
    return true;
}

void App::importImage(const std::string& path) {
    std::string err;
    // Decode as the new Image Input will (its default params), so the cache entry is reused.
    const auto decode = ImageInputNode().decode(graph_.colorManagement.linear);
    if (!cache_.get(path, true, &err, decode)) {
        status_ = "Import failed: " + err;
        return;
    }
    // Reuse an empty Image Input if there is one, otherwise add a new node.
    Graph& g = currentGraph();
    Node* target = nullptr;
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && n->paramS(0).empty()) {
            target = n.get();
            break;
        }
    if (!target) {
        target = g.addNode(ImageInputNode::staticInfo().type);
        editor_.placeAtScreen(*target, editor_.canvasCenter());
    }
    target->params[0] = path;
    editor_.select(target->id);
    status_ = "Imported " + pathToU8(u8ToPath(path).filename());
    markChanged(true);
}

void App::openExportWindow() {
    showExport_ = true;
    focusExport_ = true;
    // Suggest a file next to the project the first time.
    if (!exportPath_[0]) {
        auto base = projectPath_.empty() ? std::filesystem::path("export") : u8ToPath(projectPath_).replace_extension();
        std::snprintf(exportPath_, sizeof(exportPath_), "%s", pathToU8(base.string() + std::string(exportSettings_.extension())).c_str());
    }
}

void App::startExport(std::vector<ExportItem> items, int inputNode) {
    if (exporter_.busy() || items.empty()) return;
    exportSettings_.suffix = batchSuffix_;
    exportLog_.clear();
    // The root graph, whatever group is open: exports always render the whole project.
    exporter_.start(graph_.toJson(), std::move(items), inputNode, exportSettings_);
}

void App::pollExport() {
    for (std::string& line : exporter_.takeLog()) {
        status_ = line;
        exportLog_.push_back(std::move(line));
    }
}

void App::drawExportWindow() {
    if (!showExport_) return;
    ImGui::SetNextWindowSize(ImVec2(520, 660), ImGuiCond_FirstUseEver);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (focusExport_) ImGui::SetNextWindowFocus();
    focusExport_ = false;
    if (!ImGui::Begin("Export", &showExport_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    const bool busy = exporter_.busy();
    ExportSettings& es = exportSettings_;
    const float browseW = ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2;
    auto pathField = [&](const char* id, char* buf, size_t size) {
        ImGui::SetNextItemWidth(-browseW - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText(id, buf, size);
        ImGui::SameLine();
        ImGui::PushID(id);
        const bool clicked = ImGui::Button("Browse...");
        ImGui::PopID();
        return clicked;
    };
    auto withExt = [&](std::string path) {
        auto p = u8ToPath(path);
        p.replace_extension(es.extension());
        return pathToU8(p);
    };

    ImGui::BeginDisabled(busy);
    int tab = -1;
    if (ImGui::BeginTabBar("##exportTabs")) {
        if (ImGui::BeginTabItem("Single")) {
            tab = 0;
            ImGui::TextUnformatted("Renders the Output node at full resolution.");
            ImGui::TextUnformatted("File");
            if (pathField("##exportPath", exportPath_, sizeof(exportPath_)))
                if (auto p = saveFileDialog("Export result", kSaveImageFilter, es.extension() + 1)) {
                    es.format = int(formatFromPath(*p));
                    std::snprintf(exportPath_, sizeof(exportPath_), "%s", p->c_str());
                }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Batch")) {
            tab = 1;
            ImGui::TextWrapped("Runs every source image through this node tree and saves each result to the output folder.");
            // Which Image Input receives each source.
            std::vector<const Node*> inputs;
            for (const auto& [id, n] : graph_.nodes())
                if (n->info().type == ImageInputNode::staticInfo().type) inputs.push_back(n.get());
            if (!graph_.find(batchInput_) && !inputs.empty()) batchInput_ = inputs.front()->id;
            auto inputName = [](const Node* n) {
                std::string f = n->paramS(0).empty() ? "no file" : pathToU8(u8ToPath(n->paramS(0)).filename());
                return n->title() + " (" + f + ")";
            };
            const Node* cur = graph_.find(batchInput_);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##batchInput", cur ? inputName(cur).c_str() : "No Image Input node")) {
                for (const Node* n : inputs) {
                    ImGui::PushID(n->id);
                    if (ImGui::Selectable(inputName(n).c_str(), n->id == batchInput_)) batchInput_ = n->id;
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::Text("Sources (%d)", int(batchSources_.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Files..."))
                for (std::string& f : openFilesDialog("Add source images", kImageFileFilter))
                    if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                        batchSources_.push_back(std::move(f));
            ImGui::SameLine();
            if (ImGui::SmallButton("Add Folder..."))
                if (auto dir = folderDialog("Add every image in a folder")) {
                    std::vector<std::string> found;
                    std::error_code ec;
                    for (const auto& e : std::filesystem::directory_iterator(u8ToPath(*dir), ec))
                        if (e.is_regular_file() && isImageFile(e.path())) found.push_back(pathToU8(e.path()));
                    std::sort(found.begin(), found.end());
                    for (std::string& f : found)
                        if (std::find(batchSources_.begin(), batchSources_.end(), f) == batchSources_.end())
                            batchSources_.push_back(std::move(f));
                }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) batchSources_.clear();
            if (ImGui::BeginChild("##sources", ImVec2(0, 110), ImGuiChildFlags_Borders)) {
                int remove = -1;
                for (int i = 0; i < int(batchSources_.size()); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::SmallButton("x")) remove = i;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(pathToU8(u8ToPath(batchSources_[i]).filename()).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", batchSources_[i].c_str());
                    ImGui::PopID();
                }
                if (batchSources_.empty()) ImGui::TextDisabled("Add files, a folder, or drop images here");
                if (remove >= 0) batchSources_.erase(batchSources_.begin() + remove);
            }
            ImGui::EndChild();
            ImGui::TextUnformatted("Output folder");
            if (pathField("##batchDir", batchDir_, sizeof(batchDir_)))
                if (auto d = folderDialog("Output folder")) std::snprintf(batchDir_, sizeof(batchDir_), "%s", d->c_str());
            ImGui::SetNextItemWidth(160);
            ImGui::InputText("Name suffix", batchSuffix_, sizeof(batchSuffix_));
            if (!batchSources_.empty()) {
                ExportSettings tmp = es;
                tmp.suffix = batchSuffix_;
                ImGui::TextDisabled("e.g. %s", pathToU8(u8ToPath(batchOutputPath(batchSources_[0], batchDir_, tmp)).filename()).c_str());
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    exportTab_ = tab;

    ImGui::SeparatorText("Format");
    ImGui::SetNextItemWidth(160);
    // The path field follows the format, so what it shows is the file that gets written.
    if (ImGui::Combo("##format", &es.format, "PNG\0JPEG\0TIFF\0OpenEXR\0") && exportPath_[0])
        std::snprintf(exportPath_, sizeof(exportPath_), "%s", withExt(exportPath_).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("PNG, JPEG and TIFF are display images (view transform applied, tagged sRGB).\n"
                          "OpenEXR keeps the scene-linear values, as Blender does.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (es.format == ExportSettings::JPEG) {
        ImGui::SliderInt("##quality", &es.jpegQuality, 1, 100, "Quality %d");
    } else {
        // Blender's Color Depth: 8/16 bits for PNG and TIFF, half or full float for OpenEXR.
        const bool exr = es.format == ExportSettings::EXR;
        int hi = formatDepth(FileFormat(es.format), es.depth) > (exr ? 16 : 8) ? 1 : 0;
        if (ImGui::Combo("##depth", &hi, exr ? "Float (Half)\0Float (Full)\0" : "8 bit\00016 bit\0"))
            es.depth = exr ? (hi ? 32 : 16) : (hi ? 16 : 8);
    }
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##size", &es.sizeMode, "Original size\0Long edge\0Percent\0");
    if (es.sizeMode == ExportSettings::LongEdge) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("px", &es.longEdge, 64, 512);
        es.longEdge = std::clamp(es.longEdge, 16, 65536);
    } else if (es.sizeMode == ExportSettings::Percent) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderInt("##percent", &es.percent, 1, 100, "%d %%");
    }
    if (tab == 0) ImGui::Checkbox("Also write File Output nodes", &es.fileOutputs);
    ImGui::EndDisabled();

    ImGui::Separator();
    if (busy) {
        const Exporter::Progress pr = exporter_.progress();
        // No per-node progress from the evaluator, so within an item the bar just shows the stage.
        const float frac = pr.total ? float(pr.done) / pr.total : 0.0f;
        const std::string label = std::to_string(pr.done) + " / " + std::to_string(pr.total) + "  " + pr.stage;
        ImGui::ProgressBar(frac, ImVec2(-ImGui::CalcTextSize("Cancel").x - 30, 0), label.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) exporter_.cancel();
    } else {
        std::string why;
        std::vector<ExportItem> items;
        int input = 0;
        if (tab == 0) {
            if (!exportPath_[0]) why = "Choose a file";
            else if (!graph_.firstOfType(OutputNode::staticInfo().type)) why = "Add an Output node";
            else items.push_back({"", withExt(exportPath_)});
        } else if (tab == 1) {
            input = batchInput_;
            ExportSettings tmp = es;
            tmp.suffix = batchSuffix_;
            if (!graph_.find(input)) why = "The tree needs an Image Input node";
            else if (batchSources_.empty()) why = "Add source images";
            else if (!batchDir_[0]) why = "Choose an output folder";
            else if (!graph_.firstOfType(OutputNode::staticInfo().type)) why = "Add an Output node";
            else
                for (const std::string& src : batchSources_) items.push_back({src, batchOutputPath(src, batchDir_, tmp)});
        }
        ImGui::BeginDisabled(!why.empty());
        const std::string label = tab == 1 ? "Export " + std::to_string(items.size()) + " Images" : std::string("Export");
        if (ImGui::Button(label.c_str(), ImVec2(160, 0))) {
            if (tab == 1) std::filesystem::create_directories(u8ToPath(batchDir_));
            // Keep the path's extension in step with the chosen format.
            if (tab == 0) std::snprintf(exportPath_, sizeof(exportPath_), "%s", items[0].output.c_str());
            startExport(std::move(items), input);
        }
        ImGui::EndDisabled();
        if (!why.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", why.c_str());
        }
    }
    if (ImGui::BeginChild("##exportLog", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (const std::string& line : exportLog_) ImGui::TextWrapped("%s", line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------- helpers

void App::markChanged(bool eval) {
    modified_ = true;
    historyDirty_ = true;
    if (eval) evalDirty_ = true;
}

void App::updateTitle() {
    std::string name = projectPath_.empty() ? "Untitled" : pathToU8(u8ToPath(projectPath_).filename());
    std::string title = name + (modified_ ? " *" : "") + " - NodeLab " + kNodeLabVersion;
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }
}

bool App::pathValid(const NodePath& p) {
    if (p.empty()) return false;
    Graph* g = resolveGroupPath(graph_, std::vector<int>(p.begin(), p.end() - 1));
    return g && g->find(p.back());
}

NodePath App::resultTarget() {
    if (pathValid(previewPath_)) return previewPath_;
    int out = graph_.firstOfType(OutputNode::staticInfo().type);
    return out ? NodePath{out} : NodePath{};
}

void App::refreshDisplay(Viewer& v, bool main) {
    v.display = colormgmt::displayImage(v.shown, graph_.colorManagement);
    if (v.display) v.tex.upload(*v.display, main && clipping_);
    else v.tex.reset();
}

void App::updateTextures() {
    colordisplay::linear = graph_.colorManagement.linear;
    // Original panel: the selected Image Input (in the graph being edited), else the root's first.
    Graph& cur = currentGraph();
    Node* src = cur.find(selected_);
    if (!src || src->info().type != ImageInputNode::staticInfo().type)
        src = graph_.find(graph_.firstOfType(ImageInputNode::staticInfo().type));
    ImagePtr left;
    const ColorManagement& cm = graph_.colorManagement;
    if (src && !src->paramS(0).empty())
        left = cache_.get(src->paramS(0), true, nullptr, static_cast<const ImageInputNode&>(*src).decode(cm.linear));
    // Changing the view transform only redraws; the graph's values are unaffected.
    const bool cmChanged = !(cm == shownCm_);
    shownCm_ = cm;
    if (left != leftShown_ || cmChanged) {
        leftShown_ = left;
        if (left) leftTex_.upload(*colormgmt::displayImage(left, cm));
        else leftTex_.reset();
    }
    if (cmChanged) {
        for (size_t i = 0; i < viewers_.size(); ++i) refreshDisplay(*viewers_[i], i == 0);
        histogram_.valid = false;
    }

    if (auto res = eval_->poll()) {
        evalMs_ = res->ms;
        nodeMs_ = res->nodeMs;
        // Results are in submission order; match them to the viewers that still exist.
        const size_t nv = std::min({viewers_.size(), res->images.size(), submittedViewers_});
        for (size_t i = 0; i < nv; ++i) {
            Viewer& v = *viewers_[i];
            v.error = res->errors[i];
            v.shown = res->images[i];
            refreshDisplay(v, i == 0);
        }
        // The histogram follows the Result; recomputed only when shown.
        histogram_.valid = false;
        if (showHistogram_ && !viewers_.empty() && viewers_[0]->display) histogram_.compute(*viewers_[0]->display);
        // A mask target follows the viewers (see drawFrame).
        if (maskWanted_ && res->images.size() > submittedViewers_ && res->images[submittedViewers_])
            maskTex_.uploadTint(*res->images[submittedViewers_], 1.0f, 0.25f, 0.2f, 0.45f);
    }
}

bool App::saveFramebuffer(const std::string& path) {
    int w, h;
    glfwGetFramebufferSize(window_, &w, &h);
    if (w <= 0 || h <= 0) return false;
    std::vector<unsigned char> px(size_t(w) * h * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    Image img(w, h);
    for (int y = 0; y < h; ++y)  // GL rows are bottom-up
        for (int x = 0; x < w * 4; ++x) img.px[size_t(h - 1 - y) * w * 4 + x] = px[size_t(y) * w * 4 + x] / 255.0f;
    std::string err;
    return saveImage(path, img, err);
}

nlohmann::json App::uiState() const {
    nlohmann::json viewers = nlohmann::json::array();
    for (size_t i = 1; i < viewers_.size(); ++i) viewers.push_back({{"pin", viewers_[i]->pin}, {"sync", viewers_[i]->sync}});
    return {{"view", {view_.zoom, view_.panX, view_.panY}},
            {"preview", previewPath_},
            {"graphView", groupPath_.empty() ? editor_.viewState()
                          : groupViews_.count(std::vector<int>()) ? groupViews_.at(std::vector<int>()) : nlohmann::json()},
            {"viewers", viewers},
            {"histogram", showHistogram_},
            {"clipping", clipping_},
            {"maskOverlay", maskOverlay_},
            {"export", [&] {
                 nlohmann::json e = exportSettings_.toJson();
                 e["suffix"] = std::string(batchSuffix_);
                 e["path"] = std::string(exportPath_);
                 e["batchDir"] = std::string(batchDir_);
                 return e;
             }()}};
}

void App::applyUiState(const nlohmann::json& j) {
    view_.reset();
    viewers_.resize(1);
    previewPath_.clear();
    try {
        if (auto v = j.find("view"); v != j.end() && v->size() == 3) {
            view_.zoom = (*v)[0].get<float>();
            view_.panX = (*v)[1].get<float>();
            view_.panY = (*v)[2].get<float>();
        }
        if (auto gv = j.find("graphView"); gv != j.end()) editor_.setViewState(*gv);
        showHistogram_ = j.value("histogram", showHistogram_);
        clipping_ = j.value("clipping", clipping_);
        maskOverlay_ = j.value("maskOverlay", maskOverlay_);
        if (auto p = j.find("preview"); p != j.end()) {
            // Older projects stored a plain node id.
            if (p->is_number_integer() && p->get<int>() != 0) previewPath_ = {p->get<int>()};
            else if (p->is_array()) previewPath_ = p->get<NodePath>();
        }
        if (!pathValid(previewPath_)) previewPath_.clear();
        if (auto e = j.find("export"); e != j.end() && e->is_object()) {
            exportSettings_.fromJson(*e);
            std::snprintf(batchSuffix_, sizeof(batchSuffix_), "%s", exportSettings_.suffix.c_str());
            std::snprintf(exportPath_, sizeof(exportPath_), "%s", e->value("path", std::string()).c_str());
            std::snprintf(batchDir_, sizeof(batchDir_), "%s", e->value("batchDir", std::string()).c_str());
        }
        if (auto vs = j.find("viewers"); vs != j.end() && vs->is_array())
            for (const auto& vj : *vs) {
                auto v = std::make_unique<Viewer>();
                v->id = nextViewerId_++;
                v->pin = vj.value("pin", NodePath{});
                v->sync = vj.value("sync", false);
                viewers_.push_back(std::move(v));
            }
    } catch (const std::exception&) {
        // UI state is cosmetic; ignore anything malformed.
    }
}
