#include "haumea.h"
#include <cmath>

#define PI 3.14159265359f
#ifndef min
#define min(a, b) (a < b ? a : b)
#endif

float
ClampCameraFov(float fov)
{
	if(!std::isfinite(fov))
		return CAMERA_FOV_DEFAULT;
	return fov < CAMERA_FOV_MIN ? CAMERA_FOV_MIN :
	       fov > CAMERA_FOV_MAX ? CAMERA_FOV_MAX : fov;
}

CCamera TheCamera;

// librw's Camera::setFOV widens the view with the window's shape faster than
// the window widens: at 32:9 it is some 160 degrees across, and at 90 degrees
// FOV, or past about 4:1, over 180, which turns the picture and the sky inside
// out. Up to 16:9 keep its view; wider, keep the 16:9 view's height and only
// widen it, as the game's widescreen fixes do.
void
SetCameraFov(rw::Camera *cam, float fov, float aspect)
{
	const float wide = 16.0f/9.0f;
	if(!(aspect > wide)){
		cam->setFOV(fov, aspect);
		return;
	}
	// librw's view window at 16:9: tan of the half width, over 16:9.
	float half = 2.0f*atanf(tanf(fov*PI/720.0f) * wide*0.75f);
	float height = tanf(half)/wide;
	rw::V2d v;
	v.set(height*aspect, height);
	cam->setViewWindow(&v);
	v.set(0.0f, 0.0f);
	cam->setViewOffset(&v);
}

using rw::Quat;
using rw::V3d;

void
CCamera::Process(void)
{
	float scl = avgTimeStep*30.0f;
	float sensitivity = 1.0f;

	ImGuiIO &io = ImGui::GetIO();
	bool blockMouse = io.WantCaptureMouse || gGizmoHovered || gGizmoUsing;

	this->mx = CPad::newMouseState.x/io.DisplaySize.x;
	this->my = CPad::newMouseState.y/io.DisplaySize.y;
	this->mx = this->mx*2.0f - 1.0f;
	this->my = this->my*2.0f - 1.0f;

	using namespace rw;
	V3d dir = normalize(sub(TheCamera.m_target, TheCamera.m_position));
	V3d right = scale(normalize(cross(dir, TheCamera.m_up)), m_rwcam->viewWindow.x);
	V3d localup = scale(normalize(cross(right, dir)), m_rwcam->viewWindow.y);
	m_mouseDir = add(scale(right, this->mx), scale(localup, -this->my));
	m_mouseDir = add(m_mouseDir, m_at);
//	m_mouseDir = normalize(m_mouseDir);

	// Mouse
	// The look button is configurable. A stationary click is still handled by
	// the active editor tool, so RMB can provide look-on-drag and deselect-on-click.
	// LMB remains reserved for painting while the brush tool is active; in that
	// one case the brush falls back to RMB for camera look.
	int lookButton = gCameraLookButton;
	if(gBrushMode && lookButton == 1)
		lookButton = 3;
	bool lookBlockedByTool = gRectSelectActive || gObjectDragActive;
	// Arm on press, then start looking only after a small drag or a fly key.
	// Locking the cursor immediately made ordinary right-clicks unreliable.
	static bool looking, lookArmed;
	static int lookStartX, lookStartY;
	if(!CPad::IsMButtonDown(lookButton)){
		looking = lookArmed = false;
	}else if(CPad::IsMButtonJustDown(lookButton)){
		lookArmed = !gGizmoUsing && !lookBlockedByTool &&
			(gGizmoHovered ? lookButton != 1 : !io.WantCaptureMouse);
		lookStartX = CPad::newMouseState.x;
		lookStartY = CPad::newMouseState.y;
	}
	if(lookArmed && !looking){
		int dx = CPad::newMouseState.x - lookStartX, dy = CPad::newMouseState.y - lookStartY;
		float tolerance = 6.0f*GetUiScale();
		bool flyPressed = !CPad::IsCtrlDown() && !io.WantCaptureKeyboard && !io.WantTextInput &&
			(CPad::IsKeyDown('W') || CPad::IsKeyDown('A') || CPad::IsKeyDown('S') ||
			 CPad::IsKeyDown('D') || CPad::IsKeyDown('Q') || CPad::IsKeyDown('E'));
		if(dx*dx + dy*dy > tolerance*tolerance || flyPressed){
			looking = true;
			CPad::CancelClick();
		}
	}
	gCameraLooking = looking;
	if(looking){
		if(CPad::IsAltDown() && CPad::IsCtrlDown()){
			float dy = (CPad::oldMouseState.y - CPad::newMouseState.y);
			dolly(dy*scl);
		}else{
			float dx = (CPad::oldMouseState.x - CPad::newMouseState.x);
			float dy = (CPad::oldMouseState.y - CPad::newMouseState.y);
			turn(DEGTORAD(dx)/2.0f*scl, DEGTORAD(dy)/2.0f*scl);
		}
	}
	SetLookCursorLocked(looking);
	// roughly 3ds max controls
	if(!blockMouse && lookButton != 2 && CPad::IsMButtonDown(2)){
		if(CPad::IsAltDown() && CPad::IsCtrlDown()){
			float dy = (CPad::oldMouseState.y - CPad::newMouseState.y);
			zoom(dy*scl);
		}else if(CPad::IsAltDown()){
			float dx = (CPad::oldMouseState.x - CPad::newMouseState.x);
			float dy = (CPad::oldMouseState.y - CPad::newMouseState.y);
			orbit(DEGTORAD(dx)/2.0f*scl, -DEGTORAD(dy)/2.0f*scl);
		}else{
			float dx = (CPad::oldMouseState.x - CPad::newMouseState.x);
			float dy = (CPad::oldMouseState.y - CPad::newMouseState.y);
			float dist = distanceToTarget();
			pan(dx*scl*dist/100.0f, -dy*scl*dist/100.0f);
		}
	}

	// Keyboard: W/A/S/D fly only while looking (the look button, RMB by default,
	// held), as in Unity and Unreal, and Q/E go down and up; on their own they
	// are shortcuts. Not with
	// Ctrl (Ctrl+D duplicates, Ctrl+S saves), nor while ImGui has the keyboard
	// (a key held into a field). Alt (slow) and Shift (fast) still fly.
	bool flyKeys = looking && !CPad::IsCtrlDown() && !io.WantCaptureKeyboard && !io.WantTextInput;
	// A hold spent flying is not a click: releasing it doesn't open the
	// viewport menu or stop placing.
	if(flyKeys && (CPad::IsKeyDown('W') || CPad::IsKeyDown('A') || CPad::IsKeyDown('S') || CPad::IsKeyDown('D') ||
	               CPad::IsKeyDown('Q') || CPad::IsKeyDown('E')))
		CPad::CancelClick();
	auto flyKey = [flyKeys](int key){ return flyKeys && CPad::IsKeyDown(key); };
	float flyMul = 1.0f;
	if(CPad::IsShiftDown()) flyMul = gFlyFastMul;
	else if(CPad::IsAltDown()) flyMul = gFlySlowMul;

	static float speed = 0.0f;
	static float sidespeed = 0.0f;
	// Accelerating, the fly speed (Ctrl+Shift +/-) sets how fast it builds up
	// and how fast it gets; 2 is the default.
	float accel = 0.1f * gFlySpeed / 2.0f, maxSpeed = 70.0f * gFlySpeed / 2.0f;
	static bool wasFlyAcceleration = gFlyAcceleration;
	if(wasFlyAcceleration != gFlyAcceleration){
		speed = 0.0f;
		sidespeed = 0.0f;
		wasFlyAcceleration = gFlyAcceleration;
	}
	if(gFlyAcceleration){
		if(flyKey('W'))
			speed += accel;
		else if(flyKey('S'))
			speed -= accel;
		else
			speed = 0.0f;
	}else{
		speed = flyKey('W') ? gFlySpeed :
		        flyKey('S') ? -gFlySpeed : 0.0f;
	}
	if(speed > maxSpeed) speed = maxSpeed;
	if(speed < -maxSpeed) speed = -maxSpeed;
	dolly(speed*scl*flyMul);

	if(gFlyAcceleration){
		if(flyKey('A'))
			sidespeed -= accel;
		else if(flyKey('D'))
			sidespeed += accel;
		else
			sidespeed = 0.0f;
	}else{
		sidespeed = flyKey('A') ? -gFlySpeed :
		            flyKey('D') ? gFlySpeed : 0.0f;
	}
	if(sidespeed > maxSpeed) sidespeed = maxSpeed;
	if(sidespeed < -maxSpeed) sidespeed = -maxSpeed;
	pan(sidespeed*scl*flyMul, 0.0f);

	// Straight up and down in the world, whichever way the camera looks.
	static float upspeed = 0.0f;
	if(gFlyAcceleration){
		if(flyKey('E'))
			upspeed += accel;
		else if(flyKey('Q'))
			upspeed -= accel;
		else
			upspeed = 0.0f;
	}else{
		upspeed = flyKey('E') ? gFlySpeed :
		          flyKey('Q') ? -gFlySpeed : 0.0f;
	}
	if(upspeed > maxSpeed) upspeed = maxSpeed;
	if(upspeed < -maxSpeed) upspeed = -maxSpeed;
	if(upspeed != 0.0f){
		V3d rise = { 0.0f, 0.0f, upspeed*scl*flyMul };
		m_position = add(m_position, rise);
		m_target = add(m_target, rise);
	}



	// Pad
	CPad *pad = CPad::GetPad(0);
	sensitivity = 1.0f;
	if(pad->NewState.r2){
		sensitivity = 2.0f;
		if(pad->NewState.l2)
			sensitivity = 4.0f;
	}else if(pad->NewState.l2)
		sensitivity = 0.5f;
	if(pad->NewState.square) zoom(0.4f*sensitivity*scl);
	if(pad->NewState.cross) zoom(-0.4f*sensitivity*scl);
	orbit(pad->NewState.getLeftX()/25.0f*sensitivity*scl,
	                -pad->NewState.getLeftY()/25.0f*sensitivity*scl);
	turn(-pad->NewState.getRightX()/25.0f*sensitivity*scl,
	               pad->NewState.getRightY()/25.0f*sensitivity*scl);
	if(pad->NewState.up)
		dolly(2.0f*sensitivity*scl);
	if(pad->NewState.down)
		dolly(-2.0f*sensitivity*scl);

//	if(IsButtonJustDown(pad, start)){
//		printf("cam.position: %f, %f, %f\n", m_position.x, m_position.y, m_position.z);
//		printf("cam.target: %f, %f, %f\n", m_target.x, m_target.y, m_target.z);
//	}
}

void
CCamera::DrawTarget(void)
{
	float dist = distanceToTarget()/20.0f;
	rw::V3d x = { dist, 0.0f, 0.0f };
	rw::V3d y = { 0.0f, dist, 0.0f };
	rw::V3d z = { 0.0f, 0.0f, dist };
	RenderAxesWidget(this->m_target, x, y, z);
}

void
CCamera::update(void)
{
	m_fov = ClampCameraFov(m_fov);
	if(m_rwcam){
		SetCameraFov(m_rwcam, m_fov, m_aspectRatio);
		SetCameraFov(m_rwcam_viewer, m_fov, m_aspectRatio);

		rw::Frame *f = m_rwcam->getFrame();
		if(f){
			rw::V3d dir = sub(m_target, m_position);
			if(length(dir) < 0.0001f)
				dir = m_at;
			m_at = normalize(dir);
			// A view set straight up or down (Go to Coordinates, a saved camera) has
			// no horizon to lean on: take the up from the last heading, never flip.
			V3d lookUp = m_up;
			if(fabsf(m_at.z) > 0.9999f){
				lookUp = { m_localup.x, m_localup.y, 0.0f };
				if(length(lookUp) < 1e-4f)
					lookUp = { 0.0f, 1.0f, 0.0f };
				else
					lookUp = normalize(lookUp);
			}
			f->matrix.lookAt(m_at, lookUp);
			f->matrix.pos = m_position;
			f->updateObjects();
		}
	}
}

void
CCamera::setTarget(V3d target)
{
	m_position = sub(m_position, sub(m_target, target));
	m_target = target;
}

float
CCamera::getHeading(void)
{
	V3d dir = sub(m_target, m_position);
	float a = atan2(dir.y, dir.x)-PI/2.0f;
	return m_localup.z < 0.0f ? a-PI : a;
}

// The view turned by yaw around the world's up, then raised by pitch, but
// never past straight up or down: beyond that the picture would turn upside
// down. Returns the new view direction (as long as dir) and sets the up vectors.
static const float MAX_ELEVATION = 89.0f * 3.14159265f / 180.0f;

static V3d
turnedView(V3d dir, float yaw, float pitch, V3d *localup, V3d *up)
{
	float length = rw::length(dir);
	if(length < 1e-6f)
		return dir;
	V3d zaxis = { 0.0f, 0.0f, 1.0f };
	dir = rotate(dir, Quat::rotation(yaw, zaxis));
	V3d flat = { dir.x, dir.y, 0.0f };
	float flatLength = rw::length(flat);
	if(flatLength < 1e-5f){
		// Looking exactly down or up: the heading comes from the up vector.
		flat = { localup->x, localup->y, 0.0f };
		flat = rotate(flat, Quat::rotation(yaw, zaxis));
		if(dir.z > 0.0f)
			flat = scale(flat, -1.0f);
		flatLength = rw::length(flat);
		if(flatLength < 1e-5f)
			flat = { 1.0f, 0.0f, 0.0f }, flatLength = 1.0f;
	}
	V3d heading = scale(flat, 1.0f / flatLength);
	float elevation = atan2f(dir.z, rw::length(V3d{ dir.x, dir.y, 0.0f }));
	elevation += pitch;
	if(elevation > MAX_ELEVATION) elevation = MAX_ELEVATION;
	if(elevation < -MAX_ELEVATION) elevation = -MAX_ELEVATION;
	float c = cosf(elevation), s = sinf(elevation);
	*localup = add(scale(heading, -s), scale(zaxis, c));
	*up = zaxis;
	return scale(add(scale(heading, c), scale(zaxis, s)), length);
}

void
CCamera::turn(float yaw, float pitch)
{
	V3d dir = turnedView(sub(m_target, m_position), yaw, pitch, &m_localup, &m_up);
	m_target = add(m_position, dir);
}

void
CCamera::orbit(float yaw, float pitch)
{
	V3d dir = turnedView(sub(m_target, m_position), yaw, -pitch, &m_localup, &m_up);
	m_position = sub(m_target, dir);
}

void
CCamera::dolly(float dist)
{
	V3d dir = setlength(sub(m_target, m_position), dist);
	m_position = add(m_position, dir);
	m_target = add(m_target, dir);
}

void
CCamera::zoom(float dist)
{
	const float MAX_ZOOM_DIST = isSA() ? (float)SA_MP_WORLD_BOUND : 10000.0f;
	V3d dir = sub(m_target, m_position);
	float curdist = length(dir);
	if(dist >= curdist)
		dist = curdist-0.3f;
	dir = setlength(dir, dist);
	m_position = add(m_position, dir);
}

void
CCamera::pan(float x, float y)
{
	V3d dir = normalize(sub(m_target, m_position));
	V3d right = normalize(cross(dir, m_up));
	V3d localup = normalize(cross(right, dir));
	dir = add(scale(right, x), scale(localup, y));
	m_position = add(m_position, dir);
	m_target = add(m_target, dir);
}

void
CCamera::setDistanceFromTarget(float dist)
{
	V3d dir = sub(m_position, m_target);
	dir = scale(normalize(dir), dist);
	m_position = add(m_target, dir);
}

void
CCamera::setDistanceToTarget(float dist)
{
	V3d dir = sub(m_target, m_position);
	dir = scale(normalize(dir), dist);
	m_target = add(m_position, dir);
}

float
CCamera::distanceTo(V3d v)
{
	return length(sub(m_position, v));
}

float
CCamera::distanceToTarget(void)
{
	return length(sub(m_position, m_target));
}

// calculate minimum distance to a sphere at the target
// so the whole sphere is visible
float
CCamera::minDistToSphere(float r)
{
	float t = min(m_rwcam->viewWindow.x, m_rwcam->viewWindow.y);
	float a = atan(t);	// half FOV angle
	return r/sin(a);
}

CCamera::CCamera()
{
	m_position.set(0.0f, 6.0f, 0.0f);
	m_target.set(0.0f, 0.0f, 0.0f);

	m_up.set(0.0f, 0.0f, 1.0f);
	m_localup = m_up;
	m_fov = CAMERA_FOV_DEFAULT;
	m_aspectRatio = 1.0f;
	m_rwcam = nil;
	m_LODmult = 1.0f;
}


bool
CCamera::IsSphereVisible(rw::Sphere *sph, rw::Matrix *xform)
{
	rw::Sphere sphere = *sph;
	rw::Camera *cam = gUseViewerCam && m_rwcam_viewer ? m_rwcam_viewer : m_rwcam;
	rw::V3d::transformPoints(&sphere.center, &sphere.center, 1, xform);
	return cam->frustumTestSphere(&sphere) != rw::Camera::SPHEREOUTSIDE;
}
