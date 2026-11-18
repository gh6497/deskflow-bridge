/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

/** One local notification per Deskflow connection and disconnection, independent of media controls. */
final class ConnectionNotifications {
    static final String CHANNEL_ID = "deskflow_connections";
    private static final int NOTIFICATION_ID = 1;
    private static final int HISTORY_LIMIT = 64;

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences("connection_notifications", Context.MODE_PRIVATE);
    }

    static boolean enabled(Context context) {
        return prefs(context).getBoolean("enabled", false);
    }

    static void setEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean("enabled", enabled).apply();
    }

    static void createChannel(Context context) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null) return;
        manager.createNotificationChannel(new NotificationChannel(CHANNEL_ID,
                context.getString(R.string.connection_notification_channel),
                NotificationManager.IMPORTANCE_DEFAULT));
    }

    static void onReport(Context context, String state, String id, boolean connectionEnded) {
        boolean connected = "connected".equals(state);
        boolean disconnected = "disconnected".equals(state) || ("connecting".equals(state) && connectionEnded);
        if ((!connected && !disconnected) || !ConnectionStatus.validId(id)) return;
        SharedPreferences prefs = prefs(context);
        Set<String> seen = history(prefs, "seen_ids");
        if (connected) {
            if (!remember(prefs, "seen_ids", seen, id)) return;
        } else {
            // A failed or cancelled attempt must never look like the end of an established session.
            if (!seen.contains(id) || !remember(prefs, "ended_ids", history(prefs, "ended_ids"), id)) return;
        }
        // Consume transitions even if disabled or blocked so repeated reports never replay them.
        if (enabled(context)) post(context, connected);
    }

    private static Set<String> history(SharedPreferences prefs, String key) {
        Set<String> ids = new LinkedHashSet<>(Arrays.asList(prefs.getString(key, "").split(",")));
        ids.remove("");
        return ids;
    }

    private static boolean remember(SharedPreferences prefs, String key, Set<String> ids, String id) {
        if (!ids.add(id)) return false;
        while (ids.size() > HISTORY_LIMIT) ids.remove(ids.iterator().next());
        return prefs.edit().putString(key, String.join(",", ids)).commit();
    }

    private static void post(Context context, boolean connected) {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        if (manager == null || !manager.areNotificationsEnabled()) return;
        try {
            createChannel(context);
            NotificationChannel channel = manager.getNotificationChannel(CHANNEL_ID);
            if (channel == null || channel.getImportance() == NotificationManager.IMPORTANCE_NONE) return;
            Intent open = new Intent(context, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            PendingIntent pending = PendingIntent.getActivity(context, 0, open, PendingIntent.FLAG_IMMUTABLE);
            manager.notify(NOTIFICATION_ID, new Notification.Builder(context, CHANNEL_ID)
                    .setSmallIcon(connected ? R.drawable.ic_connection_notification : R.drawable.ic_disconnection_notification)
                    .setContentTitle(context.getString(connected ? R.string.connection_connected : R.string.connection_disconnected))
                    .setContentText(context.getString(connected ? R.string.connection_notification_text
                            : R.string.disconnection_notification_text))
                    .setContentIntent(pending)
                    .setAutoCancel(true)
                    .build());
        } catch (SecurityException ignored) {
            // Notification permission may be revoked after the check; media handling still runs.
        }
    }

    private ConnectionNotifications() {}
}
