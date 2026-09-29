#include "ui/App.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"
#include "ui/FileDialog.h"
#include "ui/Inspector.h"
#include "ui/UiScript.h"

namespace fs = std::filesystem;

static const char* kProjectFilter = "NodeLab project (*.nlproj)|*.nlproj|All files|*.*";
static const char* kImageFilter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*";
static const char* kExportFilter = "PNG image|*.png|JPEG image|*.jpg";

void dropCallback(GLFWwindow* w, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    for (int i = 0; i < count; ++i) app->drops_.emplace_back(paths[i]);
}

void closeCallback(GLFWwindow* w) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
    glfwSetWindowShouldClose(w, GLFW_FALSE);
    app->requestAction(App::Pending::Quit);
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
    const bool automated = !opt.screenshot.empty() || script.active();
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Automated runs use a fixed size and never take focus from whatever the user is doing.
    glfwWindowHint(GLFW_MAXIMIZED, automated ? GLFW_FALSE : GLFW_TRUE);
    if (automated) {
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
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    const float dpi = std::max(1.0f, xscale);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 3.0f;
    style.ScaleAllSizes(dpi);

    const char* uiFont = "C:/Windows/Fonts/segoeui.ttf";
    if (fs::exists(uiFont)) io.Fonts->AddFontFromFileTTF(uiFont, 17.0f * dpi);
    else io.FontGlobalScale = dpi;

    // While a script runs, OS input is not forwarded to ImGui so the real mouse can't interfere.
    ImGui_ImplGlfw_InitForOpenGL(window_, !script.active());
    ImGui_ImplOpenGL3_Init("#version 130");

    eval_ = std::make_unique<AsyncEvaluator>(cache_);

    if (opt.project.empty() || !openProject(opt.project)) newProject();

    int frame = 0, settled = 0;
    while (!quit_) {
        glfwWaitEventsTimeout(eval_->busy() || evalDirty_ || automated ? 0.01 : 0.05);

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
        glfwSwapBuffers(window_);
    }

    eval_.reset();
    leftTex_.reset();
    rightTex_.reset();
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

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    drawMenuBar();
    drawPanes();
    drawUnsavedModal();
    ImGui::End();

    // Kick evaluation after the UI had a chance to change the graph this frame.
    if (evalDirty_) {
        int target = previewTarget();
        if (target) {
            eval_->submit(graph_.toJson(), target);
        } else {
            rightTex_.reset();
            evalError_ = "Add an Output node (right-click the canvas).";
        }
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
    editor_.onGraphReplaced(false);
    if (!graph_.find(preview_)) preview_ = 0;
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

// ---------------------------------------------------------------- layout

static bool splitter(const char* id, bool vertical, float thickness, float length, float& frac, float total,
                     float minFrac, float maxFrac, bool invert = false) {
    ImVec2 size = vertical ? ImVec2(thickness, length) : ImVec2(length, thickness);
    ImGui::InvisibleButton(id, size);
    bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active)
        ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, active ? IM_COL32(90, 90, 110, 255) : IM_COL32(40, 40, 46, 255));
    if (active) {
        float d = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        frac = std::clamp(frac + (invert ? -d : d) / total, minFrac, maxFrac);
        return true;
    }
    return false;
}

void App::drawPanes() {
    const float split = 6.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float statusH = ImGui::GetFrameHeight();
    const float contentH = std::max(50.0f, avail.y - statusH - ImGui::GetStyle().ItemSpacing.y);
    const float leftW = avail.x * leftFrac_;
    const float rightW = avail.x * rightFrac_;
    const float midW = std::max(50.0f, avail.x - leftW - rightW - 2 * split);

    // Panes and splitters butt against each other; SameLine(0, 0) and the Y nudges remove item spacing
    // between them without changing spacing inside the panes.
    const float gapY = ImGui::GetStyle().ItemSpacing.y;

    // Left: original
    ImGui::BeginChild("left", ImVec2(leftW, contentH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        int in = leftImageNode();
        std::string label = "Original";
        if (Node* n = graph_.find(in); n && !n->paramS(0).empty())
            label += "  -  " + pathToU8(u8ToPath(n->paramS(0)).filename());
        ImGui::TextUnformatted(label.c_str());
        drawImageView("##leftview", leftTex_, view_, "Drop an image here or use File > Import Image");
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    splitter("##split_l", true, split, contentH, leftFrac_, avail.x, 0.1f, 0.45f);
    ImGui::SameLine(0, 0);

    // Middle: node editor + inspector
    ImGui::BeginChild("middle", ImVec2(midW, contentH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float editorH = std::max(80.0f, (contentH - split) * editorFrac_);
        ImGui::BeginChild("editor", ImVec2(0, editorH), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        NodeEditor::Result r = editor_.draw(graph_, selected_, preview_);
        ImGui::EndChild();
        if (r.evalChanged || r.previewChanged) evalDirty_ = true;
        if (r.docChanged) {
            modified_ = true;
            historyDirty_ = true;
        }

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gapY);
        splitter("##split_m", false, split, midW, editorFrac_, contentH, 0.25f, 0.9f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - gapY);

        ImGui::BeginChild("inspector", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (drawInspector(graph_, selected_)) markChanged(true);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 0);
    splitter("##split_r", true, split, contentH, rightFrac_, avail.x, 0.1f, 0.45f, true);
    ImGui::SameLine(0, 0);

    // Right: result
    ImGui::BeginChild("right", ImVec2(0, contentH), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        std::string label = "Result";
        if (Node* n = graph_.find(preview_)) label = "Preview: " + n->info().displayName + "  (Ctrl+click to clear)";
        ImGui::TextUnformatted(label.c_str());
        const char* emptyMsg = !evalError_.empty() ? evalError_.c_str()
                             : graph_.find(preview_) ? "This node has no output yet - connect its inputs"
                                                     : "No output yet - connect something to the Output node";
        drawImageView("##rightview", rightTex_, view_, emptyMsg);
    }
    ImGui::EndChild();

    // Status bar
    std::string st;
    if (rightTex_.valid())
        st = std::to_string(rightTex_.width()) + " x " + std::to_string(rightTex_.height()) + " preview  |  " +
             std::to_string(int(evalMs_)) + " ms";
    if (eval_->busy()) st += "  |  evaluating...";
    if (!evalError_.empty()) st += "  |  " + evalError_;
    if (!status_.empty()) st += "  |  " + status_;
    ImGui::TextDisabled("%s", st.c_str());
}

// ---------------------------------------------------------------- menus & shortcuts

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
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
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !redo_.empty())) redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editor_.hasSelection()) && editor_.duplicateSelection(graph_))
            markChanged(true);
        if (ImGui::MenuItem("Delete", "Del", false, editor_.hasSelection()) && editor_.deleteSelection(graph_, preview_))
            markChanged(true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame All Nodes", "F")) editor_.frameAll();
        if (ImGui::MenuItem("Reset Zoom", "double-click image")) view_.reset();
        if (ImGui::MenuItem("Clear Node Preview", nullptr, false, preview_ != 0)) {
            preview_ = 0;
            evalDirty_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("NodeLab 0.1 - node-based image manipulation");
        ImGui::Separator();
        ImGui::TextUnformatted("Right-click canvas: add node");
        ImGui::TextUnformatted("Drag from a pin: connect   Drag an input wire off: disconnect");
        ImGui::TextUnformatted("Delete: remove selected   Ctrl+click node: preview it");
        ImGui::TextUnformatted("Images: wheel zoom, drag pan, double-click reset");
        ImGui::Separator();
        ImGui::TextUnformatted("Wires: amber = Image, gray = Channel, blue = Number");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
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
    editor_.onGraphReplaced(true);
    editor_.select(in->id);
    resetHistory();
    projectPath_.clear();
    preview_ = selected_ = 0;
    view_.reset();
    modified_ = false;
    evalDirty_ = true;
    evalError_.clear();
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
    editor_.onGraphReplaced(true);
    projectPath_ = path;
    selected_ = 0;
    applyUiState(ui);
    resetHistory();
    modified_ = false;
    evalDirty_ = true;
    evalError_.clear();
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
    Node* target = nullptr;
    for (const auto& [id, n] : graph_.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && n->paramS(0).empty()) {
            target = n.get();
            break;
        }
    if (!target) {
        target = graph_.addNode(ImageInputNode::staticInfo().type);
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

int App::leftImageNode() const {
    if (Node* n = graph_.find(selected_); n && n->info().type == ImageInputNode::staticInfo().type) return selected_;
    return graph_.firstOfType(ImageInputNode::staticInfo().type);
}

int App::previewTarget() const {
    if (preview_ && graph_.find(preview_)) return preview_;
    return graph_.firstOfType(OutputNode::staticInfo().type);
}

void App::updateTextures() {
    // Left pane follows the selected (or first) Image Input.
    ImagePtr left;
    if (Node* n = graph_.find(leftImageNode())) left = cache_.get(n->paramS(0), true);
    if (left != leftShown_) {
        leftShown_ = left;
        if (left) leftTex_.upload(*left);
        else leftTex_.reset();
    }

    if (auto res = eval_->poll()) {
        evalMs_ = res->ms;
        evalError_ = res->error;
        if (res->image) rightTex_.upload(*res->image);
        else rightTex_.reset();
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
    return {{"view", {view_.zoom, view_.panX, view_.panY}},
            {"panes", {leftFrac_, rightFrac_, editorFrac_}},
            {"preview", preview_},
            {"graphView", editor_.viewState()}};
}

void App::applyUiState(const nlohmann::json& j) {
    view_.reset();
    try {
        if (auto v = j.find("view"); v != j.end() && v->size() == 3) {
            view_.zoom = (*v)[0].get<float>();
            view_.panX = (*v)[1].get<float>();
            view_.panY = (*v)[2].get<float>();
        }
        if (auto p = j.find("panes"); p != j.end() && p->size() == 3) {
            leftFrac_ = std::clamp((*p)[0].get<float>(), 0.1f, 0.45f);
            rightFrac_ = std::clamp((*p)[1].get<float>(), 0.1f, 0.45f);
            editorFrac_ = std::clamp((*p)[2].get<float>(), 0.25f, 0.9f);
        }
        if (auto gv = j.find("graphView"); gv != j.end()) editor_.setViewState(*gv);
        preview_ = j.value("preview", 0);
        if (!graph_.find(preview_)) preview_ = 0;
    } catch (const std::exception&) {
        // UI state is cosmetic; ignore anything malformed.
    }
}
