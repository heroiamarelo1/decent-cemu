package cemu.gamepad

import android.app.Activity
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.graphics.BitmapFactory
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.res.ColorStateList
import android.graphics.Color
import android.view.Surface
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.concurrent.thread
import kotlin.math.roundToInt
import kotlin.math.sqrt

class MainActivity : Activity(), SensorEventListener {
    private val sample = PadSample()
    private val server = DsuServer(sample)
    private lateinit var sensors: SensorManager
    private lateinit var image: PadImageView
    private lateinit var status: TextView
    private val axes = PadAxes()
    private var calibrating = false
    private var sensorBarOn = false
    private var sensorBarPulse = 0
    private var stillSamples = 0
    private var gravitySum = FloatArray(3)
    private var gyroSum = FloatArray(3)
    private var latestGyro = FloatArray(3)
    private var latestGyroTime = 0L
    private var latestMag = FloatArray(3)
    private var motionSequence = 0
    @Volatile private var streamRunning = false
    @Volatile private var audioConnected = false
    @Volatile private var audioGeneration = 0
    private var audioSocket: Socket? = null
    private var lastHatX = 0f
    private var lastHatY = 0f
    private var playRotation = Surface.ROTATION_90

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        image = findViewById(R.id.pad)
        status = findViewById(R.id.status)
        status.text = "In Decent Cemu, DSU points at ${localIp()}:26760"
        image.onPadTouch = { down, x, y ->
            sample.touch = down
            sample.touchX = (x * 1919f).roundToInt()
            sample.touchY = (y * 941f).roundToInt()
        }
        val pcIp = findViewById<EditText>(R.id.pc_ip)
        pcIp.setText(getSharedPreferences("pad_connect", Context.MODE_PRIVATE).getString("pc_ip", ""))
        findViewById<Button>(R.id.connect).setOnClickListener {
            startStream(pcIp.text.toString().trim())
        }
        holdButton(R.id.home) { sample.ps = it }
        holdButton(R.id.tv) { sample.screen = it }
        holdButton(R.id.mic) { sample.mic = it }
        val controls = findViewById<android.view.View>(R.id.controls)
        val back = findViewById<Button>(R.id.back)
        findViewById<Button>(R.id.ir).setOnClickListener { setIrMode(PadImageView.IrMode.CONTRAST, controls, back) }
        val irPlus = findViewById<Button>(R.id.ir_points)
        sensorBarOn = getSharedPreferences("pad_sensor_bar", Context.MODE_PRIVATE).getBoolean("on", false)
        paintSensorBar(irPlus)
        irPlus.setOnClickListener {
            sensorBarOn = !sensorBarOn
            getSharedPreferences("pad_sensor_bar", Context.MODE_PRIVATE).edit().putBoolean("on", sensorBarOn).apply()
            paintSensorBar(irPlus)
            pulseSensorBar(if (sensorBarOn) 2 else 1)
        }
        if (sensorBarOn)
            pulseSensorBar(2)
        back.setOnClickListener { setIrMode(PadImageView.IrMode.OFF, controls, back) }
        image.onShowControls = { setIrMode(PadImageView.IrMode.OFF, controls, back) }
        findViewById<Button>(R.id.calibrate).setOnClickListener { beginCalibration() }
        sensors = getSystemService(SENSOR_SERVICE) as SensorManager
        val prefs = getSharedPreferences("pad_axes_v4", Context.MODE_PRIVATE)
        axes.load(prefs)
        if (axes.ready)
            lockPlayOrientation(if (prefs.contains("rotation")) prefs.getInt("rotation", Surface.ROTATION_90) else displayRotation())
        server.start()
    }

    private fun beginCalibration() {
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        calibrating = true
        stillSamples = 0
        gravitySum = FloatArray(3)
        gyroSum = FloatArray(3)
        status.text = "Lay the phone flat on a table, screen up, in your play position, and keep still."
    }

    private fun displayRotation(): Int {
        return if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: windowManager.defaultDisplay.rotation
        else windowManager.defaultDisplay.rotation
    }

    private fun lockPlayOrientation(rotation: Int) {
        playRotation = rotation
        requestedOrientation = when (rotation) {
            Surface.ROTATION_270 -> ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
            else -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        }
    }

    private fun paintSensorBar(button: Button) {
        val on = sensorBarOn
        button.backgroundTintList = ColorStateList.valueOf(if (on) Color.WHITE else Color.BLACK)
        button.setTextColor(if (on) Color.BLACK else Color.WHITE)
    }

    private fun pulseSensorBar(cmd: Int) {
        sensorBarPulse += 1
        val pulse = sensorBarPulse
        sample.sensorBarCmd = cmd
        window.decorView.postDelayed({
            if (sensorBarPulse == pulse) sample.sensorBarCmd = 0
        }, 800)
    }

    private fun setIrMode(mode: PadImageView.IrMode, controls: android.view.View, back: Button) {
        val on = mode != PadImageView.IrMode.OFF
        image.irMode = mode
        controls.visibility = if (on) android.view.View.GONE else android.view.View.VISIBLE
        status.visibility = if (on) android.view.View.GONE else android.view.View.VISIBLE
        back.visibility = if (mode == PadImageView.IrMode.CONTRAST) android.view.View.VISIBLE else android.view.View.GONE
        val attrs = window.attributes
        attrs.screenBrightness = if (on) 1f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        window.attributes = attrs
        if (Build.VERSION.SDK_INT >= 30) {
            val controller = window.insetsController
            if (on) {
                controller?.hide(WindowInsets.Type.systemBars())
                controller?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            } else {
                controller?.show(WindowInsets.Type.systemBars())
            }
        }
    }

    private fun holdButton(id: Int, set: (Boolean) -> Unit) {
        findViewById<Button>(id).setOnTouchListener { _, event ->
            set(event.action != MotionEvent.ACTION_UP && event.action != MotionEvent.ACTION_CANCEL)
            true
        }
    }

    override fun onResume() {
        super.onResume()
        val delay = 10000 // requested 100 Hz; integrate the actual SensorEvent timestamps
        sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { sensors.registerListener(this, it, delay) }
        sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE)?.let { sensors.registerListener(this, it, delay) }
        sensors.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD)?.let { sensors.registerListener(this, it, delay) }
    }

    override fun onPause() {
        sensors.unregisterListener(this)
        super.onPause()
    }

    override fun onDestroy() {
        streamRunning = false
        audioSocket?.close()
        server.stop()
        super.onDestroy()
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (event.values.take(3).any { !it.isFinite() }) return
        when (event.sensor.type) {
            Sensor.TYPE_GYROSCOPE -> {
                latestGyro = event.values.copyOf(3)
                latestGyroTime = event.timestamp
            }
            Sensor.TYPE_MAGNETIC_FIELD -> latestMag = event.values.copyOf(3)
            Sensor.TYPE_ACCELEROMETER -> {
                // Do not invent motion observations when a sensor has stopped.
                val age = event.timestamp - latestGyroTime
                if (latestGyroTime == 0L || age < -20_000_000L || age > 30_000_000L) return
                if (calibrating) collectCalibration(event.values)
                if (!axes.ready || calibrating) return
                val a = axes.accel(event.values[0], event.values[1], event.values[2])
                val g = axes.gyro(latestGyro[0], latestGyro[1], latestGyro[2])
                val m = axes.magnet(latestMag[0], latestMag[1], latestMag[2])
                val deg = 180f / Math.PI.toFloat()
                sample.motion = MotionSnapshot(event.timestamp / 1000L, ++motionSequence,
                    a[0]/9.80665f, a[1]/9.80665f, a[2]/9.80665f,
                    g[0]*deg, g[1]*deg, g[2]*deg, m[0], m[1], m[2],
                    axes.edgeDown(event.values[0], event.values[1], event.values[2]))
            }
        }
    }

    private fun collectCalibration(accel: FloatArray) {
        val accelMag = sqrt(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2])
        val gyroMag = sqrt(latestGyro[0] * latestGyro[0] + latestGyro[1] * latestGyro[1] + latestGyro[2] * latestGyro[2])
        if (accelMag < 9.3f || accelMag > 10.3f || gyroMag > 0.05f || accel[2] / accelMag < 0.966f) {
            if (stillSamples > 0)
                requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
            stillSamples = 0
            gravitySum = FloatArray(3)
            gyroSum = FloatArray(3)
            runOnUiThread { status.text = "Keep still. Lay the phone flat on a table, screen up." }
            return
        }
        if (stillSamples == 0)
            lockPlayOrientation(displayRotation())
        for (i in 0..2) {
            gravitySum[i] += accel[i]
            gyroSum[i] += latestGyro[i]
        }
        stillSamples += 1
        if (stillSamples < 100) return
        val gravity = FloatArray(3) { gravitySum[it] / stillSamples }
        val bias = FloatArray(3) { gyroSum[it] / stillSamples }
        val (screenRight, screenUp) = PadAxes.screenRightAndUp(playRotation)
        axes.capture(gravity, bias, screenRight, screenUp)
        val prefs = getSharedPreferences("pad_axes_v4", Context.MODE_PRIVATE)
        axes.save(prefs)
        prefs.edit().putInt("rotation", playRotation).apply()
        calibrating = false
        runOnUiThread { status.text = "Calibrated. The screen stays in this orientation." }
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN || event.action == KeyEvent.ACTION_UP) {
            if (setKey(event.keyCode, event.action == KeyEvent.ACTION_DOWN))
                return true
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
        val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
        if (hatX != lastHatX || hatY != lastHatY) {
            lastHatX = hatX
            lastHatY = hatY
            setBit(PadBits.LEFT, hatX < -0.5f)
            setBit(PadBits.RIGHT, hatX > 0.5f)
            setBit(PadBits.UP, hatY < -0.5f)
            setBit(PadBits.DOWN, hatY > 0.5f)
        }
        val source = event.source
        if (source and InputDevice.SOURCE_JOYSTICK == 0 && source and InputDevice.SOURCE_GAMEPAD == 0)
            return super.onGenericMotionEvent(event)
        sample.lx = axisByte(event, MotionEvent.AXIS_X, false)
        sample.ly = axisByte(event, MotionEvent.AXIS_Y, true)
        sample.rx = axisByte(event, MotionEvent.AXIS_Z, false)
        sample.ry = axisByte(event, MotionEvent.AXIS_RZ, true)
        val l2 = event.getAxisValue(MotionEvent.AXIS_LTRIGGER) > 0.4f || event.getAxisValue(MotionEvent.AXIS_BRAKE) > 0.4f
        val r2 = event.getAxisValue(MotionEvent.AXIS_RTRIGGER) > 0.4f || event.getAxisValue(MotionEvent.AXIS_GAS) > 0.4f
        setBit(PadBits.L2, l2)
        setBit(PadBits.R2, r2)
        return true
    }

    private fun setKey(keyCode: Int, down: Boolean): Boolean {
        val bit = when (keyCode) {
            KeyEvent.KEYCODE_BUTTON_A -> PadBits.CROSS
            KeyEvent.KEYCODE_BUTTON_B -> PadBits.CIRCLE
            KeyEvent.KEYCODE_BUTTON_X -> PadBits.SQUARE
            KeyEvent.KEYCODE_BUTTON_Y -> PadBits.TRIANGLE
            KeyEvent.KEYCODE_BUTTON_L1 -> PadBits.L1
            KeyEvent.KEYCODE_BUTTON_R1 -> PadBits.R1
            KeyEvent.KEYCODE_BUTTON_L2 -> PadBits.L2
            KeyEvent.KEYCODE_BUTTON_R2 -> PadBits.R2
            KeyEvent.KEYCODE_BUTTON_THUMBL -> PadBits.L3
            KeyEvent.KEYCODE_BUTTON_THUMBR -> PadBits.R3
            KeyEvent.KEYCODE_BUTTON_START -> PadBits.OPTIONS
            KeyEvent.KEYCODE_BUTTON_SELECT -> PadBits.SHARE
            KeyEvent.KEYCODE_DPAD_UP -> PadBits.UP
            KeyEvent.KEYCODE_DPAD_RIGHT -> PadBits.RIGHT
            KeyEvent.KEYCODE_DPAD_DOWN -> PadBits.DOWN
            KeyEvent.KEYCODE_DPAD_LEFT -> PadBits.LEFT
            KeyEvent.KEYCODE_BUTTON_MODE -> {
                sample.ps = down
                return true
            }
            else -> return false
        }
        setBit(bit, down)
        return true
    }

    private fun setBit(bit: Int, down: Boolean) {
        sample.buttons = if (down) sample.buttons or bit else sample.buttons and bit.inv()
    }

    private fun axisByte(event: MotionEvent, axis: Int, invert: Boolean): Int {
        var value = event.getAxisValue(axis)
        if (invert) value = -value
        return ((value + 1f) * 0.5f * 255f).roundToInt().coerceIn(0, 255)
    }

    private fun startStream(ip: String) {
        if (ip.isEmpty()) return
        getSharedPreferences("pad_connect", Context.MODE_PRIVATE).edit().putString("pc_ip", ip).apply()
        streamRunning = false
        audioSocket?.close()
        audioConnected = false
        streamRunning = true
        status.text = "Connecting to $ip…"
        startAudio(ip)
        thread(name = "pad-video") {
            try {
                Socket(ip, 26761).use { socket ->
                    runOnUiThread { status.text = "Connected to $ip" }
                    val input = socket.getInputStream()
                    val header = ByteArray(4)
                    var shown = false
                    while (streamRunning) {
                        if (!readFully(input, header)) break
                        val size = ByteBuffer.wrap(header).order(ByteOrder.BIG_ENDIAN).int
                        if (size <= 0 || size > 2_000_000) break
                        val jpeg = ByteArray(size)
                        if (!readFully(input, jpeg)) break
                        val bitmap = BitmapFactory.decodeByteArray(jpeg, 0, jpeg.size) ?: continue
                        runOnUiThread {
                            image.bitmap = bitmap
                            if (!shown) {
                                status.text = if (audioConnected) "Video and audio connected" else "Video connected"
                                shown = true
                            }
                        }
                    }
                }
            } catch (ex: Exception) {
                runOnUiThread { status.text = ex.message ?: "Connection failed" }
            }
            streamRunning = false
        }
    }

    private fun startAudio(ip: String) {
        val generation = ++audioGeneration
        thread(name = "pad-audio") {
            var track: AudioTrack? = null
            try {
                val socket = Socket(ip, 26762)
                if (generation != audioGeneration) {
                    socket.close()
                    return@thread
                }
                audioSocket = socket
                audioConnected = true
                runOnUiThread {
                    if (status.text == "Video connected" || status.text == "Connected to $ip")
                        status.text = "Video and audio connected"
                }
                val rate = 48000
                val channel = AudioFormat.CHANNEL_OUT_STEREO
                val encoding = AudioFormat.ENCODING_PCM_16BIT
                val min = AudioTrack.getMinBufferSize(rate, channel, encoding).coerceAtLeast(4096)
                val bufferBytes = (rate * 4 / 5).coerceAtLeast(min * 4)
                track = AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build()
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setEncoding(encoding)
                            .setSampleRate(rate)
                            .setChannelMask(channel)
                            .build()
                    )
                    .setBufferSizeInBytes(bufferBytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()
                val playing = track ?: return@thread
                playing.play()
                val input = socket.getInputStream()
                val buf = ByteArray(2048)
                var held = 0
                while (generation == audioGeneration) {
                    val n = input.read(buf, held, buf.size - held)
                    if (n < 0) break
                    held += n
                    val even = held - held % 4
                    var off = 0
                    while (off < even && generation == audioGeneration) {
                        if (playing.playState != AudioTrack.PLAYSTATE_PLAYING)
                            playing.play()
                        val wrote = playing.write(buf, off, even - off, AudioTrack.WRITE_BLOCKING)
                        if (wrote <= 0) {
                            playing.play()
                            Thread.sleep(5)
                            break
                        }
                        off += wrote
                    }
                    if (off < held) System.arraycopy(buf, off, buf, 0, held - off)
                    held -= off
                }
            } catch (_: Exception) {
            } finally {
                try { track?.stop() } catch (_: Exception) {}
                track?.release()
                if (generation == audioGeneration) audioConnected = false
            }
        }
    }

    private fun readFully(input: java.io.InputStream, dest: ByteArray): Boolean {
        var got = 0
        while (got < dest.size) {
            val n = input.read(dest, got, dest.size - got)
            if (n < 0) return false
            got += n
        }
        return true
    }

    private fun localIp(): String {
        NetworkInterface.getNetworkInterfaces()?.toList()?.forEach { iface ->
            iface.inetAddresses.toList().forEach { address ->
                if (!address.isLoopbackAddress && address is Inet4Address)
                    return address.hostAddress ?: "?"
            }
        }
        return "?"
    }
}
