#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>
#include <glm/glm.hpp>

namespace application {
struct ConeVertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec3 normal;
};
struct ConeMesh {
    std::vector<ConeVertex> vertices;
    std::vector<uint32_t> indices;
};
inline constexpr uint32_t cone_segments = 64;
inline constexpr float cone_radius = 1.0f, cone_height = 2.0f;

inline glm::vec3 vertexColor(const glm::vec3& p) {
    return glm::clamp(glm::vec3(0.5f) + p / glm::vec3(2 * cone_radius, cone_height, 2 * cone_radius),
                      glm::vec3(0), glm::vec3(1));
}
inline ConeMesh makeConeMesh() {
    ConeMesh mesh;
    const glm::vec3 top(0, cone_height / 2, 0), center(0, -cone_height / 2, 0), down(0, -1, 0);
    // Основание и боковая поверхность не делят вершины: у них разные нормали.
    for (uint32_t i = 0; i < cone_segments; ++i) {
        const float a = 2 * std::numbers::pi_v<float> * float(i) / float(cone_segments);
        const float b = 2 * std::numbers::pi_v<float> * float(i + 1) / float(cone_segments);
        const glm::vec3 p(cone_radius * std::cos(a), center.y, cone_radius * std::sin(a));
        const glm::vec3 q(cone_radius * std::cos(b), center.y, cone_radius * std::sin(b));
        const glm::vec3 np = glm::normalize(glm::vec3(std::cos(a), cone_radius / cone_height, std::sin(a)));
        const glm::vec3 nq = glm::normalize(glm::vec3(std::cos(b), cone_radius / cone_height, std::sin(b)));
        // В верхушке нормаль неоднозначна: берём среднюю для данного сектора.
        const glm::vec3 nt = glm::normalize(np + nq);
        const uint32_t first = uint32_t(mesh.vertices.size());
        mesh.vertices.insert(mesh.vertices.end(), {
            {top, vertexColor(top), nt}, {q, vertexColor(q), nq}, {p, vertexColor(p), np},
            {center, vertexColor(center), down}, {p, vertexColor(p), down}, {q, vertexColor(q), down}
        });
        for (uint32_t j = 0; j < 6; ++j) mesh.indices.push_back(first + j);
    }
    return mesh;
}
} // namespace application

