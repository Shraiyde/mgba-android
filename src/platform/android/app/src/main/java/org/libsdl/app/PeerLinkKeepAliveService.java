package org.libsdl.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/** Keeps the active co-op network session scheduled while the game Activity is backgrounded. */
public final class PeerLinkKeepAliveService extends Service {
    private static final String CHANNEL_ID = "hns_peer_session";
    private static final int NOTIFICATION_ID = 47621;

    public static void start(android.content.Context context) {
        Intent intent = new Intent(context, PeerLinkKeepAliveService.class);
        if (Build.VERSION.SDK_INT >= 26)
            context.startForegroundService(intent);
        else
            context.startService(intent);
    }

    public static void stop(android.content.Context context) {
        context.stopService(new Intent(context, PeerLinkKeepAliveService.class));
    }

    @Override public void onCreate() {
        super.onCreate();
        createChannel();
        Intent launch = new Intent(this, SDLActivity.class);
        launch.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pending = PendingIntent.getActivity(this, 0, launch, flags);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, CHANNEL_ID)
            : new Notification.Builder(this);
        Notification notification = builder
            .setSmallIcon(android.R.drawable.stat_sys_upload)
            .setContentTitle("Heart & Soul Co-op")
            .setContentText("Keeping your co-op session connected")
            .setContentIntent(pending)
            .setOngoing(true)
            .setCategory(Notification.CATEGORY_SERVICE)
            .build();
        startForeground(NOTIFICATION_ID, notification);
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(
                CHANNEL_ID, "Co-op connection", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Keeps an active Pokémon co-op session connected in the background.");
            NotificationManager manager = getSystemService(NotificationManager.class);
            if (manager != null) manager.createNotificationChannel(channel);
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
