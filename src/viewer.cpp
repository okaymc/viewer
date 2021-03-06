//  Simple 3D Model Viewer
//  Copyright (C) 2012 Ingo Ruhnke <grumbel@gmail.com>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "viewer.hpp"

#include <SDL.h>
#include <SDL_image.h>
#include <cmath>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>
#include <thread>
#include <glm/gtx/io.hpp>

#include "assert_gl.hpp"
#include "compositor.hpp"
#include "log.hpp"
#include "material_factory.hpp"
#include "menu.hpp"
#include "mesh.hpp"
#include "model.hpp"
#include "opengl_state.hpp"
#include "program.hpp"
#include "render_context.hpp"
#include "scene.hpp"
#include "scene_manager.hpp"
#include "shader.hpp"
#include "system.hpp"
#include "text_surface.hpp"
#include "renderbuffer.hpp"

namespace {

void print_scene_graph(SceneNode* node, int depth = 0)
{
  std::cout << std::string(depth, ' ') << node
            << ": " << node->get_position()
            << " " << node->get_scale()
            << " " << node->get_orientation() << std::endl;
  for(auto const& child : node->get_children())
  {
    print_scene_graph(child.get(), depth+1);
  }
}

} // namespace

std::unique_ptr<Framebuffer> g_shadowmap;
glm::mat4 g_shadowmap_matrix;
TexturePtr g_video_texture;

void
Viewer::on_keyboard_event(SDL_KeyboardEvent key)
{
  switch (key.keysym.scancode)
  {
    case SDL_SCANCODE_TAB:
      m_cfg.m_show_menu = !m_cfg.m_show_menu;
      break;

    case SDL_SCANCODE_F3:
      m_cfg.m_show_calibration = !m_cfg.m_show_calibration;
      break;

    case SDL_SCANCODE_ESCAPE:
      exit(EXIT_SUCCESS);
      break;

    case SDL_SCANCODE_9:
      if (m_video_player)
      {
        m_video_player->seek(m_video_player->get_position() - 10 * Gst::SECOND);
      }
      break;

    case SDL_SCANCODE_0:
      if (m_video_player)
      {
        m_video_player->seek(m_video_player->get_position() + 10 * Gst::SECOND);
      }
      break;

    case SDL_SCANCODE_N:
      m_cfg.m_eye_distance += 0.01f;
      break;

    case SDL_SCANCODE_T:
      m_cfg.m_eye_distance -= 0.01f;
      break;

    case SDL_SCANCODE_C:
      m_compositor->m_ipd += 1;
      break;

    case SDL_SCANCODE_R:
      m_compositor->m_ipd -= 1;
      break;

#ifndef HAVE_OPENGLES2
    case SDL_SCANCODE_Z:
      {
        GLdouble clip_plane[] = { 0.0, 0.0, 1.0, 1.0 };

        clip_plane[0] = (rand() / double(RAND_MAX) - 0.5) * 2.0f;
        clip_plane[1] = (rand() / double(RAND_MAX) - 0.5) * 2.0f;
        clip_plane[2] = (rand() / double(RAND_MAX) - 0.5) * 2.0f;
        clip_plane[3] = (rand() / double(RAND_MAX) - 0.5) * 2.0f;

        glEnable(GL_CLIP_PLANE0);
        glClipPlane(GL_CLIP_PLANE0, clip_plane);
      }
      break;

    case SDL_SCANCODE_G:
      {
        GLdouble clip_plane[] = { 0.0, 1.0, 1.0, 0.0 };
        glClipPlane(GL_CLIP_PLANE0, clip_plane);
        glEnable(GL_CLIP_PLANE0);
      }
      break;
#endif

    case SDL_SCANCODE_D:
      m_compositor->toggle_stereo_mode();
      break;

    case SDL_SCANCODE_KP_8: // up
      m_cfg.m_eye += glm::normalize(m_cfg.m_look_at);
      break;

    case SDL_SCANCODE_KP_2: // down
      m_cfg.m_eye -= glm::normalize(m_cfg.m_look_at);
      break;

    case SDL_SCANCODE_KP_4: // left
      {
        glm::vec3 dir = glm::normalize(m_cfg.m_look_at);
        dir = glm::rotate(dir, 90.0f, m_cfg.m_up);
        m_cfg.m_eye += dir;
      }
      break;

    case SDL_SCANCODE_KP_6: // right
      {
        glm::vec3 dir = glm::normalize(m_cfg.m_look_at);
        dir = glm::rotate(dir, 90.0f, m_cfg.m_up);
        m_cfg.m_eye -= dir;
      }
      break;

    case SDL_SCANCODE_KP_7: // kp_pos1
      m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, 5.0f, m_cfg.m_up);
      break;

    case SDL_SCANCODE_KP_9: // kp_raise
      m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, -5.0f, m_cfg.m_up);
      break;

    case SDL_SCANCODE_KP_1:
      m_cfg.m_eye -= glm::normalize(m_cfg.m_up);
      break;

    case SDL_SCANCODE_KP_3:
      m_cfg.m_eye += glm::normalize(m_cfg.m_up);
      break;

    case SDL_SCANCODE_KP_MULTIPLY:
      m_cfg.m_fov += glm::radians(1.0f);
      break;

    case SDL_SCANCODE_KP_DIVIDE:
      m_cfg.m_fov -= glm::radians(1.0f);
      break;

    case SDL_SCANCODE_F1:
      {
        // Hitchcock zoom in
        //float old_eye_z = m_eye.z;
        //g_eye.z *= 1.005f;
        //g_fov = m_fov / std::atan(1.0f / old_eye_z) * std::atan(1.0f / m_eye.z);

        float old_fov = m_cfg.m_fov;
        m_cfg.m_fov += glm::radians(1.0f);
        if (m_cfg.m_fov < glm::radians(160.0f))
        {
          m_cfg.m_eye.z = m_cfg.m_eye.z
            * (2.0*tan(0.5 * old_fov))
            / (2.0*tan(0.5 * m_cfg.m_fov));
        }
        else
        {
          m_cfg.m_fov = 160.0f;
        }
        log_info("fov: %5.2f %f", m_cfg.m_fov, m_cfg.m_eye.z);
        log_info("w: %f", tan(m_cfg.m_fov /2.0f) * m_cfg.m_eye.z);
      }
      break;

    case SDL_SCANCODE_F2:
      // Hitchcock zoom out
      {
        //float old_eye_z = m_eye.z;
        //g_eye.z /= 1.005f;
        //g_fov = m_fov / std::atan(1.0f / old_eye_z) * std::atan(1.0f / m_eye.z);

        float old_fov = m_cfg.m_fov;
        m_cfg.m_fov -= 1.0f;
        if (m_cfg.m_fov >= 7.0f)
        {
          m_cfg.m_eye.z = m_cfg.m_eye.z
            * (2.0*tan(0.5 * old_fov))
            / (2.0*tan(0.5 * m_cfg.m_fov));
        }
        else
        {
          m_cfg.m_fov = 7.0f;
        }
        log_info("fov: %5.2f %f", m_cfg.m_fov, m_cfg.m_eye.z);
        log_info("w: %f", tan(m_cfg.m_fov/2.0f) * m_cfg.m_eye.z);
      }
      break;

    case SDL_SCANCODE_F10:
      //glutReshapeWindow(1600, 1000);
      break;

    case SDL_SCANCODE_F11:
      //glutFullScreen();
      break;

    default:
      log_info("unknown key: %d", static_cast<int>(key.keysym.sym));
      break;
  }
}

void
Viewer::init_scene(std::vector<std::string> const& model_filenames)
{
  assert_gl("init()");

#ifndef HAVE_OPENGLES2
  { // setup the material that is used by the SceneManager for the
    // shadowmap rendering pass
    MaterialPtr material = std::make_unique<Material>();
    material->cull_face(GL_FRONT);
    material->enable(GL_CULL_FACE);
    material->enable(GL_DEPTH_TEST);
    material->set_uniform("MVP", UniformSymbol::ModelViewProjectionMatrix);
    material->set_program(Program::create(Shader::from_file(GL_VERTEX_SHADER, "src/glsl/shadowmap.vert"),
                                          Shader::from_file(GL_FRAGMENT_SHADER, "src/glsl/shadowmap.frag")));
    m_scene_manager->set_override_material(material);
  }
#endif

  // build a scene
  for(auto const& model_filename : model_filenames)
  {
    auto node = Scene::from_file(model_filename);

    std::cout << "SceneGraph(" << model_filename << "):\n";
    print_scene_graph(node.get());

    m_scene_manager->get_world()->attach_child(std::move(node));
  }

#ifndef HAVE_OPENGLES2
  if (true)
  { // create a skybox
    auto mesh = Mesh::create_skybox(500.0f);
    ModelPtr model = std::make_shared<Model>();
    model->add_mesh(std::move(mesh));
    model->set_material(MaterialFactory::get().create("skybox"));

    auto node = m_scene_manager->get_world()->create_child();
    node->attach_model(model);
  }
#endif

  m_dot_surface = TextSurface::create("+", TextProperties().set_line_width(3.0f));

  init_menu();

  assert_gl("init()");
}

void
Viewer::init_video_player(VideoOptions const& cfg)
{
  MaterialPtr video_material = MaterialFactory::get().from_file("data/room/video.material");

  if (cfg.flat_canvas)
  {
    auto node = m_scene_manager->get_world()->create_child();
    ModelPtr model = std::make_shared<Model>();

    model->add_mesh(Mesh::create_plane(5.0f));
    node->set_position(glm::vec3(0.0f, 0.0f, -10.0f));
    node->set_orientation(glm::quat(glm::vec3(glm::half_pi<float>(), 0.0f, 0.0f)));
    node->set_scale(glm::vec3(4.0f, 1.0f, 2.25f));

    model->set_material(video_material);
    node->attach_model(model);
  }
  else
  { // round canvas
    auto node = m_scene_manager->get_world()->create_child();

    int rings = 32;
    int segments = 32;

    ModelPtr model = std::make_shared<Model>();
    model->set_material(video_material);
    model->add_mesh(Mesh::create_curved_screen(15.0f, cfg.hfov, cfg.vfov, rings, segments));
    node->attach_model(model);
  }
}

void
Viewer::init_menu()
{
  m_menu = std::make_unique<Menu>(TextProperties().set_font_size(24.0f).set_line_width(4.0f));
  //g_menu->add_item("eye.x", &g_eye.x);
  //g_menu->add_item("eye.y", &g_eye.y);
  //g_menu->add_item("eye.z", &g_eye.z);

  m_menu->add_item("wiimote.camera_control", &m_cfg.m_wiimote_camera_control);

  m_menu->add_item("slowfactor", &m_cfg.m_slow_factor, 0.01f, 0.0f);

  m_menu->add_item("depth.near_z", &m_cfg.m_near_z, 0.01, 0.0f);
  m_menu->add_item("depth.far_z",  &m_cfg.m_far_z, 1.0f);

  m_menu->add_item("convergence", &m_cfg.m_convergence, 0.1f);

  m_menu->add_item("shadowmap.fov", &m_cfg.m_shadowmap_fov, 1.0f);

  m_menu->add_item("FOV", &m_cfg.m_fov, 0.05f);
#if 0
  m_menu->add_item("Barrel Power", &m_barrel_power, 0.01f);
#endif
  //g_menu->add_item("AspectRatio", &m_aspect_ratio, 0.05f, 0.5f, 4.0f);

  m_menu->add_item("eye.distance", &m_cfg.m_eye_distance, 0.1f);

  m_menu->add_item("light.up",  &m_cfg.m_light_up, 1.0f);
  m_menu->add_item("light.angle",  &m_cfg.m_light_angle, 0.1f);

  m_menu->add_item("wiimote.distance_scale",  &m_cfg.m_distance_scale, 0.01f);
  m_menu->add_item("wiimote.scale_x", &m_wiimote_scale.x, 0.01f);
  m_menu->add_item("wiimote.scale_y", &m_wiimote_scale.y, 0.01f);

  //m_menu->add_item("shadow map", &m_render_shadowmap);
}

void
Viewer::on_mouse_motion_event(SDL_MouseMotionEvent const& ev)
{
  if (ev.state & SDL_BUTTON_LMASK)
  {
    float angle_d = -0.0025f;

    m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, angle_d * ev.xrel, m_cfg.m_up);

    glm::vec3 cross = glm::cross(m_cfg.m_look_at, m_cfg.m_up);
    m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, angle_d * ev.yrel, cross);
  }
}

void
Viewer::on_mouse_button_event(Window& window, SDL_MouseButtonEvent const& ev)
{
  if (ev.button == SDL_BUTTON_LEFT)
  {
    if (ev.type == SDL_MOUSEBUTTONDOWN)
    {
      //window.grab(true);
      SDL_SetRelativeMouseMode(SDL_TRUE);
    }
    else if (ev.type == SDL_MOUSEBUTTONUP)
    {
      //window.grab(false);
      SDL_SetRelativeMouseMode(SDL_FALSE);
    }
  }
}

void
Viewer::on_mouse_wheel_event(SDL_MouseWheelEvent const& ev)
{
  m_cfg.m_fov += glm::radians(static_cast<float>(ev.y));
}

void
Viewer::process_events(Window& window, GameController& gamecontroller)
{
  SDL_Event ev;
  while(SDL_PollEvent(&ev))
  {
    switch(ev.type)
    {
      case SDL_WINDOWEVENT:
        switch (ev.window.event)
        {
          case SDL_WINDOWEVENT_RESIZED:
            m_compositor->reshape(*this, ev.window.data1, ev.window.data2);
            break;

          default:
            break;
        }
        break;

      case SDL_QUIT:
        exit(EXIT_SUCCESS);
        break;

      case SDL_KEYUP:
        break;

      case SDL_KEYDOWN:
        on_keyboard_event(ev.key);
        break;

      case SDL_MOUSEMOTION:
        on_mouse_motion_event(ev.motion);
        break;

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        on_mouse_button_event(window, ev.button);
        break;

      case SDL_MOUSEWHEEL:
        on_mouse_wheel_event(ev.wheel);
        break;

      case SDL_CONTROLLERAXISMOTION:
        //log_debug("controller axis: %d %d", static_cast<int>(ev.caxis.axis), ev.caxis.value);
        switch(ev.caxis.axis)
        {
          case SDL_CONTROLLER_AXIS_LEFTX:
            m_stick.dir.x = -ev.caxis.value / 32768.0f;
            break;

          case SDL_CONTROLLER_AXIS_LEFTY:
            m_stick.dir.z = -ev.caxis.value / 32768.0f;
            break;

          case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
          case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            m_stick.rot.z = (SDL_GameControllerGetAxis(gamecontroller.get_handle(), SDL_CONTROLLER_AXIS_TRIGGERRIGHT) -
                             SDL_GameControllerGetAxis(gamecontroller.get_handle(), SDL_CONTROLLER_AXIS_TRIGGERLEFT)) / 32768.0f;
            break;

          case SDL_CONTROLLER_AXIS_RIGHTX:
            m_stick.rot.y = -ev.caxis.value / 32768.0f;
            break;

          case SDL_CONTROLLER_AXIS_RIGHTY:
            m_stick.rot.x = -ev.caxis.value / 32768.0f;
            break;
        }
        break;

      case SDL_CONTROLLERBUTTONUP:
      case SDL_CONTROLLERBUTTONDOWN:
        //log_debug("controller button: %d %d", static_cast<int>(ev.cbutton.button), static_cast<int>(ev.cbutton.state));
        switch(ev.cbutton.button)
        {
          case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            m_stick.dir.y = -ev.cbutton.state;
            break;

          case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            m_stick.dir.y = +ev.cbutton.state;
            break;

          case SDL_CONTROLLER_BUTTON_B:
            if (m_wiimote_manager)
            {
              m_wiimote_manager->reset_gyro_orientation();
            }
            break;

          case SDL_CONTROLLER_BUTTON_X:
            //g_light_angle += 1.0f;
            m_stick.light_rotation = ev.cbutton.state;
            break;

          case SDL_CONTROLLER_BUTTON_START:
            if (ev.cbutton.state)
            {
              m_cfg.m_show_menu = !m_cfg.m_show_menu;
            }
            break;

          case SDL_CONTROLLER_BUTTON_BACK:
            if (ev.cbutton.state)
            {
              m_cfg.m_show_dots = !m_cfg.m_show_dots;
            }
            break;

          case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (ev.cbutton.state)
            {
              m_stick.hat |= SDL_HAT_UP;
              m_hat_autorepeat = SDL_GetTicks() + 500;
            }
            else
            {
              m_stick.hat &= ~SDL_HAT_UP;
            }
            break;

          case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (ev.cbutton.state)
            {
              m_stick.hat |= SDL_HAT_DOWN;
              m_hat_autorepeat = SDL_GetTicks() + 500;
            }
            else
            {
              m_stick.hat &= ~SDL_HAT_DOWN;
            }
            break;

          case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (ev.cbutton.state)
            {
              m_stick.hat |= SDL_HAT_LEFT;
              m_hat_autorepeat = SDL_GetTicks() + 500;
            }
            else
            {
              m_stick.hat &= ~SDL_HAT_LEFT;
            }
            break;

          case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (ev.cbutton.state)
            {
              m_stick.hat |= SDL_HAT_RIGHT;
              m_hat_autorepeat = SDL_GetTicks() + 500;
            }
            else
            {
              m_stick.hat &= ~SDL_HAT_RIGHT;
            }
            break;
        }
        break;

      case SDL_JOYAXISMOTION:
        //log_debug("joystick axis: %d %d", static_cast<int>(ev.jaxis.axis), ev.jaxis.value);
        break;

      case SDL_JOYBUTTONDOWN:
      case SDL_JOYBUTTONUP:
        //log_debug("joystick button: %d %d", static_cast<int>(ev.jbutton.button), static_cast<int>(ev.jbutton.state));
        break;

      case SDL_JOYHATMOTION:
        break;

      default:
        break;
    }
  }
}

void
Viewer::update_menu()
{
  auto current_time = SDL_GetTicks();
  if (m_stick.hat != m_old_stick.hat || (m_stick.hat && m_hat_autorepeat < current_time))
  {
    if (m_hat_autorepeat < current_time)
    {
      m_hat_autorepeat = current_time + 100 + (m_hat_autorepeat - current_time);
    }

    if (m_stick.hat & SDL_HAT_UP)
    {
      m_menu->up();
    }

    if (m_stick.hat & SDL_HAT_DOWN)
    {
      m_menu->down();
    }

    if (m_stick.hat & SDL_HAT_LEFT)
    {
      m_menu->left();
    }

    if (m_stick.hat & SDL_HAT_RIGHT)
    {
      m_menu->right();
    }
  }
}

void
Viewer::update_freeflight_mode(float dt)
{
  float delta = dt * 5.0f * m_cfg.m_slow_factor;

  {
    // forward/backward
    m_cfg.m_eye += glm::normalize(m_cfg.m_look_at) * m_stick.dir.z * delta;

    // up/down
    m_cfg.m_eye += glm::normalize(m_cfg.m_up) * m_stick.dir.y * delta;

    // left/right
    glm::vec3 dir = glm::normalize(m_cfg.m_look_at);
    dir = glm::rotate(dir, 90.0f, m_cfg.m_up);
    m_cfg.m_eye += glm::normalize(dir) * m_stick.dir.x * delta;
  }

  { // handle rotation
    float angle_d = 20.0f;

    // yaw
    m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, angle_d * m_stick.rot.y * delta, m_cfg.m_up);

    // roll
    m_cfg.m_up = glm::rotate(m_cfg.m_up, angle_d * m_stick.rot.z * delta, m_cfg.m_look_at);

    // pitch
    glm::vec3 cross = glm::cross(m_cfg.m_look_at, m_cfg.m_up);
    m_cfg.m_up = glm::rotate(m_cfg.m_up, angle_d * m_stick.rot.x * delta, cross);
    m_cfg.m_look_at = glm::rotate(m_cfg.m_look_at, angle_d * m_stick.rot.x * delta, cross);
  }
}

void
Viewer::update_fps_mode(float dt)
{
  float focus_distance = glm::length(m_cfg.m_look_at);
  auto tmp = m_cfg.m_look_at;
  auto xz_dist = glm::sqrt(tmp.x * tmp.x + tmp.z * tmp.z);
  float pitch = glm::atan(tmp.y, xz_dist);
  float yaw   = glm::atan(tmp.z, tmp.x);

  yaw   += -m_stick.rot.y * 2.0f * dt;
  pitch += m_stick.rot.x * 2.0f * dt;

  pitch = glm::clamp(pitch, -glm::half_pi<float>() + 0.001f, glm::half_pi<float>() - 0.001f);

  if (false && m_cfg.m_wiimote_camera_control)
  {
    pitch = 0.0f;
  }

  glm::vec3 forward(glm::cos(yaw), 0.0f, glm::sin(yaw));

  //log_debug("focus distance: %f", focus_distance);
  //log_debug("yaw: %f pitch: %f", yaw, pitch);

  // forward/backward
  m_cfg.m_eye += 10.0f * forward * m_stick.dir.z * dt * m_cfg.m_slow_factor;

  // strafe
  m_cfg.m_eye += 10.0f * glm::vec3(forward.z, 0.0f, -forward.x) * m_stick.dir.x * dt * m_cfg.m_slow_factor;

  // up/down
  m_cfg.m_eye.y += 10.0f * m_stick.dir.y * dt * m_cfg.m_slow_factor;

  m_cfg.m_look_at = focus_distance * glm::vec3(glm::cos(pitch) * glm::cos(yaw),
                                         glm::sin(pitch),
                                         glm::cos(pitch) * glm::sin(yaw));

  float f = sqrt(m_cfg.m_look_at.x * m_cfg.m_look_at.x + m_cfg.m_look_at.z * m_cfg.m_look_at.z);
  m_cfg.m_up.x = -m_cfg.m_look_at.x/f * m_cfg.m_look_at.y;
  m_cfg.m_up.y = f;
  m_cfg.m_up.z = -m_cfg.m_look_at.z/f * m_cfg.m_look_at.y;
  m_cfg.m_up = glm::normalize(m_cfg.m_up);
}

void
Viewer::process_keyboard(float dt)
{
  Uint8 const* state = SDL_GetKeyboardState(NULL);

  auto key2float = [state](size_t lhs, size_t rhs) -> float {
    if (state[lhs] && !state[rhs]) { return 1.0f; }
    else if (state[rhs] && !state[lhs]) { return -1.0f; }
    else { return 0; }
  };

  m_stick.dir.x = key2float(SDL_SCANCODE_LEFT,
                            SDL_SCANCODE_RIGHT);
  m_stick.dir.y = key2float(SDL_SCANCODE_PAGEUP,
                            SDL_SCANCODE_PAGEDOWN);
  m_stick.dir.z = key2float(SDL_SCANCODE_UP,
                            SDL_SCANCODE_DOWN);
}

void
Viewer::process_joystick(float dt)
{
  { // apply deadzone
    float deadzone = 0.2f;
    if (fabs(m_stick.dir.x) < deadzone) m_stick.dir.x = 0.0f;
    if (fabs(m_stick.dir.y) < deadzone) m_stick.dir.y = 0.0f;
    if (fabs(m_stick.dir.z) < deadzone) m_stick.dir.z = 0.0f;

    if (fabs(m_stick.rot.x) < deadzone) m_stick.rot.x = 0.0f;
    if (fabs(m_stick.rot.y) < deadzone) m_stick.rot.y = 0.0f;
    if (fabs(m_stick.rot.z) < deadzone) m_stick.rot.z = 0.0f;
  }

  update_menu();


  if (false)
  {
    log_debug("stick: %2.2f %2.2f %2.2f  -  %2.2f %2.2f %2.2f",
              m_stick.dir.x, m_stick.dir.y, m_stick.dir.z,
              m_stick.rot.x, m_stick.rot.y, m_stick.rot.z);
  }

  float delta = dt * 5.0f * m_cfg.m_slow_factor;

  if (m_stick.light_rotation)
  {
    //log_debug("light angle: %f", m_light_angle);
    m_cfg.m_light_angle += delta * 30.0f;
  }

  // update_freeflight_mode(dt);
  update_fps_mode(dt);

  m_old_stick = m_stick;
}

void
Viewer::main_loop(Window& window, GameController& gamecontroller)
{
  int num_frames = 0;
  unsigned int start_ticks = SDL_GetTicks();

  int ticks = SDL_GetTicks();
  while(true)
  {
    int next = SDL_GetTicks();
    int delta = next - ticks;
    ticks = next;

    m_compositor->render(*this);
    window.swap();

    SDL_Delay(1);

    process_events(window, gamecontroller);
    process_joystick(delta / 1000.0f);
    process_keyboard(delta / 1000.0f);

    if (false && m_wiimote_manager)
    {
      //g_wiimote_manager->update();
      //g_wiimote_manager->get_accumulated();
      m_wiimote_gyro_node->set_orientation(m_wiimote_manager->get_gyro_orientation());
      m_wiimote_gyro_node->set_position(glm::vec3(-0.1f, 0.0f, -0.5f));

      m_wiimote_node->set_orientation(m_wiimote_manager->get_orientation());
      m_wiimote_node->set_position(glm::vec3(0.0f, 0.0f, -0.5f));

      m_wiimote_accel_node->set_orientation(m_wiimote_manager->get_accel_orientation());
      m_wiimote_accel_node->set_position(glm::vec3(0.1f, 0.0f, -0.5f));
    }

    num_frames += 1;

    if (num_frames > 100)
    {
      int t = SDL_GetTicks() - start_ticks;
      std::cout << "frames: " << num_frames << " time: " << t
                << " frame_delay: " << static_cast<float>(t) / static_cast<float>(num_frames)
                << " fps: " << static_cast<float>(num_frames) / static_cast<float>(t) * 1000.0f
                << std::endl;

      num_frames = 0;
      start_ticks = SDL_GetTicks();
    }

    if (m_video_player)
    {
      m_video_player->update();
      g_video_texture = m_video_player->get_texture();
    }
  }
}

void
Viewer::update_offsets(glm::vec2 p1, glm::vec2 p2)
{
  if (p1.x > p2.x)
  {
    std::swap(p1, p2);
  }

  glm::vec2 r = p2 - p1;
  float angle = glm::atan(-r.y, r.x);
  m_cfg.m_roll_offset = angle;
  m_cfg.m_distance_offset = m_cfg.m_distance_scale * glm::length(r) / 2.0f * glm::tan(glm::radians(m_cfg.m_fov));
  glm::vec2 c = (p1+p2)/2.0f;

  c -= glm::vec2(512, 384);
  c = glm::rotate(c, m_cfg.m_roll_offset);
  c += glm::vec2(512, 384);

  m_cfg.m_yaw_offset   = ((c.x / 1024.0f) - 0.5f) * glm::half_pi<float>() * m_wiimote_scale.x;
  m_cfg.m_pitch_offset = ((c.y /  768.0f) - 0.5f) * glm::half_pi<float>() * m_wiimote_scale.y;

  m_wiimote_dot1.x = p1.x / 1024.0f;
  m_wiimote_dot1.y = 1.0f - (p1.y /  768.0f);
  m_wiimote_dot2.x = p2.x / 1024.0f;
  m_wiimote_dot2.y = 1.0f - (p2.y /  768.0f);

  //std::cout << "offset: " << boost::format("%4.2f %4.2f %4.2f") % m_roll_offset % m_yaw_offset % m_pitch_offset << std::endl;
}

void
Viewer::parse_args(int argc, char** argv, Options& opts)
{
  for(int i = 1; i < argc; ++i)
  {
    if (argv[i][0] == '-')
    {
      if (strcmp("--wiimote", argv[i]) == 0)
      {
        opts.wiimote = true;
      }
      else if (strcmp("--video", argv[i]) == 0)
      {
        opts.video.filename = argv[i+1];
        ++i;
      }
      else if (strcmp("--video3d", argv[i]) == 0)
      {
        opts.video.stereo = true;
        opts.video.filename = argv[i+1];
        ++i;
      }
      else if (strcmp("--video3d-fov", argv[i]) == 0)
      {
        if (sscanf(argv[i+1], "%fx%f", &opts.video.hfov, &opts.video.vfov) != 2)
        {
          throw std::runtime_error("expected --video3d-fov HFOVxVFOV, got '" + std::string(argv[i]) + "'");
        }
        else
        {
          opts.video.hfov = glm::radians(opts.video.hfov);
          opts.video.vfov = glm::radians(opts.video.vfov);
          ++i;
        }
      }
      else if (strcmp("--help", argv[i]) == 0 ||
               strcmp("-h", argv[i]) == 0)
      {
        std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                  << "\n"
                  << "Options:\n"
                  << "  --wiimote          Enable Wiimote support\n"
                  << "  --video FILE       Play video\n"
                  << "  --video3d FILE     Play 3D video\n"
                  << "  --video3d-fov H:V  Horizontal and vertical FOV\n";
        exit(0);
      }
      else
      {
        throw std::runtime_error("unknown option: " + std::string(argv[i]));
      }
    }
    else
    {
      opts.models.emplace_back(argv[i]);
    }
  }
}

int
Viewer::main(int argc, char** argv)
{
  Options opts;
  parse_args(argc, argv, opts);

  System system = System::create();
  Window window = system.create_gl_window("OpenGL Viewer", m_screen_w, m_screen_h, false, 0);
  //Joystick joystick = system.create_joystick();
  GameController gamecontroller = system.create_gamecontroller();

#ifndef HAVE_OPENGLES2
  // glew throws 'invalid enum' error in OpenGL3.3Core, thus we eat up the error code
  glewExperimental = true;
  glewInit();
  glGetError();

  // In OpenGL3.3Core VAO are mandatory, this hack creates one
  GLuint vao;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
#endif

  if (opts.wiimote)
  {
    m_wiimote_manager = std::make_unique<WiimoteManager>();
  }

  m_compositor = std::make_unique<Compositor>(m_screen_w, m_screen_h);
  m_scene_manager = std::make_unique<SceneManager>();

  if (!opts.video.filename.empty())
  {
    Gst::init(argc, argv);
    std::cout << "Playing video: " << opts.video.filename << std::endl;
    m_video_player = std::make_unique<VideoProcessor>(opts.video.filename);
    init_video_player(opts.video);
  }

  init_scene(opts.models);

  std::cout << "main: " << std::this_thread::get_id() << std::endl;

  main_loop(window, gamecontroller);

  return 0;
}

int
main(int argc, char** argv)
{
  Viewer viewer;
  return viewer.main(argc, argv);
}

// EOF //
