#include <SDL/SDL.h>
#include <math.h>
#include <vector>
#include "vector.h"
#include "util.h"
#include "sdl.h"
#include "color.h"
#include "camera.h"
#include "geometry.h"
#include "shading.h"
#include "environment.h"
#include "mesh.h"
#include "random_generator.h"
#include "scene.h"
#include "lights.h"
using std::vector;


Color vfb[VFB_MAX_SIZE][VFB_MAX_SIZE];

bool visibilityCheck(const Vector& start, const Vector& end);

Color raytrace(Ray ray)
{
	if (ray.depth > scene.settings.maxTraceDepth) return Color(0, 0, 0);
	Node* closestNode = NULL;
	double closestDist = INF;
	IntersectionInfo closestInfo;
	for (auto& node: scene.nodes) {
		IntersectionInfo info;
		if (!node->intersect(ray, info)) continue;

		if (info.distance < closestDist) {
			closestDist = info.distance;
			closestNode = node;
			closestInfo = info;
		}
	}
	// check if the closest intersection point is actually a light:
	bool hitLight = false;
	Color hitLightColor;
	for (auto& light: scene.lights) {
		if (light->intersect(ray, closestDist)) {
			hitLight = true;
			hitLightColor = light->getColor();
		}
	}
	if (hitLight) return hitLightColor;

	// check if we hit the sky:
	if (closestNode == NULL) {
		if (scene.environment) return scene.environment->getEnvironment(ray.dir);
		else return Color(0, 0, 0);
	} else {
		if ((ray.flags & RF_DEBUG) && ray.depth == 0)
			printf("Found intersection at %.2lf\n", closestInfo.distance);
		closestInfo.rayDir = ray.dir;
		if (closestNode->bump)
			closestNode->bump->modifyNormal(closestInfo);
		return closestNode->shader->shade(ray, closestInfo);
	}
}

Color explicitLightSample(const Ray& ray, const IntersectionInfo& info, const Color& pathMultiplier, Shader* shader, Random& rnd)
{
	// try to end a path by explicitly sampling a light. If there are no lights, we can't do that:
	if (scene.lights.empty()) return Color(0, 0, 0);

	// choose a random light:
	int lightIdx = rnd.randint(0, scene.lights.size() - 1);
	Light* chosenLight = scene.lights[lightIdx];

	// evaluate light's solid angle as viewed from the intersection point, x:
	Vector x = info.ip;
	double solidAngle = chosenLight->solidAngle(x);

	// is light is too small or invisible?
	if (solidAngle == 0) return Color(0, 0, 0);

	// choose a random point on the light:
	int samplesInLight = chosenLight->getNumSamples();
	int randSample = rnd.randint(0, samplesInLight - 1);

	Vector pointOnLight;
	Color unused;
	chosenLight->getNthSample(randSample, x, pointOnLight, unused);

	// camera -> ... path ... -> x -> lightPos
	//                       are x and lightPos visible?
	if (!visibilityCheck(x + info.normal * 1e-6, pointOnLight))
		return Color(0, 0, 0);

	// get the emitted light energy (color * power):
	Color L = chosenLight->getColor();


	// evaluate BRDF. It might be zero (e.g., pure reflection), so bail out early if that's the case
	Vector w_out = pointOnLight - x;
	w_out.normalize();
	Color brdfAtPoint = shader->eval(info, ray.dir, w_out);
	if (brdfAtPoint.intensity() == 0) return Color(0, 0, 0);

	// probability to hit this light's projection on the hemisphere
	// (conditional probability, since we're specifically aiming for this light):
	float probHitLightArea = 1.0f / solidAngle;

	// probability to pick this light out of all N lights:
	float probPickThisLight = 1.0f / scene.lights.size();

	// combined probability of this generated w_out ray:
	float chooseLightProb = probHitLightArea * probPickThisLight;

	/* Light flux (Li) */ /* BRDFs@path*/  /*last BRDF*/ /*MC probability*/
	return     L       *   pathMultiplier * brdfAtPoint / chooseLightProb;
}

Color pathtrace(Ray ray, const Color& pathMultiplier, Random& rnd)
{
	if (ray.depth > scene.settings.maxTraceDepth) return Color(0, 0, 0);
	if (pathMultiplier.intensity() < 0.001f) return Color(0, 0, 0);
	Node* closestNode = NULL;
	double closestDist = INF;
	IntersectionInfo closestInfo;
	for (auto& node: scene.nodes) {
		IntersectionInfo info;
		if (!node->intersect(ray, info)) continue;

		if (info.distance < closestDist) {
			closestDist = info.distance;
			closestNode = node;
			closestInfo = info;
		}
	}
	// check if the closest intersection point is actually a light:
	bool hitLight = false;
	Color hitLightColor;
	for (auto& light: scene.lights) {
		if (light->intersect(ray, closestDist)) {
			hitLight = true;
			hitLightColor = light->getColor();
		}
	}
	if (hitLight) {
		if (!(ray.flags & RF_DIFFUSE)) {
			// forbid light contributions after a diffuse reflection
			return hitLightColor * pathMultiplier;
		} else
			return Color(0, 0, 0);
	}

	// check if we hit the sky:
	if (closestNode == NULL) {
		if (scene.environment)
			return scene.environment->getEnvironment(ray.dir) * pathMultiplier;
		else return Color(0, 0, 0);
	}

	closestInfo.rayDir = ray.dir;
	if (closestNode->bump)
		closestNode->bump->modifyNormal(closestInfo);

	// ("sampling the light"):
	// try to end the current path with explicit sampling of some light
	Color contribLight = explicitLightSample(ray, closestInfo, pathMultiplier,
											closestNode->shader, rnd);
	// ("sampling the BRDF"):
	// also try to extend the current path randomly:
	Ray w_out = ray;
	w_out.depth++;
	Color brdf;
	float pdf;
	closestNode->shader->spawnRay(closestInfo, ray.dir, w_out, brdf, pdf);

	if (pdf == -1) return Color(1, 0, 0); // BRDF not implemented
	if (pdf == 0) return Color(0, 0, 0);  // BRDF is zero


	Color contribGI = pathtrace(w_out, pathMultiplier * brdf / pdf, rnd);
	return contribLight + contribGI;
}

bool visibilityCheck(const Vector& start, const Vector& end)
{
	Ray ray;
	ray.start = start;
	ray.dir = end - start;
	ray.dir.normalize();

	double targetDist = (end - start).length();

	for (auto& node: scene.nodes) {
		IntersectionInfo info;
		if (!node->intersect(ray, info)) continue;

		if (info.distance < targetDist) {
			return false;
		}
	}
	return true;
}

void debugRayTrace(int x, int y)
{
	Ray ray = scene.camera->getScreenRay(x, y);
	ray.flags |= RF_DEBUG;
	raytrace(ray);
}

Color raytraceSinglePixel(double x, double y, int whichCamera = CAMERA_CENTRAL)
{
	auto getRay = scene.camera->dof ?
		[](double x, double y, int whichCamera) {
			return scene.camera->getDOFRay(x, y, whichCamera);
		} :
		[](double x, double y, int whichCamera) {
			return scene.camera->getScreenRay(x, y, whichCamera);
		};

	auto trace = scene.settings.gi ?
		[](const Ray& ray) {
			Random& rnd = getRandomGen();
			return pathtrace(ray, Color(1, 1, 1), rnd);
		} :
		[](const Ray& ray) {
			return raytrace(ray);
		};


	Ray ray = getRay(x, y, whichCamera);
    return trace(ray);
}

Color renderAAPixel(int x, int y, int whichCamera = CAMERA_CENTRAL)
{
	const double kernel[5][2] = {
		{ 0.0, 0.0 },
		{ 0.6, 0.0 },
		{ 0.0, 0.6 },
		{ 0.3, 0.3 },
		{ 0.6, 0.6 },
	};
	Color sum(0, 0, 0);
	for (int i = 0; i < COUNT_OF(kernel); i++)
		sum += raytraceSinglePixel(x + kernel[i][0], y + kernel[i][1], whichCamera);
	return sum / double(COUNT_OF(kernel));
}

Color renderDOFPixel(int x, int y, int whichCamera = CAMERA_CENTRAL)
{
	Random& rnd = getRandomGen();
	Color sum(0, 0, 0);
	for (int i = 0; i < scene.camera->numSamples; i++) {
		sum += raytraceSinglePixel(x + rnd.randdouble(), y + rnd.randdouble(), whichCamera);
	}
	return sum / scene.camera->numSamples;
}

Color renderGIPixel(int x, int y, int whichCamera = CAMERA_CENTRAL)
{
	Color sum(0, 0, 0);
	int N = scene.settings.numPaths;

	Random rnd = getRandomGen();
	for (int i = 0; i < N; i++) {
		Ray ray = scene.camera->getScreenRay(
			x + rnd.randdouble(), y + rnd.randdouble(), whichCamera
		);
		sum += pathtrace(ray, Color(1, 1, 1), rnd);
	}

	return sum / N;
}

Color renderPixel(int x, int y, int whichCamera = CAMERA_CENTRAL)
{
	if (scene.camera->dof) {
		return renderDOFPixel(x, y, whichCamera);
	} else if (scene.settings.gi) {
		return renderGIPixel(x, y, whichCamera);
	} else {
		if (scene.settings.wantAA) {
			return renderAAPixel(x, y, whichCamera);
		} else {
			return raytraceSinglePixel(x, y, whichCamera);
		}
	}
}

void render()
{
	scene.beginFrame();
	vector<Rect> buckets = getBucketsList();

	if (scene.settings.wantPrepass || scene.settings.gi) {
		// We render the whole screen in three passes.
		// 1) First pass - use very coarse resolution rendering, tracing a single ray for a 16x16 block:
		for (Rect& r: buckets) {
			for (int dy = 0; dy < r.h; dy += 16) {
				int ey = min(r.h, dy + 16);
				for (int dx = 0; dx < r.w; dx += 16) {
					int ex = min(r.w, dx + 16);
					Color c = raytraceSinglePixel(r.x0 + dx + ex / 2, r.y0 + dy + ey / 2);
					if (!drawRect(Rect(r.x0 + dx, r.y0 + dy, r.x0 + ex, r.y0 + ey), c))
						return;
				}
			}
		}
	}

    int cycleCnt = 1;
    if(scene.camera->stereoSeparation || scene.settings.output3DTV)
        ++cycleCnt;

    for(int i = 0; i < 2; ++i)
    {
        int k = 0;
        int j = 1;
        if(scene.camera->stereoSeparation)
                k = 2 - i;

        if(scene.settings.output3DTV)
        {
            j = 2;
            k = i + 1;
        }

        for (Rect& r: buckets) {
            for (int y = r.y0; y < r.y1; y++)
                for (int x = r.x0; x < r.x1; x++)
                {
                    if(scene.camera->stereoSeparation || scene.settings.output3DTV)
                    {
                        if(x%2 == 1)
                        {
                            vfb[y][x/2 + i * scene.settings.frameWidth/j] += renderPixel(x, y, k);
                            vfb[y][x/2 + i * scene.settings.frameWidth/j] /=2;
                        }
                        else
                            vfb[y][x/2 + i * scene.settings.frameWidth/j] = renderPixel(x, y, k);
                    }
                    else
                        vfb[y][x] = renderPixel(x, y);
                }
            if (!displayVFBRect(r, vfb)) return;
        }
	}
}

int renderSceneThread(void* /*unused*/)
{
	render();
	rendering = false;
	return 0;
}

const char* DEFAULT_SCENE = "data/meshes.qdmg";

int main ( int argc, char** argv )
{
	initRandom(42);
	Color::init_sRGB_cache();
	const char* sceneFile = argc == 2 ? argv[1] : DEFAULT_SCENE;
	if (!scene.parseScene(sceneFile)) {
		printf("Could not parse the scene!\n");
		return -1;
	}

	int Width = scene.settings.frameWidth;
	if(scene.camera->stereoSeparation && !scene.settings.output3DTV)
        Width *= 2;

	initGraphics(Width, scene.settings.frameHeight);
	setWindowCaption("Quad Damage: preparing...");

	scene.beginRender();

	setWindowCaption("Quad Damage: rendering...");
	Uint32 startTicks = SDL_GetTicks();
	renderScene_threaded();
	Uint32 elapsedMs = SDL_GetTicks() - startTicks;
	printf("Render took %.2fs\n", elapsedMs / 1000.0f);
	setWindowCaption("Quad Damage: rendered in %.2fs\n", elapsedMs / 1000.0f);

	displayVFB(vfb);
	waitForUserExit();
	closeGraphics();
	printf("Exited cleanly\n");
	return 0;
}
