#include "main.h"
#include "preview.h"
#include <cstring>
#include "profile_log/logCore.hpp"

#pragma warning(push)
#pragma warning(disable:4996)

static std::string startTimeString;

// For camera controls
static bool leftMousePressed = false;
static bool rightMousePressed = false;
static bool middleMousePressed = false;
static double lastX;
static double lastY;

static bool camchanged = true;
static float dtheta = 0, dphi = 0;
static glm::vec3 cammove;

float zoom, theta, phi;
glm::vec3 cameraPosition;
glm::vec3 ogLookAt; // for recentering the camera

Scene *scene;
RenderState *renderState;
int iteration;
bool paused;

int width;
int height;

#if ENABLE_CACHE_FIRST_INTERSECTION
extern bool cacheFirstIntersection;
extern bool firstIntersectionCached;
#endif // ENABLE_CACHE_FIRST_INTERSECTION
#if ENABLE_PROFILE_LOG
bool saveProfileLog = false;
#endif // ENABLE_PROFILE_LOG

//-------------------------------
//-------------MAIN--------------
//-------------------------------

extern void unitTest();

int main(int argc, char** argv) {
    startTimeString = currentTimeString();

    std::string sceneFileStr;
    const char* sceneFile = nullptr;

    if (argc < 2) {
        //printf("Usage: %s SCENEFILE.txt\n", argv[0]);
        //return 1;
        //sceneFile = "../scenes/cornell_testOutline.txt";
        //sceneFile = "../scenes/cornell.txt";
        //sceneFile = "../scenes/cornellMF.txt";
        //sceneFile = "../scenes/cornell2.txt";
        //sceneFile = "../scenes/sphere.txt";
        //sceneFile = "../scenes/cornell_ramp.txt";

        //sceneFile = "../scenes/PA_BVH2000.txt";
        //sceneFile = "../scenes/PA_BVH135280.txt";

        //sceneFile = "../scenes/cornell_garage_kit.txt";
        //sceneFile = "../scenes/cornell_garage_kit_microfacet.txt";
        std::cout << "Input sceneFile: " << std::flush;
        std::cin >> sceneFileStr;
        sceneFile = sceneFileStr.c_str();
    }
    else {
        sceneFile = argv[1];
    }
#if ENABLE_PROFILE_LOG
    if (argc < 3) {
        std::cout << "Save profile log? (1/0): " << std::flush;
        std::cin >> saveProfileLog;
    }
    else {
        saveProfileLog = atoi(argv[2]);
    }
#endif // ENABLE_PROFILE_LOG

    // Load scene file
    scene = new Scene(sceneFile);

    // Set up camera stuff from loaded path tracer settings
    iteration = 0;
    renderState = &scene->state;
    Camera &cam = renderState->camera;
    width = cam.resolution.x;
    height = cam.resolution.y;

    glm::vec3 view = cam.view;
    glm::vec3 up = cam.up;
    glm::vec3 right = glm::cross(view, up);
    up = glm::cross(right, view);

    cameraPosition = cam.position;

    // compute phi (horizontal) and theta (vertical) relative 3D axis
    // so, (0 0 1) is forward, (0 1 0) is up
    glm::vec3 viewXZ = glm::vec3(view.x, 0.0f, view.z);
    glm::vec3 viewZY = glm::vec3(0.0f, view.y, view.z);
    phi = glm::acos(glm::dot(glm::normalize(viewXZ), glm::vec3(0, 0, -1)));
    theta = glm::acos(glm::dot(glm::normalize(viewZY), glm::vec3(0, 1, 0)));
    ogLookAt = cam.lookAt;
    zoom = glm::length(cam.position - ogLookAt);

    // Initialize CUDA and GL components
    init();

    unitTest();

    // GLFW main loop
    mainLoop();

    delete scene;
    pathtraceFree();
    cudaDeviceSynchronize();
    return 0;
}

void saveImage() {
    float samples = static_cast<float>(iteration);
    // output image file
    Image::image img(width, height);

    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            int index = x + (y * width);
            glm::vec3 pix = renderState->image[index];
#if false//!PREGATHER_FINAL_IMAGE
            img.setPixel(width - 1 - x, y, glm::vec3(pix) / samples);
#else // PREGATHER_FINAL_IMAGE
            img.setPixel(width - 1 - x, y, glm::vec3(pix));
#endif // PREGATHER_FINAL_IMAGE
        }
    }

    std::string filename = renderState->imageName;
    std::ostringstream ss;
    ss << filename << "." << startTimeString << "." << samples << "samp";
    filename = ss.str();

    // CHECKITOUT
    img.savePNG(filename);
    //img.saveHDR(filename);  // Save a Radiance HDR file
}

void runCuda() {
    if (camchanged) {
        iteration = 0;
        Camera &cam = renderState->camera;
        cameraPosition.x = zoom * sin(phi) * sin(theta);
        cameraPosition.y = zoom * cos(theta);
        cameraPosition.z = zoom * cos(phi) * sin(theta);

        cam.view = -glm::normalize(cameraPosition);
        glm::vec3 v = cam.view;
        glm::vec3 u = glm::vec3(0, 1, 0);//glm::normalize(cam.up);
        glm::vec3 r = glm::cross(v, u);
        cam.up = glm::cross(r, v);
        cam.right = r;

        cam.position = cameraPosition;
        cameraPosition += cam.lookAt;
        cam.position = cameraPosition;
        camchanged = false;
    }

    // Map OpenGL buffer object for writing from CUDA on a single GPU
    // No data is moved (Win & Linux). When mapped to CUDA, OpenGL should not use this buffer

    if (iteration == 0) {
        pathtraceFree();
        pathtraceInit(scene);
    }

    if (static_cast<unsigned int>(iteration) < renderState->iterations) {
        uchar4 *pbo_dptr = NULL;
        iteration++;
        cudaGLMapBufferObject((void**)&pbo_dptr, pbo);

        // execute the kernel
        int frame = 0;
        pathtrace(pbo_dptr, frame, iteration);

        // unmap buffer object
        cudaGLUnmapBufferObject(pbo);
    } else {
        saveImage();
        pathtraceFree();
        cudaDeviceReset();
        exit(EXIT_SUCCESS);
    }
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    if (action == GLFW_PRESS) {
        switch (key) {
        case GLFW_KEY_P:
            paused = !paused;
            break;
        case GLFW_KEY_ESCAPE:
            saveImage();
            glfwSetWindowShouldClose(window, GL_TRUE);
            break;
        case GLFW_KEY_S:
            saveImage();
            break;
        case GLFW_KEY_SPACE:
            camchanged = true;
            renderState = &scene->state;
            renderState->camera.lookAt = ogLookAt;
            break;
        case GLFW_KEY_UP:
            camchanged = true;
            //renderState = &scene->state;
            //renderState->camera.lookAt = ogLookAt;
            ++renderState->traceDepth;
            break;
        case GLFW_KEY_DOWN:
            camchanged = true;
            //renderState = &scene->state;
            //renderState->camera.lookAt = ogLookAt;
            renderState->traceDepth = std::max(0, renderState->traceDepth - 1);
            break;
        case GLFW_KEY_RIGHT:
            camchanged = true;
            //renderState = &scene->state;
            //renderState->camera.lookAt = ogLookAt;
            renderState->recordDepth = std::min(renderState->traceDepth, renderState->recordDepth + 1);
            break;
        case GLFW_KEY_LEFT:
            camchanged = true;
            //renderState = &scene->state;
            //renderState->camera.lookAt = ogLookAt;
            renderState->recordDepth = std::max(-1, renderState->recordDepth - 1);
            break;
#if ENABLE_CACHE_FIRST_INTERSECTION
        case GLFW_KEY_C:
            cacheFirstIntersection = !cacheFirstIntersection;
            firstIntersectionCached = false;
            break;
#endif // ENABLE_CACHE_FIRST_INTERSECTION
        case GLFW_KEY_0:
        case GLFW_KEY_1:
        case GLFW_KEY_2:
        case GLFW_KEY_3:
        case GLFW_KEY_4:
        case GLFW_KEY_5:
        case GLFW_KEY_6:
        case GLFW_KEY_7:
        case GLFW_KEY_8:
        case GLFW_KEY_9:
        {
            size_t index = key - GLFW_KEY_0;
            if (index < scene->postprocesses.size()) {
                scene->postprocesses[index].second = !scene->postprocesses[index].second;
            }
        }
            break;
        }
    }
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
  leftMousePressed = (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS);
  rightMousePressed = (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS);
  middleMousePressed = (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS);
}

void mousePositionCallback(GLFWwindow* window, double xpos, double ypos) {
  if (xpos == lastX || ypos == lastY) return; // otherwise, clicking back into window causes re-start
  if (leftMousePressed) {
    // compute new camera parameters
    phi -= static_cast<float>(xpos - lastX) / width;
    theta -= static_cast<float>(ypos - lastY) / height;
    theta = std::fmax(0.001f, std::fmin(theta, PI));
    camchanged = true;
  }
  else if (rightMousePressed) {
    zoom += static_cast<float>(ypos - lastY) / height;
    zoom = std::fmax(0.1f, zoom);
    camchanged = true;
  }
  else if (middleMousePressed) {
    renderState = &scene->state;
    Camera &cam = renderState->camera;
    glm::vec3 forward = cam.view;
    forward.y = 0.0f;
    forward = glm::normalize(forward);
    glm::vec3 right = cam.right;
    right.y = 0.0f;
    right = glm::normalize(right);

    cam.lookAt -= (float) (xpos - lastX) * right * 0.01f;
    cam.lookAt += (float) (ypos - lastY) * forward * 0.01f;
    camchanged = true;
  }
  lastX = xpos;
  lastY = ypos;
}

void unitTest() {
    printf("---Start Unit Test---\n");
    glm::vec3 a(0.f, 1.f, 0.f);
    glm::vec3 b(1.f, -1.f, 2.f);
    glm::vec3 c = glm::max(a, b);
    printf("c = glm::max(<%f,%f,%f>,<%f,%f,%f>) = <%f,%f,%f>\n",
        a.r, a.g, a.b,
        b.r, b.g, b.b,
        c.r, c.g, c.b);
    printf("---End Unit Test---\n");
}

#pragma warning(pop)
