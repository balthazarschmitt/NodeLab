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

#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/io/IONodes.h"
#include "ui/FileDialog.h"
#include "ui/Inspector.h"
#include "ui/UiScript.h"

namespace fs = std::filesystem;

static const char* kProjectFilter = "NodeLab project (*.nlproj)|*.nlproj|All files|*.*";
static const char* kImageFilter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*";
static const char* kExportFilter = "PNG image|*.png|JPEG image|*.jpg";
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
    std::erase_if(viewers_, [](const std::unique_ptr<Viewer>& v) { return v->id != 0 && !v->open; });

    drawUnsavedModal();

    // Kick evaluation after the UI had a chance to change the graph this frame.
    if (evalDirty_) {
        submittedTargets_.clear();
        for (auto& v : viewers_) submittedTargets_.push_back(v->id == 0 || v->pin.empty() ? resultTarget() : v->pin);
        eval_->submit(graph_.toJson(), submittedTargets_);
        evalDirty_ = false;
    }
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
    std::string title = "Original" + (leftLabel_.empty() ? "" : "  -  " + leftLabel_) + "###Original";
    if (ImGui::Begin(title.c_str(), &showOriginal_, kCanvasFlags))
        drawImageView("##leftview", leftTex_, view_, "Drop an image here or use File > Import Image");
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
        NodeEditor::Result r = editor_.draw(g, selected_, preview);
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
    if (ImGui::Begin(title.c_str(), open, kCanvasFlags)) {
        // Toolbar: what is shown, and pin controls for extra viewers.
        NodePath shown = isMain || v.pin.empty() ? resultTarget() : v.pin;
        std::string label = shown.empty() ? "(nothing)" : pathLabel(shown);
        if (isMain && !previewPath_.empty()) label = "Preview: " + label + "  (Ctrl+click it again to clear)";
        if (!isMain) {
            if (ImGui::SmallButton("Pin Selected") && selected_) {
                v.pin = groupPath_;
                v.pin.push_back(selected_);
                evalDirty_ = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(v.pin.empty());
            if (ImGui::SmallButton("Unpin")) {
                v.pin.clear();
                evalDirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (!v.pin.empty()) label = "Pinned: " + label;
        }
        ImGui::TextDisabled("%s", label.c_str());
        const char* emptyMsg = !v.error.empty() ? v.error.c_str()
                               : shown.empty() ? "Add an Output node (right-click the canvas)"
                                               : "No output yet - connect this node's inputs";
        drawImageView(isMain ? "##result" : "##viewer", v.tex, isMain ? view_ : v.view, emptyMsg);
    }
    ImGui::End();
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
            if (!main.error.empty()) st += "  |  " + main.error;
            if (!status_.empty()) st += "  |  " + status_;
            ImGui::TextDisabled("%s", st.c_str());
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- menus & shortcuts

void App::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New", "Ctrl+N")) requestAction(Pending::New);
        if (ImGui::MenuItem("Open Project...", "Ctrl+O")) requestAction(Pending::Open);
        if (ImGui::MenuItem("Save", "Ctrl+S")) saveProject(false);
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) saveProject(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Import Image...", "Ctrl+I"))
            if (auto p = openFileDialog("Import image", kImageFilter)) importImage(*p);
        if (ImGui::MenuItem("Export Result...", "Ctrl+E")) exportResult();
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
        if (ImGui::MenuItem("Delete", "Del", false, editor_.hasSelection()) && editor_.deleteSelection(g, preview))
            markChanged(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Group Selected", "Ctrl+G", false, editor_.hasSelection()) && editor_.groupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Ungroup", "Ctrl+Alt+G", false, editor_.selectedGroup(g) != 0) && editor_.ungroupSelection(g))
            markChanged(true);
        if (ImGui::MenuItem("Edit Group", "Tab", false, editor_.selectedGroup(g) != 0)) enterGroup(editor_.selectedGroup(g));
        if (ImGui::MenuItem("Exit Group", "Tab", false, !groupPath_.empty())) exitGroup();
        ImGui::Separator();
        if (ImGui::MenuItem("Frame Selected", "Ctrl+J") && editor_.frameSelection(g)) markChanged(false);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Original", nullptr, &showOriginal_);
        ImGui::MenuItem("Result", nullptr, &showResult_);
        ImGui::MenuItem("Node Editor", nullptr, &showEditor_);
        ImGui::MenuItem("Inspector", nullptr, &showInspector_);
        if (ImGui::MenuItem("New Viewer")) {
            auto v = std::make_unique<Viewer>();
            v->id = nextViewerId_++;
            if (selected_) {
                v->pin = groupPath_;
                v->pin.push_back(selected_);
            }
            viewers_.push_back(std::move(v));
            evalDirty_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) resetLayout_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Frame All Nodes", "F")) editor_.frameAll();
        if (ImGui::MenuItem("Reset Image Zoom", "double-click image")) view_.reset();
        if (ImGui::MenuItem("Clear Node Preview", nullptr, false, !previewPath_.empty())) {
            previewPath_.clear();
            evalDirty_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("NodeLab 0.2 - node-based image manipulation");
        ImGui::Separator();
        ImGui::TextUnformatted("Right-click canvas: add node          Drag pin to empty space: add connected node");
        ImGui::TextUnformatted("Drag empty space: pan   Wheel: zoom   Shift+drag: box select   F: frame all");
        ImGui::TextUnformatted("Ctrl+click node: preview   Ctrl+D: duplicate   Del: delete");
        ImGui::TextUnformatted("Ctrl+G: group   Ctrl+Alt+G: ungroup   Tab: enter / exit group   Ctrl+J: frame");
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
        if (auto p = openFileDialog("Import image", kImageFilter)) importImage(*p);
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_E)) exportResult();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
        redo();
}

void App::handleDrops() {
    auto drops = std::move(drops_);
    drops_.clear();
    for (const auto& p : drops) {
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

std::string App::pathLabel(const NodePath& p) {
    std::string label;
    Graph* g = &graph_;
    for (size_t i = 0; i < p.size() && g; ++i) {
        Node* n = g->find(p[i]);
        if (!n) return "(missing)";
        if (!label.empty()) label += " > ";
        label += n->info().displayName;
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

// ---------------------------------------------------------------- project lifecycle

void App::newProject() {
    graph_.clear();
    Node* in = graph_.addNode(ImageInputNode::staticInfo().type, 40, 80);
    Node* out = graph_.addNode(OutputNode::staticInfo().type, 460, 80);
    graph_.connect(in->id, 0, out->id, 0);
    groupPath_.clear();
    groupViews_.clear();
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
    if (!cache_.get(path, true, &err)) {
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

void App::exportResult() {
    int outId = graph_.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        status_ = "Nothing to export: add an Output node";
        return;
    }
    auto p = saveFileDialog("Export result", kExportFilter, "png");
    if (!p) return;
    // Full-resolution pass on the UI thread; a progress UI can come later.
    try {
        EvalContext ctx;
        ctx.proxy = false;
        ctx.cache = &cache_;
        initContextSize(graph_, ctx);
        Evaluator ev;
        ImagePtr img = ev.evaluateDisplay(graph_, outId, ctx);
        std::string err;
        if (!img) status_ = "Export failed: Output node has no input";
        else if (!saveImage(*p, *img, err)) status_ = "Export failed: " + err;
        else status_ = "Exported " + pathToU8(u8ToPath(*p).filename()) + " (" + std::to_string(img->w) + " x " +
                       std::to_string(img->h) + ")";
    } catch (const std::exception& e) {
        status_ = std::string("Export failed: ") + e.what();
    }
}

// ---------------------------------------------------------------- helpers

void App::markChanged(bool eval) {
    modified_ = true;
    historyDirty_ = true;
    if (eval) evalDirty_ = true;
}

void App::updateTitle() {
    std::string name = projectPath_.empty() ? "Untitled" : pathToU8(u8ToPath(projectPath_).filename());
    std::string title = name + (modified_ ? " *" : "") + " - NodeLab";
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

void App::updateTextures() {
    // Original panel: the selected Image Input (in the graph being edited), else the root's first.
    Graph& cur = currentGraph();
    Node* src = cur.find(selected_);
    if (!src || src->info().type != ImageInputNode::staticInfo().type)
        src = graph_.find(graph_.firstOfType(ImageInputNode::staticInfo().type));
    ImagePtr left;
    leftLabel_.clear();
    if (src && !src->paramS(0).empty()) {
        left = cache_.get(src->paramS(0), true);
        leftLabel_ = pathToU8(u8ToPath(src->paramS(0)).filename());
    }
    if (left != leftShown_) {
        leftShown_ = left;
        if (left) leftTex_.upload(*left);
        else leftTex_.reset();
    }

    if (auto res = eval_->poll()) {
        evalMs_ = res->ms;
        // Results are in submission order; match them to the viewers that still exist.
        for (size_t i = 0; i < viewers_.size() && i < res->images.size(); ++i) {
            Viewer& v = *viewers_[i];
            v.error = res->errors[i];
            if (res->images[i]) v.tex.upload(*res->images[i]);
            else v.tex.reset();
        }
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
    for (size_t i = 1; i < viewers_.size(); ++i) viewers.push_back({{"pin", viewers_[i]->pin}});
    return {{"view", {view_.zoom, view_.panX, view_.panY}},
            {"preview", previewPath_},
            {"graphView", groupPath_.empty() ? editor_.viewState()
                          : groupViews_.count(std::vector<int>()) ? groupViews_.at(std::vector<int>()) : nlohmann::json()},
            {"viewers", viewers}};
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
        if (auto p = j.find("preview"); p != j.end()) {
            // Older projects stored a plain node id.
            if (p->is_number_integer() && p->get<int>() != 0) previewPath_ = {p->get<int>()};
            else if (p->is_array()) previewPath_ = p->get<NodePath>();
        }
        if (!pathValid(previewPath_)) previewPath_.clear();
        if (auto vs = j.find("viewers"); vs != j.end() && vs->is_array())
            for (const auto& vj : *vs) {
                auto v = std::make_unique<Viewer>();
                v->id = nextViewerId_++;
                v->pin = vj.value("pin", NodePath{});
                viewers_.push_back(std::move(v));
            }
    } catch (const std::exception&) {
        // UI state is cosmetic; ignore anything malformed.
    }
}
