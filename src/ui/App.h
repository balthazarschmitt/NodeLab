#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/Export.h"
#include "io/ImageCache.h"
#include "ui/DisplayWorker.h"
#include "ui/ImageView.h"
#include "ui/LibraryPanel.h"
#include "ui/NodeEditor.h"
#include "ui/Theme.h"
#include "ui/ViewerOverlay.h"

struct GLFWwindow;
class GroupNode;

class App {
public:
    App();
    ~App();
    struct RunOptions {
        std::string project;     // .nlproj to open at startup
        std::string screenshot;  // debug: save a frame once evaluation settles, then exit
        std::string script;      // debug: UI automation script (see UiScript.h)
        bool gpu = false;        // debug: --device gpu, so automated runs use the GPU device too
    };
    int run(const RunOptions& opt);

private:
    enum class Pending { None, New, Open, OpenFolder, OpenPhoto, Quit };

    // An image panel showing a node's output. viewers_[0] is the main "Result" viewer, which
    // follows the Ctrl+click preview (or the Output node); extra viewers can pin any node.
    struct Viewer {
        int id = 0;
        bool open = true;
        NodePath pin;  // empty = follow the preview / Output node
        GLTexture tex;
        ViewState view;
        bool sync = false;  // share zoom/pan with Original and Result
        std::string error;
        // The node's output (scene values). A result evaluated on the GPU device stays there
        // (shownGpu) and shown is downloaded only when the eyedropper needs its pixels.
        ImagePtr shown;
        Value shownGpu;
        bool hasShown() const { return shown || !shownGpu.empty(); }
        int shownWidth() const {
            int w = shown ? shown->w : 0, h = 0;
            if (!shown) shownGpu.size(w, h);
            return w;
        }
        // Zoomed in: a sharper image of the visible part (AsyncEvaluator::Detail) over tex.
        ViewDetail detail;
        GLTexture detailTex;
        ImagePtr detailShown;  // scene values of detailTex
        ViewDetail pendingDetail;  // where detailShown goes, once its texture is ready
        ViewInfo info;         // what the view showed last frame
    };

    // project lifecycle
    void newProject();
    bool openProject(const std::string& path);
    bool saveProject(bool saveAs);
    void importImage(const std::string& path);
    // Library (File > Open Folder): photos edited in sidecar projects, saved automatically when
    // switching photos, as in Lightroom.
    void openFolder(const std::string& dirU8);
    void openLibraryPhoto(int index);  // asks about a non-library project's unsaved changes first
    void loadLibraryPhoto(int index);
    // The project is a library photo's sidecar.
    bool libraryPhotoOpen() const;
    // Saves the library photo's sidecar if it changed (or always, with force).
    bool saveLibraryPhoto(bool force = false);
    void copyEdit();
    void pasteEdit();
    void exportSelected();
    void drawLibraryWindow();
    // After a RAW was chosen for an Image Input: a fresh project switches its view to AgX.
    void applyRawLook();
    void drawColorMenu();
    void refreshDisplay(Viewer& v, bool main);
    void refreshDetail(Viewer& v, bool main);
    void dropDetail(Viewer& v);
    // Textures are prepared by display_ (DisplayWorker) and uploaded when ready; slots name them,
    // and a slot's sequence number drops results that were superseded meanwhile.
    static constexpr int kSlotLeft = 1, kSlotLeftDetail = 2, kSlotMask = 3;
    int detailSlot(const Viewer& v) const { return &v == &left_ ? kSlotLeftDetail : 101 + 2 * v.id; }
    static int mainSlot(const Viewer& v) { return 100 + 2 * v.id; }
    void requestDisplay(int slot, const ImagePtr& scene, bool clipping, bool histogram, bool tint = false,
                        const Value& gpuScene = Value());
    void dropDisplay(int slot) { ++displaySeq_[slot]; }
    void applyDisplays();
    // Detail requests for the zoomed-in views, and the proxy size the views call for.
    std::vector<AsyncEvaluator::Detail> wantedDetails();
    int wantedProxyEdge() const;
    // Nothing evaluating or about to be (scripts and screenshots wait for it).
    bool idle() const {
        return !eval_->busy() && !display_.busy() && !evalDirty_ && !refineAfterGesture_ && (lastWanted_.empty() || sameDetails(lastWanted_, details_));
    }
    std::vector<Viewer*> detailViews();  // the Original's (left_) and the viewers
    static bool sameDetails(const std::vector<AsyncEvaluator::Detail>& a, const std::vector<AsyncEvaluator::Detail>& b);
    static constexpr int kMinProxyEdge = 768, kMaxProxyEdge = 2048;
    static constexpr double kDraftAfterMs = 100;  // previews slower than this draft while dragging
    void openExportWindow();
    void drawExportWindow();
    void startExport(std::vector<ExportItem> items, int inputNode);
    void pollExport();
    void requestAction(Pending action);  // asks about unsaved changes first
    void performAction(Pending action);

    // frame
    void drawFrame();
    void drawMainMenu();
    void drawStatusBar();
    void buildLayout(unsigned dockId);  // layoutPreset_'s docked layout
    void drawOriginalWindow();
    void drawEditorWindow();
    void drawInspectorWindow();
    void drawInspectorContents();
    void drawInspectorOverlay();
    void drawPreferencesWindow();
    ColorManagement newProjectColor() const;  // a new project's working space and view
    void drawViewerWindow(Viewer& v, bool isMain);
    void drawUnsavedModal();
    void drawConvertModal();
    void handleShortcuts();
    void handleDrops();

    // groups
    Graph& currentGraph();
    GroupNode* currentGroupOwner();  // group whose inside is being edited, or null at the root
    void enterGroup(int nodeId);
    void exitGroup();
    void setGroupPath(std::vector<int> path);
    std::string pathLabel(const NodePath& p);
    void openViewer(NodePath pin);
    void finishPick(const PickRequest& pick);  // applies an eyedropper pick to its Color param
    Node* overlayNode();  // selected node with on-image controls, or null
    void drawResultToolbar(Node* ov);
    // Toolbar's Add Mask: a new Basic before the Output, driven by a new mask (one undo step).
    void addMask(int kind);

    void markChanged(bool eval);
    void resetHistory();
    bool commitHistory();
    bool canUndo() const;
    void undo();
    void redo();
    void restoreSnapshot(const nlohmann::json& j);
    void updateTitle();
    NodePath resultTarget();
    bool pathValid(const NodePath& p);
    void updateTextures();
    bool saveFramebuffer(const std::string& path);
    nlohmann::json uiState() const;
    void applyUiState(const nlohmann::json& j);

    GLFWwindow* window_ = nullptr;
    ImageCache cache_;
    std::unique_ptr<AsyncEvaluator> eval_;
    Graph graph_;  // root graph
    NodeEditor editor_;

    std::vector<int> groupPath_;  // group ids from the root to the graph being edited
    std::map<std::vector<int>, nlohmann::json> groupViews_;  // editor pan/zoom per level

    GLTexture leftTex_;
    ImagePtr leftShown_;    // scene values, for the eyedropper
    ImagePtr leftDecoded_, leftExposed_;  // the decoded file, and with the Baseline Exposure applied
    float leftGain_ = 1.0f;
    int leftNode_ = 0;      // the Image Input it shows, if in the root graph (for its detail)
    Viewer left_;           // the Original's detail and view info (its tex is leftTex_)
    ColorManagement shownCm_;  // the colour management the textures were made with
    ViewState view_;  // shared by Original and Result so they stay in sync
    std::vector<std::unique_ptr<Viewer>> viewers_;
    int nextViewerId_ = 1;
    std::vector<NodePath> submittedTargets_;
    size_t submittedViewers_ = 0;  // viewers in the last submission; a mask target may follow them

    // Result viewer extras: on-image controls, mask overlay, histogram and clipping warnings.
    NodeOverlay overlay_;
    GLTexture maskTex_;
    NodePath overlayPath_;      // node the overlay edits (crop mode / mask target follow it)
    bool maskWanted_ = false;   // a mask node is selected and the overlay is on
    bool maskOverlay_ = true, showHistogram_ = false, clipping_ = false;
    Histogram histogram_;
    DisplayWorker display_;
    std::map<int, uint64_t> displaySeq_;

    int selected_ = 0;       // selected node in the current graph
    NodePath previewPath_;   // Ctrl+click preview; empty = Output node
    int previewPin_ = 0;     // which output of the preview node (Ctrl+Shift+click cycles)
    std::string projectPath_;
    bool modified_ = false;
    bool evalDirty_ = true;
    bool gestureWas_ = false;  // a drag/slider/text gesture was active last frame
    // Full-resolution viewing (Phase F): the preview's proxy follows the view size; a slow graph
    // shows a half-size draft while dragging and is refined once the gesture ends; zoomed-in
    // views get sharp details of their visible part after the preview.
    int proxyEdge_ = ImageCache::kProxyEdge;
    double previewMs_ = 0;       // the last full preview's time
    bool refineAfterGesture_ = false;
    std::vector<AsyncEvaluator::Detail> details_;  // as last submitted (or dropped)
    std::vector<AsyncEvaluator::Detail> lastWanted_;
    double detailsChangedAt_ = -1;  // when the wanted details last changed (they settle first)
    std::vector<int> submittedPins_;
    std::string lastTitle_;

    std::string status_;

    // Export window (File > Export): renders on a background thread so the UI never freezes.
    Exporter exporter_;
    ExportSettings exportSettings_;
    bool showExport_ = false, focusExport_ = false;
    int exportTab_ = 0;  // tab shown last frame: 0 single, 1 batch (drops go to batch sources)
    char exportPath_[1024] = {};
    char batchDir_[1024] = {};
    char batchSuffix_[64] = "_edit";
    int batchInput_ = 0;  // Image Input node fed each source
    std::vector<std::string> batchSources_;
    std::vector<std::string> exportLog_;
    double evalMs_ = 0;
    std::unordered_map<int, double> nodeMs_;  // top-level node timings from the last evaluation
    std::unordered_map<int, bool> nodeGpu_;   // ...and which of them ran on the GPU
    // Blender's Performance > Compositor settings, kept per user (preferences.json) since they
    // depend on the machine: Device GPU or CPU, Precision Auto (half floats on the GPU) or Full.
    bool gpuDevice_ = true, gpuFull_ = false;
    int gpuFallbacks_ = 0;
    std::string gpuError_;  // why the GPU is unavailable, or the last node that fell back
    void loadPreferences();
    void savePreferences() const;

    // Edit > Preferences (preferences.json, like the compositor settings above).
    // Layout presets for View > Layout, like Blender's workspaces; Reset Layout rebuilds the chosen one.
    enum LayoutPreset { LayoutDefault, LayoutCompositing, LayoutPhoto, LayoutSideBySide, LayoutNodeFocus, kLayouts };
    static constexpr const char* kLayoutNames[kLayouts] = {"Default", "Compositing", "Photo", "Side by Side", "Node Focus"};
    int layoutPreset_ = LayoutDefault;
    // The Inspector floats over the Node Editor's top-right corner while a node is selected,
    // instead of being a docked panel. Automated runs keep the panel (their scripts click in it).
    bool inspectorOverlay_ = true;
    int newView_ = ColorManagement::Standard, newLook_ = ColorManagement::None;  // new projects' view
    std::vector<theme::Theme> customThemes_;  // saved with Save As in Preferences > Themes
    bool showPreferences_ = false;
    int prefsSection_ = 0;
    bool prefsDirty_ = false;  // saved once no widget is being dragged
    bool drawCompositorSettings();  // Device and Precision; true when changed
    char themeName_[64] = {};
    // The Node Editor's canvas as last drawn, where the inspector overlay goes.
    bool editorShown_ = false;
    ImVec2 editorMin_, editorMax_;
    unsigned editorViewport_ = 0;

    LibraryPanel library_;
    bool showLibrary_ = true;
    bool libraryLayoutChecked_ = false;  // an older layout without the Library was rebuilt
    int pendingPhoto_ = -1;              // for Pending::OpenPhoto
    nlohmann::json copiedEdit_;          // Copy Edit: the graph, with copiedFrom_ its photo
    std::string copiedFrom_;
    bool editorFocused_ = false;         // the Node Editor has keyboard focus (X deletes there)

    // panels / layout
    bool showOriginal_ = true, showEditor_ = true, showInspector_ = true, showResult_ = true;
    bool resetLayout_ = false;
    bool automated_ = false;
    std::string iniPath_;

    std::vector<nlohmann::json> undo_, redo_;
    nlohmann::json committed_;  // graph as of the last snapshot
    bool historyDirty_ = false;

    Pending pending_ = Pending::None;
    bool openUnsavedModal_ = false;
    bool convertPrompt_ = false;  // Color > Convert Project to Scene-Linear was chosen
    bool quit_ = false;
    std::vector<std::string> drops_;

    friend void dropCallback(GLFWwindow*, int, const char**);
    friend void closeCallback(GLFWwindow*);
    friend void refreshCallback(GLFWwindow*);
    // Draws and presents one frame outside the main loop (see refreshCallback); null in tests.
    std::function<void()> redraw_;
    bool inFrame_ = false;
};
