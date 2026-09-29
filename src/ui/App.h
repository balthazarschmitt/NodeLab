#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/ImageCache.h"
#include "ui/ImageView.h"
#include "ui/NodeEditor.h"

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
    };
    int run(const RunOptions& opt);

private:
    enum class Pending { None, New, Open, Quit };

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
        ImagePtr shown;     // the image in tex, for the eyedropper
    };

    // project lifecycle
    void newProject();
    bool openProject(const std::string& path);
    bool saveProject(bool saveAs);
    void importImage(const std::string& path);
    void exportResult();
    void requestAction(Pending action);  // asks about unsaved changes first
    void performAction(Pending action);

    // frame
    void drawFrame();
    void drawMainMenu();
    void drawStatusBar();
    void buildDefaultLayout(unsigned dockId);
    void drawOriginalWindow();
    void drawEditorWindow();
    void drawInspectorWindow();
    void drawViewerWindow(Viewer& v, bool isMain);
    void drawUnsavedModal();
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
    ImagePtr leftShown_;
    std::string leftLabel_;
    ViewState view_;  // shared by Original and Result so they stay in sync
    std::vector<std::unique_ptr<Viewer>> viewers_;
    int nextViewerId_ = 1;
    std::vector<NodePath> submittedTargets_;

    int selected_ = 0;       // selected node in the current graph
    NodePath previewPath_;   // Ctrl+click preview; empty = Output node
    int previewPin_ = 0;     // which output of the preview node (Ctrl+Shift+click cycles)
    std::string projectPath_;
    bool modified_ = false;
    bool evalDirty_ = true;
    std::string lastTitle_;

    std::string status_;
    double evalMs_ = 0;

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
    bool quit_ = false;
    std::vector<std::string> drops_;

    friend void dropCallback(GLFWwindow*, int, const char**);
    friend void closeCallback(GLFWwindow*);
};
