package com.cubs.cam;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.IBinder;
import android.util.Log;
import java.io.InputStream;
import java.net.Socket;

/** Plays the sound of iOS: raw 48 kHz 16-bit stereo PCM from sndsrv (127.0.0.1:7778) through an AudioTrack. */
public class SndService extends Service {
    static final String T = "CubsSnd";
    Thread th;

    @Override
    public IBinder onBind(Intent i) { return null; }

    @Override
    public int onStartCommand(Intent in, int flags, int startId) {
        if (th != null) return START_STICKY;
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel("snd", "snd", NotificationManager.IMPORTANCE_LOW));
        Notification n = new Notification.Builder(this, "snd").setContentTitle("iOS sound").setSmallIcon(android.R.drawable.ic_media_play).build();
        startForeground(8, n);
        th = new Thread(new Runnable() { public void run() { loop(); } }, "snd");
        th.start();
        return START_STICKY;
    }

    void loop() {
        int min = AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack tr = new AudioTrack.Builder()
            .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(new AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT).setSampleRate(48000).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
            .setBufferSizeInBytes(Math.max(min * 4, 32768)).setTransferMode(AudioTrack.MODE_STREAM).build();
        tr.play();
        Log.i(T, "AudioTrack state " + tr.getState() + ", min buffer " + min);
        byte[] buf = new byte[8192];
        long total = 0, last = 0;
        for (;;) {
            try {
                Socket s = new Socket("127.0.0.1", 7778);
                InputStream is = s.getInputStream();
                Log.i(T, "connected to sndsrv");
                int r;
                while ((r = is.read(buf)) > 0) {
                    tr.write(buf, 0, r);
                    total += r / 4;
                    if (total - last >= 48000) {
                        last = total;
                        long peak = 0;
                        for (int i = 0; i + 1 < r; i += 2) {
                            int v = (short) ((buf[i] & 0xff) | (buf[i + 1] << 8));
                            if (Math.abs(v) > peak) peak = Math.abs(v);
                        }
                        Log.i(T, "played " + total + " frames, last peak " + peak);
                    }
                }
                s.close();
            } catch (Exception e) {
                try { Thread.sleep(1000); } catch (Exception x) { }
            }
        }
    }
}
