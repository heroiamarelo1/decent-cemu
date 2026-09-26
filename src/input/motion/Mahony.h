#pragma once

#include <algorithm>
#include <math.h>
#include <cmath>
#include "util/math/quaternion.h"

class MahonySensorFusion
{
public:
	MahonySensorFusion()
	{
		// assume default forward pose (holding controller in hand, tilted forward so the sticks/buttons face upward)
		m_imuQ.Assign(sqrtf(0.5), sqrtf(0.5), 0.0f, 0.0f);
	}

	// Seed pitch and roll from gravity instead of waiting for the feedback
	// loop to converge from an arbitrary pose. Gravity cannot determine yaw,
	// so the initial yaw remains zero. This is the same physical constraint
	// used by complementary filters for real motion controllers.
	bool initializeFromAcceleration(float ax, float ay, float az)
	{
		Vector3f accel(ax, ay, az);
		const float length = accel.Length();
		if (length < 0.75f || length > 1.35f)
			return false;
		accel.Scale(1.0f / length);

		const float dot = std::clamp(accel.z, -1.0f, 1.0f);
		if (dot > 0.9999f)
			m_imuQ.Assign(1.0f, 0.0f, 0.0f, 0.0f);
		else if (dot < -0.9999f)
			m_imuQ.Assign(0.0f, 1.0f, 0.0f, 0.0f);
		else
		{
			const float s = std::sqrt((1.0f + dot) * 2.0f);
			// Quaternionf::GetVectorZ uses the transposed rotation convention.
			// These signs make GetVectorZ() equal the measured gravity vector.
			m_imuQ.Assign(s * 0.5f, accel.y / s, -accel.x / s, 0.0f);
			m_imuQ.NormalizeXYZW();
		}
		m_rollWinding = 0;
		m_pitchWinding = 0;
		m_yawWinding = 0;
		calcOrientation();
		return true;
	}

	// gx, gy, gz are in radians/sec
	void updateIMU(float deltaTime, float gx, float gy, float gz, float ax, float ay, float az)
	{
		Vector3f av(ax, ay, az);
		Vector3f gv(gx, gy, gz);
		if (deltaTime > 0.2f)
			deltaTime = 0.2f; // dont let stutter mess up the internal state
		const float accelMag = sqrtf(ax * ax + ay * ay + az * az);
		const bool gravityOk = accelMag > 0.85f && accelMag < 1.15f;
		// A slow tilt still changes which way is up. Only a pose that holds
		// still is allowed to move the bias, or gameplay walks the heading.
		updateGyroBias(gx, gy, gz, gravityOk && accelDirectionStable(ax, ay, az, deltaTime));
		gv.x -= m_gyroBias[0];
		gv.y -= m_gyroBias[1];
		gv.z -= m_gyroBias[2];

		const float gyroMag = sqrtf(gv.x * gv.x + gv.y * gv.y + gv.z * gv.z);
		if (gyroMag < 0.05f && gravityOk)
			return;

		// ignore small angles to avoid drift due to bias (especially on yaw)
		if (fabs(gv.x) < 0.015f)
			gv.x = 0.0f;
		if (fabs(gv.y) < 0.015f)
			gv.y = 0.0f;
		if (fabs(gv.z) < 0.015f)
			gv.z = 0.0f;

		// cemuLog_logDebug(LogType::Force, "[IMU Quat] time {:7.4} | {:7.2} {:7.2} {:7.2} {:7.2} | gyro( - bias) {:7.4} {:7.4} {:7.4} | acc {:7.2} {:7.2} {:7.2} | GyroBias {:7.4} {:7.4} {:7.4}", deltaTime, m_imuQ.x, m_imuQ.y, m_imuQ.z, m_imuQ.w, gv.x, gv.y, gv.z, ax, ay, az, m_gyroBias[0], m_gyroBias[1], m_gyroBias[2]);

		if (gravityOk)
		{
			av.Normalize();
			Vector3f grav = m_imuQ.GetVectorZ();
			grav.Scale(0.5f);
			Vector3f errorFeedback = grav.Cross(av);
			// apply scaled feedback
			gv -= errorFeedback;
		}
		gv.Scale(0.5f * deltaTime);
		m_imuQ += (m_imuQ * Quaternionf(0.0f, gv.x, gv.y, gv.z));
		m_imuQ.NormalizeXYZW();
		updateOrientationAngles();
	}

	float getRollRadians()
	{
		return m_roll + (float)m_rollWinding * 2.0f * 3.14159265f;
	}

	float getPitchRadians()
	{
		return m_pitch + (float)m_pitchWinding * 2.0f * 3.14159265f;
	}

	float getYawRadians()
	{
		return m_yaw + (float)m_yawWinding * 2.0f * 3.14159265f;
	}

	void getQuaternion(float q[4]) const
	{
		q[0] = m_imuQ.w;
		q[1] = m_imuQ.x;
		q[2] = m_imuQ.y;
		q[3] = m_imuQ.z;
	}

	void resetGyroBias()
	{
		for (int i = 0; i < 3; ++i)
		{
			m_gyroBias[i] = 0.0f;
			m_gyroTotalSum[i] = 0.0;
		}
		m_gyroTotalSampleCount = 0;
	}

	void getGyroBias(float gBias[3]) const
	{
		gBias[0] = m_gyroBias[0];
		gBias[1] = m_gyroBias[1];
		gBias[2] = m_gyroBias[2];
	}

private:

	// calculate roll, yaw and pitch in radians. (-0.5 to 0.5)
	void calcOrientation()
	{
		float sinr_cosp = 2.0f * (m_imuQ.z * m_imuQ.w + m_imuQ.x * m_imuQ.y);
		float cosr_cosp = 1.0f - 2.0f * (m_imuQ.w * m_imuQ.w + m_imuQ.x * m_imuQ.x);
		m_roll = std::atan2(sinr_cosp, cosr_cosp);

		// pitch (y-axis rotation)
		float sinp = 2.0f * (m_imuQ.z * m_imuQ.x - m_imuQ.y * m_imuQ.w);
		if (std::abs(sinp) >= 1.0)
			m_pitch = std::copysign(3.14159265359f / 2.0f, sinp);
		else
			m_pitch = std::asin(sinp);

		// yaw (z-axis rotation)
		float siny_cosp = 2.0f * (m_imuQ.z * m_imuQ.y + m_imuQ.w * m_imuQ.x);
		float cosy_cosp = 1.0f - 2.0f * (m_imuQ.x * m_imuQ.x + m_imuQ.y * m_imuQ.y);
		m_yaw = std::atan2(siny_cosp, cosy_cosp);
	}

	void updateOrientationAngles()
	{
		auto calcWindingCountChange = [](float prevAngle, float newAngle) -> int
		{
			if (newAngle > prevAngle)
			{
				float angleDif = newAngle - prevAngle;
				if (angleDif > 3.14159265f)
					return -1;
			}
			else if (newAngle < prevAngle)
			{
				float angleDif = prevAngle - newAngle;
				if (angleDif > 3.14159265f)
					return 1;
			}
			return 0;
		};
		float prevRoll = m_roll;
		float prevPitch = m_pitch;
		float prevYaw = m_yaw;
		calcOrientation();
		// calculate roll, yaw and pitch including winding count to match what VPAD API returns
		m_rollWinding += calcWindingCountChange(prevRoll, m_roll);
		m_pitchWinding += calcWindingCountChange(prevPitch, m_pitch);
		m_yawWinding += calcWindingCountChange(prevYaw, m_yaw);
	}

	// True when gravity has stayed put, so this sample is rest and not a tilt.
	bool accelDirectionStable(float ax, float ay, float az, float dt)
	{
		if (!m_hasStillMark)
		{
			m_hasStillMark = true;
			m_stillMark[0] = ax;
			m_stillMark[1] = ay;
			m_stillMark[2] = az;
			m_stillTimer = 0.0f;
			m_lastStable = false;
			return false;
		}
		m_stillTimer += dt;
		if (m_stillTimer < 0.35f)
			return m_lastStable;
		const float dx = ax - m_stillMark[0];
		const float dy = ay - m_stillMark[1];
		const float dz = az - m_stillMark[2];
		m_stillMark[0] = ax;
		m_stillMark[1] = ay;
		m_stillMark[2] = az;
		m_stillTimer = 0.0f;
		m_lastStable = (dx * dx + dy * dy + dz * dz) < (0.025f * 0.025f);
		return m_lastStable;
	}

	void updateGyroBias(float gx, float gy, float gz, bool still)
	{
		// A Switch Pro can sit at about 0.09 rad/s. Real tilting is larger, and
		// the old average never forgot those samples, so the heading walked off.
		if (!still || fabs(gx) >= 0.25f || fabs(gy) >= 0.25f || fabs(gz) >= 0.25f)
			return;

		if (m_gyroTotalSampleCount < 40)
		{
			m_gyroTotalSum[0] += gx;
			m_gyroTotalSum[1] += gy;
			m_gyroTotalSum[2] += gz;
			m_gyroTotalSampleCount++;
			if (m_gyroTotalSampleCount >= 20)
			{
				m_gyroBias[0] = (float)(m_gyroTotalSum[0] / (double)m_gyroTotalSampleCount);
				m_gyroBias[1] = (float)(m_gyroTotalSum[1] / (double)m_gyroTotalSampleCount);
				m_gyroBias[2] = (float)(m_gyroTotalSum[2] / (double)m_gyroTotalSampleCount);
			}
			return;
		}
		constexpr float k = 0.005f;
		m_gyroBias[0] = m_gyroBias[0] * (1.0f - k) + gx * k;
		m_gyroBias[1] = m_gyroBias[1] * (1.0f - k) + gy * k;
		m_gyroBias[2] = m_gyroBias[2] * (1.0f - k) + gz * k;
	}

	private:
		Quaternionf m_imuQ; // current orientation
		// angle data
		float m_roll{};
		float m_pitch{};
		float m_yaw{};
		int m_rollWinding{};
		int m_pitchWinding{};
		int m_yawWinding{};
		// gyro bias
		float m_gyroBias[3]{};
		double m_gyroTotalSum[3]{};
		uint64 m_gyroTotalSampleCount{};
		float m_stillMark[3]{};
		float m_stillTimer{};
		bool m_hasStillMark{};
		bool m_lastStable{};
};
