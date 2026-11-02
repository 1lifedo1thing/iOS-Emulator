# Candidate15 orientation transform source audit

This audit covers product source `04effe952ef84bf1fa6b9143902909fc9a337c6a`
and shared native source `9425e6a83450cda204a0c9c91bf2d0dd8a6acef1`.
Candidate15 remains immutable. These are source formulas, not an executed
orientation, wire-consumption or visual guest acceptance result.

The selected source does not show a display-rotation mismatch in
`GuestTestViewport`. Adding a 90-degree rotation to this helper would disagree
with the production pixels and input mapper.

## Selected paths and equations

`app/src/main/cpp/lighttouch_jni.cpp::viewport` computes, for raw guest dimensions
`Fw,Fh` and destination dimensions `Dw,Dh`:

```
s = min(Dw/Fw, Dh/Fh)
imageWidth = Fw*s; imageHeight = Fh*s
left = (Dw-imageWidth)/2; top = (Dh-imageHeight)/2
```

The native presenter uses the locked `ANativeWindow_Buffer` dimensions for the
destination. Its source sampling is `sx=floor((px-left)*Fw/imageWidth)` and
`sy=floor((py-top)*Fh/imageHeight)`. It swaps BGRA channels to RGBA but does not
swap axes or rotate pixels. Surface attachment uses natural buffer dimensions
with `ANativeWindow_setBuffersGeometry(window,0,0,RGBA_8888)`; no native buffer
rotation call appears in this source.

`GuestDisplay.onTouchEvent` passes View-local MotionEvent pixels and View
dimensions, plus `display.rotation*90`, through Binder.
`lighttouch_jni.cpp::Native_touch` calculates the same aspect-fit viewport with
those View dimensions, then explicitly calls `lt_map_contact(...,rotation=0)`
and ignores the supplied physical rotation. The shared
`Portable/src/Runtime.cpp::lt_map_contact` case0 returns
`nx=(px-left)/imageWidth, ny=(py-top)/imageHeight`. Its case90 would instead
return `(ny,1-nx)`; that branch is not selected by Android touch.

`GuestTestViewport.point` is the inverse of this selected case0:
`px=left+guestX*imageWidth, py=top+guestY*imageHeight`. It uses measured raw
guest frame dimensions and current View dimensions. Raw framebuffer axes stay
upright inside the requested landscape layout. Event receipts already record
physical display rotation; viewport receipts also identify native mapping
rotation0. Sensor rotation is a separate, deferred contract.

## Exact numeric example

Take a raw guest frame `320x480`, a landscape View and buffer `1200x600`, and
physical display metadata rotation90. The selected equations give:

```
s = min(1200/320, 600/480) = 1.25
image = 400x600; left=400; top=0
guest normalized point = (0.25, 0.60)
helper injected pixel = (400+0.25*400, 0+0.60*600) = (500,360)
presenter source pixel = (80,288)
native case0 inverse = ((500-400)/400, 360/600) = (0.25,0.60)
```

The physical rotation argument does not change either selected formula. Calling
the shared case90 would produce `(0.60,0.75)`, which disagrees with the sampled
guest location. Swapping the helper's raw frame dimensions for rotation90 would
produce a `900x600` image starting at `left=150`; the same point would be
`(375,360)`, outside the actual native image's left edge400. Native would cancel
that batch rather than deliver the intended contact.

## Practical limits

This proof uses View and buffer dimensions with the same aspect ratio and
natural coordinate correspondence. Actual orientation acceptance must retain
View sizes, native buffer sizes/stride, surface generation, frame dimensions,
rotation metadata, wire records and screenshots. If an OS layout or Surface
composition changes that correspondence, its measured geometry needs separate
analysis; the formulas alone do not establish a target result. Frame dimensions
are sampled before dispatch and are not an atomic cross-process snapshot of a
future frame. No production mapper, presenter or stimulus point calculation was
changed by this audit.
