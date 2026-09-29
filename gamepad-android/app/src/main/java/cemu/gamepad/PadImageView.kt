package cemu.gamepad

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

class PadImageView(context: Context, attrs: AttributeSet?) : View(context, attrs) {
    var bitmap: Bitmap? = null
        set(value) {
            val previous = field
            field = value
            if (previous != null && previous != value && !previous.isRecycled)
                previous.recycle()
            invalidate()
        }
    var onPadTouch: ((down: Boolean, x: Float, y: Float) -> Unit)? = null
    var onShowControls: (() -> Unit)? = null
    enum class IrMode { OFF, CONTRAST, POINTS }

    var irMode: IrMode = IrMode.OFF
        set(value) {
            field = value
            paint.colorFilter = if (value == IrMode.CONTRAST) irFilter else null
            paint.isFilterBitmap = value == IrMode.OFF
            invalidate()
        }
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val spotPaint = Paint().apply { color = Color.WHITE }
    private val image = RectF()
    private val irFilter = ColorMatrixColorFilter(ColorMatrix().apply {
        setSaturation(0f)
        val scale = 8f
        val translate = (1f - scale) * 128f
        postConcat(ColorMatrix(floatArrayOf(
            scale, 0f, 0f, 0f, translate,
            0f, scale, 0f, 0f, translate,
            0f, 0f, scale, 0f, translate,
            0f, 0f, 0f, 1f, 0f
        )))
    })

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.BLACK)
        val frame = bitmap
        updateImageRect()
        val viewW = width.toFloat()
        if (width <= 0 || height <= 0) return
        if (irMode == IrMode.POINTS) {
            val dpi = resources.displayMetrics.xdpi
            val radius = 6f * dpi / 25.4f
            val y = radius + 4f * dpi / 25.4f
            val xInset = radius + 10f * dpi / 25.4f
            canvas.drawCircle(xInset, y, radius, spotPaint)
            canvas.drawCircle(viewW - xInset, y, radius, spotPaint)
            return
        }
        if (frame != null && !frame.isRecycled && image.width() > 0f && image.height() > 0f)
            canvas.drawBitmap(frame, null, image, paint)
    }

    private var controlsGesture = false

    private fun updateImageRect() {
        val frame = bitmap
        val viewW = width.toFloat()
        val viewH = height.toFloat()
        if (viewW <= 0f || viewH <= 0f) { image.setEmpty(); return }
        val frameW = if (frame != null && !frame.isRecycled) frame.width.toFloat() else 854f
        val frameH = if (frame != null && !frame.isRecycled) frame.height.toFloat() else 480f
        val scale = minOf(viewW / frameW, viewH / frameH)
        val w = frameW * scale
        val h = frameH * scale
        image.set((viewW - w) / 2f, (viewH - h) / 2f, (viewW + w) / 2f, (viewH + h) / 2f)
    }

    fun cancelPadTouch() {
        onPadTouch?.invoke(false, 0f, 0f)
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val action = event.actionMasked
        if (action == MotionEvent.ACTION_DOWN) controlsGesture = false
        if (action == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount >= 2) {
            controlsGesture = true
            cancelPadTouch()
            onShowControls?.invoke()
            return true
        }
        if (controlsGesture) {
            cancelPadTouch()
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)
                controlsGesture = false
            return true
        }
        // The drawing and input paths share this calculation, including after an FS resize.
        updateImageRect()
        val down = action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL
        if (image.isEmpty || bitmap == null || !image.contains(event.x, event.y)) {
            cancelPadTouch() // Letterbox borders do not become taps on a game edge.
            return true
        }
        val x = ((event.x - image.left) / image.width()).coerceIn(0f, 1f)
        val y = ((event.y - image.top) / image.height()).coerceIn(0f, 1f)
        onPadTouch?.invoke(down, x, y)
        return true
    }
}
