package cemu.gamepad

import android.content.SharedPreferences
import kotlin.math.sqrt

// Turns the phone's sensor axes into the GamePad axes captured while the phone lay flat.
class PadAxes {
    private var right = floatArrayOf(1f, 0f, 0f)
    private var up = floatArrayOf(0f, 1f, 0f)
    private var out = floatArrayOf(0f, 0f, 1f)
    private var bias = floatArrayOf(0f, 0f, 0f)
    var ready = false
        private set

    fun load(prefs: SharedPreferences) {
        if (!prefs.getBoolean("ready", false)) return
        right = prefs.getVector("right")
        up = prefs.getVector("up")
        out = prefs.getVector("out")
        bias = prefs.getVector("bias")
        ready = true
    }

    fun save(prefs: SharedPreferences) {
        prefs.edit()
            .putBoolean("ready", true)
            .putVector("right", right)
            .putVector("up", up)
            .putVector("out", out)
            .putVector("bias", bias)
            .apply()
    }

    fun capture(gravity: FloatArray, gyroBias: FloatArray, screenRight: FloatArray, screenUp: FloatArray) {
        // Calibration measures bias, never bends the physical sensor basis.
        // Screen axes form one orthonormal, right-handed frame in every pose.
        right = normalize(screenRight)
        out = normalize(cross(right, screenUp))
        up = normalize(cross(out, right))
        bias = gyroBias.copyOf()
        ready = true
    }

    fun edgeDown(x: Float, y: Float, z: Float): Int {
        if (!ready) return 0
        val v = floatArrayOf(x, y, z)
        val alongRight = dot(right, v)
        val alongUp = dot(up, v)
        val alongOut = dot(out, v)
        val ar = kotlin.math.abs(alongRight)
        val au = kotlin.math.abs(alongUp)
        val ao = kotlin.math.abs(alongOut)
        return when {
            ao >= ar && ao >= au -> if (alongOut > 0f) 5 else 6
            ar >= au -> if (alongRight > 0f) 1 else 2
            else -> if (alongUp > 0f) 3 else 4
        }
    }

    fun accel(x: Float, y: Float, z: Float): FloatArray {
        val v = map(x, y, z)
        return floatArrayOf(v[0], -v[1], -v[2])
    }

    fun gyro(x: Float, y: Float, z: Float): FloatArray {
        return map(x - bias[0], y - bias[1], z - bias[2])
    }

    fun magnet(x: Float, y: Float, z: Float): FloatArray = map(x, y, z)

    private fun map(x: Float, y: Float, z: Float): FloatArray {
        val v = floatArrayOf(x, y, z)
        val mx = dot(right, v)
        val my = dot(up, v)
        val mz = dot(out, v)
        // Measured with the phone still. Screen-up was sent as +Z and the game
        // drew that pitched onto its side. The pose it treats as lying flat is +Y,
        // and screen-toward-you is -Z.
        return floatArrayOf(mx, mz, -my)
    }

    companion object {
        fun screenRightAndUp(rotation: Int): Pair<FloatArray, FloatArray> {
            return when (rotation) {
                android.view.Surface.ROTATION_90 -> floatArrayOf(0f, -1f, 0f) to floatArrayOf(1f, 0f, 0f)
                android.view.Surface.ROTATION_270 -> floatArrayOf(0f, 1f, 0f) to floatArrayOf(-1f, 0f, 0f)
                android.view.Surface.ROTATION_180 -> floatArrayOf(-1f, 0f, 0f) to floatArrayOf(0f, -1f, 0f)
                else -> floatArrayOf(1f, 0f, 0f) to floatArrayOf(0f, 1f, 0f)
            }
        }

        private fun SharedPreferences.getVector(name: String) = floatArrayOf(
            getFloat("${name}0", 0f),
            getFloat("${name}1", 0f),
            getFloat("${name}2", 0f)
        )

        private fun SharedPreferences.Editor.putVector(name: String, value: FloatArray): SharedPreferences.Editor {
            putFloat("${name}0", value[0])
            putFloat("${name}1", value[1])
            putFloat("${name}2", value[2])
            return this
        }
    }
}

private fun dot(a: FloatArray, b: FloatArray) = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

private fun cross(a: FloatArray, b: FloatArray) = floatArrayOf(
    a[1] * b[2] - a[2] * b[1],
    a[2] * b[0] - a[0] * b[2],
    a[0] * b[1] - a[1] * b[0]
)

private fun normalize(v: FloatArray): FloatArray {
    val length = sqrt(dot(v, v))
    if (length < 1e-4f) return floatArrayOf(0f, 0f, 1f)
    return floatArrayOf(v[0] / length, v[1] / length, v[2] / length)
}
