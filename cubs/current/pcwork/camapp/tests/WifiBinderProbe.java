package com.cubs.probe;

import android.net.wifi.IWifiManager;
import android.os.IBinder;
import android.os.ServiceManager;

/** Read-only privileged Binder probe; prints no network details. */
public final class WifiBinderProbe {
    public static void main(String[] args) {
        try {
            IBinder binder = ServiceManager.checkService("wifi");
            if (binder == null) { System.out.println("wifi-binder-unavailable"); return; }
            IWifiManager wifi = IWifiManager.Stub.asInterface(binder);
            int state = wifi.getWifiEnabledState();
            System.out.println("wifi-binder-read-ok state=" + state);
        } catch (Throwable error) {
            System.out.println("wifi-binder-read-failed " + error.getClass().getName());
        }
    }
}
