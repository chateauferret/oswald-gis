#include "CameraController.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

constexpr float pitchLimit = 1.45F;
constexpr double doubleClickSeconds = 0.35;
constexpr double doubleClickDistance = 6.0;

} // namespace

void CameraController::HandleMouseButton(bool pressed, double cursorX, double cursorY,
                                         double time, int viewportWidth, int viewportHeight,
                                         bool captureMouse, bool allowRecentering) {
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY) || !std::isfinite(time)) {
        dragging_ = false;
        lastClickTime_ = -1.0;
        return;
    }
    if (!pressed) {
        dragging_ = false;
        return;
    }
    if (captureMouse) {
        dragging_ = false;
        lastClickTime_ = -1.0;
        return;
    }

    const double dx = cursorX - lastClickX_;
    const double dy = cursorY - lastClickY_;
    if (allowRecentering && lastClickTime_ >= 0.0 && time >= lastClickTime_ &&
        time - lastClickTime_ <= doubleClickSeconds &&
        dx * dx + dy * dy <= doubleClickDistance * doubleClickDistance) {
        Recenter(cursorX, cursorY, viewportWidth, viewportHeight);
        lastClickTime_ = -1.0;
        dragging_ = false;
        return;
    }

    lastClickTime_ = allowRecentering ? time : -1.0;
    lastClickX_ = cursorX;
    lastClickY_ = cursorY;
    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
    dragging_ = true;
}

void CameraController::HandleCursorPosition(double cursorX, double cursorY,
                                            bool buttonDown, bool captureMouse) {
    if (!std::isfinite(cursorX) || !std::isfinite(cursorY)) {
        dragging_ = false;
        lastClickTime_ = -1.0;
        return;
    }
    if (!buttonDown || !dragging_) {
        return;
    }
    if (captureMouse) {
        dragging_ = false;
        lastClickTime_ = -1.0;
        return;
    }

    const double dx = cursorX - lastCursorX_;
    const double dy = cursorY - lastCursorY_;
    if (dx * dx + dy * dy > 1.0) {
        lastClickTime_ = -1.0;
    }
    constexpr float rotationPerPixel = 0.005F;
    yaw_ += static_cast<float>(dx) * rotationPerPixel;
    pitch_ = std::clamp(pitch_ + static_cast<float>(dy) * rotationPerPixel,
                        -pitchLimit, pitchLimit);
    lastCursorX_ = cursorX;
    lastCursorY_ = cursorY;
}

void CameraController::HandleScroll(double yOffset, bool captureMouse) {
    if (captureMouse || !std::isfinite(yOffset)) {
        return;
    }
    zoom_ = std::clamp(zoom_ * std::exp(static_cast<float>(yOffset) * 0.12F), 0.4F, 4.0F);
}

void CameraController::Update(float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F) {
        return;
    }
    if (autoSpin_) {
        yaw_ += glm::radians(spinRate_ * deltaSeconds);
    }
}

void CameraController::Reset() {
    yaw_ = 0.0F;
    pitch_ = 0.0F;
    zoom_ = 1.0F;
    dragging_ = false;
    lastClickTime_ = -1.0;
}

void CameraController::SetAutoSpin(bool enabled) {
    autoSpin_ = enabled;
}

void CameraController::SetSpinRate(float degreesPerSecond) {
    if (!std::isfinite(degreesPerSecond) || degreesPerSecond < 0.0F) {
        throw std::invalid_argument("Camera spin rate must be finite and nonnegative.");
    }
    spinRate_ = degreesPerSecond;
}

glm::mat4 CameraController::GetModelMatrix() const {
    const auto yaw = glm::rotate(glm::mat4(1.0F), yaw_, glm::vec3(0.0F, 0.0F, 1.0F));
    const auto pitch = glm::rotate(glm::mat4(1.0F), pitch_, glm::vec3(1.0F, 0.0F, 0.0F));
    return pitch * yaw;
}

bool CameraController::Recenter(double cursorX, double cursorY,
                                int viewportWidth, int viewportHeight) {
    if (viewportWidth <= 0 || viewportHeight <= 0 || !std::isfinite(cursorX) ||
        !std::isfinite(cursorY)) {
        return false;
    }

    const float aspect = static_cast<float>(viewportWidth) / viewportHeight;
    const float horizontal = 1.15F * std::max(aspect, 1.0F) / zoom_;
    const float vertical = 1.15F * std::max(1.0F / aspect, 1.0F) / zoom_;
    const auto projection = glm::ortho(-horizontal, horizontal, -vertical, vertical, 0.1F, 10.0F);
    const auto view = glm::lookAt(glm::vec3(0.0F, -3.0F, 0.0F), glm::vec3(0.0F),
                                  glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::mat4 inverse = glm::inverse(projection * view * GetModelMatrix());
    const float ndcX = static_cast<float>(2.0 * cursorX / viewportWidth - 1.0);
    const float ndcY = static_cast<float>(1.0 - 2.0 * cursorY / viewportHeight);
    glm::vec4 nearPoint = inverse * glm::vec4(ndcX, ndcY, -1.0F, 1.0F);
    glm::vec4 farPoint = inverse * glm::vec4(ndcX, ndcY, 1.0F, 1.0F);
    if (nearPoint.w == 0.0F || farPoint.w == 0.0F) {
        return false;
    }
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    const glm::vec3 origin(nearPoint);
    const glm::vec3 direction = glm::vec3(farPoint) - origin;
    const float a = glm::dot(direction, direction);
    const float b = glm::dot(origin, direction);
    const float c = glm::dot(origin, origin) - 1.0F;
    const float discriminant = b * b - a * c;
    if (a <= 0.0F || discriminant < 0.0F) {
        return false;
    }

    const float root = std::sqrt(discriminant);
    float t = (-b - root) / a;
    if (t < 0.0F || t > 1.0F) {
        t = (-b + root) / a;
    }
    if (t < 0.0F || t > 1.0F) {
        return false;
    }

    const glm::vec3 hit = glm::normalize(origin + t * direction);
    const float horizontalRadius = std::hypot(hit.x, hit.y);
    if (horizontalRadius > std::numeric_limits<float>::epsilon()) {
        yaw_ = std::atan2(-hit.x, -hit.y);
    }
    pitch_ = std::clamp(std::atan2(hit.z, horizontalRadius), -pitchLimit, pitchLimit);
    return true;
}