#include "SceneCamera.h"
#include "SceneNode.h"
#include "Utils/Logging.h"
#include <iostream>
#include "Utils/Eigen.h"

using namespace wallpaper;
using namespace Eigen;

void SceneCamera::SetDirectLookAt(const Vector3d& eye, const Vector3d& center, const Vector3d& up) {
	m_direct_lookat = true;
	m_eye           = eye;
	m_center        = center;
	m_up_vec        = up;
	Update();
}

Vector3d SceneCamera::GetPosition() const {
	if(m_direct_lookat) {
		if (m_reflect_y0) return { m_eye.x(), -m_eye.y(), m_eye.z() };
		return m_eye;
	}
	if(m_node) {
		return Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
	}
	return Vector3d::Zero();
}

Vector3d SceneCamera::GetDirection() const {
	if(m_direct_lookat) {
		return (m_center - m_eye).normalized();
	}
	if(m_node) {
		return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
	}
	return -Vector3d::UnitZ();
}

Matrix4d SceneCamera::GetViewMatrix() const {
	return m_viewMat;
}

Matrix4d SceneCamera::GetViewProjectionMatrix() const {
	return m_viewProjectionMat;
}

void SceneCamera::CalculateViewProjectionMatrix() {
	// CalculateViewMatrix
	{
		if(m_direct_lookat) {
			if (m_reflect_y0) {
				// Reflect camera about Y=0 for planar reflection
				Vector3d eye    = { m_eye.x(),    -m_eye.y(),    m_eye.z() };
				Vector3d center = { m_center.x(), -m_center.y(), m_center.z() };
				Vector3d up     = { m_up_vec.x(), -m_up_vec.y(), m_up_vec.z() };
				m_viewMat = LookAt(eye, center, up);
			} else {
				m_viewMat = LookAt(m_eye, m_center, m_up_vec);
			}
		} else if(m_node) {
			Affine3d nodeTrans(m_node->GetLocalTrans());
			Vector3d eye = nodeTrans * Vector3d::Zero();
			Vector3d center = nodeTrans * (-Vector3d::UnitZ());
			Vector3d up = Vector3d::UnitY();
			m_viewMat = LookAt(eye, center, up);
		} else
			m_viewMat = Matrix4d::Identity();
	};

	if(m_perspective) {
		m_viewProjectionMat = Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * m_viewMat;
	} else {
		double left = -m_width/2.0f;
		double right = m_width/2.0f;
		double bottom = -m_height/2.0f;
		double up = m_height/2.0f;
		m_viewProjectionMat = Ortho(left, right, bottom, up, m_nearClip, m_farClip) * m_viewMat;
	}
}

void SceneCamera::Update() {
	CalculateViewProjectionMatrix();
}


void SceneCamera::AttatchNode(std::shared_ptr<SceneNode> node) {
	if(!node) {
		LOG_ERROR("Attach a null node to camera");
		return;
	}
	m_node = node;
	Update();
}

void SceneCamera::LoadPaths(std::vector<CameraPath> paths) {
	m_paths       = std::move(paths);
	m_currentPath = 0;
	m_pathTime    = 0;
	if (!m_paths.empty() && m_paths[0].keyframes.size() >= 1) {
		auto& kf = m_paths[0].keyframes[0];
		SetDirectLookAt(kf.eye, kf.center, kf.up);
	}
}

void SceneCamera::AdvanceTime(double dt) {
	if (m_paths.empty()) return;

	m_pathTime += dt;
	auto& path = m_paths[m_currentPath];

	// Move to next path when current one finishes
	if (m_pathTime >= path.duration) {
		m_pathTime -= path.duration;
		m_currentPath = (m_currentPath + 1) % m_paths.size();
		auto& next_path = m_paths[m_currentPath];
		// Clamp in case dt was very large
		if (m_pathTime >= next_path.duration) m_pathTime = 0;
	}

	auto& cur = m_paths[m_currentPath];
	if (cur.keyframes.size() < 2) return;

	// Find the two keyframes surrounding the current time
	auto& kf0 = cur.keyframes[0];
	auto& kf1 = cur.keyframes[1];

	double t = (cur.duration > 0) ? m_pathTime / cur.duration : 0;
	if (t < 0) t = 0;
	if (t > 1) t = 1;

	Vector3d eye    = kf0.eye    + t * (kf1.eye    - kf0.eye);
	Vector3d center = kf0.center + t * (kf1.center - kf0.center);
	Vector3d up     = (kf0.up    + t * (kf1.up     - kf0.up)).normalized();

	SetDirectLookAt(eye, center, up);
}