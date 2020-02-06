// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved
// STL
#include <random>
#include <cmath>

#include <EGL.h>
#include <PTexLib.h>
#include <pangolin/image/image_convert.h>

#include "MirrorRenderer.h"

struct Room{
  double x_min_, x_max_;
  double y_min_, y_max_;
  double z_min_, z_max_;
  Room(double x_min, double x_max, double y_min, double y_max, double z_min, double z_max)
    : x_min_(x_min), x_max_(x_max), y_min_(y_min), y_max_(y_max), z_min_(z_min), z_max_(z_max) {}
  Room(double x_min, double x_max, double y_min, double y_max, double z)
    : x_min_(x_min), x_max_(x_max), y_min_(y_min), y_max_(y_max), z_min_(z), z_max_(z) {}
  double area() {return (x_max_ - x_min_) * (y_max_ - y_min_);}
};

int main(int argc, char* argv[]) {
  std::default_random_engine generator;

  ASSERT(argc == 1 || argc == 2, "Usage: ./ReplicaGenerator /path/to/replica_folder [/path/to/output]");

  std::vector<std::string> scenes = {"apartment_0", 
                                     "apartment_1", 
                                     "apartment_2", 
                                     "frl_apartment_0",
                                     "frl_apartment_1",
                                     "frl_apartment_2",
                                     "frl_apartment_3",
                                     "frl_apartment_5",
                                     "hotel_0",
                                     "office_0",
                                     "office_1",
                                     "office_2",
                                     "office_3",
                                     "office_4",
                                     "room_0",
                                     "room_1",
                                     "room_2"};
  
  std::map<std::string, std::vector<Room>> scene_to_rooms;
  scene_to_rooms["apartment_1"].emplace_back(-0.9, 0.9, 0.5, 4.56, 0.28);
  scene_to_rooms["apartment_1"].emplace_back(3.5, 6.75, 4.35, 5.9, 0.28);

  std::string folder(argv[1]);

  const std::string meshFile(folder+"/"+scenes[1]+"/mesh.ply");
  const std::string atlasFolder(folder+"/"+scenes[1]+"/textures/");
  const std::string surfaceFile(folder+"/"+scenes[1]+"/glass.sur");
  ASSERT(pangolin::FileExists(meshFile));
  ASSERT(pangolin::FileExists(atlasFolder));
  ASSERT(pangolin::FileExists(surfaceFile));

  const int width = 1920;
  const int height = 1080;
  bool renderDepth = true;
  float depthScale = 65535.0f * 0.1f;

  // Setup EGL
  EGLCtx egl;

  egl.PrintInformation();

  //Don't draw backfaces
  const GLenum frontFace = GL_CCW;
  glFrontFace(frontFace);

  // Setup a framebuffer
  pangolin::GlTexture render(width, height);
  pangolin::GlRenderBuffer renderBuffer(width, height);
  pangolin::GlFramebuffer frameBuffer(render, renderBuffer);

  pangolin::GlTexture depthTexture(width, height, GL_R32F, false, 0, GL_RED, GL_FLOAT, 0);
  pangolin::GlFramebuffer depthFrameBuffer(depthTexture, renderBuffer);

  // Setup a camera
  pangolin::OpenGlRenderState s_cam(
    pangolin::ProjectionMatrixRDF_BottomLeft(
        width,
        height,
        width / 2.0f,
        width / 2.0f,
        (width - 1.0f) / 2.0f,
        (height - 1.0f) / 2.0f,
        0.1f,
        100.0f),
    pangolin::ModelViewLookAtRDF(0, 0, 0, 1.5, 1.5, 0, 0, 0, 1));

  // Start at some origin
  Eigen::Matrix4d T_camera_world = s_cam.GetModelViewMatrix();

  // And move to the left
  Eigen::Matrix4d T_new_old = Eigen::Matrix4d::Identity();

  T_new_old.topRightCorner(3, 1) = Eigen::Vector3d(0.025, 0, 0);

  // load mirrors
  std::vector<MirrorSurface> mirrors;
  if (surfaceFile.length()) {
    std::ifstream file(surfaceFile);
    picojson::value json;
    picojson::parse(json, file);

    for (size_t i = 0; i < json.size(); i++) {
      mirrors.emplace_back(json[i]);
    }
    std::cout << "Loaded " << mirrors.size() << " mirrors" << std::endl;
  }

  const std::string shadir = STR(SHADER_DIR);
  MirrorRenderer mirrorRenderer(mirrors, width, height, shadir);

  // load mesh and textures
  PTexMesh ptexMesh(meshFile, atlasFolder);
  ptexMesh.SetExposure(0.0055);
  ptexMesh.SetGamma(2.4);
  ptexMesh.SetSaturation(1.5);

  pangolin::ManagedImage<Eigen::Matrix<uint8_t, 3, 1>> image(width, height);
  pangolin::ManagedImage<float> depthImage(width, height);
  pangolin::ManagedImage<uint16_t> depthImageInt(width, height);

  // Render some frames
  const size_t numFrames = 100;
  for (size_t i = 0; i < numFrames; i++) {
    std::cout << "\rRendering frame " << i + 1 << "/" << numFrames << "... ";
    std::cout.flush();

    // Render
    frameBuffer.Bind();
    glPushAttrib(GL_VIEWPORT_BIT);
    glViewport(0, 0, width, height);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

    glEnable(GL_CULL_FACE);

    ptexMesh.Render(s_cam);

    glDisable(GL_CULL_FACE);

    glPopAttrib(); //GL_VIEWPORT_BIT
    frameBuffer.Unbind();

    for (size_t i = 0; i < mirrors.size(); i++) {
      MirrorSurface& mirror = mirrors[i];
      // capture reflections
      mirrorRenderer.CaptureReflection(mirror, ptexMesh, s_cam, frontFace);

      frameBuffer.Bind();
      glPushAttrib(GL_VIEWPORT_BIT);
      glViewport(0, 0, width, height);

      // render mirror
      mirrorRenderer.Render(mirror, mirrorRenderer.GetMaskTexture(i), s_cam);

      glPopAttrib(); //GL_VIEWPORT_BIT
      frameBuffer.Unbind();
    }

    // Download and save
    render.Download(image.ptr, GL_RGB, GL_UNSIGNED_BYTE);

    char filename[1000];
    snprintf(filename, 1000, "frame%06zu.jpg", i);

    pangolin::SaveImage(
      image.UnsafeReinterpret<uint8_t>(),
      pangolin::PixelFormatFromString("RGB24"),
      std::string(filename));

    if (renderDepth) {
      // render depth
      depthFrameBuffer.Bind();
      glPushAttrib(GL_VIEWPORT_BIT);
      glViewport(0, 0, width, height);
      glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

      glEnable(GL_CULL_FACE);

      ptexMesh.RenderDepth(s_cam, depthScale);

      glDisable(GL_CULL_FACE);

      glPopAttrib(); //GL_VIEWPORT_BIT
      depthFrameBuffer.Unbind();

      depthTexture.Download(depthImage.ptr, GL_RED, GL_FLOAT);

      // convert to 16-bit int
      for(size_t i = 0; i < depthImage.Area(); i++)
          depthImageInt[i] = static_cast<uint16_t>(depthImage[i] + 0.5f);

      snprintf(filename, 1000, "depth%06zu.png", i);
      // pangolin::SaveImage(
      //   depthImageInt.UnsafeReinterpret<uint8_t>(),
      //   pangolin::PixelFormatFromString("GRAY16LE"),
      //   std::string(filename), true, 34.0f);
    }

    // Move the camera
    T_camera_world = T_camera_world * T_new_old.inverse();
    std::uniform_int_distribution<size_t> room_index_generator(0, scene_to_rooms[scenes[1]].size()-1);
    int room_index = room_index_generator(generator);
    const auto& room = scene_to_rooms[scenes[1]][room_index];
    std::uniform_real_distribution<double> x_generator(room.x_min_,room.x_max_);
    std::uniform_real_distribution<double> y_generator(room.y_min_,room.y_max_);
    std::uniform_real_distribution<double> angle_generator(0,2*M_PI);
    std::uniform_real_distribution<double> la_z_offset_generator(-1.0,1.0);

    double x = x_generator(generator);
    double y = y_generator(generator);
    double z = 0;
    double angle = angle_generator(generator);
    double la_z_offset = la_z_offset_generator(generator);


    s_cam.SetModelViewMatrix(pangolin::ModelViewLookAtRDF(x, y, z, x+std::cos(angle), y+std::sin(angle), z+la_z_offset, 0, 0, 1));

    // s_cam.GetModelViewMatrix() = T_camera_world;
  }
  std::cout << "\rRendering frame " << numFrames << "/" << numFrames << "... done" << std::endl;

  return 0;
}

