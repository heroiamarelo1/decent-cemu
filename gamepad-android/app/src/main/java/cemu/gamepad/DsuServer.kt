package cemu.gamepad

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.CRC32
import kotlin.concurrent.thread

data class MotionSnapshot(
    val timestampUs: Long = 0, val sequence: Int = 0,
    val accelX: Float = 0f, val accelY: Float = 0f, val accelZ: Float = 0f,
    val gyroX: Float = 0f, val gyroY: Float = 0f, val gyroZ: Float = 0f,
    val magX: Float = 0f, val magY: Float = 0f, val magZ: Float = 0f,
    val edgeDown: Int = 0
)

class PadSample {
    @Volatile var active = false
    @Volatile var motion = MotionSnapshot()
    @Volatile var buttons: Int = 0
    @Volatile var lx: Int = 128
    @Volatile var ly: Int = 128
    @Volatile var rx: Int = 128
    @Volatile var ry: Int = 128
    @Volatile var touch: Boolean = false
    @Volatile var touchX: Int = 0
    @Volatile var touchY: Int = 0
    @Volatile var ps: Boolean = false
    @Volatile var mic: Boolean = false
    @Volatile var screen: Boolean = false
    // 0 leaves the emulator option alone. 1 turns it off, 2 turns it on.
    @Volatile var sensorBarCmd: Int = 0
    // Which screen edge gravity pulls toward: 0 unknown, 1 right, 2 left, 3 top, 4 bottom, 5 screen, 6 back.
}

object PadBits {
    const val SHARE = 1 shl 0
    const val L3 = 1 shl 1
    const val R3 = 1 shl 2
    const val OPTIONS = 1 shl 3
    const val UP = 1 shl 4
    const val RIGHT = 1 shl 5
    const val DOWN = 1 shl 6
    const val LEFT = 1 shl 7
    const val L2 = 1 shl 8
    const val R2 = 1 shl 9
    const val L1 = 1 shl 10
    const val R1 = 1 shl 11
    const val TRIANGLE = 1 shl 12
    const val CIRCLE = 1 shl 13
    const val CROSS = 1 shl 14
    const val SQUARE = 1 shl 15
}

class DsuServer(private val sample: PadSample) {
    @Volatile var running = false
    private var socket: DatagramSocket? = null
    private var packetIndex = 0

    fun start() {
        if (running) return
        running = true
        socket = DatagramSocket(26760)
        thread(name = "dsu") {
            val buffer = ByteArray(256)
            while (running) {
                try {
                    val packet = DatagramPacket(buffer, buffer.size)
                    socket?.receive(packet) ?: break
                    val reply = reply(packet.data, packet.length) ?: continue
                    socket?.send(DatagramPacket(reply, reply.size, packet.address, packet.port))
                } catch (_: Exception) {
                    if (!running) break
                }
            }
        }
    }

    fun stop() {
        running = false
        socket?.close()
        socket = null
    }

    private fun reply(data: ByteArray, length: Int): ByteArray? {
        if (length < 20) return null
        val inBuf = ByteBuffer.wrap(data, 0, length).order(ByteOrder.LITTLE_ENDIAN)
        val magic = ByteArray(4)
        inBuf.get(magic)
        if (magic[0] != 'D'.code.toByte()) return null
        inBuf.short
        inBuf.short
        inBuf.int
        val uid = inBuf.int
        val type = inBuf.int
        return when (type) {
            0x100000 -> versionPacket(uid)
            0x100001 -> portInfoPacket(uid, 0)
            0x100002 -> dataPacket(uid)
            else -> null
        }
    }

    private fun portInfoPacket(uid: Int, index: Int): ByteArray {
        val packet = ByteArray(32)
        val buf = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(byteArrayOf('D'.code.toByte(), 'S'.code.toByte(), 'U'.code.toByte(), 'S'.code.toByte()))
        buf.putShort(1001)
        buf.putShort(16)
        buf.putInt(0)
        buf.putInt(uid)
        buf.putInt(0x100001)
        buf.put(index.toByte())
        buf.put(if (sample.active) 2 else 0)
        buf.put(2)
        buf.put(2)
        buf.put(byteArrayOf(0x02, 0x67, 0x60, 0x00, 0x00, 0x01))
        buf.put(5)
        buf.put(if (sample.active) 1 else 0)
        stampCrc(packet, packet.size)
        return packet
    }

    private fun dataPacket(uid: Int): ByteArray {
        val live = if (sample.active) sample else PadSample()
        val motion = live.motion // one immutable sensor observation per datagram
        val body = 100
        val packet = ByteArray(body)
        val buf = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(byteArrayOf('D'.code.toByte(), 'S'.code.toByte(), 'U'.code.toByte(), 'S'.code.toByte()))
        buf.putShort(1001)
        buf.putShort((body - 16).toShort())
        buf.putInt(0)
        buf.putInt(uid)
        buf.putInt(0x100002)
        buf.put(0)
        buf.put(if (live.active) 2 else 0)
        buf.put(2)
        buf.put(2)
        buf.put(byteArrayOf(0x02, 0x67, 0x60, 0x00, 0x00, 0x01))
        buf.put(5)
        buf.put(if (live.active) 1 else 0)
        packetIndex += 1
        buf.putInt(packetIndex)
        val buttons = live.buttons
        buf.put((buttons and 0xFF).toByte())
        buf.put(((buttons shr 8) and 0xFF).toByte())
        buf.put(if (live.ps) 1 else 0)
        buf.put(0)
        buf.put(live.lx.toByte())
        buf.put(live.ly.toByte())
        buf.put(live.rx.toByte())
        buf.put(live.ry.toByte())
        buf.put(if (buttons and PadBits.LEFT != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.DOWN != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.RIGHT != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.UP != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.SQUARE != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.CROSS != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.CIRCLE != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.TRIANGLE != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.R1 != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.L1 != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.R2 != 0) 0xFF.toByte() else 0)
        buf.put(if (buttons and PadBits.L2 != 0) 0xFF.toByte() else 0)
        buf.put(if (live.touch) 1 else 0)
        buf.put(0)
        buf.putShort(if (live.touch) live.touchX.coerceIn(0, 1919).toShort() else 0)
        buf.putShort(if (live.touch) live.touchY.coerceIn(0, 941).toShort() else 0)
        buf.put(0)
        buf.put(0)
        buf.putShort(0)
        buf.putShort(0)
        buf.putLong(motion.timestampUs)
        buf.putFloat(motion.accelX)
        buf.putFloat(motion.accelY)
        buf.putFloat(motion.accelZ)
        buf.putFloat(motion.gyroX)
        buf.putFloat(motion.gyroY)
        buf.putFloat(motion.gyroZ)
        stampCrc(packet, body)
        val out = ByteArray(body + 24)
        packet.copyInto(out)
        val extra = ByteBuffer.wrap(out, body, 24).order(ByteOrder.LITTLE_ENDIAN)
        extra.putFloat(motion.magX)
        extra.putFloat(motion.magY)
        extra.putFloat(motion.magZ)
        extra.put(if (live.mic) 1 else 0)
        extra.put(if (live.screen) 1 else 0)
        extra.put(live.sensorBarCmd.toByte())
        extra.put(motion.edgeDown.toByte())
        extra.put(byteArrayOf(68, 67, 77, 50)) // DCM2: coherent sensor contract
        extra.putInt(motion.sequence)
        return out
    }

    private fun stampCrc(packet: ByteArray, size: Int) {
        val crc = CRC32()
        crc.update(packet, 0, size)
        ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN).putInt(8, crc.value.toInt())
    }

    private fun versionPacket(uid: Int): ByteArray {
        val packet = ByteArray(24)
        val buf = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN)
        buf.put(byteArrayOf('D'.code.toByte(), 'S'.code.toByte(), 'U'.code.toByte(), 'S'.code.toByte()))
        buf.putShort(1001)
        buf.putShort(8)
        buf.putInt(0)
        buf.putInt(uid)
        buf.putInt(0x100000)
        buf.putShort(1001)
        buf.putShort(0)
        stampCrc(packet, packet.size)
        return packet
    }
}
