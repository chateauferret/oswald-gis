#include "CameraController.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testDragAndCapture() {
    CameraController camera;
    camera.HandleMouseButton(true, 100.0, 100.0, 1.0, 800, 600, false);
    camera.HandleCursorPosition(200.0, 140.0, true, false);
    check(std::abs(camera.GetYaw() - 0.5F) < 1e-6F, "Horizontal drag did not rotate yaw.");
    check(std::abs(camera.GetPitch() - 0.2F) < 1e-6F, "Vertical drag did not rotate pitch.");
    camera.HandleMouseButton(false, 200.0, 140.0, 1.1, 800, 600, false);
    const auto rotated = camera.GetModelMatrix();
    check(std::abs(rotated[0][0] - 1.0F) > 1e-3F, "Camera rotation matrix remained unchanged.");

    camera.HandleMouseButton(true, 200.0, 140.0, 2.0, 800, 600, true);
    camera.HandleCursorPosition(500.0, 500.0, true, true);
    check(std::abs(camera.GetYaw() - 0.5F) < 1e-6F &&
          std::abs(camera.GetPitch() - 0.2F) < 1e-6F,
          "Captured UI input changed the globe rotation.");
}

void testPitchClampAndZoom() {
    CameraController camera;
    camera.HandleMouseButton(true, 0.0, 0.0, 1.0, 800, 600, false);
    camera.HandleCursorPosition(0.0, 1000.0, true, false);
    check(std::abs(camera.GetPitch() - 1.45F) < 1e-6F, "Pitch exceeded its positive limit.");
    camera.HandleCursorPosition(0.0, -1000.0, true, false);
    check(std::abs(camera.GetPitch() + 1.45F) < 1e-6F, "Pitch exceeded its negative limit.");

    camera.HandleScroll(1.0, false);
    check(camera.GetZoom() > 1.0F, "Positive wheel input did not zoom in.");
    camera.HandleScroll(-100.0, false);
    check(std::abs(camera.GetZoom() - 0.4F) < 1e-6F, "Zoom lower bound was not enforced.");
    camera.HandleScroll(100.0, false);
    check(std::abs(camera.GetZoom() - 4.0F) < 1e-6F, "Zoom upper bound was not enforced.");
    const float capturedZoom = camera.GetZoom();
    camera.HandleScroll(-2.0, true);
    check(camera.GetZoom() == capturedZoom, "Captured UI scroll changed the zoom.");
}

void testMiddleButtonNavigation() {
    CameraController camera;
    camera.HandleMouseButton(true, 400, 300, 1.0, 800, 600, false, false);
    camera.HandleCursorPosition(450, 320, true, false);
    check(std::abs(camera.GetYaw() - 0.25F) < 1e-6F &&
          std::abs(camera.GetPitch() - 0.1F) < 1e-6F,
          "Middle-button drag did not rotate the globe.");
    camera.HandleMouseButton(false, 450, 320, 1.05, 800, 600, false, false);
    camera.HandleMouseButton(true, 450, 320, 1.2, 800, 600, false, false);
    check(std::abs(camera.GetYaw() - 0.25F) < 1e-6F &&
          std::abs(camera.GetPitch() - 0.1F) < 1e-6F,
          "Repeated middle clicks recentered the globe.");
    camera.HandleCursorPosition(470, 320, true, false);
    check(std::abs(camera.GetYaw() - 0.35F) < 1e-6F,
          "Repeated middle clicks prevented further dragging.");
    camera.HandleMouseButton(false, 470, 320, 1.3, 800, 600, false, false);
    camera.HandleCursorPosition(500, 350, false, false);
    check(std::abs(camera.GetYaw() - 0.35F) < 1e-6F,
          "Middle-button release did not stop dragging.");
    camera.HandleMouseButton(true, 470, 320, 2.0, 800, 600, true, false);
    camera.HandleCursorPosition(500, 350, true, true);
    check(std::abs(camera.GetYaw() - 0.35F) < 1e-6F,
          "Captured middle-button input rotated the globe.");
}

void testDoubleClickRecentering() {
    CameraController camera;
    constexpr int width = 800;
    constexpr int height = 600;
    camera.HandleMouseButton(true, 400.0, 300.0, 1.0, width, height, false);
    camera.HandleMouseButton(false, 400.0, 300.0, 1.05, width, height, false);
    camera.HandleMouseButton(true, 400.0, 300.0, 1.2, width, height, false);
    check(std::abs(camera.GetYaw()) < 1e-6F && std::abs(camera.GetPitch()) < 1e-6F,
          "Center double-click did not leave the centered view unchanged.");

    CameraController offCenter;
    offCenter.HandleMouseButton(true, 520.0, 300.0, 2.0, width, height, false);
    offCenter.HandleMouseButton(false, 520.0, 300.0, 2.05, width, height, false);
    offCenter.HandleMouseButton(true, 520.0, 300.0, 2.2, width, height, false);
    check(std::abs(offCenter.GetYaw()) > 0.01F,
          "Double-click on the visible globe did not recenter the view.");

    CameraController miss;
    miss.HandleMouseButton(true, 20.0, 20.0, 3.0, width, height, false);
    miss.HandleMouseButton(false, 20.0, 20.0, 3.05, width, height, false);
    miss.HandleMouseButton(true, 20.0, 20.0, 3.2, width, height, false);
    check(std::abs(miss.GetYaw()) < 1e-6F && std::abs(miss.GetPitch()) < 1e-6F,
          "Ray miss unexpectedly changed the camera.");
}

} // namespace

int main() {
    try {
        testDragAndCapture();
        testPitchClampAndZoom();
        testMiddleButtonNavigation();
        testDoubleClickRecentering();
        std::cout << "Camera controller tests passed.\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Camera controller test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}