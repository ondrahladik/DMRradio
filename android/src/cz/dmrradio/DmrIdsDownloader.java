package cz.dmrradio;

import android.util.Log;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

public class DmrIdsDownloader {
    private static final String TAG = "DmrIdsDownloader";

    public static boolean download(String urlString, String savePath) {
        HttpURLConnection conn = null;
        FileOutputStream out = null;
        try {
            File target = new File(savePath);
            File parent = target.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs() && !parent.exists()) {
                Log.e(TAG, "Cannot create parent directory: " + parent);
                return false;
            }

            URL url = new URL(urlString);
            conn = (HttpURLConnection) url.openConnection();
            conn.setInstanceFollowRedirects(true);
            conn.setConnectTimeout(15000);
            conn.setReadTimeout(30000);
            conn.connect();

            int code = conn.getResponseCode();
            if (code < HttpURLConnection.HTTP_OK || code >= HttpURLConnection.HTTP_MULT_CHOICE) {
                Log.e(TAG, "HTTP " + code + " " + conn.getResponseMessage());
                return false;
            }

            InputStream in = new BufferedInputStream(conn.getInputStream());
            out = new FileOutputStream(target, false);
            byte[] buf = new byte[8192];
            int read;
            while ((read = in.read(buf)) != -1) {
                out.write(buf, 0, read);
            }
            out.flush();
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Download failed", e);
            return false;
        } finally {
            if (out != null) {
                try { out.close(); } catch (Exception ignored) {}
            }
            if (conn != null) {
                conn.disconnect();
            }
        }
    }
}
