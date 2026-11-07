package com.cubs.probe;

import android.net.wifi.IActionListener;
import android.net.wifi.IWifiManager;
import android.net.wifi.WifiConfiguration;
import android.os.ServiceManager;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Invalid null-SSID config only; supplies no usable network or credential. */
public final class WifiBinderInvalidConfigProbe {
    public static void main(String[] args) {
        try {
            IWifiManager wifi=IWifiManager.Stub.asInterface(ServiceManager.checkService("wifi"));
            if (wifi==null) { System.out.println("wifi-service-unavailable"); return; }
            WifiConfiguration config=new WifiConfiguration();
            config.SSID=null;
            config.setSecurityParams(WifiConfiguration.SECURITY_TYPE_OPEN);
            CountDownLatch done=new CountDownLatch(1);
            IActionListener callback=new IActionListener.Stub() {
                @Override public void onSuccess() { System.out.println("unexpected-success"); done.countDown(); }
                @Override public void onFailure(int reason) { System.out.println("invalid-config-rejected reason="+reason); done.countDown(); }
            };
            wifi.connect(config,-1,callback,"com.android.shell",null);
            if (!done.await(5,TimeUnit.SECONDS)) System.out.println("no-callback");
        } catch (Throwable e) { System.out.println("invalid-config-exception "+e.getClass().getName()); }
    }
}
