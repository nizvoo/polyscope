#include "polyscope/polyscope.h"

#include <iostream>

#ifdef _WIN32
#undef APIENTRY
#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WGL
#include <GLFW/glfw3native.h>
#endif

#include "imgui.h"
#include "polyscope/imgui_render.h"

#include "polyscope/pick.h"
#include "polyscope/view.h"

namespace polyscope {

// === Declare storage global members

namespace state {

bool initialized = false;

double lengthScale = 1.0;
std::tuple<geometrycentral::Vector3, geometrycentral::Vector3> boundingBox;
Vector3 center{0, 0, 0};

std::map<StructureType, std::map<std::string, Structure*>> structureCategories{
    {StructureType::PointCloud, {}},
    {StructureType::SurfaceMesh, {}},
    {StructureType::CameraView, {}},
    {StructureType::RaySet, {}},
};
std::map<std::string, PointCloud*> pointClouds;
std::map<std::string, SurfaceMesh*> surfaceMeshes;
std::map<std::string, CameraView*> cameraViews;
std::map<std::string, RaySet*> raySets;

std::function<void()> userCallback;
size_t screenshotInd = 0;

} // namespace state

namespace options {

std::string programName = "Polyscope";
int verbosity = 2;
std::string printPrefix = "Polyscope: ";
bool exceptionOnError = true;
bool debugDrawPickBuffer = false;

} // namespace options

// Small callback function for GLFW errors
void error_print_callback(int error, const char* description) {
  std::cerr << "GLFW emitted error: " << description << std::endl;
}

// Forward declare compressed binary font functions
unsigned int getCousineRegularCompressedSize();
const unsigned int* getCousineRegularCompressedData();

// Helper to manage pick buffer
namespace {

// Pick buffer state
GLuint pickFramebuffer, rboPickDepth, rboPickColor, currPickBufferWidth, currPickBufferHeight;

void allocatePickRenderbuffers() {

  glGenRenderbuffers(1, &rboPickDepth);
  glBindRenderbuffer(GL_RENDERBUFFER, rboPickDepth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, view::bufferWidth, view::bufferHeight);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rboPickDepth);

  glGenRenderbuffers(1, &rboPickColor);
  glBindRenderbuffer(GL_RENDERBUFFER, rboPickColor);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, view::bufferWidth, view::bufferHeight);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboPickColor);

  currPickBufferWidth = view::bufferWidth;
  currPickBufferHeight = view::bufferHeight;
}

void initPickBuffer() {

  // Create the new buffer
  glGenFramebuffers(1, &pickFramebuffer);

  // Bind to the new buffer
  glBindFramebuffer(GL_FRAMEBUFFER, pickFramebuffer);

  allocatePickRenderbuffers();
}
}; // namespace

// === Core global functions

void init() {
  if (state::initialized) {
    throw std::logic_error(options::printPrefix + "Initialize called twice");
  }

  // === Initialize glfw
  glfwSetErrorCallback(error_print_callback);
  if (!glfwInit()) {
    throw std::runtime_error(options::printPrefix + "ERROR: Failed to initialize glfw");
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
  imguirender::mainWindow = glfwCreateWindow(1280, 720, options::programName.c_str(), NULL, NULL);
  glfwMakeContextCurrent(imguirender::mainWindow);
  glfwSwapInterval(1); // Enable vsync

  // === Initialize openGL
  // Load openGL functions (using GLAD)
#ifndef __APPLE__
  if (!gladLoadGL()) {
    throw std::runtime_error(options::printPrefix + "ERROR: Failed to load openGL using GLAD");
  }
#endif
  if (options::verbosity > 0) {
    std::cout << options::printPrefix << "Loaded openGL version: " << glGetString(GL_VERSION) << std::endl;
  }

#ifdef __APPLE__
  // Hack to classify the process as interactive
  glfwPollEvents();
#endif

  // Update the width and heigh
  glfwMakeContextCurrent(imguirender::mainWindow);
  glfwGetWindowSize(imguirender::mainWindow, &view::windowWidth, &view::windowHeight);
  glfwGetFramebufferSize(imguirender::mainWindow, &view::bufferWidth, &view::bufferHeight);

  // Set up ImGUI glfw bindings
  imguirender::ImGui_ImplGlfwGL3_Init(imguirender::mainWindow, true);

  ImGuiIO& io = ImGui::GetIO();
  ImFontConfig config;
  config.OversampleH = 5;
  config.OversampleV = 5;
  // io.Fonts->AddFontDefault();
  // io.Fonts->AddFontFromFileTTF(
  //     "../deps/imgui/imgui/extra_fonts/Cousine-Regular.ttf", 15.0f, &config);
  ImFont* font = io.Fonts->AddFontFromMemoryCompressedTTF(getCousineRegularCompressedData(),
                                                          getCousineRegularCompressedSize(), 15.0f, &config);
  // ImGui::StyleColorsLight();

  // Initialize common shaders
  gl::GLProgram::initCommonShaders();

  // Initialize pick buffer
  initPickBuffer();

  state::initialized = true;
}

namespace {


void evaluatePickQuery(int xPos, int yPos) {

  glBindFramebuffer(GL_FRAMEBUFFER, pickFramebuffer);

  if ((int)currPickBufferWidth != view::bufferWidth || (int)currPickBufferHeight != view::bufferHeight) {
    // Delete the existing renderbuffers
    GLuint rBuffers[2] = {rboPickDepth, rboPickColor};
    glDeleteRenderbuffers(2, rBuffers);

    // Allocate some new one
    allocatePickRenderbuffers();
  }

  // Set the draw buffer
  GLenum buffers[] = {GL_COLOR_ATTACHMENT0};
  glDrawBuffers(1, buffers);

  // Clear the pick buffer
  glViewport(0, 0, view::bufferWidth, view::bufferHeight);
  glClearColor(0.0, 0.0, 0.0, 0.0);
  glClearDepth(1.);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  // Render pick buffer
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  for (auto cat : state::structureCategories) {
    for (auto x : cat.second) {
      x.second->drawPick();
    }
  }
  glFlush();
  glFinish();

  // Read from the pick buffer
  float result[4];
  glReadPixels(xPos, view::bufferHeight - yPos, 1, 1, GL_RGBA, GL_FLOAT, &result);
  gl::checkGLError(true);


  size_t ind = pick::vecToInd(geometrycentral::Vector3{result[0], result[1], result[2]});

  if (ind == 0) {
    pick::resetPick();
  } else {
    pick::setCurrentPickElement(ind, ImGui::GetIO().MouseDoubleClicked[0]);
  }
}


float dragDistSinceLastRelease = 0.0;

void processMouseEvents() {
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantCaptureMouse) {

    // Handle drags
    if (ImGui::IsMouseDragging(0)) {
      Vector2 dragDelta{io.MouseDelta.x / view::windowWidth, -io.MouseDelta.y / view::windowHeight};
      view::processMouseDrag(dragDelta, !io.KeyShift);

      dragDistSinceLastRelease += std::abs(dragDelta.x);
      dragDistSinceLastRelease += std::abs(dragDelta.y);
    }
    // Handle picks
    else {

      if (ImGui::IsMouseReleased(0)) {

        ImVec2 dragDelta = ImGui::GetMouseDragDelta(0);
        if (dragDistSinceLastRelease < .01) {
          ImVec2 p = ImGui::GetMousePos();
          evaluatePickQuery(io.DisplayFramebufferScale.x * p.x, io.DisplayFramebufferScale.y * p.y);
        }

        dragDistSinceLastRelease = 0.0;
      }
    }
  }
}

void drawStructures() {
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);

  for (auto cat : state::structureCategories) {
    for (auto x : cat.second) {

      // Draw the pick buffer for debugging purposes
      if (options::debugDrawPickBuffer) {
        x.second->drawPick();
      }
      // The normal case
      else {
        x.second->draw();
      }
    }
  }
}

void buildPolyscopeGui() {
  // Create window
  static bool showPolyscopeWindow = true;
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);

  ImGui::Begin("Polyscope", &showPolyscopeWindow, ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::ColorEdit3("background color", (float*)&view::bgColor, ImGuiColorEditFlags_NoInputs);
  if (ImGui::Button("Reset view")) {
    view::flyToDefault();
  }
  if (ImGui::Button("Screenshot")) {
    screenshot();
  }
  ImGui::Text("%.1f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

  // == Debugging-related options
  ImGui::SetNextTreeNodeOpen(false, ImGuiCond_FirstUseEver);
  if (ImGui::TreeNode("debug")) {
    ImGui::Checkbox("Show pick buffer", &options::debugDrawPickBuffer);
    ImGui::TreePop();
  }

  ImGui::End();
}

void buildStructureGui() {
  // Create window
  static bool showStructureWindow = true;
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  ImGui::Begin("Structures", &showStructureWindow);

  for (auto cat : state::structureCategories) {
    std::string catName = getStructureTypeName(cat.first);
    std::map<std::string, Structure*>& structures = cat.second;

    ImGui::PushID(catName.c_str()); // ensure there are no conflicts with
                                    // identically-named labels

    // Build the structure's UI
    ImGui::SetNextTreeNodeOpen(structures.size() > 0, ImGuiCond_FirstUseEver);
    if (ImGui::CollapsingHeader(("Category: " + catName + " (" + std::to_string(structures.size()) + ")").c_str())) {
      // Draw shared GUI elements for all instances of the structure
      if (structures.size() > 0) {
        structures.begin()->second->drawSharedStructureUI();
      }

      for (auto x : structures) {
        ImGui::SetNextTreeNodeOpen(structures.size() <= 2,
                                   ImGuiCond_FirstUseEver); // closed by default if more than 2
        x.second->drawUI();
      }
    }

    ImGui::PopID();
  }

  ImGui::End();
}

void buildUserGui() {
  if (state::userCallback) {
    ImGui::PushID("user_callback");
    state::userCallback();
    ImGui::PopID();
  }
}

void buildPickGui() {
  if (pick::haveSelection) {
    ImGui::Begin("Selection", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    size_t pickInd;
    Structure* structure = pick::getCurrentPickElement(pickInd);

    ImGui::TextUnformatted((getStructureTypeName(structure->type) + ": " + structure->name).c_str());
    ImGui::Separator();
    structure->drawPickUI(pickInd);

    ImGui::End();
  }
}
} // namespace

bool checkStructureNameInUse(std::string name, bool throwError = true) {
  for (const auto cat : state::structureCategories) {
    if (cat.second.find(name) != cat.second.end()) {
      if (throwError) {
        error("Structure name " + name + " is already in use.");
      }
      return true;
    }
  }

  return false;
}

void draw(bool withUI = true) {

  // Ensure the default framebuffer is bound
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Clear out the gui
  glViewport(0, 0, view::bufferWidth, view::bufferHeight);
  glClearColor(view::bgColor[0], view::bgColor[1], view::bgColor[2], view::bgColor[3]);
  glClearDepth(1.);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  // Draw structures in the scene
  drawStructures();

  // Draw the GUI
  if (withUI) {
    ImGui::Render();
  }

  glfwSwapBuffers(imguirender::mainWindow);
}


void show() {
  view::resetCameraToDefault();

  // Main loop
  while (!glfwWindowShouldClose(imguirender::mainWindow)) {
    // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to
    // tell if dear imgui wants to use your inputs.
    // - When io.WantCaptureMouse is true, do not dispatch mouse input data to
    // your main application.
    // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input
    // data to your main application. Generally you may always pass all inputs
    // to dear imgui, and hide them from your application based on those two
    // flags.

    // Update the width and heigh
    glfwMakeContextCurrent(imguirender::mainWindow);
    glfwGetWindowSize(imguirender::mainWindow, &view::windowWidth, &view::windowHeight);
    glfwGetFramebufferSize(imguirender::mainWindow, &view::bufferWidth, &view::bufferHeight);

    glfwPollEvents();
    imguirender::ImGui_ImplGlfwGL3_NewFrame();

    processMouseEvents();

    // Build the GUI components
    ImGui::ShowTestWindow();
    buildPolyscopeGui();
    buildStructureGui();
    buildUserGui();
    buildPickGui();

    // Process UI events

    // TODO handle picking if needed

    // === Rendering
    draw();
  }
}

void registerPointCloud(std::string name, const std::vector<Vector3>& points, bool replaceIfPresent) {
  bool inUse = checkStructureNameInUse(name, !replaceIfPresent);
  if (inUse) {
    removeStructure(name);
  }

  state::pointClouds[name] = new PointCloud(name, points);
  state::structureCategories[StructureType::PointCloud][name] = state::pointClouds[name];

  updateStructureExtents();
}

void registerSurfaceMesh(std::string name, Geometry<Euclidean>* geom, bool replaceIfPresent) {
  bool inUse = checkStructureNameInUse(name, !replaceIfPresent);
  if (inUse) {
    removeStructure(name);
  }

  state::surfaceMeshes[name] = new SurfaceMesh(name, geom);
  state::structureCategories[StructureType::SurfaceMesh][name] = state::surfaceMeshes[name];

  updateStructureExtents();
}

void registerCameraView(std::string name, CameraParameters p, bool replaceIfPresent) {
  bool inUse = checkStructureNameInUse(name, !replaceIfPresent);
  if (inUse) {
    removeStructure(name);
  }

  state::cameraViews[name] = new CameraView(name, p);
  state::structureCategories[StructureType::CameraView][name] = state::cameraViews[name];

  updateStructureExtents();
}

void registerRaySet(std::string name, const std::vector<std::vector<RayPoint>>& r, bool replaceIfPresent) {
  bool inUse = checkStructureNameInUse(name, !replaceIfPresent);
  if (inUse) {
    removeStructure(name);
  }

  state::raySets[name] = new RaySet(name, r);
  state::structureCategories[StructureType::RaySet][name] = state::raySets[name];

  updateStructureExtents();
}

PointCloud* getPointCloud(std::string name) {
  if (state::pointClouds.find(name) == state::pointClouds.end()) {
    error("No point cloud with name " + name + " registered");
    return nullptr;
  }
  return state::pointClouds[name];
}

SurfaceMesh* getSurfaceMesh(std::string name) {
  if (state::surfaceMeshes.find(name) == state::surfaceMeshes.end()) {
    error("No surface mesh with name " + name + " registered");
    return nullptr;
  }
  return state::surfaceMeshes[name];
}

CameraView* getCameraView(std::string name) {
  if (state::cameraViews.find(name) == state::cameraViews.end()) {
    error("No camera view with name " + name + " registered");
    return nullptr;
  }
  return state::cameraViews[name];
}

RaySet* getRaySet(std::string name) {
  if (state::raySets.find(name) == state::raySets.end()) {
    error("No ray set with name " + name + " registered");
    return nullptr;
  }
  return state::raySets[name];
}

void removeStructure(std::string name) {
  // Point cloud
  if (state::pointClouds.find(name) != state::pointClouds.end()) {
    pick::clearPickIfStructureSelected(state::pointClouds.find(name)->second);
    delete state::pointClouds[name];
    state::pointClouds.erase(name);
    state::structureCategories[StructureType::PointCloud].erase(name);
    updateStructureExtents();
    return;
  }

  // Surface mesh
  if (state::surfaceMeshes.find(name) != state::surfaceMeshes.end()) {
    pick::clearPickIfStructureSelected(state::surfaceMeshes.find(name)->second);
    delete state::surfaceMeshes[name];
    state::surfaceMeshes.erase(name);
    state::structureCategories[StructureType::SurfaceMesh].erase(name);
    updateStructureExtents();
    return;
  }

  // Camera view
  if (state::cameraViews.find(name) != state::cameraViews.end()) {
    pick::clearPickIfStructureSelected(state::cameraViews.find(name)->second);
    delete state::cameraViews[name];
    state::cameraViews.erase(name);
    state::structureCategories[StructureType::CameraView].erase(name);
    updateStructureExtents();
    return;
  }

  // Ray set
  if (state::raySets.find(name) != state::raySets.end()) {
    pick::clearPickIfStructureSelected(state::raySets.find(name)->second);
    delete state::raySets[name];
    state::raySets.erase(name);
    state::structureCategories[StructureType::RaySet].erase(name);
    updateStructureExtents();
    return;
  }

  error("No structure named: " + name + " to remove.");
}

void removeAllStructures() {
  for (auto x : state::pointClouds) delete x.second;
  for (auto x : state::surfaceMeshes) delete x.second;
  state::pointClouds.clear();
  state::surfaceMeshes.clear();
  state::structureCategories.clear();
  updateStructureExtents();
  pick::resetPick();
}

void updateStructureExtents() {
  // Compute length scale and bbox as the max of all structures
  state::lengthScale = 0.0;
  Vector3 minBbox = Vector3{1, 1, 1} * std::numeric_limits<double>::infinity();
  Vector3 maxBbox = -Vector3{1, 1, 1} * std::numeric_limits<double>::infinity();

  for (auto cat : state::structureCategories) {
    for (auto x : cat.second) {
      state::lengthScale = std::max(state::lengthScale, x.second->lengthScale());
      auto bbox = x.second->boundingBox();
      minBbox = geometrycentral::componentwiseMin(minBbox, std::get<0>(bbox));
      maxBbox = geometrycentral::componentwiseMax(maxBbox, std::get<1>(bbox));
    }
  }

  if (!minBbox.isFinite() || !maxBbox.isFinite()) {
    minBbox = -Vector3{1, 1, 1};
    maxBbox = Vector3{1, 1, 1};
  }
  std::get<0>(state::boundingBox) = minBbox;
  std::get<1>(state::boundingBox) = maxBbox;

  // If we got a bounding box but not a length scale we can use the size of the
  // box as a scale. If we got neither, we'll end up with a constant near 1 due
  // to the above correction
  if (state::lengthScale == 0) {
    state::lengthScale = norm(maxBbox - minBbox);
  }

  // Center is center of bounding box
  state::center = 0.5 * (minBbox + maxBbox);
}

void error(std::string message) {
  if (options::exceptionOnError) {
    throw std::logic_error(options::printPrefix + message);
  } else {
    std::cout << options::printPrefix << message << std::endl;
  }
}

void screenshot(std::string filename) {

  // Make sure we render first
  draw(false);

  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  int w = viewport[2];
  int h = viewport[3];


  size_t buffSize = w * h * 3;
  unsigned char* buff = new unsigned char[buffSize];
  glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buff);
  saveImage(filename, buff, w, h, 3);

  delete[] buff;
}

void screenshot() {

  char buff[50];
  snprintf(buff, 50, "screenshot_%06zu.png", state::screenshotInd);
  std::string defaultName(buff);

  screenshot(defaultName);

  state::screenshotInd++;
}

// Helpers/data for the color palette below
namespace {
static int iPaletteColor = -1;
std::vector<std::array<float, 3>> paletteColors{
    {{171 / 255., 71 / 255., 188 / 255.}}, // purple
    {{66 / 255., 165 / 255., 245 / 255.}}, // light blue
    {{38 / 255., 166 / 255., 154 / 255.}}, // greenish
    {{255 / 255., 167 / 255., 38 / 255.}}, // orange
    {{38 / 255., 198 / 255., 218 / 255.}}  // teal
};
} // anonymous namespace
std::array<float, 3> getNextPaletteColor() {
  // -1 means initialization needed
  if (iPaletteColor == -1) {
    iPaletteColor = randomInt(0, paletteColors.size() - 1);
  }

  std::array<float, 3> color = paletteColors[iPaletteColor];
  iPaletteColor = (iPaletteColor + 1) % paletteColors.size();
  return color;
}

} // namespace polyscope
