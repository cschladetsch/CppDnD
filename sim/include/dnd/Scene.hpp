#pragma once
#include <string>

#include "dnd/Json.hpp"

namespace dnd {

// A camera in the scene. World units are grid squares (one square is 5 ft): x runs east, y runs south (grid rows),
// z is up from the floor. yaw is degrees clockwise from north (0 looks up the grid, 90 looks east); pitch is degrees
// above the horizon (negative looks down); fov is the vertical field of view in degrees.
struct Camera {
    double x = 0, y = 0, z = 1.06, yaw = 0, pitch = 0, fov = 70;

    static Camera FromJson(const Json& j, const Camera& fallback);  // missing or bad fields keep the fallback; the result is clamped
    static Camera FromJson(const Json& j);
    Json ToJson() const;
    void Clamp();
};

// The scene as a camera sees it: a floor grid with walls and cover, and the people standing on it. Built from Game::View():
// in combat it is the real battlefield; otherwise it is an open 16x12 field with the party at the south end and the people
// of the place at the north. The default camera is the controlled hero's eyes, turned toward the nearest foe (or person).
//   {mode, grid: {width, height, rows}, props: [{id, name, team: party|foe|npc, kind, x, y, life, hero}],
//    hero: {id, name}, location: {name, description}, camera: {...}}
Json BuildScene(const Json& view);

// Where a world point lands in a camera's picture: sx and sy in [-1, 1] across the frame (sx right, sy up), depth along the
// view direction. False if the point is behind the camera.
bool Project(const Camera& cam, double aspect, double wx, double wy, double wz, double& sx, double& sy, double& depth);

// Words for the camera itself: shot angle, lens, the way it faces.
std::string CameraWords(const Camera& cam);

// A prompt subject for what this camera sees in the scene: camera words, the place, then who is in view and where.
std::string DescribeView(const Json& scene, const Camera& cam, double aspect = 16.0 / 9.0);

}  // namespace dnd
