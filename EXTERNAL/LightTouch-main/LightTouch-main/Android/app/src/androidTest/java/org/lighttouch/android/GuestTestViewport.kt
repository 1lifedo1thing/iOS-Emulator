package org.lighttouch.android

import android.os.Bundle
import org.json.JSONObject
import kotlin.math.min

/** Test stimulus geometry for the upright, aspect-fit guest image, not a UI input mapper. */
internal class GuestTestViewport(status: Bundle, private val viewWidth: Int, private val viewHeight: Int) {
    private val observed = requireNotNull(status.getBundle("observations")) { "Guest frame observations unavailable" }
    private val frameWidth = observed.getLong("guestFrameWidth")
    private val frameHeight = observed.getLong("guestFrameHeight")
    private val scale: Double
    private val left: Double
    private val top: Double

    init {
        require(viewWidth > 0 && viewHeight > 0 && frameWidth > 0 && frameHeight > 0) { "Actual guest/View dimensions unavailable" }
        scale = min(viewWidth.toDouble() / frameWidth, viewHeight.toDouble() / frameHeight)
        left = (viewWidth - frameWidth * scale) / 2
        top = (viewHeight - frameHeight * scale) / 2
    }

    fun point(guestX: Float, guestY: Float): Pair<Float, Float> {
        require(guestX.isFinite() && guestY.isFinite() && guestX > 0f && guestX < 1f && guestY > 0f && guestY < 1f)
        return Pair((left + guestX.toDouble() * frameWidth * scale).toFloat(), (top + guestY.toDouble() * frameHeight * scale).toFloat())
    }

    fun receipt(): JSONObject = JSONObject().put("viewWidth", viewWidth).put("viewHeight", viewHeight)
        .put("nativeContactMappingRotationDegrees", 0)
        .put("axisContract", "upright raw guest framebuffer in aspect-fit view; physical display rotation is recorded separately")
        .put("guestFrameWidth", frameWidth).put("guestFrameHeight", frameHeight)
        .put("guestImageLeftPx", left).put("guestImageTopPx", top)
        .put("guestImageWidthPx", frameWidth * scale).put("guestImageHeightPx", frameHeight * scale)
}
