/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.SystemClock;
import android.provider.Settings;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

/** Explicit ADB shell reports; the manifest requires the sender's DUMP permission. */
public final class ConnectionStatusReceiver extends BroadcastReceiver {
    private static final String ACTION = "com.github.gh6497.deskflowotg.CONNECTION_STATUS";
    private static final String PREFS = "connection_status";

    record Report(String state, String transport) {}

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !ACTION.equals(intent.getAction())) return;
        int version = intent.getIntExtra("version", 0);
        if (version != 1 && version != 2) return;
        String state = intent.getStringExtra("state");
        if (!ConnectionStatus.valid(state)) return;
        String id = version == 2 ? intent.getStringExtra("connection_id") : null;
        boolean knownId = ConnectionStatus.validId(id);
        SharedPreferences status = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        // A queued desktop heartbeat cannot resurrect a session already ended by local USB removal.
        if (knownId && id.equals(status.getString("usb_ended_id", "")) && !"disconnected".equals(state)) return;
        status.edit()
                .putString("state", state)
                .putString("transport", version == 2 ? ConnectionStatus.transport(intent.getStringExtra("transport")) : "")
                .putLong("received", SystemClock.elapsedRealtime())
                .putInt("boot", bootCount(context))
                .putString("connection_id", knownId ? id : "")
                .putBoolean("usb_required", knownId && intent.getBooleanExtra("usb_required", false)).apply();
        if (version != 2) return; // Old reports have no connection ID; display state only.
        ConnectionNotifications.onReport(context, state, id, intent.getBooleanExtra("connection_ended", false));
        SharedPreferences playback = AutoplaySettings.prefs(context);
        // Ordered serialization keeps exactly the most recent IDs across restarts;
        // SharedPreferences string sets do not preserve eviction order.
        Set<String> consumed = new LinkedHashSet<>(Arrays.asList(
                playback.getString("consumed_ids", "").split(",")));
        consumed.remove("");
        if (!AutoplayPolicy.shouldConsume(id, state,
                intent.getBooleanExtra("playback_ready", false),
                intent.getBooleanExtra("play_event", false), consumed.contains(id))) return;
        consumed.add(id);
        while (consumed.size() > 64) consumed.remove(consumed.iterator().next());
        // Persist BEFORE touching a player. No retries on heartbeat, process or
        // listener restart, missing permission/session, disabled setting or failure.
        if (!playback.edit().putString("consumed_ids", String.join(",", consumed)).commit()) return;
        DeskflowNotificationListenerService.playOnConnection(context);
    }

    static void onUsbDisconnected(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (!prefs.getBoolean("usb_required", false)) return;
        String state = current(context);
        if (!"connected".equals(state) && !"disconnecting".equals(state)) return;
        String id = prefs.getString("connection_id", "");
        if (!ConnectionStatus.validId(id)) return;
        prefs.edit().putString("state", "disconnected").putString("usb_ended_id", id)
                .putLong("received", SystemClock.elapsedRealtime()).apply();
        ConnectionNotifications.onReport(context, "disconnected", id, false);
    }

    static String current(Context context) {
        return report(context).state();
    }

    static Report report(Context context) {
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String state = ConnectionStatus.current(prefs.getString("state", "unknown"),
                prefs.getLong("received", -1), prefs.getInt("boot", -1),
                SystemClock.elapsedRealtime(), bootCount(context));
        return new Report(state, "unknown".equals(state) ? "" : prefs.getString("transport", ""));
    }

    private static int bootCount(Context context) {
        return Settings.Global.getInt(context.getContentResolver(), Settings.Global.BOOT_COUNT, -1);
    }
}
