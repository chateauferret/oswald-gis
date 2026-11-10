#pragma once

#include <glm/glm.hpp>

class CameraController {
public:
    void HandleMouseButton(bool pressed, double cursorX, double cursorY, double time,
                           int viewportWidth, int viewportHeight, bool captureMouse,
                           bool allowRecentering = true);
    void HandleCursorPosition(double cursorX, double cursorY, bool buttonDown,
                              bool captureMouse);
    void HandleScroll(double yOffset, bool captureMouse);

    glm::mat4 GetModelMatrix() const;
    float GetZoom() const { return zoom_; }
    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }

private:
    bool Recenter(double cursorX, double cursorY, int viewportWidth, int viewportHeight);

    float yaw_ = 0.0F;
    float pitch_ = 0.0F;
    float zoom_ = 1.0F;
    double lastCursorX_ = 0.0;
    double lastCursorY_ = 0.0;
    double lastClickX_ = 0.0;
    double lastClickY_ = 0.0;
    double lastClickTime_ = -1.0;
    bool dragging_ = false;
};