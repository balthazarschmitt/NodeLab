#pragma once
#include <memory>
#include <string>
#include <vector>

#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/ImageCache.h"
#include "ui/ImageView.h"
#include "ui/NodeEditor.h"

struct GLFWwindow;

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
    void drawMenuBar();
    void drawPanes();
    void drawUnsavedModal();
    void handleShortcuts();
    void handleDrops();

    void markChanged(bool eval);
    void updateTitle();
    int leftImageNode() const;
    int previewTarget() const;
    void updateTextures();
    bool saveFramebuffer(const std::string& path);
    nlohmann::json uiState() const;
    void applyUiState(const nlohmann::json& j);

    GLFWwindow* window_ = nullptr;
    ImageCache cache_;
    std::unique_ptr<AsyncEvaluator> eval_;
    Graph graph_;
    NodeEditor editor_;

    GLTexture leftTex_, rightTex_;
    ImagePtr leftShown_;
    ViewState view_;

    int selected_ = 0;
    int preview_ = 0;
    std::string projectPath_;
    bool modified_ = false;
    bool evalDirty_ = true;
    std::string lastTitle_;

    std::string status_;
    std::string evalError_;
    double evalMs_ = 0;

    float leftFrac_ = 0.28f, rightFrac_ = 0.28f, editorFrac_ = 0.66f;

    Pending pending_ = Pending::None;
    bool openUnsavedModal_ = false;
    bool quit_ = false;
    std::vector<std::string> drops_;

    friend void dropCallback(GLFWwindow*, int, const char**);
    friend void closeCallback(GLFWwindow*);
};
