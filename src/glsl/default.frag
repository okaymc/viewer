//  Simple 3D Model Viewer
//  Copyright (C) 2012-2013 Ingo Ruhnke <grumbel@gmail.com>
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

struct LightInfo
{
  vec3  diffuse;
  vec3  ambient;
  vec3  specular;

  vec3 position;
};

struct MaterialInfo
{
  vec3  diffuse;
  vec3  ambient;
  vec3  specular;
  vec3  emission;
  float shininess;

  sampler2D diffuse_texture;
  sampler2D specular_texture;

#ifdef REFLECTION_TEXTURE
  samplerCube reflection_texture;
#endif
};

uniform LightInfo light;
uniform MaterialInfo material;

uniform mat4 ModelViewMatrix;
uniform mat3 NormalMatrix;
//uniform mat4 ProjectionMatrix;
uniform mat4 MVP;

varying vec3 world_normal;
varying vec3 frag_normal;
varying vec3 frag_position;
varying vec2 frag_uv;

// ---------------------------------------------------------------------------
// shadow map
uniform sampler2DShadow ShadowMap;
varying vec4 shadow_position;

float offset_lookup(sampler2DShadow map,
                    vec4 loc,
                    vec2 offset)
{
  vec2 texmapscale = vec2(1.0/textureSize(map, 0).x, 1.0/textureSize(map, 0).y);
  //vec2 texmapscale = vec2(1.0/1600.0, 1.0/1000.0);
  //vec2 texmapscale = vec2(0.02, 0.02) / loc.q;

  return textureProj(map, vec4(loc.st + offset * texmapscale * loc.q,
                               //loc.p-0.0001,//ortho
                               loc.p, //perspective
                               loc.q));
}

#if defined(SHADOW_VALUE_1)

float shadow_value()
{
  return textureProj(ShadowMap, shadow_position);
}

#elif defined(SHADOW_VALUE_16)

float shadow_value()
{
  float shadowCoeff = 1.0f;
  if (shadow_position.w > 0.0)
  {
    float sum = 0;
    for (float y = -1.5; y <= 1.5; y += 1.0)
      for (float x = -1.5; x <= 1.5; x += 1.0)
      {
        sum += offset_lookup(ShadowMap, shadow_position, vec2(x, y));
      }

    shadowCoeff = sum / 16;
  }
  return shadowCoeff;
}

#else // defined(SHADOW_VALUE_4)

float shadow_value()
{
  vec2 offset = vec2(greaterThan(fract(gl_FragCoord.xy * 0.5),
                                 vec2(0.25, 0.25)));
  //vec2 offset;
  //offset.x = fract(gl_FragCoord.xy * 0.5);
  //offset.y = fract(gl_FragCoord.xy * 0.5);
  offset.y += offset.x;  // y ^= x in floating point

  if (offset.y > 1.1)
    offset.y = 0;
  return (
    offset_lookup(ShadowMap, shadow_position, offset + vec2(-1.5,  0.5)) +
    offset_lookup(ShadowMap, shadow_position, offset + vec2( 0.5,  0.5)) +
    offset_lookup(ShadowMap, shadow_position, offset + vec2(-1.5, -1.5)) +
    offset_lookup(ShadowMap, shadow_position, offset + vec2( 0.5, -1.5))
    ) * 0.25;
}

#endif

// ---------------------------------------------------------------------------
vec3 phong_model(vec3 position, vec3 normal, vec3 diff, vec3 spec)
{
  vec3 intensity = light.ambient * material.ambient * diff;

  vec3 N = normalize(normal);
  vec3 L = normalize(light.position - position); // eye dir

  float lambertTerm = dot(N, L);
  if(lambertTerm > 0.0)
  {
    float shadow = shadow_value();

    // in light
    intensity += light.diffuse * lambertTerm * diff * shadow;

    vec3 E = normalize(-position); // eye vec
    vec3 R = reflect(-L, N);

    float specular = pow( max(dot(R, E), 0.0), material.shininess );

    intensity += light.specular * specular * spec * shadow;
  }
  else
  {
    // in shadow
  }

  return intensity;
}

// ---------------------------------------------------------------------------

#if defined(DIFFUSE_COLOR_FROM_MATERIAL)

vec3 diffuse_color()
{
  return material.diffuse;
}

#else //defined(DIFFUSE_COLOR_FROM_TEXTURE)
vec3 diffuse_color()
{
  return material.diffuse * texture2D(material.diffuse_texture, frag_uv).rgb;
}

#endif

#if defined(SPECULAR_COLOR_FROM_TEXTURE)

vec3 specular_color()
{
  return material.specular * texture2D(material.specular_texture, frag_uv).rgb;
}

#else //defined(SPECULAR_COLOR_FROM_MATERIAL)
vec3 specular_color()
{
  return material.specular;
}

#endif

void main(void)
{
  //float light = texture(LightMap, world_normal, 3);
  vec3 diff = diffuse_color();
  vec3 spec = specular_color();
  vec3 intensity = phong_model(frag_position, frag_normal, diff, spec);

#if defined(REFLECTION_TEXTURE)
  vec3 o = reflect(frag_position, normalize(frag_normal));
  vec4 col = textureCube(material.reflection_texture, normalize(o));
  intensity = mix(intensity, col.rgb,
                  0.25 + 0.5 * (1.0 - dot(vec3(0, 0, 1), normalize(frag_normal))));
#endif

  gl_FragColor = vec4(intensity, 1.0);
}

/* EOF */
