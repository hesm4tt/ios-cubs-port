package com.cubs.probe;

import android.os.IBinder;
import android.os.ServiceManager;
import com.android.internal.telephony.ITelephony;

/** Read-only diagnostic: no subscription identifiers, carrier names or location. */
public final class CellularBinderProbe {
    public static void main(String[] args) {
        try {
            IBinder binder = ServiceManager.checkService("phone");
            if (binder == null) { System.out.println("cellular-service-unavailable"); return; }
            boolean enabled = ITelephony.Stub.asInterface(binder).isRadioOn("com.android.shell");
            System.out.println("cellular-radio=" + (enabled ? "on" : "off"));
        } catch (Throwable ignored) {
            // Platform exception messages can include identifying data.
            System.out.println("cellular-radio-unavailable");
        }
    }
}
