#version 450
layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec3 vertexColor;
layout(location = 0) out vec4 outColor;
layout(std140, set = 0, binding = 0) uniform Scene {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 viewDirection;
    vec4 ambient;
    vec4 lightDirection;
    vec4 lightColor;
    uvec4 lightCounts;
} scene;
layout(std140, set = 1, binding = 0) uniform Model {
    mat4 model;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specular;
} object;
struct PointLight {
    vec4 position;
    vec4 colorIntensity;
};
struct SpotLight {
    vec4 position;
    vec4 direction;
    vec4 colorIntensity;
    vec4 cone;
};
layout(std430, set = 0, binding = 1) readonly buffer Points {
    PointLight lights[];
} points;
layout(std430, set = 0, binding = 2) readonly buffer Spots {
    SpotLight lights[];
} spots;

vec3 safeNormalize(vec3 v) {
    return v / sqrt(max(dot(v, v), 0.000001));
}
vec3 blinnPhong(vec3 N, vec3 V, vec3 L, vec3 base, vec3 radiance) {
    float diffuse = max(dot(N, L), 0.0);
    if (diffuse <= 0.0) return vec3(0.0);
    vec3 H = safeNormalize(L + V);
    float specular = pow(max(dot(N, H), 0.0), object.specular.w);
    return (base * diffuse + object.specular.rgb * specular) * radiance;
}
void main() {
    vec3 N = safeNormalize(worldNormal);
    vec3 V = scene.cameraPosition.w > 0.5
        ? safeNormalize(scene.cameraPosition.xyz - worldPosition)
        : -scene.viewDirection.xyz;
    vec3 base = object.albedo.rgb;
    if (object.albedo.w > 0.5) base *= vertexColor;
    vec3 result = base * scene.ambient.rgb;
    result += blinnPhong(N, V, -scene.lightDirection.xyz, base, scene.lightColor.rgb);
    for (uint i = 0; i < scene.lightCounts.x; ++i) {
        PointLight light = points.lights[i];
        vec3 delta = light.position.xyz - worldPosition;
        // Защита от деления на ноль непосредственно в позиции источника.
        float distanceSquared = max(dot(delta, delta), 0.0001);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w / distanceSquared;
        result += blinnPhong(N, V, safeNormalize(delta), base, radiance);
    }
    for (uint i = 0; i < scene.lightCounts.y; ++i) {
        SpotLight light = spots.lights[i];
        vec3 delta = light.position.xyz - worldPosition;
        vec3 L = safeNormalize(delta);
        float theta = dot(-L, light.direction.xyz);
        float innerCos = light.cone.x;
        float outerCos = light.cone.y;
        float coneFactor;
        if (innerCos - outerCos > 0.000001)
            coneFactor = clamp((theta - outerCos) / (innerCos - outerCos), 0.0, 1.0);
        else
            coneFactor = step(outerCos, theta);
        float distanceSquared = max(dot(delta, delta), 0.0001);
        vec3 radiance = light.colorIntensity.rgb * light.colorIntensity.w * coneFactor / distanceSquared;
        result += blinnPhong(N, V, L, base, radiance);
    }
    outColor = vec4(result, 1.0);
}

