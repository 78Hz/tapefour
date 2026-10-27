package com.hz78.tapefour.pocket;

import android.app.Activity;
import android.content.Context;
import android.media.AudioManager;
import android.os.Bundle;
import android.view.KeyEvent;

/**
    TF-P1 Pocket's window on Android. The side buttons set the phone's media volume, the
    volume the cassette plays at, whatever is on screen and whether or not the tape is moving
    (the player's on-screen volume keys set the same one).
*/
public class PocketActivity extends Activity
{
    @Override
    protected void onCreate (Bundle state)
    {
        super.onCreate (state);
        setVolumeControlStream (AudioManager.STREAM_MUSIC);
    }

    @Override
    public boolean dispatchKeyEvent (KeyEvent event)
    {
        final int code = event.getKeyCode();
        if (code == KeyEvent.KEYCODE_VOLUME_UP || code == KeyEvent.KEYCODE_VOLUME_DOWN)
        {
            if (event.getAction() == KeyEvent.ACTION_DOWN)
            {
                AudioManager audio = (AudioManager) getSystemService (Context.AUDIO_SERVICE);
                if (audio != null)
                    audio.adjustStreamVolume (AudioManager.STREAM_MUSIC,
                                              code == KeyEvent.KEYCODE_VOLUME_UP ? AudioManager.ADJUST_RAISE
                                                                                 : AudioManager.ADJUST_LOWER,
                                              AudioManager.FLAG_SHOW_UI);
            }
            return true;   // handled here, down and up: nothing else sees the side buttons
        }
        return super.dispatchKeyEvent (event);
    }
}
