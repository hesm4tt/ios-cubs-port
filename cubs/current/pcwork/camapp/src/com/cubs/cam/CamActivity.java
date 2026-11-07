package com.cubs.cam;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** Starts the foreground service CamService with the same extras (id, w, h) and finishes at once. */
public class CamActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent i = new Intent(this, CamService.class);
        i.putExtra("id", getIntent().getStringExtra("id"));
        i.putExtra("w", getIntent().getIntExtra("w", 640));
        i.putExtra("h", getIntent().getIntExtra("h", 480));
        startForegroundService(i);
        finish();
    }
}
