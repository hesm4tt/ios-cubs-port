package com.cubs.probe;

import android.net.wifi.IActionListener;
import android.net.wifi.IWifiManager;
import android.os.IBinder;
import android.os.ServiceManager;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Invalid network id only: does not supply or create a network configuration. */
public final class WifiBinderInvalidConnectProbe {
    public static void main(String[] args) {
        try {
            IBinder binder = ServiceManager.checkService("wifi");
            if (binder == null) { System.out.println("wifi-binder-unavailable"); return; }
            CountDownLatch done = new CountDownLatch(1);
            IActionListener callback = new IActionListener.Stub() {
                @Override public void onSuccess() { System.out.println("unexpected-success"); done.countDown(); }
                @Override public void onFailure(int reason) { System.out.println("rejected-invalid-network reason=" + reason); done.countDown(); }
            };
            IWifiManager.Stub.asInterface(binder).connect(null, -1, callback, "com.android.shell", null);
            if (!done.await(5, TimeUnit.SECONDS)) System.out.println("no-callback");
        } catch (Throwable error) {
            System.out.println("invalid-connect-exception " + error.getClass().getName());
        }
    }
}
