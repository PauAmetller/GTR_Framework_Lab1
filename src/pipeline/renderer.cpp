#include "renderer.h"

#include <algorithm> //sort

#include "camera.h"
#include "../gfx/gfx.h"
#include "../gfx/shader.h"
#include "../gfx/mesh.h"
#include "../gfx/texture.h"
#include "../gfx/fbo.h"
#include "../pipeline/prefab.h"
#include "../pipeline/material.h"
#include "../pipeline/animation.h"
#include "../utils/utils.h"
#include "../extra/hdre.h"
#include "../core/ui.h"

#include "scene.h"


using namespace SCN;

//some globals
GFX::Mesh sphere;
GFX::Mesh cube;

GFX::FBO* gbuffers = nullptr;
GFX::FBO* illumination = nullptr;
GFX::FBO* ssao_fbo = nullptr;
GFX::FBO* ssao_blurr = nullptr;
GFX::FBO* irr_fbo = nullptr;
GFX::FBO* reflection_fbo = nullptr;
GFX::FBO* volumetric_fbo = nullptr;
GFX::FBO* final_fbo = nullptr;
GFX::FBO* postFxA_fbo = nullptr;
GFX::FBO* postFxB_fbo = nullptr;

std::vector<vec3> random_points;
std::vector<float> weights;

GFX::Texture* cloned_depth_texture = nullptr;

//For motion blurr
mat4 viewprojection_previous_frame;

GFX::Mesh plane;

vec3 start;
vec3 end;
vec3 dim;
int probe_size;

vec3 start_reflect;
vec3 end_reflect;
vec3 dim_reflect;
int probe_size_reflect;

Renderer::Renderer(const char* shader_atlas_filename)
{
	render_wireframe = false;
	render_boundaries = false;
	deactivate_ambient_light = false;
	albedo_texture = false;
	emissive_texture = false;
	occlusion_texture = true;
	metallicRoughness_texture = false;
	normalMap_texture = false;
	white_textures = false;
	skip_lights = false;
	skip_shadows = false;
	skip_alpha_renderables = false;
	Remove_PBR = false;
	show_ssao = false;
	ssao_texture = false;
	blurr = false;
	scene = nullptr;
	skybox_cubemap = nullptr;
	moon_light = nullptr;
	deactivate_tonemapper = false;
	Linear_space = false;
	probes_grid = false;
	reflection_probes_grid = false;
	volumetric_light = false;
	decal = false;
	deactivate_motion_blur = false;
	enableReflectionCapture = false;
	enableIrradianceCapture = false;
	apply_irradiance = true;
	interpolate = false;
	apply_reflection = false;

	irradiance_factor = 1.0;
	reflection_factor = 1.0;

	pipeline_mode = ePipelineMode::DEFERRED;
	show_gbuffer = eShowGBuffer::NONE;
	ssao_mode = eSSAOMODE::SSAO_PLUS;

	shadow_map_size = 1024;
	irradiance_capture_size = 64;

	if (!GFX::Shader::LoadAtlas(shader_atlas_filename))
		exit(1);
	GFX::checkGLErrors();

	sphere.createSphere(1.0f);
	sphere.uploadToVRAM();
	cube.createCube(1.0f);
	cube.uploadToVRAM();

	for (int i = 0; i < NUM_SHADOW_MAPS; i++) {
		shadow_maps[i] = nullptr;
	}


	ssao_radius = 5.0;
	ssao_max_distance = 1.0f;
	ssao_linear = 2.2f;
	random_points = generateSpherePoints(64, 1, false);
	kernel_size = 5;
	sigma = 1.0f;
	weights = calculate_weights(kernel_size, sigma);

	//Volumetric
	air_density = 0.004;
	weight_ambient_light = 1.0;

	//Tonemapper
	scale = 1.0;
	average_lum = 1.0;
	lumwhite2 = 1.0;
	igamma = 1.0;

	probe_size = 3;

	start = vec3(-80, 0, -90);
	end = vec3(80, 80, 90);
	dim = vec3(10, 4, 10);
	initProbes(start, end, dim);


	probe_size_reflect = 3;
	start_reflect = vec3(-80, 0, -90);
	end_reflect = vec3(80, 80, 90);
	dim_reflect = vec3(10, 4, 10);
	initProbesReflection(start_reflect, end_reflect, dim_reflect);
	plane.createPlane(1);
}


void Renderer::setupScene()
{
	if (scene->skybox_filename.size())
		skybox_cubemap = GFX::Texture::Get(std::string(scene->base_folder + "/" + scene->skybox_filename).c_str());
	else
		skybox_cubemap = nullptr;
}

//////////////////
void Renderer::extractRenderables(SCN::Node* node, Camera* camera) {

	if (!node->visible)
		return;

	Matrix44 node_model = node->getGlobalMatrix(true);

	//Render if node has mesh and material
	if (node->material && node->mesh) {

		//compute the bounding box of the object in the world space
		BoundingBox world_bounding = transformBoundingBox(node_model, node->mesh->box);

		Renderable re;
		re.model = node_model;
		re.mesh = node->mesh;
		re.material = node->material;
		re.distance_to_camera = camera->eye.distance(world_bounding.center);
		re.bounding = world_bounding;
		renderables.push_back(re);
		if (re.material->alpha_mode == SCN::eAlphaMode::BLEND) {
			if(!skip_alpha_renderables)
				alphaRenderables.push_back(re);
		}
		else {
			opaqueRenderables.push_back(re);
		}
	}

	//iterate recursibely with its children entities
	for (size_t i = 0; i < node->children.size(); i++) {
		extractRenderables(node->children[i], camera);
	}
}

void Renderer::extractSceneInfo(SCN::Scene* scene, Camera* camera) {

	renderables.clear();
	lights.clear();
	decals.clear();
	directional_lights.clear();
	point_and_spot_lights.clear();
	opaqueRenderables.clear();
	alphaRenderables.clear();
	moon_light = nullptr;

	//prepare entities
	for (size_t i = 0; i < scene->entities.size(); i++) {

		BaseEntity* ent = scene->entities[i];

		//Skip if entity not visible
		if (!ent->visible)
			continue;

		if(ent->getType() == eEntityType::PREFAB){

			PrefabEntity* pent = (SCN::PrefabEntity*)ent;
			if (pent->prefab) {
				extractRenderables(&pent->root, camera);
			}
		} else if (ent->getType() == eEntityType::LIGHT) {

			LightEntity* light = (SCN::LightEntity*)ent;
			mat4 model = light->root.getGlobalMatrix();

			if (light->light_type == SCN::eLightType::DIRECTIONAL || camera->testSphereInFrustum(model.getTranslation(), light->max_distance)) {
				lights.push_back(light);
			}
			if (!moon_light && light->light_type == SCN::eLightType::DIRECTIONAL) {
				moon_light = light;
			}
			if (light->light_type == eLightType::DIRECTIONAL) {
				directional_lights.push_back(light);
			}
			else if (camera->testSphereInFrustum(model.getTranslation(), light->max_distance)) {
				point_and_spot_lights.push_back(light);
			}
		} else if (ent->getType() == eEntityType::DECAL) {

			DecalEntity* decal = (SCN::DecalEntity*)ent;
			decals.push_back(decal);
		}
	}
}

void Renderer::generateShadowMaps(Camera* main_camera) {
	shadow_map_index = 0;
	
	for (LightEntity* light : lights) {
		light->has_shadow_map = false;;
		if (light->light_type == SCN::eLightType::POINT || !light->cast_shadows) {
			continue;
		}
		else {
			light->has_shadow_map = true;
			if (shadow_maps[shadow_map_index] == nullptr || shadow_maps[shadow_map_index]->width != shadow_map_size) {
				if (shadow_maps[shadow_map_index])
					delete shadow_maps[shadow_map_index];
				shadow_maps[shadow_map_index] = new GFX::FBO();
				shadow_maps[shadow_map_index]->setDepthOnly(shadow_map_size, shadow_map_size);
			}
			shadow_maps[shadow_map_index]->bind();
			glClear(GL_DEPTH_BUFFER_BIT);

			Camera camera;
			vec3 pos= light->getGlobalPosition();
			if (light->light_type == SCN::eLightType::DIRECTIONAL) {
				pos = main_camera->eye;
				camera.lookAt(pos, pos + light->root.global_model.frontVector() * -1.0f, vec3(0, 1, 0));
				camera.setOrthographic(light->area * -0.5, light->area * 0.5, light->area * -0.5, light->area * 0.5, light->near_distance, light->max_distance);
			}
			else if (light->light_type == SCN::eLightType::SPOT) {
				vec3 front = light->root.model.rotateVector(vec3(0, 0, 1));
				camera.lookAt(pos, pos + front * -1.0f, vec3(0, 1, 0));
				camera.setPerspective(light->cone_info.y, 1.0f, light->near_distance, light->max_distance);
			}

			// compute texel size in world units, where frustum size is the distance from left to right in the camera
			float grid = light->area / (float)shadow_maps[shadow_map_index]->width;

			camera.enable();

			//snap camera X,Y to that size in camera space assumingthe frustum is square, otherwise compute gridx and gridy
			camera.view_matrix.M[3][0] = round(camera.view_matrix.M[3][0] / grid) * grid;
			camera.view_matrix.M[3][1] = round(camera.view_matrix.M[3][1] / grid) * grid;

			//update viewproj matrix (be sure no one changes it)
			camera.viewprojection_matrix = camera.view_matrix * camera.projection_matrix;

			for (auto& re : opaqueRenderables) {
				if (camera.testBoxInFrustum(re.bounding.center, re.bounding.halfsize))
					renderMeshWithMaterialFlat(re.model, re.mesh, re.material);
			}
			light->shadowmap_view_projection = camera.viewprojection_matrix;

			shadow_maps[shadow_map_index]->unbind();
			light->shadow_map_index = shadow_map_index;

			if (shadow_map_index < NUM_SHADOW_MAPS - 1)
				shadow_map_index++;
			else
				printf("Max number of shadow maps achieved, incrise it to be able to have more shadow maps (NUM_SHADOW_MAPS)");
		}
	}
}
////////////////


void Renderer::renderScene(SCN::Scene* scene, Camera* camera)
{
	this->scene = scene;
	setupScene();
	extractSceneInfo(scene, camera);
	if (!skip_shadows)
		generateShadowMaps(camera);

	capturePlanarReflection(camera);

	vec2 size = CORE::getWindowSize();

	//Postprocessing
	if (!final_fbo || (final_fbo->width != size.x || final_fbo->height != size.y))
	{
		final_fbo = new GFX::FBO();
		final_fbo->create(size.x, size.y, 1, GL_RGB, GL_HALF_FLOAT);
	}

	if (!postFxA_fbo || (postFxA_fbo->width != size.x || postFxA_fbo->height != size.y))
	{
		postFxA_fbo = new GFX::FBO();
		postFxA_fbo->create(size.x, size.y, 1, GL_RGB, GL_HALF_FLOAT);
	}

	if (!postFxB_fbo || (postFxB_fbo->width != size.x || postFxB_fbo->height != size.y))
	{
		postFxB_fbo = new GFX::FBO();
		postFxB_fbo->create(size.x, size.y, 1, GL_RGB, GL_HALF_FLOAT);
	}

	if (pipeline_mode == ePipelineMode::FORWARD) {
		final_fbo->bind();
			renderSceneForward(scene, camera);
		final_fbo->unbind();
		renderPostFx(final_fbo->color_textures[0], final_fbo->depth_texture);
	}
	else if (pipeline_mode == ePipelineMode::DEFERRED) {
		renderSceneDeferred(scene, camera);
		renderPostFx(final_fbo->color_textures[0], gbuffers->depth_texture);
	}

	opaqueRenderables.clear();
	alphaRenderables.clear();
}

void Renderer::renderSceneForward(SCN::Scene* scene, Camera* camera)
{
	camera->enable();

	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::checkGLErrors();

	//render skybox
	if(skybox_cubemap)
		renderSkybox(skybox_cubemap);

	//////////////
	// Sort by distance_to_camera from far to near
	std::sort(alphaRenderables.begin(), alphaRenderables.end(), [](Renderable& a, Renderable& b) {return (a.distance_to_camera > b.distance_to_camera);});

	// Render opaque objects first
	for (Renderable& re : opaqueRenderables) {
		//if bounding box inside camera
		if (camera->testBoxInFrustum(re.bounding.center, re.bounding.halfsize))
			if (skip_lights) {
				renderMeshWithMaterial(re.model, re.mesh, re.material);
			}
			else {
				renderMeshWithMaterialLights(re.model, re.mesh, re.material);
			}
	}

	// Then render alpha objects
	for (Renderable& re : alphaRenderables) {
		//if bounding box inside camera
		if (camera->testBoxInFrustum(re.bounding.center, re.bounding.halfsize))
			if (skip_lights) {
				renderMeshWithMaterial(re.model, re.mesh, re.material);
			}
			else {
				renderMeshWithMaterialLights(re.model, re.mesh, re.material);
			}
	}
	////////////////
}

void Renderer::renderSceneDeferred(SCN::Scene* scene, Camera* camera) {

	vec2 size = CORE::getWindowSize();
	GFX::Mesh* quad = GFX::Mesh::getQuad();
	//generear los GBuffers
	if (!gbuffers || (gbuffers->width != size.x || gbuffers->height != size.y))
	{
		gbuffers = new GFX::FBO();
		gbuffers->create(size.x, size.y, 4, GL_RGBA, GL_UNSIGNED_BYTE, true);

		//For the decals
		cloned_depth_texture = new GFX::Texture(size.x, size.y, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
	}

	//illumination
	if (!illumination || (illumination->width != size.x || illumination->height != size.y)) {
		illumination = new GFX::FBO();
		illumination->create(size.x, size.y, 1, GL_RGBA, GL_FLOAT, true);
	}

	//ssao
	if (!ssao_fbo || (ssao_fbo->width != size.x || ssao_fbo->height != size.y))
	{
		ssao_fbo = new GFX::FBO();
		//ssao_fbo->create(size.x / 2.0, size.y / 2.0, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
		ssao_fbo->create(size.x, size.y, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
		ssao_fbo->color_textures[0]->setName("SSAO");
	}

	if (!ssao_blurr || (ssao_blurr->width != size.x || ssao_blurr->height != size.y))
	{
		ssao_blurr = new GFX::FBO();
		ssao_blurr->create(size.x, size.y, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
		ssao_blurr->color_textures[0]->setName("SSAO_BLURR");
		weights = calculate_weights(kernel_size, sigma);
	}

	//Volumetric
	if (!volumetric_fbo || (volumetric_fbo->width != size.x / 2 || volumetric_fbo->height != size.y / 2))
	{
		volumetric_fbo = new GFX::FBO();
		volumetric_fbo->create(size.x / 2, size.y / 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, false);
	}

	gbuffers->bind();
	camera->enable();

	glEnable(GL_DEPTH_TEST);

	glClearColor(0, 0, 0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Sort by distance_to_camera from near to far to avoid overdraw
	std::sort(opaqueRenderables.begin(), opaqueRenderables.end(), [](Renderable& a, Renderable& b) {return (a.distance_to_camera < b.distance_to_camera); });

	// Render opaque objects first
	for (Renderable& re : opaqueRenderables) {
		//if bounding box inside camera
		if (camera->testBoxInFrustum(re.bounding.center, re.bounding.halfsize))
			renderMeshWithMaterialGBuffers(re.model, re.mesh, re.material);
	}

	gbuffers->unbind();


	if (decal && decals.size()) {

		//copy gbuffers depth to other texture
		gbuffers->depth_texture->copyTo(cloned_depth_texture);

		//draw again inside the gbuffers
		gbuffers->bind();

		GFX::Shader* decal_shader = GFX::Shader::Get("decals");
		assert(decal_shader);
		decal_shader->enable();
		decal_shader->setUniform("u_depth_texture", cloned_depth_texture, 0);
		decal_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		decal_shader->setUniform("u_iRes", vec2(1.0 / (float)size.x, 1.0 / (float)size.y));
		cameraToShader(camera, decal_shader);

		glEnable(GL_DEPTH_TEST);
		glDepthMask(false);
		glDepthFunc(GL_GEQUAL);
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CW);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glColorMask(true, true, true, false);
		gbuffers->enableBuffers(true, false, false, false);

		for (DecalEntity* decal : decals) {
			//draw cube per decal
			decal_shader->setUniform("u_model", decal->root.getGlobalMatrix());
			Matrix44 i_model = decal->root.getGlobalMatrix();
			i_model.inverse();
			decal_shader->setUniform("u_imodel", i_model);
			GFX::Texture* decal_texture = GFX::Texture::Get(decal->texture.c_str());
			if (!decal_texture)
				continue;
			decal_shader->setUniform("u_decal_texture", decal_texture, 5);

			cube.render(GL_TRIANGLES);
		}
		//back to normal
		glDepthMask(true);
		glDepthFunc(GL_LEQUAL);
		glFrontFace(GL_CCW);
		glColorMask(true, true, true, true);

		decal_shader->disable();
		gbuffers->unbind();

	}

	// SSAO Pass
	ssao_fbo->bind();
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	GFX::Shader* ssao_shader = GFX::Shader::Get("ssao");
	assert(ssao_shader);
	ssao_shader->enable();
	cameraToShader(camera, ssao_shader);
	ssao_shader->setUniform("u_radius", ssao_radius);
	ssao_shader->setUniform("u_max_distance", ssao_max_distance);
	ssao_shader->setUniform("u_depth_texture", gbuffers->depth_texture, 0);
	ssao_shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
	ssao_shader->setUniform("u_iRes", vec2(1.0 / (float)ssao_fbo->color_textures[0]->width, 1.0 / (float)ssao_fbo->color_textures[0]->height));
	ssao_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	ssao_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
	ssao_shader->setUniform3Array("u_points", (float*)&random_points[0], random_points.size());
	ssao_shader->setUniform("u_linear_factor", ssao_linear);
	ssao_shader->setUniform("u_front", camera->front);
	ssao_shader->setUniform("u_far", camera->far_plane);
	ssao_shader->setUniform("u_near", camera->near_plane);
	if (ssao_mode == eSSAOMODE::SSAO_PLUS)
		ssao_shader->setUniform("u_ssao_plus", 1);
	else
		ssao_shader->setUniform("u_ssao_plus", 0);
	quad->render(GL_TRIANGLES);
	ssao_shader->disable();
	ssao_fbo->unbind();

	// SSAO Blur Pass
	ssao_blurr->bind();
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	GFX::Shader* ssao_blurr_shader = GFX::Shader::Get("blurr");
	assert(ssao_blurr_shader);
	ssao_blurr_shader->enable();
	ssao_blurr_shader->setUniform("u_texture", ssao_fbo->color_textures[0], 0);
	ssao_blurr_shader->setUniform("u_kernel_size", kernel_size);
	ssao_blurr_shader->setUniform1Array("u_weight", (float*)&weights[0], kernel_size);
	quad->render(GL_TRIANGLES);
	ssao_blurr_shader->disable();
	ssao_blurr->unbind();

	ssao_fbo->color_textures[0]->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	illumination->bind();
	camera->enable();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::checkGLErrors();


	if (skybox_cubemap)
		renderSkybox(skybox_cubemap);

	GFX::Texture* texture_ssao = NULL;

	if (!ssao_texture && !blurr)
		texture_ssao = ssao_fbo->color_textures[0];
	else if (!ssao_texture && blurr)
		texture_ssao = ssao_blurr->color_textures[0];

	if (texture_ssao == NULL)
		texture_ssao = GFX::Texture::getWhiteTexture(); //a 1x1 white texture
	
	if (illumination) {

		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		glEnable(GL_CULL_FACE);
		glDisable(GL_BLEND);

		GFX::Shader* shader = GFX::Shader::Get("deferred_global");

		assert(shader);
		shader->enable();
		GbuffersToShader(gbuffers, shader);

		shader->setUniform("u_ambient_light", deactivate_ambient_light ? vec3(0.0) : scene->ambient_light);
		shader->setUniform("u_ao_texture", texture_ssao, 4);
		shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
		shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		shader->setUniform("u_emissive_first", vec3(1.0));
		shader->setUniform("u_light_type", 0);
		shader->setUniform("u_linear_factor", ssao_linear);
		shader->setUniform("u_linear_space", Linear_space ? 1 : 0);

		if (directional_lights.size() && (!skip_lights)) {

			for (LightEntity* light : directional_lights) {
				lightToShader(light, shader);

				quad->render(GL_TRIANGLES);
				glEnable(GL_BLEND);
				glBlendFunc(GL_ONE, GL_ONE);
				shader->setUniform("u_ambient_light", vec3(0.0));
				shader->setUniform("u_emissive_first", vec3(0.0));
			}
		}
		else {
			quad->render(GL_TRIANGLES);
		}

		if (point_and_spot_lights.size() && (!skip_lights)) {

			shader = GFX::Shader::Get("deferred_ws");

			assert(shader);
			shader->enable();
			GbuffersToShader(gbuffers, shader);

			shader->setUniform("u_ambient_light", vec3(0.0));
			shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
			shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
			shader->setUniform("u_emissive_first", vec3(0.0));
			shader->setUniform("u_linear_space", Linear_space ? 1 : 0);

			cameraToShader(camera, shader);

			//only draw if the pixel is behind
			glDepthFunc(GL_GREATER);
			glEnable(GL_BLEND);
			glEnable(GL_CULL_FACE);
			glFrontFace(GL_CW);
			glBlendFunc(GL_ONE, GL_ONE);
			glDepthMask(GL_FALSE);

			for (LightEntity* light : point_and_spot_lights) {

				mat4 m;
				vec3 lightpos = light->root.model.getTranslation();
				m.setTranslation(lightpos.x, lightpos.y, lightpos.z);
				m.scale(light->max_distance, light->max_distance, light->max_distance);
				shader->setUniform("u_model", m);

				lightToShader(light, shader);

				sphere.render(GL_TRIANGLES);
			}
			glFrontFace(GL_CCW);
			glDisable(GL_CULL_FACE);
			glDepthFunc(GL_LESS);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);

			shader->disable();
		}

	}

	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);

	// Sort by distance_to_camera from far to near
	std::sort(alphaRenderables.begin(), alphaRenderables.end(), [](Renderable& a, Renderable& b) {return (a.distance_to_camera > b.distance_to_camera); });

	// Then render alpha objects
	for (Renderable& re : alphaRenderables) {
		//if bounding box inside camera
		if (camera->testBoxInFrustum(re.bounding.center, re.bounding.halfsize))
			if (skip_lights) {
				renderMeshWithMaterial(re.model, re.mesh, re.material);
			}
			else {
				renderMeshWithMaterialLights(re.model, re.mesh, re.material);
			}
	}


	if (probes_texture && apply_irradiance && interpolate)
	{
		GFX::Shader* irr_inter_shader = GFX::Shader::Get("irradiance_interpol");
		assert(irr_inter_shader);
		irr_inter_shader->enable();

		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND); //disabled just to see irradiance
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);

		probes_info.num_probes = probes.size();

		// we send every data necessary
		irr_inter_shader->setUniform("u_irr_start", probes_info.start);
		irr_inter_shader->setUniform("u_irr_end", probes_info.end);
		irr_inter_shader->setUniform("u_irr_dims", probes_info.dim);
		irr_inter_shader->setUniform("u_irr_delta", probes_info.delta);
		irr_inter_shader->setUniform("u_num_probes", (int)probes_info.num_probes);
		irr_inter_shader->setUniform("u_probes_texture", probes_texture, 4);

		// you need also pass the distance factor, for now leave it as 0.0
		irr_inter_shader->setUniform("u_irr_normal_distance", 0.0f);
		irr_inter_shader->setUniform("u_color_texture", gbuffers->color_textures[0], 0);
		irr_inter_shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
		irr_inter_shader->setUniform("u_extra_texture", gbuffers->color_textures[2], 2);
		irr_inter_shader->setUniform("u_depth_texture", gbuffers->depth_texture, 3);

		irr_inter_shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
		irr_inter_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		irr_inter_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		irr_inter_shader->setUniform("u_factor", irradiance_factor);

		quad->render(GL_TRIANGLES);

		irr_inter_shader->disable();
	}else if (probes_texture && apply_irradiance && !interpolate)
	{
		GFX::Shader* irr_shader = GFX::Shader::Get("irradiance");
		assert(irr_shader);
		irr_shader->enable();

		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND); //disabled just to see irradiance
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);

		probes_info.num_probes = probes.size();

		// we send every data necessary
		irr_shader->setUniform("u_irr_start", probes_info.start);
		irr_shader->setUniform("u_irr_end", probes_info.end);
		irr_shader->setUniform("u_irr_dims", probes_info.dim);
		irr_shader->setUniform("u_irr_delta", probes_info.delta);
		irr_shader->setUniform("u_num_probes", (int)probes_info.num_probes);
		irr_shader->setUniform("u_probes_texture", probes_texture, 4);

		// you need also pass the distance factor, for now leave it as 0.0
		irr_shader->setUniform("u_irr_normal_distance", 0.0f);
		irr_shader->setUniform("u_color_texture", gbuffers->color_textures[0], 0);
		irr_shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
		irr_shader->setUniform("u_extra_texture", gbuffers->color_textures[2], 2);
		irr_shader->setUniform("u_depth_texture", gbuffers->depth_texture, 3);

		irr_shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
		irr_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		irr_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		irr_shader->setUniform("u_factor", irradiance_factor);

		quad->render(GL_TRIANGLES);

		irr_shader->disable();
	}

	if (reflection_fbo != NULL && apply_reflection)
	{
		GFX::Shader* ref_shader = GFX::Shader::Get("reflection");
		assert(ref_shader);
		ref_shader->enable();

		glDisable(GL_DEPTH_TEST);
		glEnable(GL_BLEND); //disabled just to see reflection
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);

		reflection_probes_info.num_probes = reflection_probes.size();

		// we send every data necessary
		ref_shader->setUniform("u_refl_start", reflection_probes_info.start);
		ref_shader->setUniform("u_refl_end", reflection_probes_info.end);
		ref_shader->setUniform("u_refl_dims", reflection_probes_info.dim);
		ref_shader->setUniform("u_refl_delta", reflection_probes_info.delta);
		ref_shader->setUniform("u_num_refl_probes", (int)reflection_probes_info.num_probes);
		ref_shader->setUniform("u_probes_texture", reflection_fbo->color_textures[0], 4);

		ref_shader->setUniform("u_color_texture", gbuffers->color_textures[0], 0);
		ref_shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
		ref_shader->setUniform("u_extra_texture", gbuffers->color_textures[2], 2);
		ref_shader->setUniform("u_depth_texture", gbuffers->depth_texture, 3);

		ref_shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
		ref_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		ref_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		ref_shader->setUniform("u_refl_factor", reflection_factor);

		quad->render(GL_TRIANGLES);

		ref_shader->disable();
	}

	//

	glDepthFunc(GL_LESS);
	glEnable(GL_DEPTH_TEST);

	if (planar_reflection_fbo && show_gbuffer != eShowGBuffer::DEPTH && show_gbuffer != eShowGBuffer::GBUFFERS)
	{
		GFX::Shader* planar_shader = GFX::Shader::getDefaultShader("planar");
		planar_shader->enable();
		mat4 model;
		model.scale(1000, 1000, 1000);
		planar_shader->setUniform("u_model", model);
		planar_shader->setUniform("u_iRes", vec2(1.0 / size.x, 1.0 / size.y));
		cameraToShader(camera, planar_shader);
		planar_shader->setTexture("u_texture", planar_reflection_fbo->color_textures[0], 0);
		plane.render(GL_TRIANGLES);
		planar_shader->disable();
	}

	if (enableIrradianceCapture) {
		captureProbes();
		enableIrradianceCapture = false;
	}
	if (probes_grid) {
		renderProbes(probe_size);
	}
	else {
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
	}

	if (enableReflectionCapture) {
		captureReflectionProbes();
		enableReflectionCapture = false;
	}
	if (reflection_probes_grid) {
		renderReflectionProbes(probe_size_reflect);
	}
	else {
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glEnable(GL_CULL_FACE);
	}
	illumination->unbind();

	if (volumetric_fbo) {

		volumetric_fbo->bind();

		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);

		GFX::Shader* volumetric_shader = GFX::Shader::Get("volumetric_lights");
		assert(volumetric_shader);
		volumetric_shader->enable();
		cameraToShader(camera, volumetric_shader);
		volumetric_shader->setUniform("u_depth_texture", gbuffers->depth_texture, 0);
		volumetric_shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
		volumetric_shader->setUniform("u_iRes", vec2(1.0 / volumetric_fbo->width, 1.0 / volumetric_fbo->height));
		volumetric_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		volumetric_shader->setUniform("u_ambient_light", deactivate_ambient_light ? vec3(0.0) : scene->ambient_light);
		volumetric_shader->setUniform("u_air_density", air_density);
		volumetric_shader->setUniform("u_weight_ambient_light", weight_ambient_light);
		volumetric_shader->setUniform("u_time", (float)getTime() * 0.001f);

		if (lights.size() && (!skip_lights)) {
			for (LightEntity* light : lights) {

				lightToShader(light, volumetric_shader);
				quad->render(GL_TRIANGLES);
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
		}
		else {
			volumetric_shader->setUniform("u_light_type", 0);
			quad->render(GL_TRIANGLES);
		}

		volumetric_shader->disable();
		volumetric_fbo->unbind();

		glDisable(GL_BLEND);

	}


	final_fbo->bind();
	if (show_gbuffer == eShowGBuffer::NONE)
		//and render the texture into the screen
		illumination->color_textures[0]->toViewport();
	if (show_gbuffer == eShowGBuffer::COLOR)
		gbuffers->color_textures[0]->toViewport();
	if (show_gbuffer == eShowGBuffer::NORMAL)
		gbuffers->color_textures[1]->toViewport();
	if (show_gbuffer == eShowGBuffer::EMISSIVE)
		gbuffers->color_textures[2]->toViewport();
	if (show_gbuffer == eShowGBuffer::EXTRA)
		gbuffers->color_textures[3]->toViewport();
	if (show_gbuffer == eShowGBuffer::DEPTH)
	{
		GFX::Shader* depth_shader = GFX::Shader::Get("depth");
		depth_shader->enable();
		depth_shader->setUniform("u_camera_nearfar", vec2(camera->near_plane, camera->far_plane));
		gbuffers->depth_texture->toViewport(depth_shader);
	}
	if (show_gbuffer == eShowGBuffer::GBUFFERS) {
		//set an area of the screen and render fullscreen quad
		glViewport(0, size.y * 0.5, size.x * 0.5, size.y * 0.5);
		gbuffers->color_textures[0]->toViewport(); //colorbuffer
		if (volumetric_light) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			volumetric_fbo->color_textures[0]->toViewport();
		}

		glViewport(size.x * 0.5, size.y * 0.5, size.x * 0.5, size.y * 0.5);
		gbuffers->color_textures[1]->toViewport(); //normalbuffer
		if (volumetric_light) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			volumetric_fbo->color_textures[0]->toViewport();
		}

		glViewport(size.x * 0.5, 0.0, size.x * 0.5, size.y * 0.5);
		gbuffers->color_textures[2]->toViewport(); //emissivelbuffer
		if (volumetric_light) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			volumetric_fbo->color_textures[0]->toViewport();
		}

		//for the depth remember to linearize when displaying it
		glViewport(0, 0, size.x * 0.5, size.y * 0.5);
		GFX::Shader* depth_shader = GFX::Shader::Get("depth");
		depth_shader->enable();
		vec2 near_far = vec2(camera->near_plane, camera->far_plane);
		depth_shader->setUniform("u_camera_nearfar", near_far);
		gbuffers->depth_texture->toViewport(depth_shader);
		if (volumetric_light) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			volumetric_fbo->color_textures[0]->toViewport();
		}

		//set the viewport back to full screen
		glViewport(0, 0, size.x, size.y);

	}
	else {
		if (volumetric_light) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			volumetric_fbo->color_textures[0]->toViewport();
		}
	}

	if (show_ssao)
		if (!blurr)
			ssao_fbo->color_textures[0]->toViewport();
		else
			ssao_blurr->color_textures[0]->toViewport();

	final_fbo->unbind();
}


//Create a loop to apply as many postProcessing as you need
void Renderer::renderPostFx(GFX::Texture* final_frame, GFX::Texture* depth_buffer)
{

	vec2 size = CORE::getWindowSize();
	Camera* camera = Camera::current;
	vec2 iRes = vec2(1.0 / (float)size.x, 1.0 / (float)size.y);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);

	//Define global at the end
	std::vector<std::string> PostProcessing_list = { "motion_blurr", "tone_mapper" };
	int number_of_postProcessing = PostProcessing_list.size();

	int use_fbo = -1;
	int not_apply_count = 0;
	GFX::Texture* frame = final_frame;

	for (int i = 0; i < number_of_postProcessing; i++) {
		use_fbo = (use_fbo + 1) % 2;
		if (use_fbo == 0) {
			postFxA_fbo->bind();
			int result = applyPostProcessing(PostProcessing_list[i], frame, depth_buffer, iRes, camera);
			postFxA_fbo->unbind();
			if (result != 1) {
				frame = postFxA_fbo->color_textures[0];
			}
		}
		else if (use_fbo == 1) {
			postFxB_fbo->bind();
			int result = applyPostProcessing(PostProcessing_list[i], frame, depth_buffer, iRes, camera);
			postFxB_fbo->unbind();
			if (result != 1) {
				frame = postFxB_fbo->color_textures[0];
			}
		}
	}

	frame->toViewport();
}


//Post Processing shaders
int Renderer::applyPostProcessing(std::string shader_name, GFX::Texture* frame, GFX::Texture* depth_buffer, vec2 iRes, Camera* camera) {
	if (shader_name == "motion_blurr") {
		return applyMotionBlurr(frame, depth_buffer, iRes, camera);
	}
	else if (shader_name == "tone_mapper") 
	{
		return applyToneMapper(frame);
	} //... Put all other postprocessing
	else {
		return 0;
	}
}


int Renderer::applyMotionBlurr(GFX::Texture* frame, GFX::Texture* depth_buffer, vec2 iRes, Camera* camera) {
	if (!deactivate_motion_blur) {
		GFX::Shader* shader = GFX::Shader::Get("motion_blurr");
		shader->enable();
		shader->setUniform("u_iRes", iRes);
		shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
		shader->setUniform("u_viewprojection_prev", viewprojection_previous_frame);
		shader->setUniform("u_depth_texture", depth_buffer, 1);
		viewprojection_previous_frame = camera->viewprojection_matrix;
		frame->toViewport(shader);
		return 0;
	}
	else {
		return 1;
	}
}

int Renderer::applyToneMapper(GFX::Texture* frame) {
	if (!deactivate_tonemapper) {
		GFX::Shader* shader = GFX::Shader::Get("tone_mapper");
		shader->enable();
		shader->setUniform("u_scale", scale);
		shader->setUniform("u_average_lum", average_lum);
		shader->setUniform("u_lumwhite2", lumwhite2);
		shader->setUniform("u_igamma", float(1.0 / igamma));
		frame->toViewport(shader);
		return 0;
	}
	else {
		return 1;
	}
}


void Renderer::renderSkybox(GFX::Texture* cubemap)
{
	Camera* camera = Camera::current;

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	if (render_wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	GFX::Shader* shader = GFX::Shader::Get("skybox");
	if (!shader)
		return;
	shader->enable();

	Matrix44 m;
	m.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	m.scale(10, 10, 10);
	shader->setUniform("u_model", m);
	cameraToShader(camera, shader);
	shader->setUniform("u_texture", cubemap, 0);
	sphere.render(GL_TRIANGLES);
	shader->disable();
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glEnable(GL_DEPTH_TEST);
}

//renders a node of the prefab and its children
void Renderer::renderNode(SCN::Node* node, Camera* camera)
{
	if (!node->visible)
		return;

	//compute global matrix
	Matrix44 node_model = node->getGlobalMatrix(true);

	//does this node have a mesh? then we must render it
	if (node->mesh && node->material)
	{
		//compute the bounding box of the object in world space (by using the mesh bounding box transformed to world space)
		BoundingBox world_bounding = transformBoundingBox(node_model,node->mesh->box);
		
		//if bounding box is inside the camera frustum then the object is probably visible
		if (camera->testBoxInFrustum(world_bounding.center, world_bounding.halfsize) )
		{
			if(render_boundaries)
				node->mesh->renderBounding(node_model, true);
			renderMeshWithMaterial(node_model, node->mesh, node->material);
		}
	}

	//iterate recursively with children
	for (int i = 0; i < node->children.size(); ++i)
		renderNode( node->children[i], camera);
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterialFlat(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	GFX::Texture* texture = NULL;
	Camera* camera = Camera::current;

	texture = material->textures[SCN::eTextureChannel::ALBEDO].texture;

	if (texture == NULL)
		texture = GFX::Texture::getWhiteTexture(); //a 1x1 white texture

	glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	shader = GFX::Shader::Get("texture");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_model", model);
	cameraToShader(camera, shader);

	shader->setUniform("u_color", material->color);
	if (texture)
		shader->setUniform("u_texture", texture, 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == SCN::eAlphaMode::MASK ? material->alpha_cutoff : 0.001f);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterialGBuffers(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	GFX::Texture* texture = NULL;
	GFX::Texture* textureEmissive = NULL;
	GFX::Texture* textureMetallicRoughness = NULL;
	GFX::Texture* textureNormalMap = NULL;
	GFX::Texture* textureOcclusion = NULL;
	Camera* camera = Camera::current;

	if (!white_textures) {
		if (!albedo_texture)
			texture = material->textures[SCN::eTextureChannel::ALBEDO].texture;
		if (!emissive_texture)
			textureEmissive = material->textures[SCN::eTextureChannel::EMISSIVE].texture;
		if (!metallicRoughness_texture)
			textureMetallicRoughness = material->textures[SCN::eTextureChannel::METALLIC_ROUGHNESS].texture;
		if (!normalMap_texture)
			textureNormalMap = material->textures[SCN::eTextureChannel::NORMALMAP].texture;
		if (!occlusion_texture)
			textureOcclusion = material->textures[SCN::eTextureChannel::OCCLUSION].texture;
	}

	if (texture == NULL)
		texture = GFX::Texture::getWhiteTexture(); //a 1x1 white texture
	if (textureEmissive == NULL)
		textureEmissive = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureMetallicRoughness == NULL)
		textureMetallicRoughness = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureNormalMap == NULL)
		textureNormalMap = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureOcclusion == NULL)
		textureOcclusion = GFX::Texture::getWhiteTexture(); //a 1x1 white texture

	glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	shader = GFX::Shader::Get("gbuffers");
	assert(shader);
	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_model", model);
	cameraToShader(camera, shader);

	shader->setUniform("u_color", material->color);
	shader->setUniform("u_texture", texture, 0);
	shader->setUniform("u_texture_emissive", textureEmissive, 1);
	if (!white_textures && !emissive_texture) {
		shader->setUniform("u_emissive_factor", material->emissive_factor);
	}
	else {
		shader->setUniform("u_emissive_factor", vec3(0.0));
	}
	shader->setUniform("u_texture_metallic_roughness", textureMetallicRoughness, 2);
	shader->setUniform("u_texture_normalmap", textureNormalMap, 3);
	shader->setUniform("u_texture_occlusion", textureOcclusion, 4);
	shader->setUniform("u_norm_contr", normalMap_texture);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == SCN::eAlphaMode::MASK ? material->alpha_cutoff : 0.001f);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();
}

//renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	GFX::Texture* texture = NULL;
	Camera* camera = Camera::current;
	
	if (!white_textures) {
		texture = material->textures[SCN::eTextureChannel::ALBEDO].texture;
	}
	//texture = material->emissive_texture;
	//texture = material->metallic_roughness_texture;
	//texture = material->normal_texture;
	//texture = material->occlusion_texture;
	if (texture == NULL)
		texture = GFX::Texture::getWhiteTexture(); //a 1x1 white texture

	//select the blending
	if (material->alpha_mode == SCN::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if(material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
    assert(glGetError() == GL_NO_ERROR);

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	shader = GFX::Shader::Get("texture");

    assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_model", model);
	cameraToShader(camera, shader);
	float t = getTime();
	shader->setUniform("u_time", t );

	shader->setUniform("u_color", material->color);
	if(texture)
		shader->setUniform("u_texture", texture, 0);

	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == SCN::eAlphaMode::MASK ? material->alpha_cutoff : 0.001f);

	if (render_wireframe)
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
}

//renders a mesh given its transform and material, with lights /////////////
void Renderer::renderMeshWithMaterialLights(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	GFX::Texture* textureAlbedo = NULL;
	GFX::Texture* textureEmissive = NULL;
	GFX::Texture* textureMetallicRoughness = NULL;
	GFX::Texture* textureNormalMap = NULL;
	GFX::Texture* textureOcclusion = NULL;
	Camera* camera = Camera::current;
	if (!white_textures) {
		if(!albedo_texture)
			textureAlbedo = material->textures[SCN::eTextureChannel::ALBEDO].texture;
		if (!emissive_texture)
			textureEmissive = material->textures[SCN::eTextureChannel::EMISSIVE].texture;
		if (!metallicRoughness_texture)
			textureMetallicRoughness = material->textures[SCN::eTextureChannel::METALLIC_ROUGHNESS].texture;
		if (!normalMap_texture)
			textureNormalMap = material->textures[SCN::eTextureChannel::NORMALMAP].texture;
		if (!occlusion_texture)
			textureOcclusion = material->textures[SCN::eTextureChannel::OCCLUSION].texture;
	}

	if (textureAlbedo == NULL)
		textureAlbedo = GFX::Texture::getWhiteTexture(); //a 1x1 white texture
	if (textureEmissive == NULL)
		textureEmissive = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureMetallicRoughness == NULL)
		textureMetallicRoughness = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureNormalMap == NULL)
		textureNormalMap = GFX::Texture::getWhiteTexture(); //a 1x1 white 
	if (textureOcclusion == NULL)
		textureOcclusion = GFX::Texture::getWhiteTexture(); //a 1x1 white texture

	//select the blending
	if (material->alpha_mode == SCN::eAlphaMode::BLEND)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);

	//select if render both sides of the triangles
	if (material->two_sided)
		glDisable(GL_CULL_FACE);
	else
		glEnable(GL_CULL_FACE);
	assert(glGetError() == GL_NO_ERROR);

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	shader = GFX::Shader::Get("light");

	assert(glGetError() == GL_NO_ERROR);

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	//upload uniforms
	shader->setUniform("u_model", model);
	cameraToShader(camera, shader);
	float t = getTime();
	shader->setUniform("u_time", t);
	////////////
	if (!deactivate_ambient_light) {
		shader->setUniform("u_ambient_light", scene->ambient_light);
	}
	else {
		shader->setUniform("u_ambient_light", vec3(0.0));
	}
	if (!white_textures && !emissive_texture) {
		shader->setUniform("u_emissive_factor", material->emissive_factor);
	}
	else {
		shader->setUniform("u_emissive_factor", vec3(0.0));
	}
	shader->setUniform("u_norm_contr", normalMap_texture);
	shader->setUniform("u_linear_space", Linear_space ? 1 : 0);

	shader->setUniform("u_color", material->color);
	shader->setUniform("u_texture_albedo", textureAlbedo, 0);
	shader->setUniform("u_texture_emissive", textureEmissive, 1);
	shader->setUniform("u_texture_metallic_roughness", textureMetallicRoughness, 2);
	shader->setUniform("u_texture_normalmap", textureNormalMap, 3);
	shader->setUniform("u_texture_occlusion", textureOcclusion, 4);
	///////////////
	//this is used to say which is the alpha threshold to what we should not paint a pixel on the screen (to cut polygons according to texture alpha)
	shader->setUniform("u_alpha_cutoff", material->alpha_mode == SCN::eAlphaMode::MASK ? material->alpha_cutoff : 0.001f);

	if (render_wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	////////////
	if (material->alpha_mode != SCN::eAlphaMode::BLEND){
		glDisable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	}
	glDepthFunc(GL_LEQUAL);

	//multi_pass
	if (lights.size()) {
		for (LightEntity* light : lights) {
			lightToShader(light, shader);

			mesh->render(GL_TRIANGLES);
			glEnable(GL_BLEND);
			shader->setUniform("u_ambient_light", vec3(0.0));
			shader->setUniform("u_emissive_factor", vec3(0.0));
		}
	}
	else {
		shader->setUniform("u_light_type", 0);
		mesh->render(GL_TRIANGLES);
	}

	glDepthFunc(GL_LESS);
	//////////////
	
	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}
/////////////


void SCN::Renderer::initProbes(vec3 start, vec3 end, vec3 dim)
{
	//Probes
	//define bounding of the grid and num probes
	probes_info.start.set(start.x, start.y, start.z);
	probes_info.end.set(end.x, end.y, end.z);
	probes_info.dim.set(dim.x, dim.y, dim.z);

	//compute the vector from one corner to the other
	vec3 delta = (probes_info.end - probes_info.start);
	//compute delta from one probe to the next one
	delta.x /= (probes_info.dim.x - 1);
	delta.y /= (probes_info.dim.y - 1);
	delta.z /= (probes_info.dim.z - 1);
	probes_info.delta = delta; //store

	probes.clear();

	//lets compute the centers
	//pay attention at the order at which we add them
	for (int z = 0; z < probes_info.dim.z; ++z)
		for (int y = 0; y < probes_info.dim.y; ++y)
			for (int x = 0; x < probes_info.dim.x; ++x)
			{
				sProbe p;
				p.local.set(x, y, z);

				//index in the linear array
				p.index = x + y * probes_info.dim.x + z *
					probes_info.dim.x * probes_info.dim.y;

				//and its position
				p.pos = probes_info.start +
					probes_info.delta * Vector3f(x, y, z);
				probes.push_back(p);
			}
}

void SCN::Renderer::initProbesReflection(vec3 start, vec3 end, vec3 dim)
{
	//Probes
	//define bounding of the grid and num probes
	reflection_probes_info.start.set(start.x, start.y, start.z);
	reflection_probes_info.end.set(end.x, end.y, end.z);
	reflection_probes_info.dim.set(dim.x, dim.y, dim.z);

	//compute the vector from one corner to the other
	vec3 delta = (reflection_probes_info.end - reflection_probes_info.start);
	//compute delta from one probe to the next one
	delta.x /= (reflection_probes_info.dim.x - 1);
	delta.y /= (reflection_probes_info.dim.y - 1);
	delta.z /= (reflection_probes_info.dim.z - 1);
	reflection_probes_info.delta = delta; //store

	reflection_probes.clear();

	//lets compute the centers
	//pay attention at the order at which we add them
	for (int z = 0; z < reflection_probes_info.dim.z; ++z)
		for (int y = 0; y < reflection_probes_info.dim.y; ++y)
			for (int x = 0; x < reflection_probes_info.dim.x; ++x)
			{
				sReflectionProbe p;
				p.local.set(x, y, z);

				//index in the linear array
				p.index = x + y * reflection_probes_info.dim.x + z *
					reflection_probes_info.dim.x * reflection_probes_info.dim.y;

				//and its position
				p.pos = reflection_probes_info.start +
					reflection_probes_info.delta * Vector3f(x, y, z);
				reflection_probes.push_back(p);
			}
}

void SCN::Renderer::renderProbe(vec3 pos, float scale, SphericalHarmonics& shs)
{
	Camera* camera = Camera::current;

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glEnable(GL_CULL_FACE);

	GFX::Shader* shader = GFX::Shader::Get("probe");
	if (!shader)
		return;
	shader->enable();

	Matrix44 m;
	m.setTranslation(pos.x, pos.y, pos.z);
	m.scale(scale, scale, scale);
	shader->setUniform("u_model", m);
	cameraToShader(camera, shader);
	shader->setUniform3Array("u_coeffs", shs.coeffs[0].v, 9);
	sphere.render(GL_TRIANGLES);
	shader->disable();

	glDepthFunc(GL_LESS);
	glEnable(GL_DEPTH_TEST);
}

void SCN::Renderer::renderProbes(float scale)
{
	for (auto& probe : probes)
	{
		renderProbe(probe.pos, scale, probe.sh);
	}

}

void SCN::Renderer::captureProbe(sProbe& p)
{
	static FloatImage images[6]; //here we will store the six views

	if (!irr_fbo || irr_fbo->height != irradiance_capture_size)
	{
		irr_fbo = new GFX::FBO();
		irr_fbo->create(irradiance_capture_size, irradiance_capture_size, 1, GL_RGB, GL_FLOAT, false);
	}

	Camera cam;
	//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	for (int i = 0; i < 6; ++i) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		vec3 eye = p.pos;
		vec3 front = cubemapFaceNormals[i][2];
		vec3 center = p.pos + front;
		vec3 up = cubemapFaceNormals[i][1];
		cam.lookAt(eye, center, up);
		cam.enable();

		//render the scene from this point of view
		irr_fbo->bind();
		vec3 color = scene->background_color;
		skybox_cubemap = nullptr;
		scene->background_color = vec3(0.0);
		renderSceneForward(scene, &cam);
		skybox_cubemap = GFX::Texture::Get(std::string(scene->base_folder + "/" + scene->skybox_filename).c_str());
		scene->background_color = color;
		irr_fbo->unbind();
		
		//read the pixels back and store in a FloatImage
		images[i].fromTexture(irr_fbo->color_textures[0]);
	}

	//compute the coefficients given the six images
	p.sh = computeSH(images); //You can decide whether using degamma or not

}

void SCN::Renderer::captureProbes()
{
	for (auto& probe : probes)
	{
		captureProbe(probe);
	}

	//create the texture to store the probes (do this ONCE!!!)
	if (probes_texture)
		delete probes_texture;

	probes_texture = new GFX::Texture(
		9, //9 coefficients per probe
		probes.size(), //as many rows as probes
		GL_RGB, //3 channels per coefficient
		GL_FLOAT); //they require a high range

	//we must create the color information for the texture. because every SH are 27 floats in the RGB,RGB,... order, we can create an array of SphericalHarmonics and use it as pixels of the texture
	SphericalHarmonics* sh_data = NULL;
	sh_data = new SphericalHarmonics[probes_info.dim.x * probes_info.dim.y * probes_info.dim.z];

	//here we fill the data of the array with our probes in x,y,z order
	for (int i = 0; i < probes.size(); ++i) {
		sh_data[i] = probes[i].sh;
	}

	//now upload the data to the GPU as a texture
	probes_texture->upload(GL_RGB, GL_FLOAT, false, (uint8*)sh_data);

	//disable any texture filtering when reading
	probes_texture->bind();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

	//always free memory after allocating it!!!
	delete[] sh_data;
}


void SCN::Renderer::renderReflectionProbe(sReflectionProbe& p, float scale)
{
	Camera* camera = Camera::current;

	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glCullFace(GL_BACK); 
	glEnable(GL_CULL_FACE);

	GFX::Shader* shader = GFX::Shader::Get("reflectionProbe");
	if (!shader)
		return;
	shader->enable();

	GFX::Texture* texture = p.texture ? p.texture : skybox_cubemap;
	if(!texture)
		return;

	Matrix44 m;
	m.setTranslation(p.pos.x, p.pos.y, p.pos.z);
	m.scale(scale, scale, scale);
	shader->setUniform("u_model", m);
	cameraToShader(camera, shader);
	shader->setTexture("u_environment_texture", texture, 0);
	shader->setUniform("u_camera_position", camera->eye);
	sphere.render(GL_TRIANGLES);
	shader->disable();

	glDepthFunc(GL_LESS);
	glEnable(GL_DEPTH_TEST);
}

void SCN::Renderer::renderReflectionProbes(float scale)
{
	for (auto& ref : reflection_probes)
		renderReflectionProbe(ref, scale);
}

void SCN::Renderer::captureReflectionProbe(sReflectionProbe& p)
{
	if (!p.texture)
	{
		p.texture = new GFX::Texture();
		p.texture->createCubemap(128, 128, nullptr, GL_RGB, GL_HALF_FLOAT, true);
	}
	if (!reflection_fbo)
		reflection_fbo = new GFX::FBO();

	Camera cam;
	//set the fov to 90 and the aspect to 1
	cam.setPerspective(90, 1, 0.1, 1000);

	for (int i = 0; i < 6; ++i) //for every cubemap face
	{
		//compute camera orientation using defined vectors
		vec3 eye = p.pos;
		vec3 front = cubemapFaceNormals[i][2];
		vec3 center = p.pos + front;
		vec3 up = cubemapFaceNormals[i][1];
		cam.lookAt(eye, center, up);
		cam.enable();

		reflection_fbo->setTexture(p.texture, i);

		reflection_fbo->bind();
		renderSceneForward(scene, &cam);
		reflection_fbo->unbind();
	}
	//generate the mipmaps
	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
	p.texture->generateMipmaps();
}

void SCN::Renderer::captureReflectionProbes()
{
	for (auto& p : reflection_probes)
		captureReflectionProbe(p);
}

void SCN::Renderer::capturePlanarReflection(Camera* camera)
{
	vec2 size = CORE::getWindowSize();
	if (!planar_reflection_fbo)
	{
		planar_reflection_fbo = new GFX::FBO();
		planar_reflection_fbo->create(size.x, size.y, 1, GL_RGB, GL_UNSIGNED_BYTE);
		planar_reflection_fbo->color_textures[0]->setName("reflection");
	}

	planar_reflection_fbo->bind();

	Camera cam;
	cam = *camera;

	cam.eye = camera->eye * vec3(1, -1, 1);
	cam.center = camera->center * vec3(1, -1, 1);
	cam.up = camera->up * vec3(1, -1, 1);

	renderSceneForward(scene, &cam);

	planar_reflection_fbo->unbind();
}


void SCN::Renderer::cameraToShader(Camera* camera, GFX::Shader* shader)
{
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix );
	shader->setUniform("u_camera_position", camera->eye);
}

void SCN::Renderer::lightToShader(LightEntity* light, GFX::Shader* shader) 
{
	shader->setUniform("u_light_type", (int)light->light_type);
	shader->setUniform("u_light_position", light->root.global_model.getTranslation());
	shader->setUniform("u_light_color", light->color * light->intensity);
	shader->setUniform("u_light_max_distance", light->max_distance);
	shader->setUniform("u_light_cone_info", vec2(cos(light->cone_info.x * DEG2RAD), cos(light->cone_info.y * DEG2RAD)));
	shader->setUniform("u_light_front", light->root.model.frontVector().normalize());
	shader->setUniform("u_PBR", !Remove_PBR ? 1 : 0);

	if (light->has_shadow_map) {
		shader->setUniform("u_light_cast_shadow", !skip_shadows ? 1 : 0);
		shader->setUniform("u_shadow_map", shadow_maps[light->shadow_map_index]->depth_texture, 8);
		shader->setUniform("u_shadow_map_view_projection", light->shadowmap_view_projection);
		shader->setUniform("u_shadow_bias", float(light->shadow_bias + 0.002));
	}
}

void SCN::Renderer::GbuffersToShader(GFX::FBO* gbuffers, GFX::Shader* shader) {
	shader->setUniform("u_color_texture", gbuffers->color_textures[0], 0);
	shader->setUniform("u_normal_texture", gbuffers->color_textures[1], 1);
	shader->setUniform("u_emissive_occlusion_texture", gbuffers->color_textures[2], 2);
	//shader->setUniform("u_normalmap_texture", gbuffers->color_textures[3], 3);
	shader->setUniform("u_depth_texture", gbuffers->depth_texture, 3);
}

#ifndef SKIP_IMGUI

void Renderer::showUI()
{
	
	ImGui::Combo("Pipeline", (int*)&pipeline_mode, "FLAT\0FORWARD\0DEFERRED\0", ePipelineMode::PIPELINE_COUNT);
	ImGui::Combo("GBuffers", (int*)&show_gbuffer, "NONE\0COLOR\0NORMAL\0EMISSIVE\0DEPTH\0EXTRA\0GBUFFERS", eShowGBuffer::GBUFFERS_COUNT);

	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Boundaries", &render_boundaries);

	ImGui::Checkbox("Work in Linear Space", &Linear_space);

	if (ImGui::TreeNode("Texture OPTIONS")) {
		ImGui::Checkbox("Deactivate Albedo Texture", &albedo_texture);
		ImGui::Checkbox("Deactivate Emissive Texture", &emissive_texture);
		ImGui::Checkbox("Deactivate MetallicRoughness Texture", &metallicRoughness_texture);
		ImGui::Checkbox("Deactivate NormalMap Texture", &normalMap_texture);
		ImGui::Checkbox("Deactivate Occlusion Texture", &occlusion_texture);
		ImGui::Checkbox("Remove Textures", &white_textures);
		ImGui::Checkbox("Remove Alpha Renderables", &skip_alpha_renderables);
		ImGui::Checkbox("Remove PBR", &Remove_PBR);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Light OPTIONS")) {
		ImGui::Checkbox("Deactivate Ambient Light", &deactivate_ambient_light);
		ImGui::Checkbox("Remove Lights", &skip_lights);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Shadow OPTIONS")) {
		ImGui::Checkbox("Remove Shadows", &skip_shadows);
		// Create a slider for the exponent
		if (ImGui::SliderInt("Shadowmap Size", &power_of_two_shadowmap, 7, 12)) {
			// Calculate the actual shadowmap size as a power of two
			shadow_map_size = (1 << power_of_two_shadowmap);
		}
		// Display the actual shadowmap size
		ImGui::Text("Actual Shadowmap Size: %d", shadow_map_size);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("SSAO OPTIONS (Only for DEFERRED)")) {
		ImGui::Combo("SSAO Mode", (int*)&ssao_mode, "SSAO\0SSAO+\0", eSSAOMODE::SSAO_COUNT);
		ImGui::Checkbox("Remove SSAO", &ssao_texture);
		ImGui::Checkbox("Show only SSAO", &show_ssao);
		ImGui::DragFloat("Radius", &ssao_radius, 0.01f, 0.0f);
		ImGui::DragFloat("Max Distance", &ssao_max_distance, 0.001f, 0.001f, 1.0f);

		ImGui::Checkbox("Blurr", &blurr);
		ImGui::DragInt("Kernel Size", &kernel_size, 1.0f, 1.0f, 5.0f);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Tonemapper OPTIONS (Only for DEFERRED)")) {
		ImGui::Checkbox("Deactivate Tonemapper", &deactivate_tonemapper);
		ImGui::DragFloat("Scale", &scale, 0.01f, 0.001f, 10.0f);
		ImGui::DragFloat("Average Lum", &average_lum, 0.001f, 0.001f, 10.0f);
		ImGui::DragFloat("Lum White2", &lumwhite2, 0.01f, 0.001f, 10.0f);
		ImGui::DragFloat("Igamma", &igamma, 0.001f, 0.001f, 10.0f);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Irradiance OPTIONS (Only for DEFERRED)")) {
		ImGui::Checkbox("Render Irradiance Probes", &probes_grid);
		// Flag to track changes
		bool valuesChanged = false;

		// Add drag fields for start, end, and dim vectors
		valuesChanged |= ImGui::DragFloat3("Start", &start[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");
		valuesChanged |= ImGui::DragFloat3("End", &end[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");
		valuesChanged |= ImGui::DragFloat3("Dim", &dim[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");

		// If any value changed, call initProbes
		if (valuesChanged) {
			initProbes(start, end, dim);
		}
		ImGui::DragFloat("Factor", &irradiance_factor, 0.01f, 0.001, 1.5, "%.3f");
		ImGui::SliderInt("Probe \nSize", &probe_size, 1, 7);

		if (ImGui::SliderInt("Irradiance \nCapture \nResolution", &power_of_two_irradiance, 2, 10)) {
			// Calculate the actual shadowmap size as a power of two
			irradiance_capture_size = (1 << power_of_two_irradiance);
		}
		// Display the actual shadowmap size
		ImGui::Text("Actual Irradiance Capture Resolution: %d", irradiance_capture_size);
		ImGui::Checkbox("Capture Irradiance", &enableIrradianceCapture);
		ImGui::Checkbox("Apply Irradiance", &apply_irradiance);
		ImGui::Checkbox("Interpolate", &interpolate);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Reflection OPTIONS")) {
		ImGui::Checkbox("Render Reflection Probes", &reflection_probes_grid);
		// Flag to track changes
		bool valuesChanged2 = false;

		// Add drag fields for start, end, and dim vectors
		valuesChanged2 |= ImGui::DragFloat3("Start", &start_reflect[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");
		valuesChanged2 |= ImGui::DragFloat3("End", &end_reflect[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");
		valuesChanged2 |= ImGui::DragFloat3("Dim", &dim_reflect[0], 0.1f, -FLT_MAX, FLT_MAX, "%.3f");

		// If any value changed, call initProbes
		if (valuesChanged2) {
			initProbesReflection(start_reflect, end_reflect, dim_reflect);
		}
		ImGui::DragFloat("Factor", &reflection_factor, 0.001f, 0.001, 2, "%.3f");
		ImGui::SliderInt("Probe \nSize", &probe_size_reflect, 1, 7);
		ImGui::Checkbox("Capture Reflections", &enableReflectionCapture);
		ImGui::Checkbox("Apply Reflection", &apply_reflection);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Volumetric Fog OPTIONS")) {
		ImGui::Checkbox("Add volumetric Fog", &volumetric_light);
		ImGui::DragFloat("Air density", &air_density, 0.00001f, 0.0f, 1.0f, "%.5f");
		ImGui::DragFloat("Ambient Light Weight", &weight_ambient_light, 0.1f, 0.5f, 10.0f);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Decals OPTIONS")) {
		ImGui::Checkbox("Decals", &decal);
		ImGui::TreePop();
	}
	if (ImGui::TreeNode("Postprocessing OPTIONS")) {
		ImGui::Checkbox("Deactivate Motion Blur", &deactivate_motion_blur);
	    ImGui::TreePop();
	}
}

#else
void Renderer::showUI() {}
#endif
