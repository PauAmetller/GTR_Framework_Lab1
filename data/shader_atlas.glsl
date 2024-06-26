//example of some shaders compiled
flat basic.vs flat.fs
texture basic.vs texture.fs
light basic.vs light.fs
skybox basic.vs skybox.fs
depth quad.vs depth.fs
multi basic.vs multi.fs
gbuffers basic.vs gbuffers.fs
deferred_global quad.vs deferred_global.fs
deferred_ws basic.vs deferred_global.fs
ssao quad.vs ssao.fs
blurr quad.vs blurr.fs
tone_mapper quad.vs tonemapper.fs
probe basic.vs probe.fs
reflectionProbe basic.vs reflectionProbe.fs
irradiance quad.vs irradiance.fs
reflection quad.vs reflection.fs
irradiance_interpol quad.vs irradiance_interpol.fs
planar basic.vs planar.fs
//fog only moon_light
volumetric quad.vs volumetric.fs
//fog for all lights
volumetric_lights quad.vs volumetric_lights.fs
decals basic.vs decal.fs
motion_blurr quad.vs motion_blurr.fs

\basic.vs

#version 330 core

in vec3 a_vertex;
in vec3 a_normal;
in vec2 a_coord;
in vec4 a_color;

uniform vec3 u_camera_pos;

uniform mat4 u_model;
uniform mat4 u_viewprojection;

//this will store the color for the pixel shader
out vec3 v_position;
out vec3 v_world_position;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_color;

uniform float u_time;

void main()
{	
	//calcule the normal in camera space (the NormalMatrix is like ViewMatrix but without traslation)
	v_normal = (u_model * vec4( a_normal, 0.0) ).xyz;
	
	//calcule the vertex in object space
	v_position = a_vertex;
	v_world_position = (u_model * vec4( v_position, 1.0) ).xyz;
	
	//store the color in the varying var to use it from the pixel shader
	v_color = a_color;

	//store the texture coordinates
	v_uv = a_coord;

	//calcule the position of the vertex using the matrices
	gl_Position = u_viewprojection * vec4( v_world_position, 1.0 );
}

\quad.vs

#version 330 core

in vec3 a_vertex;
in vec2 a_coord;
out vec2 v_uv;

void main()
{	
	v_uv = a_coord;
	gl_Position = vec4( a_vertex, 1.0 );
}


\flat.fs

#version 330 core

uniform vec4 u_color;

out vec4 FragColor;

void main()
{
	FragColor = u_color;
}

\GammaToLinear

vec3 degamma(vec3 c)
{
	return pow(c,vec3(2.2));
}

vec3 gamma(vec3 c)
{
	return pow(c,vec3(1.0/2.2));
}

\ComputeShadow

//Shadow_map resources
uniform int u_light_cast_shadow;
uniform sampler2D u_shadow_map;
uniform mat4 u_shadow_map_view_projection;
uniform float u_shadow_bias;

float computeShadow( vec3 wp){
	//project our 3D position to the shadowmap
	vec4 proj_pos = u_shadow_map_view_projection * vec4(wp,1.0);

	//from homogeneus space to clip space
	vec2 shadow_uv = proj_pos.xy / proj_pos.w;

	//from clip space to uv space
	shadow_uv = shadow_uv * 0.5 + vec2(0.5);

	//it is outside on the sides, or it is before near or behind far plane
	if( shadow_uv.x < 0.0 || shadow_uv.y < 0.0 || shadow_uv.x > 1.0 || shadow_uv.y > 1.0){
		return 1.0;
	}

	//get point depth [-1 .. +1] in non-linear space
	float real_depth = (proj_pos.z - u_shadow_bias) / proj_pos.w;

	//normalize from [-1..+1] to [0..+1] still non-linear
	real_depth = real_depth * 0.5 + 0.5;

	//it is before near or behind far plane
	if(real_depth < 0.0 || real_depth > 1.0)
		return 1.0;


	//read depth from depth buffer in [0..+1] non-linear
	float shadow_depth = texture( u_shadow_map, shadow_uv).x;

	//compute final shadow factor by comparing
	float shadow_factor = 1.0;

	//we can compare them, even if they are not linear
	if( shadow_depth < real_depth )
		shadow_factor = 0.0;
	return shadow_factor;

}

\specullar_function

float GGX(float NdotV, float k){
	return NdotV / (NdotV * (1.0 - k) + k);
}
	
float G_Smith( float NdotV, float NdotL, float roughness)
{
	float k = pow(roughness + 1.0, 2.0) / 8.0;
	return GGX(NdotL, k) * GGX(NdotV, k);
}

// Fresnel term with scalar optimization(f90=1)
float F_Schlick( const in float VoH, const in float f0)
{
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (1.0 - f0) * f;
}

// Fresnel term with colorized fresnel
vec3 F_Schlick( const in float VoH, const in vec3 f0)
{
	float f = pow(1.0 - VoH, 5.0);
	return f0 + (vec3(1.0) - f0) * f;
}

#define RECIPROCAL_PI 0.3183098861837697
#define PI 3.14159265359

vec3 Fd_Lambert(vec3 color) {
    return color/PI;
}

// Diffuse Reflections: Disney BRDF using retro-reflections using F term, this is much more complex!!
float Fd_Burley ( const in float NoV, const in float NoL,const in float LoH, const in float linearRoughness)
{
        float f90 = 0.5 + 2.0 * linearRoughness * LoH * LoH;
        float lightScatter = F_Schlick(NoL, 1.0);//, f90);  //Check latter
        float viewScatter  = F_Schlick(NoV, 1.0);//, f90);
        return lightScatter * viewScatter * RECIPROCAL_PI;
}

// Normal Distribution Function using GGX Distribution
float D_GGX (	const in float NoH, const in float linearRoughness )
{
	float a2 = linearRoughness * linearRoughness;
	float f = (NoH * NoH) * (a2 - 1.0) + 1.0;
	return a2 / (PI * f * f);
}

//this is the cook torrance specular reflection model
vec3 specularBRDF( float roughness, vec3 f0, float NoH, float NoV, float NoL, float LoH )
{
	float a = roughness * roughness;

	// Normal Distribution Function
	float D = D_GGX( NoH, a );

	// Fresnel Function
	vec3 F = F_Schlick( LoH, f0 );

	// Visibility Function (shadowing/masking)
	float G = G_Smith( NoV, NoL, roughness );
		
	// Norm factor
	vec3 spec = D * G * F;
	spec /= (4.0 * NoL * NoV + 1e-6);

	return spec;
}

\normalmap_functions

mat3 cotangent_frame(vec3 N, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx( p );
	vec3 dp2 = dFdy( p );
	vec2 duv1 = dFdx( uv );
	vec2 duv2 = dFdy( uv );
	
	// solve the linear system
	vec3 dp2perp = cross( dp2, N );
	vec3 dp1perp = cross( N, dp1 );
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
 
	// construct a scale-invariant frame 
	float invmax = inversesqrt( max( dot(T,T), dot(B,B) ) );
	return mat3( T * invmax, B * invmax, N );
}

vec3 perturbNormal(vec3 N, vec3 WP, vec2 uv, vec3 normal_pixel)
{
	normal_pixel = normal_pixel * 255./127. - 128./127.;
	mat3 TBN = cotangent_frame(N, WP, uv);
	return normalize(TBN * normal_pixel);
}


\ComputeLights

	vec3 light_add;
	float NoL;
	float shadow_factor = 1.0;
	if ( u_light_type == DIRECTIONALLIGHT)
	{
		L = u_light_front;
		NdotL = clamp( dot(N,L), 0.0, 1.0 );
		light_add = u_light_color;
		if(u_light_cast_shadow == 1)
			shadow_factor = computeShadow(v_world_position);
	}
	else if (u_light_type == SPOTLIGHT || u_light_type == POINTLIGHT) //spot and point
	{
		L = u_light_position - v_world_position;
		float dist = length(L);
		L = L / dist; 
		vec3 L = normalize(L);
		NdotL = clamp( dot(N,L), 0.0, 1.0 );

		float att_factor = u_light_max_distance - dist;
		att_factor /= u_light_max_distance;
		att_factor = max(att_factor, 0.0);

		float min_angle_cos = u_light_cone_info.y;
		float max_angle_cos = u_light_cone_info.x;
		if (u_light_type == SPOTLIGHT){
			NdotL = 1.0;
			vec3 D = normalize(u_light_front);
			float cos_angle = dot( D, L );
			if( cos_angle < min_angle_cos  ){
	 			att_factor = 0.0;
			} else if ( cos_angle < max_angle_cos) {
				att_factor *= (cos_angle - min_angle_cos) / (max_angle_cos - min_angle_cos);
			}
			if(u_light_cast_shadow == 1)
				shadow_factor = computeShadow(v_world_position);
		}

		
		light_add = u_light_color * att_factor;
	} 
	
	vec3 H = normalize(V + L);
	NoL = NdotL;
	float NoH = clamp( dot(N, H), 0.0, 1.0 );
	float NoV = clamp( dot(N, V), 0.0, 1.0 );
	float LoH = clamp( dot(L, H), 0.0, 1.0 );

	//we compute the reflection in base to the color and the metalness
	vec3 f0 = mix( vec3(0.5), color.xyz, metalness );

	//metallic materials do not have diffuse
	vec3 diffuseColor = (1.0 - metalness) * color.xyz;

	//compute the specular
	vec3 Fr_d = specularBRDF(roughness, f0, NoH, NoV, NoL, LoH); 

	// Here we use the Burley, but you can replace it by the Lambert.
	float linearRoughness = roughness * roughness;
	//vec3 Fd_d = diffuseColor * Fd_Lambert(color.xyz); 
	vec3 Fd_d = diffuseColor * Fd_Burley(NoV,NoL,LoH,linearRoughness); 
	
	//add diffuse and specular reflection
	vec3 direct = Fr_d + Fd_d;
	if (u_PBR == 0){
		direct = vec3(1.0);
	}
	light_add *= shadow_factor * NdotL * direct;


\gbuffers.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform sampler2D u_texture_emissive;
uniform sampler2D u_texture_normalmap;
uniform sampler2D u_texture_metallic_roughness;
uniform sampler2D u_texture_occlusion;
uniform float u_time;
uniform float u_alpha_cutoff;
uniform vec3 u_emissive_factor;
uniform bool u_norm_contr;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalColor;
layout(location = 2) out vec4 EmissiveOcclusion;
layout(location = 3) out vec4 ExtraColor;

#include "normalmap_functions"

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture, uv );
	
	if(color.a < u_alpha_cutoff)
		discard;
	
	vec3 N = normalize(v_normal);
	vec3 normal_pixel = texture( u_texture_normalmap, uv ).xyz;
	if(!u_norm_contr){
    		N = perturbNormal(v_normal, v_world_position, v_uv, normal_pixel);
	}

	FragColor = vec4(color.xyz, texture( u_texture_metallic_roughness, uv ).z);
	NormalColor = vec4(N * 0.5 + vec3(0.5), texture( u_texture_metallic_roughness, uv ).y);

	EmissiveOcclusion = vec4(texture( u_texture_emissive, uv ).xyz * u_emissive_factor, texture( u_texture_occlusion, uv ).x);

	ExtraColor = vec4(fract(v_world_position), 1.0);
} 

\texture.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_alpha_cutoff;

out vec4 FragColor;

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture, v_uv );

	if(color.a < u_alpha_cutoff)
		discard;

	FragColor = color;
}

\planar.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_texture;
uniform vec2 u_iRes;
uniform vec3 u_camera_position;

out vec4 FragColor;

void main()
{
	vec3 E = v_world_position - u_camera_position;
	E = normalize(E);
	vec3 N = normalize(v_normal);
	vec2 uv = gl_FragCoord.xy * u_iRes;
	uv.x = 1.0 -uv.x;
	vec3 color = texture( u_texture, uv).xyz;
	float f = 1.0 - max(0.0, dot(E, -N));
	FragColor = vec4(color * f, 1.0);
}



\probes

const float Pi = 3.141592654;
const float CosineA0 = Pi;
const float CosineA1 = (2.0 * Pi) / 3.0;
const float CosineA2 = Pi * 0.25;
struct SH9 { float c[9]; }; //to store weights
struct SH9Color { vec3 c[9]; }; //to store colors

void SHCosineLobe(in vec3 dir, out SH9 sh) //SH9
{
	// Band 0
	sh.c[0] = 0.282095 * CosineA0;
	// Band 1
	sh.c[1] = 0.488603 * dir.y * CosineA1; 
	sh.c[2] = 0.488603 * dir.z * CosineA1;
	sh.c[3] = 0.488603 * dir.x * CosineA1;
	// Band 2
	sh.c[4] = 1.092548 * dir.x * dir.y * CosineA2;
	sh.c[5] = 1.092548 * dir.y * dir.z * CosineA2;
	sh.c[6] = 0.315392 * (3.0 * dir.z * dir.z - 1.0) * CosineA2;
	sh.c[7] = 1.092548 * dir.x * dir.z * CosineA2;
	sh.c[8] = 0.546274 * (dir.x * dir.x - dir.y * dir.y) * CosineA2;
}

vec3 ComputeSHIrradiance(in vec3 normal, in SH9Color sh)
{
	// Compute the cosine lobe in SH, oriented about the normal direction
	SH9 shCosine;
	SHCosineLobe(normal, shCosine);
	// Compute the SH dot product to get irradiance
	vec3 irradiance = vec3(0.0);
	for(int i = 0; i < 9; ++i)
		irradiance += sh.c[i] * shCosine.c[i];

	return irradiance;
}


\probe.fs

#version 330 core

in vec3 v_world_position;
in vec3 v_normal;

uniform vec3 u_coeffs[9];

#include "probes"

out vec4 FragColor;

void main()
{
	vec3 color;
	vec3 N = normalize(v_normal);
	SH9Color sh;
	for(int i = 0; i < 9; ++i)
		sh.c[i] = u_coeffs[i];

	color.xyz = max(vec3(0.0), ComputeSHIrradiance(N, sh));

	FragColor = vec4(color, 1.0);
}

\reflectionProbe.fs

#version 330 core

in vec3 v_world_position;
in vec3 v_normal;

uniform vec3 u_camera_position;
uniform samplerCube u_environment_texture;

out vec4 FragColor;

void main()
{
	vec3 N = normalize(v_normal);
	vec3 E = v_world_position - u_camera_position;
	vec3 R = reflect(E, N); //for reflection
	//vec3 = refract(E, N, 0.1); //For refraction
	vec3 color = textureLod(u_environment_texture, R, 0.0).xyz;
	FragColor = vec4(max(color, vec3(0.0)), 1.0);
}


\ssao.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_depth_texture;
uniform sampler2D u_normal_texture;
uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;
uniform float u_radius;
uniform vec3 u_points[64];
uniform float u_max_distance;
uniform float u_linear_factor;
uniform vec3 u_camera_position;
uniform vec3 u_front;
uniform float u_far;
uniform float u_near;
uniform int u_ssao_plus;

layout(location = 0) out vec4 FragColor;

//random value from uv
float rand(vec2 co)
{
    return fract(sin(dot(co, vec2(12.9898, 78.233))) * 43758.5453123);
}

//create rotation matrix from arbitrary axis and angle
mat4 rotationMatrix( vec3 axis, float angle )
{
    axis = normalize(axis);
    float s = sin(angle);
    float c = cos(angle);
    float oc = 1.0 - c;
    
    return mat4(oc * axis.x * axis.x + c, oc * axis.x * axis.y - axis.z * s,  oc * axis.z * axis.x + axis.y * s,  0.0, oc * axis.x * axis.y + axis.z * s,  oc * axis.y * axis.y + c, oc * axis.y * axis.z - axis.x * s,  0.0,oc * axis.z * axis.x - axis.y * s,  oc * axis.y * axis.z + axis.x * s,  oc * axis.z * axis.z + c,           0.0, 0.0, 0.0, 0.0, 1.0);
}

float depthToLinear(float z)
{
	return u_near * (z + 1.0) / (u_far + u_near - z * (u_far - u_near));
}


void main()
{
	vec2 uv = gl_FragCoord.xy * u_iRes.xy;
	vec3 N = texture( u_normal_texture, uv ).xyz * 2.0 - vec3(1.0);
	N = normalize(N);
	float depth = texture( u_depth_texture,uv).x;
	if(depth == 1.0)
		discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	float dist = length(u_camera_position - v_world_position);
	
	int num = 64;
	
	for(int i = 0; i < 64; ++i)
	{
		vec3 random_point = u_points[i];
		
		if(u_ssao_plus == 1.0)
		{
			//check in which side of the normal
			if(dot(N,random_point) < 0.0)
				random_point *= -1.0;
		}

		vec3 offset = random_point * u_radius  * (dist * 0.001);
		mat4 rot = rotationMatrix( u_front, rand(gl_FragCoord.xy));
		offset = (rot * vec4(offset, 1.0)).xyz;
		vec3 p = v_world_position + offset;

		vec4 proj = u_viewprojection * vec4(p, 1.0);
		proj.xy /= proj.w; //convert to clipspace from homogeneous
		//apply a tiny bias to its z before converting to clip-space
		proj.z = (proj.z - 0.005) / proj.w;
		proj.xyz = proj.xyz * 0.5 + vec3(0.5); //to [0..1]

		//read p true depth
		float pdepth = texture( u_depth_texture, proj.xy ).x;
		//linearize the depth
		pdepth = depthToLinear(pdepth) ;
		float projz = depthToLinear(proj.z);
		float diff = pdepth - projz;
		//check how far it is
		if( diff < 0.0 && abs(diff) < u_max_distance) 
			num--;
	}

	float ao = float(num) / 64.0;

	ao = pow(ao, 1.0/u_linear_factor);
	FragColor = vec4(ao, ao, ao, 1.0); 
}

\irradiance_interpol.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_color_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;
uniform sampler2D u_extra_texture;
uniform sampler2D u_probes_texture;

uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform vec3 u_irr_start;
uniform vec3 u_irr_end;
uniform vec3 u_irr_dims;
uniform float u_irr_normal_distance;
uniform float u_irr_delta;
uniform int u_num_probes;
uniform float u_factor;
out vec4 FragColor;

#include "probes"

vec3 fetchSH(vec3 indices, vec3 N)
{
	
	float row = indices.x + indices.y * u_irr_dims.x + indices.z * u_irr_dims.x * u_irr_dims.y;
	float row_uv = (row + 1.0) / (u_num_probes + 1.0);
	SH9Color sh;
	const float d_uvx = 1.0 / 9.0;
    for (int i = 0; i < 9; ++i)
    {
        vec2 coeffs_uv = vec2((float(i) + 0.5) * d_uvx, row_uv);
        sh.c[i] = texture(u_probes_texture, coeffs_uv).xyz;
    }
    return ComputeSHIrradiance(N, sh);
}

void main()
{
	vec2 uv = gl_FragCoord.xy *u_iRes.xy;
	vec4 color = texture( u_color_texture, uv);
	vec3 N = texture( u_normal_texture, uv ).xyz * 2.0 - vec3(1.0);
	float depth = texture( u_depth_texture, uv).x;

	if(depth == 1.0)
		discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);	
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 worldpos = proj_worldpos.xyz / proj_worldpos.w;
	N = normalize(N);	

	//computing nearest probe index based on world position
	vec3 irr_range = u_irr_end - u_irr_start;
	vec3 irr_local_pos = clamp( worldpos - u_irr_start + N * u_irr_normal_distance, vec3(0.0), irr_range );

	//convert from world pos to grid pos
	vec3 irr_norm_pos = irr_local_pos / u_irr_delta;

	//round values as we cannot fetch between rows for now
	vec3 local_indices = floor( irr_norm_pos );

	//now we have the interpolation factors
	vec3 factors = irr_norm_pos - local_indices; 

	// Compute indices for the 8 surrounding probes
   	vec3 indicesLBF = local_indices; // Left-Bottom-Far
    vec3 indicesRBF = local_indices; indicesRBF.x += 1.0; // Right-Bottom-Far
    vec3 indicesLTF = local_indices; indicesLTF.y += 1.0; // Left-Top-Far
    vec3 indicesRTF = local_indices; indicesRTF.x += 1.0; indicesRTF.y += 1.0; // Right-Top-Far
    vec3 indicesLBN = local_indices; indicesLBN.z += 1.0; // Left-Bottom-Near
    vec3 indicesRBN = local_indices; indicesRBN.x += 1.0; indicesRBN.z += 1.0; // Right-Bottom-Near
    vec3 indicesLTN = local_indices; indicesLTN.y += 1.0; indicesLTN.z += 1.0; // Left-Top-Near
    vec3 indicesRTN = local_indices; indicesRTN.x += 1.0; indicesRTN.y += 1.0; indicesRTN.z += 1.0; // Right-Top-Near

    // Compute irradiance for every corner
    vec3 irrLBF = fetchSH(indicesLBF, N);
    vec3 irrRBF = fetchSH(indicesRBF, N);
    vec3 irrLTF = fetchSH(indicesLTF, N);
    vec3 irrRTF = fetchSH(indicesRTF, N);
    vec3 irrLBN = fetchSH(indicesLBN, N);
    vec3 irrRBN = fetchSH(indicesRBN, N);
    vec3 irrLTN = fetchSH(indicesLTN, N);
    vec3 irrRTN = fetchSH(indicesRTN, N);

	vec3 irrTF = mix( irrLTF, irrRTF, factors.x );
	//vec3 irrTF = mix( irrLTF, irrRTF, 0.5 );
	vec3 irrBF = mix( irrLBF, irrRBF, factors.x );
	vec3 irrTN = mix( irrLTN, irrRTN, factors.x );
	vec3 irrBN = mix( irrLBN, irrRBN, factors.x );

	vec3 irrT = mix( irrTF, irrTN, factors.z );
	vec3 irrB = mix( irrBF, irrBN, factors.z );

	vec3 irradiance = mix( irrB, irrT, factors.y );
    irradiance *= u_factor;

	FragColor = vec4(max(irrTF, vec3(0.0)), 1.0);

}

\irradiance.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_color_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;
uniform sampler2D u_extra_texture;
uniform sampler2D u_probes_texture;

uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform vec3 u_irr_start;
uniform vec3 u_irr_end;
uniform vec3 u_irr_dims;
uniform float u_irr_normal_distance;
uniform float u_irr_delta;
uniform int u_num_probes;
uniform float u_factor;
out vec4 FragColor;

#include "probes"

void main()
{
	vec2 uv = gl_FragCoord.xy *u_iRes.xy;
	vec4 color = texture( u_color_texture, uv);
	vec3 N = texture( u_normal_texture, uv ).xyz * 2.0 - vec3(1.0);
	float depth = texture( u_depth_texture, uv).x;

	if(depth == 1.0)
		discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);	
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 worldpos = proj_worldpos.xyz / proj_worldpos.w;
	N = normalize(N);	

	//computing nearest probe index based on world position
	vec3 irr_range = u_irr_end - u_irr_start;
	vec3 irr_local_pos = clamp( worldpos - u_irr_start + N * u_irr_normal_distance, vec3(0.0), irr_range );

	//convert from world pos to grid pos
	vec3 irr_norm_pos = irr_local_pos / u_irr_delta;

	//round values as we cannot fetch between rows for now
	vec3 local_indices = round( irr_norm_pos );

	//compute in which row is the probe stored
	float row = local_indices.x + 
	local_indices.y * u_irr_dims.x + 
	local_indices.z * u_irr_dims.x * u_irr_dims.y;

	//find the UV.y coord of that row in the probes texture
	float row_uv = (row + 1.0) / (u_num_probes + 1.0);

	SH9Color sh;
	//fill the coefficients
	const float d_uvx = 1.0 / 9.0;
	for(int i = 0; i < 9; ++i)
	{
		vec2 coeffs_uv = vec2( (float(i)+0.5) * d_uvx, row_uv );
		sh.c[i] = texture( u_probes_texture, coeffs_uv).xyz;
	}

	//now we can use the coefficients to compute the irradiance
	vec3 irradiance = ComputeSHIrradiance( N, sh );

    irradiance *= u_factor;

	FragColor = vec4(max(irradiance, vec3(0.0)), 1.0);

}

\reflection.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_color_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;
uniform sampler2D u_extra_texture;
uniform samplerCube u_probes_texture;

uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform vec3 u_refl_start;
uniform vec3 u_refl_end;
uniform vec3 u_refl_dims;
uniform float u_refl_normal_distance;
uniform float u_refl_delta;
uniform int u_num_refl_probes;
uniform float u_refl_factor;
out vec4 FragColor;


void main()
{
	vec2 uv = gl_FragCoord.xy *u_iRes.xy;
	vec4 color = texture( u_color_texture, uv);
	vec3 N = texture( u_normal_texture, uv ).xyz * 2.0 - vec3(1.0);
	float depth = texture( u_depth_texture, uv).x;


	if(depth == 1.0)
		discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);	
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 worldpos = proj_worldpos.xyz / proj_worldpos.w;
	N = normalize(N);	

	//computing nearest probe index based on world position
	vec3 refl_range = u_refl_end - u_refl_start;
	vec3 refl_local_pos = clamp( worldpos - u_refl_start + N * u_refl_normal_distance, vec3(0.0), refl_range );

	//convert from world pos to grid pos
	vec3 refl_norm_pos = refl_local_pos / u_refl_delta;

	//round values as we cannot fetch between rows for now
	vec3 local_indices = round( refl_norm_pos );

	//compute in which row is the probe stored
	float row = local_indices.x + 
	local_indices.y * u_refl_dims.x + 
	local_indices.z * u_refl_dims.x * u_refl_dims.y;

	//find the UV.y coord of that row in the probes texture
	float row_uv = (row + 1.0) / (u_num_refl_probes + 1.0);

	float metalness = color.a;
	float roughness = texture(u_normal_texture, uv).a;
	
	vec3 reflection = texture(u_probes_texture, normalize(vec3(worldpos - u_refl_start))).xyz;
    reflection *= metalness;
    reflection *= u_refl_factor;

	FragColor = vec4(max(reflection, vec3(0.0)), 1.0);

}

\deferred_global.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;
in vec3 v_world_position;
in vec3 v_normal;

uniform sampler2D u_color_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;
uniform sampler2D u_emissive_occlusion_texture;
//uniform sampler2D u_normalmap_texture;
uniform sampler2D u_ao_texture;

uniform vec3 u_ambient_light;

uniform vec3 u_light_position;
uniform vec3 u_light_color;
uniform int u_light_type;
uniform vec3 u_light_front;
uniform vec2 u_light_cone_info;
uniform float u_light_max_distance;
uniform vec3 u_emissive_first;
uniform mat4 u_inverse_viewprojection;
uniform vec2 u_iRes;
uniform vec3 u_camera_pos;
uniform float u_linear_factor;
uniform int u_linear_space;

uniform int u_PBR;

#define POINTLIGHT 1
#define SPOTLIGHT 2
#define DIRECTIONALLIGHT 3

#define LINEAR_SPACE 1

out vec4 FragColor;
out float glFragDepth;

#include "GammaToLinear"

#include "ComputeShadow"

#include "specullar_function"

#include "normalmap_functions"

void main()
{
	vec2 uv = gl_FragCoord.xy *u_iRes.xy;
	float depth = texture( u_depth_texture, uv).x;

	if(depth == 1.0)
		discard;

	vec4 GB0 = texture( u_color_texture, uv );
	vec4 GB1 = texture( u_normal_texture, uv );
	float occlusion = texture(u_emissive_occlusion_texture, uv).w; 
	vec3 emissive;
	vec3 color;
	if (u_linear_space == LINEAR_SPACE){
		color = degamma(GB0.xyz);
		emissive = degamma(texture(u_emissive_occlusion_texture, uv).xyz);
	} else {
		color = GB0.xyz;
		emissive = texture(u_emissive_occlusion_texture, uv).xyz;
	}
	float metalness = GB0.a;
	float roughness = GB1.a;

	float ao_factor = texture( u_ao_texture, uv ).x;

	ao_factor = pow( ao_factor, 1.0/u_linear_factor);

	ao_factor = clamp( ao_factor, 0.0, 1.0);

	vec3 light = u_ambient_light * occlusion * ao_factor;
	//vec3 light = u_ambient_light * ao_factor;


	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	vec3 L;
	vec3 normal = GB1.xyz * 2.0 - vec3(1.0);
	vec3 N = normalize(normal);
	
	vec3 V = normalize(u_camera_pos - v_world_position);

	float NdotL = 0.0;
	
	#include "ComputeLights"

	vec3 final_color;

	final_color = ((NdotL * light_add) + light) * color.xyz + emissive * u_emissive_first;

	if (u_linear_space == LINEAR_SPACE){
		FragColor = vec4(gamma(final_color), 1.0);
	} else {
		FragColor = vec4(final_color, 1.0);
	}
	glFragDepth = depth;

}


\light.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform vec4 u_color;
uniform sampler2D u_texture_albedo;
uniform sampler2D u_texture_emissive;
uniform sampler2D u_texture_occlusion;
uniform sampler2D u_texture_normalmap;
uniform sampler2D u_texture_metallic_roughness; //Still not used
uniform float u_time;
uniform float u_alpha_cutoff;
uniform bool u_norm_contr;
uniform vec3 u_camera_pos;

uniform vec3 u_ambient_light;
uniform vec3 u_emissive_factor;
uniform vec3 u_light_position;
uniform vec3 u_light_color;
uniform vec3 u_light_front;
uniform float u_light_max_distance;
uniform vec2 u_light_cone_info;

uniform int u_light_type;
uniform int u_PBR;
uniform int u_linear_space;

#define POINTLIGHT 1
#define SPOTLIGHT 2
#define DIRECTIONALLIGHT 3

#define LINEAR_SPACE 1

out vec4 FragColor;

#include "GammaToLinear"

#include "ComputeShadow"

#include "specullar_function"

#include "normalmap_functions"

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture_albedo, v_uv );
	vec3 emissive;

	if(color.a < u_alpha_cutoff)
		discard;

	if(v_world_position.y < 0.0)
		discard;

	if (u_linear_space == LINEAR_SPACE){
		color.xyz = degamma(color.xyz);
		emissive = degamma(texture(u_texture_emissive, v_uv).xyz);
	} else {
		emissive = texture(u_texture_emissive, v_uv).xyz;
	}
	
	float metalness = texture(u_texture_metallic_roughness, v_uv).z;
	float roughness = texture(u_texture_metallic_roughness, v_uv).y;

	vec3 light = u_ambient_light * texture(u_texture_occlusion, v_uv).x;
	
	vec3 L;
	vec3 N = normalize(v_normal);
	vec3 normal = texture(u_texture_normalmap, v_uv).xyz;
	if(!u_norm_contr){
    		N = perturbNormal(v_normal, v_world_position, v_uv, normal);
	}
	float NdotL = 0.0;
	vec3 V = normalize(u_camera_pos - v_world_position);

	#include "ComputeLights"

	light += (NdotL * light_add);

	vec4 final_color;
	if (u_linear_space == LINEAR_SPACE){
		final_color.xyz = gamma((color.xyz * light) + u_emissive_factor * emissive);
	} else {
		final_color.xyz = (color.xyz * light) + u_emissive_factor * emissive;
	}
	final_color.a = color.a;
	
	FragColor = final_color;
}


\skybox.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;

uniform samplerCube u_texture;
uniform vec3 u_camera_position;
out vec4 FragColor;

void main()
{
	vec3 E = v_world_position - u_camera_position;
	vec4 color = textureLod( u_texture, E, 0.0 );
	FragColor = color;
}


\multi.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_alpha_cutoff;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalColor;

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture, uv );

	if(color.a < u_alpha_cutoff)
		discard;

	vec3 N = normalize(v_normal);

	FragColor = color;
	NormalColor = vec4(N,1.0);
}


\depth.fs

#version 330 core

uniform vec2 u_camera_nearfar;
uniform sampler2D u_texture; //depth map
in vec2 v_uv;
out vec4 FragColor;

void main()
{
	float n = u_camera_nearfar.x;
	float f = u_camera_nearfar.y;
	float z = texture2D(u_texture,v_uv).x;
	if( n == 0.0 && f == 1.0 )
		FragColor = vec4(z);
	else
		FragColor = vec4( n * (z + 1.0) / (f + n - z * (f - n)) );
}


\instanced.vs

#version 330 core

in vec3 a_vertex;
in vec3 a_normal;
in vec2 a_coord;

in mat4 u_model;

uniform vec3 u_camera_pos;

uniform mat4 u_viewprojection;

//this will store the color for the pixel shader
out vec3 v_position;
out vec3 v_world_position;
out vec3 v_normal;
out vec2 v_uv;

void main()
{	
	//calcule the normal in camera space (the NormalMatrix is like ViewMatrix but without traslation)
	v_normal = (u_model * vec4( a_normal, 0.0) ).xyz;
	
	//calcule the vertex in object space
	v_position = a_vertex;
	v_world_position = (u_model * vec4( a_vertex, 1.0) ).xyz;
	
	//store the texture coordinates
	v_uv = a_coord;

	//calcule the position of the vertex using the matrices
	gl_Position = u_viewprojection * vec4( v_world_position, 1.0 );
}

\blurr.fs

#version 330 core

in vec2 v_uv;

uniform sampler2D u_texture;
uniform int u_kernel_size;
const int MAX_KERNEL_SIZE = 100;
uniform float u_weight[MAX_KERNEL_SIZE];

layout(location = 0) out vec4 FragColor;

void main()
{             
	vec2 uv = v_uv;
    	vec2 tex_offset = 1.0 / vec2(textureSize(u_texture, 0)); // gets size of single texel
    	vec3 result = texture(u_texture, uv).rgb * u_weight[0];
	float total_weight = u_weight[0];
	int half_kernel = u_kernel_size / 2;
        for(int i = -half_kernel; i < half_kernel; ++i)
        {
		for(int j = -half_kernel; j < half_kernel; ++j)
        	{
			result += texture(u_texture, uv + vec2(tex_offset.x * float(i), tex_offset.y * float(j))).rgb * u_weight[abs(i) + abs(j)];
			total_weight += u_weight[abs(i) + abs(j)];
		}
        }

    	FragColor = vec4(result / total_weight, 1.0);
}

\motion_blurr.fs

#version 330 core

uniform sampler2D u_texture;
uniform sampler2D u_depth_texture;
uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection_prev;
uniform vec2 u_iRes;

layout(location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * u_iRes.xy;
	float depth = texture( u_depth_texture, uv).x;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	vec4 prev = u_viewprojection_prev * vec4(v_world_position, 1.0);
	prev.xyz /= prev.w;
	prev.xy = (prev.xy + vec2(1.0)) / 2.0;

	vec4 color = vec4(0.0);

	for(int i = 0; i < 10; ++i)
		color += texture(u_texture, mix(uv, prev.xy, i/9.0));
	color /= 9.0;
	FragColor = color;
	//FragColor = vec4(1.0);
}


\Noise

float rand(vec2 n) { 
	return fract(sin(dot(n, vec2(12.9898, 4.1414))) * 43758.5453);
}

float noise(vec2 p){
	vec2 ip = floor(p);
	vec2 u = fract(p);
	u = u*u*(3.0-2.0*u);
	
	float res = mix(
		mix(rand(ip),rand(ip+vec2(1.0,0.0)),u.x),
		mix(rand(ip+vec2(0.0,1.0)),rand(ip+vec2(1.0,1.0)),u.x),u.y);
	return res*res;
}

float mod289(float x){return x - floor(x * (1.0 / 289.0)) * 289.0;}
vec4 mod289(vec4 x){return x - floor(x * (1.0 / 289.0)) * 289.0;}
vec4 perm(vec4 x){return mod289(((x * 34.0) + 1.0) * x);}

float noise(vec3 p){
    vec3 a = floor(p);
    vec3 d = p - a;
    d = d * d * (3.0 - 2.0 * d);

    vec4 b = a.xxyy + vec4(0.0, 1.0, 0.0, 1.0);
    vec4 k1 = perm(b.xyxy);
    vec4 k2 = perm(k1.xyxy + b.zzww);

    vec4 c = k2 + a.zzzz;
    vec4 k3 = perm(c);
    vec4 k4 = perm(c + 1.0);

    vec4 o1 = fract(k3 * (1.0 / 41.0));
    vec4 o2 = fract(k4 * (1.0 / 41.0));

    vec4 o3 = o2 * d.z + o1 * (1.0 - d.z);
    vec2 o4 = o3.yw * d.x + o3.xz * (1.0 - d.x);

    return o4.y * d.y + o4.x * (1.0 - d.y);
}

\volumetric.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_depth_texture;
uniform sampler2D u_normal_texture;
uniform vec3 u_camera_position;
uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform vec3 u_light_color;
uniform vec3 u_light_position;

uniform vec3 u_ambient_light;
uniform float u_air_density;
uniform float u_time;

#include "ComputeShadow"

#include "Noise"

layout(location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * u_iRes.xy;
	vec3 N = texture( u_normal_texture, v_uv ).xyz * 2.0 - vec3(1.0);
	N = normalize(N);
	float depth = texture( u_depth_texture, v_uv).x;
	//if(depth == 1.0)
		//discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	const int MAX_ITERATIONS = 64;
	
	vec3 V;
	float dist;
        V = v_world_position - u_camera_position;
        dist = min(length(V), 500.0);
        V /= dist;

	float step_dist = dist / float(MAX_ITERATIONS);
	vec3 ray_step = V * step_dist;
	vec3 current_pos = u_camera_position;
	current_pos += ray_step * noise(gl_FragCoord.xy);

	vec3 light = u_ambient_light;
	float translucency = 1.0;


	for(int i = 0; i < MAX_ITERATIONS; ++i)
	{
		float particle_density = max(0.0, noise(current_pos * vec3(0.1,0.3,0.1) + vec3(u_time, 0.0, u_time)) - max(0.0, current_pos.y) * 0.002);
		light += computeShadow(current_pos) * 0.001 * u_light_color * step_dist * particle_density;		

		current_pos += ray_step;
		translucency -= u_air_density * step_dist * particle_density;
		if( translucency <= 0.0 )
			break;
	}

	FragColor = vec4(light, clamp(1.0 - translucency, 0.0, 1.0)); 
}

\volumetric_lights.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_depth_texture;
uniform sampler2D u_normal_texture;
uniform vec3 u_camera_position;
uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform vec3 u_light_color;
uniform vec3 u_light_position;
uniform int u_light_type;
uniform vec3 u_light_front;
uniform float u_light_max_distance;
uniform vec2 u_light_cone_info;

uniform vec3 u_ambient_light;
uniform float u_weight_ambient_light;
uniform float u_air_density;
uniform float u_time;

#define POINTLIGHT 1
#define SPOTLIGHT 2
#define DIRECTIONALLIGHT 3

#include "ComputeShadow"

#include "Noise"

layout(location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * u_iRes.xy;
	vec3 N = texture( u_normal_texture, v_uv ).xyz * 2.0 - vec3(1.0);
	N = normalize(N);
	float depth = texture( u_depth_texture, v_uv).x;
	//if(depth == 1.0)
		//discard;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	const int MAX_ITERATIONS = 64;
	
	vec3 V;
	float dist;
        V = v_world_position - u_camera_position;
        dist = min(length(V), 500.0);
        V /= dist;

	float step_dist = dist / float(MAX_ITERATIONS);
	vec3 ray_step = V * step_dist;
	vec3 current_pos = u_camera_position;
	current_pos += ray_step * noise(gl_FragCoord.xy);

	vec3 light = u_ambient_light;
	float translucency = 1.0;


	for(int i = 0; i < MAX_ITERATIONS; ++i)
	{
		float height_factor = max(0.0, current_pos.y) * 0.002;
		float particle_density = max(0.0, noise(current_pos * vec3(0.1,0.3,0.1) + vec3(u_time, 0.0, u_time)) - height_factor);		
		float shadow_factor = 1.0;
		vec3 add_light = vec3(0.0);
		if (u_light_cast_shadow == 0)
			shadow_factor = computeShadow(current_pos);
		
		if (u_light_type == DIRECTIONALLIGHT)
		{
			add_light = u_light_color;		
		}
		else if (u_light_type == POINTLIGHT || u_light_type == SPOTLIGHT)
		{
			vec3 L = u_light_position - current_pos;
			float dist = length(L);
			L /= dist;
			L = normalize(L);
		
			float att_factor = u_light_max_distance - dist;
			att_factor /= u_light_max_distance;
			att_factor = max(att_factor, 0.0);

			float min_angle_cos = u_light_cone_info.y;
			float max_angle_cos = u_light_cone_info.x;
			if (u_light_type == SPOTLIGHT){
				vec3 D = normalize(u_light_front);
				float cos_angle = dot( D, L );
				if( cos_angle < min_angle_cos  ){
	 				att_factor = 0.0;
				} else if ( cos_angle < max_angle_cos) {
					att_factor *= (cos_angle - min_angle_cos) / (max_angle_cos - min_angle_cos);
				}
			}
			add_light = u_light_color * att_factor;
		}
		
		add_light *= shadow_factor;
		light += step_dist * particle_density * ((1.0 / float(MAX_ITERATIONS)) / u_weight_ambient_light) * add_light;
		translucency -= u_air_density * step_dist * particle_density;

		current_pos += ray_step;

		if( translucency <= 0.0 )
			break;
	}
	if(light == vec3(0.0))
		discard;

	FragColor = vec4(light, clamp(1.0 - translucency, 0.0, 1.0)); 
}


\tonemapper.fs

#version 330 core

in vec2 v_uv;

uniform sampler2D u_texture;
uniform float u_scale; //color scale before tonemapper
uniform float u_average_lum; 
uniform float u_lumwhite2;
uniform float u_igamma; //inverse gamma

out vec4 FragColor;

void main()
{
	vec2 uv = v_uv;
	vec4 color = texture2D( u_texture, uv );
	vec3 rgb = color.xyz;

	float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
	float L = (u_scale / u_average_lum) * lum;
	float Ld = (L * (1.0 + L / u_lumwhite2)) / (1.0 + L);

	rgb = (rgb / lum) * Ld;
	rgb = max(rgb,vec3(0.001));
	rgb = pow( rgb, vec3( u_igamma ) );
	gl_FragColor = vec4( rgb, color.a );

}


\decal.fs

#version 330 core

in vec3 v_position;
in vec2 v_uv;

uniform sampler2D u_depth_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_color_texture;
uniform sampler2D u_emissive_occlusion_texture;
uniform sampler2D u_decal_texture;
uniform vec3 u_camera_position;
uniform mat4 u_inverse_viewprojection;
uniform mat4 u_viewprojection;
uniform vec2 u_iRes;

uniform mat4 u_imodel;

layout(location = 0) out vec4 FragColor;

void main()
{
	vec2 uv = gl_FragCoord.xy * u_iRes.xy;

	float depth = texture( u_depth_texture, uv).x;

	vec4 screen_pos = vec4(uv.x*2.0-1.0, uv.y*2.0-1.0, depth*2.0-1.0, 1.0);
	vec4 proj_worldpos = u_inverse_viewprojection * screen_pos;
	vec3 v_world_position = proj_worldpos.xyz / proj_worldpos.w;

	vec3 localpos = (u_imodel * vec4(v_world_position, 1.0)).xyz;

	//if outside of the volume
	if(     localpos.x < -0.5 || localpos.x > 0.5 ||
    		localpos.y < -0.5 || localpos.y > 0.5 ||
    		localpos.z < -0.5 || localpos.z > 0.5 )
		discard;

	//use XZ as UVs, remap to 0..1 range
	vec2 decal_uv = localpos.xz + vec2(0.5);

	vec4 color = texture( u_decal_texture, decal_uv);

	//skip transparent pixels
	if(color.a == 0.0)
		discard;

	FragColor = color; 
}

