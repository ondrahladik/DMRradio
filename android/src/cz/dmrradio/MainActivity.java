package cz.dmrradio;

import android.content.pm.PackageManager;
import org.qtproject.qt.android.bindings.QtActivity;

/**
 * Custom activity that restarts the background service after POST_NOTIFICATIONS
 * is granted, so the foreground notification appears on first launch.
 */
public class MainActivity extends QtActivity {

    @Override
    public void onRequestPermissionsResult(int requestCode,
                                           String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);

        for (int i = 0; i < permissions.length; i++) {
            if ("android.permission.POST_NOTIFICATIONS".equals(permissions[i])
                    && grantResults[i] == PackageManager.PERMISSION_GRANTED) {
                // Permission just granted — restart service so it re-posts the notification.
                BackgroundService.start(this);
                break;
            }
        }
    }
}
