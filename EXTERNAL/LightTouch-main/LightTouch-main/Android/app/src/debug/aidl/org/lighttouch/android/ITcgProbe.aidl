package org.lighttouch.android;
import android.os.Bundle;
interface ITcgProbe {
    Bundle start();
    Bundle status();
    void finish();
}
