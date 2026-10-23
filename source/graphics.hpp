#pragma once
#include "cone_mesh.hpp"
#include "gpu_data.hpp"
#include "graphics_internal.hpp"
#include "light_markers.hpp"

namespace graphics {
bool initializeCone(const application::ConeMesh& mesh);
bool renderScene(const internal::FrameData& frame,
                 const SceneUniform& scene, const ModelUniform& model,
                 const std::vector<PointLightGpu>& points, const std::vector<SpotLightGpu>& spots,
                 const application::LightMarkers& markers);
void shutdownCone();
} // namespace graphics

