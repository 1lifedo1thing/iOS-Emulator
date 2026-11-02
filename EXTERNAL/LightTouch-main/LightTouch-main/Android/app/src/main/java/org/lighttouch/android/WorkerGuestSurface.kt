package org.lighttouch.android

import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView

/** A worker incarnation owns its SurfaceView/BufferQueue as well as its lease.
 * A mutable engine closure must never hand a retired producer's Surface to the
 * next process before Compose disposes the old View. Same-worker recomposition
 * preserves the View; worker replacement constructs a new one.
 */
@Composable
internal fun WorkerGuestSurface(
    worker: IEngine?, currentWorker: () -> IEngine?, mayClaim: (GuestDisplay) -> Boolean,
    ownerTicket: () -> Long, onError: (String) -> Unit,
    onCreated: (GuestDisplay) -> Unit, onReleased: (GuestDisplay) -> Unit,
    keepScreenOn: Boolean, modifier: Modifier = Modifier
) {
    val identity = WorkerSurfaceIdentity(worker?.asBinder())
    key(identity) {
        AndroidView(factory = { context ->
            lateinit var view: GuestDisplay
            view = GuestDisplay(context,
                onError = { message -> if (identity.matches(currentWorker()?.asBinder())) onError(message) },
                isActiveOwner = { identity.matches(currentWorker()?.asBinder()) && mayClaim(view) },
                // Cleanup retains A's captured Binder/token after replacement.
                // Admission above cannot attach/touch B from this retired View.
                ownerTicket = ownerTicket) { worker }
            onCreated(view)
            view
        }, modifier = modifier, onReset = null, onRelease = { view ->
            view.retireDisplay()
            onReleased(view)
        }, update = { it.keepScreenOn = keepScreenOn })
    }
}
