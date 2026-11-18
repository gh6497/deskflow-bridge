#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the real receiver/settings on a tiny host Android API fake, no phone/SDK.

This tests dispatch and persistent consumption, not Android permission enforcement,
notification presentation or notification-listener binding.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


STUBS = {
    "android/content/BroadcastReceiver.java": """
package android.content;
public abstract class BroadcastReceiver {
    public abstract void onReceive(Context context, Intent intent);
}
""",
    "android/content/Context.java": """
package android.content;
import android.app.NotificationManager;
import java.util.HashMap;
import java.util.Map;
public class Context {
    public static final int MODE_PRIVATE = 0;
    public final NotificationManager notifications = new NotificationManager();
    private final Map<String, SharedPreferences> stores = new HashMap<>();
    public SharedPreferences getSharedPreferences(String name, int mode) {
        return stores.computeIfAbsent(name, key -> new SharedPreferences());
    }
    public Object getContentResolver() { return this; }
    public <T> T getSystemService(Class<T> type) { return type.cast(notifications); }
    public String getString(int id) { return Integer.toString(id); }
}
""",
    "android/content/SharedPreferences.java": """
package android.content;
import java.util.HashMap;
import java.util.Map;
public class SharedPreferences {
    private final Map<String, Object> values = new HashMap<>();
    public boolean writable = true;
    public String getString(String k, String v) { return (String) values.getOrDefault(k, v); }
    public long getLong(String k, long v) { return (Long) values.getOrDefault(k, v); }
    public int getInt(String k, int v) { return (Integer) values.getOrDefault(k, v); }
    public boolean getBoolean(String k, boolean v) { return (Boolean) values.getOrDefault(k, v); }
    public Editor edit() { return new Editor(); }
    public final class Editor {
        private final Map<String, Object> staged = new HashMap<>();
        public Editor putString(String k, String v) { staged.put(k, v); return this; }
        public Editor putLong(String k, long v) { staged.put(k, v); return this; }
        public Editor putInt(String k, int v) { staged.put(k, v); return this; }
        public Editor putBoolean(String k, boolean v) { staged.put(k, v); return this; }
        public void apply() { values.putAll(staged); }
        public boolean commit() { if (!writable) return false; apply(); return true; }
    }
}
""",
    "android/content/Intent.java": """
package android.content;
import java.util.HashMap;
import java.util.Map;
public class Intent {
    public static final int FLAG_ACTIVITY_NEW_TASK = 1, FLAG_ACTIVITY_CLEAR_TOP = 2,
            FLAG_ACTIVITY_SINGLE_TOP = 4;
    public final Class<?> activity;
    private final String action;
    private final Map<String, Object> extras = new HashMap<>();
    public Intent() { this("com.github.gh6497.deskflowotg.CONNECTION_STATUS"); }
    public Intent(String action) { this.action = action; activity = null; }
    public Intent(Context context, Class<?> activity) { this.activity = activity; action = null; }
    public Intent addFlags(int flags) { return this; }
    public String getAction() { return action; }
    public boolean hasExtra(String key) { return extras.containsKey(key); }
    public Intent put(String k, Object v) { extras.put(k, v); return this; }
    public String getStringExtra(String k) { return (String) extras.get(k); }
    public int getIntExtra(String k, int v) { return (Integer) extras.getOrDefault(k, v); }
    public boolean getBooleanExtra(String k, boolean v) { return (Boolean) extras.getOrDefault(k, v); }
}
""",
    "android/hardware/usb/UsbManager.java": """
package android.hardware.usb;
public final class UsbManager {
    public static final String ACTION_USB_ACCESSORY_DETACHED = "android.hardware.usb.action.USB_ACCESSORY_DETACHED";
}
""",
    "android/util/Log.java": """
package android.util;
public final class Log {
    public static int i(String tag, String message) { return 0; }
}
""",
    "android/app/Notification.java": """
package android.app;
import android.content.Context;
public final class Notification {
    public boolean autoCancel;
    public PendingIntent contentIntent;
    public CharSequence title;
    public static final class Builder {
        private final Notification notification = new Notification();
        public Builder(Context context, String channel) {}
        public Builder setSmallIcon(int icon) { return this; }
        public Builder setContentTitle(CharSequence title) { notification.title = title; return this; }
        public Builder setContentText(CharSequence text) { return this; }
        public Builder setContentIntent(PendingIntent intent) { notification.contentIntent = intent; return this; }
        public Builder setAutoCancel(boolean enabled) { notification.autoCancel = enabled; return this; }
        public Notification build() { return notification; }
    }
}
""",
    "android/app/NotificationChannel.java": """
package android.app;
public final class NotificationChannel {
    public int importance;
    public NotificationChannel(String id, CharSequence name, int importance) { this.importance = importance; }
    public int getImportance() { return importance; }
}
""",
    "android/app/NotificationManager.java": """
package android.app;
public final class NotificationManager {
    public static final int IMPORTANCE_DEFAULT = 3, IMPORTANCE_NONE = 0;
    public boolean enabled = true, denyOnNotify;
    public int posts, lastId;
    public Notification lastNotification;
    private NotificationChannel channel;
    public boolean areNotificationsEnabled() { return enabled; }
    public void createNotificationChannel(NotificationChannel value) { if (channel == null) channel = value; }
    public NotificationChannel getNotificationChannel(String id) { return channel; }
    public void notify(int id, Notification notification) {
        if (denyOnNotify) throw new SecurityException();
        ++posts;
        lastId = id;
        lastNotification = notification;
    }
}
""",
    "android/app/PendingIntent.java": """
package android.app;
import android.content.Context;
import android.content.Intent;
public final class PendingIntent {
    public static final int FLAG_IMMUTABLE = 1;
    public final Intent intent;
    private PendingIntent(Intent intent) { this.intent = intent; }
    public static PendingIntent getActivity(Context context, int code, Intent intent, int flags) {
        if (flags != FLAG_IMMUTABLE) throw new AssertionError("must be immutable");
        return new PendingIntent(intent);
    }
}
""",
    "com/github/gh6497/deskflowotg/MainActivity.java": """
package com.github.gh6497.deskflowotg;
public final class MainActivity {}
""",
    "com/github/gh6497/deskflowotg/R.java": """
package com.github.gh6497.deskflowotg;
final class R {
    static final class drawable {
        static final int ic_connection_notification = 1, ic_disconnection_notification = 5;
    }
    static final class string {
        static final int connection_notification_channel = 2, connection_connected = 3,
                connection_notification_text = 4, connection_disconnected = 6, disconnection_notification_text = 7;
    }
}
""",
    "android/os/SystemClock.java": """
package android.os;
public final class SystemClock {
    public static long now = 1000;
    public static long elapsedRealtime() { return now; }
}
""",
    "android/provider/Settings.java": """
package android.provider;
public final class Settings {
    public static final class Global {
        public static final String BOOT_COUNT = "boot_count";
        public static int boot = 1;
        public static int getInt(Object resolver, String key, int fallback) { return boot; }
    }
}
""",
    "com/github/gh6497/deskflowotg/DeskflowNotificationListenerService.java": """
package com.github.gh6497.deskflowotg;
import android.content.Context;
final class DeskflowNotificationListenerService {
    static boolean bound = true;
    static int dispatched, played, pauseRequests;
    static void pauseAllPlayback(Context context) { ++pauseRequests; }
    static void playOnConnection(Context context) {
        ++dispatched;
        String history = AutoplaySettings.prefs(context).getString("consumed_ids", "");
        if (history.isEmpty()) throw new AssertionError("must persist before media side effect");
        if (bound && AutoplaySettings.enabled(context) && !AutoplaySettings.target(context).isEmpty())
            ++played;
    }
}
""",
    "com/github/gh6497/deskflowotg/ReceiverHostTest.java": """
package com.github.gh6497.deskflowotg;
import android.app.NotificationManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.hardware.usb.UsbManager;
import android.os.SystemClock;
import android.provider.Settings;
public final class ReceiverHostTest {
    private static String id(int n) { return String.format("%032x", n); }
    private static Intent event(int n, boolean edge, boolean ready, String state) {
        return new Intent().put("version", 2).put("connection_id", id(n))
                .put("state", state).put("play_event", edge).put("playback_ready", ready);
    }
    private static void check(boolean result) { if (!result) throw new AssertionError(); }
    private static void testNotifications() {
        Context context = new Context();
        ConnectionStatusReceiver receiver = new ConnectionStatusReceiver();
        NotificationManager manager = context.notifications;
        check(!ConnectionNotifications.enabled(context));
        receiver.onReceive(context, event(1, false, false, "connected"));
        ConnectionNotifications.setEnabled(context, true);
        receiver.onReceive(context, event(1, true, true, "connected")); // enabling mid-session does not alert
        receiver.onReceive(context, event(2, false, false, "connecting"));
        receiver.onReceive(context, event(2, false, false, "disconnected"));
        receiver.onReceive(context, event(2, false, false, "connected").put("version", 1));
        receiver.onReceive(context, event(2, false, false, "connected").put("connection_id", "none"));
        check(manager.posts == 0);
        receiver.onReceive(context, event(2, false, false, "connected")); // does not wait for audio/play
        check(manager.posts == 1 && manager.lastNotification.autoCancel);
        check(manager.lastNotification.contentIntent.intent.activity == MainActivity.class);
        int notificationId = manager.lastId;
        receiver = new ConnectionStatusReceiver();
        receiver.onReceive(context, event(2, true, true, "connected")); // playback event isn't a new connection
        SystemClock.now += ConnectionStatus.LEASE_MS;
        receiver.onReceive(context, event(2, false, true, "connected")); // lease expiry doesn't reset dedup
        check(manager.posts == 1);
        receiver.onReceive(context, event(3, false, false, "connected"));
        check(manager.posts == 2 && manager.lastId == notificationId); // replace, don't stack
        manager.enabled = false;
        receiver.onReceive(context, event(4, false, false, "connected"));
        manager.enabled = true;
        receiver.onReceive(context, event(4, false, false, "connected"));
        manager.getNotificationChannel(ConnectionNotifications.CHANNEL_ID).importance = NotificationManager.IMPORTANCE_NONE;
        receiver.onReceive(context, event(5, false, false, "connected"));
        manager.getNotificationChannel(ConnectionNotifications.CHANNEL_ID).importance = NotificationManager.IMPORTANCE_DEFAULT;
        receiver.onReceive(context, event(5, false, false, "connected"));
        check(manager.posts == 2); // granting permission or unblocking a channel doesn't replay
        manager.denyOnNotify = true;
        int dispatched = DeskflowNotificationListenerService.dispatched;
        receiver.onReceive(context, event(6, true, true, "connected"));
        check(manager.posts == 2 && DeskflowNotificationListenerService.dispatched == dispatched + 1);
        manager.denyOnNotify = false;
        receiver.onReceive(context, event(6, false, false, "connected"));
        check(manager.posts == 2);
        System.out.println("PASS: opt-in connection notifications, permissions, reconnect and heartbeat dedup");
    }
    private static void testDisconnectionNotifications() {
        Context context = new Context();
        ConnectionStatusReceiver receiver = new ConnectionStatusReceiver();
        NotificationManager manager = context.notifications;
        ConnectionNotifications.setEnabled(context, true);
        receiver.onReceive(context, event(1, false, false, "connecting"));
        receiver.onReceive(context, event(1, false, false, "disconnected"));
        check(manager.posts == 0); // failed handshake
        receiver.onReceive(context, event(2, false, false, "connected"));
        check(manager.posts == 1);
        int notificationId = manager.lastId;
        receiver.onReceive(context, event(2, false, false, "disconnecting"));
        receiver.onReceive(context, event(2, false, false, "disconnected").put("version", 1));
        receiver.onReceive(context, event(3, false, false, "disconnected"));
        check(manager.posts == 1); // not final, old protocol, or not a known session
        receiver.onReceive(context, event(2, false, false, "disconnected"));
        check(manager.posts == 2 && manager.lastId == notificationId);
        check(manager.lastNotification.title.equals(context.getString(R.string.connection_disconnected)));
        receiver = new ConnectionStatusReceiver();
        receiver.onReceive(context, event(2, false, false, "disconnected"));
        receiver.onReceive(context, event(2, false, false, "connected"));
        check(manager.posts == 2); // repeated end or delayed heartbeat
        receiver.onReceive(context, event(3, false, false, "connected"));
        receiver.onReceive(context, event(3, false, false, "connecting"));
        check(manager.posts == 3); // connecting alone does not end a session
        Intent reconnect = event(3, false, false, "connecting").put("connection_ended", true);
        receiver.onReceive(context, reconnect);
        receiver.onReceive(context, reconnect);
        receiver.onReceive(context, event(3, false, false, "disconnected"));
        check(manager.posts == 4 && "disconnected".equals(ConnectionStatusReceiver.current(context)));
        receiver.onReceive(context, event(4, false, false, "connected"));
        manager.enabled = false;
        receiver.onReceive(context, event(4, false, false, "disconnected"));
        manager.enabled = true;
        receiver.onReceive(context, event(4, false, false, "disconnected"));
        check(manager.posts == 5); // a missed permission must not replay a disconnect
        ConnectionNotifications.setEnabled(context, false);
        receiver.onReceive(context, event(5, false, false, "connected"));
        receiver.onReceive(context, event(5, false, false, "disconnected"));
        ConnectionNotifications.setEnabled(context, true);
        receiver.onReceive(context, event(5, false, false, "disconnected"));
        check(manager.posts == 5); // switch off also consumes the event
        System.out.println("PASS: disconnection notifications, failed attempts, reconnect and dedup");
    }
    private static Intent usbState(boolean connected) {
        return new Intent("android.hardware.usb.action.USB_STATE").put("connected", connected);
    }
    private static Intent wired(int id, String state) {
        return event(id, false, false, state).put("usb_required", true);
    }
    private static void testUsbRemoval() {
        Context context = new Context();
        UsbDisconnectReceiver usb = new UsbDisconnectReceiver();
        ConnectionStatusReceiver desktop = new ConnectionStatusReceiver();
        NotificationManager manager = context.notifications;
        ConnectionNotifications.setEnabled(context, true);
        int paused = DeskflowNotificationListenerService.pauseRequests;
        usb.onReceive(context, usbState(false)); // sticky baseline is not a removal
        usb.onReceive(context, usbState(true));
        desktop.onReceive(context, wired(1, "connected"));
        usb.onReceive(context, usbState(false)); // no desktop disconnect report needed
        check(manager.posts == 2 && DeskflowNotificationListenerService.pauseRequests == paused + 1);
        check(manager.lastNotification.title.equals(context.getString(R.string.connection_disconnected)));
        check("disconnected".equals(ConnectionStatusReceiver.current(context)));
        desktop.onReceive(context, event(1, true, true, "connected").put("usb_required", true));
        check("disconnected".equals(ConnectionStatusReceiver.current(context)));
        desktop.onReceive(context, event(1, false, false, "disconnected"));
        usb.onReceive(context, usbState(false));
        check(manager.posts == 2);
        usb.onReceive(context, usbState(true));
        desktop.onReceive(context, event(2, false, false, "connected")); // BLE/wireless UHID
        usb.onReceive(context, usbState(false));
        check(manager.posts == 3 && DeskflowNotificationListenerService.pauseRequests == paused + 2);
        check("connected".equals(ConnectionStatusReceiver.current(context)));
        usb.onReceive(context, usbState(true));
        desktop.onReceive(context, wired(3, "connected"));
        desktop.onReceive(context, wired(3, "disconnecting"));
        usb.onReceive(context, new Intent(UsbManager.ACTION_USB_ACCESSORY_DETACHED));
        check(manager.posts == 5);
        usb.onReceive(context, usbState(true));
        desktop.onReceive(context, wired(4, "connected"));
        SystemClock.now += ConnectionStatus.LEASE_MS;
        usb.onReceive(context, usbState(false));
        check(manager.posts == 6 && "unknown".equals(ConnectionStatusReceiver.current(context)));
        System.out.println("PASS: device-side USB detach, shared pause path, ADB dedup and wireless isolation");
    }
    public static void main(String[] args) {
        Context context = new Context();
        ConnectionStatusReceiver receiver = new ConnectionStatusReceiver();
        SharedPreferences prefs = AutoplaySettings.prefs(context);
        check(!AutoplaySettings.enabled(context));
        prefs.edit().putString("target", "music").apply();
        receiver.onReceive(context, event(1, true, true, "connected")); // off by default
        check(DeskflowNotificationListenerService.played == 0);
        prefs.edit().putBoolean("enabled", true).apply();
        receiver.onReceive(context, event(1, true, true, "connected")); // no retroactive enable
        receiver.onReceive(context, event(1, false, true, "connected")); // heartbeat
        check(DeskflowNotificationListenerService.dispatched == 1);
        receiver.onReceive(context, event(2, true, false, "connected")); // wait for audio
        check(DeskflowNotificationListenerService.dispatched == 1);
        receiver.onReceive(context, event(2, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == 1);
        receiver = new ConnectionStatusReceiver(); // object/process state is not the dedup source
        receiver.onReceive(context, event(2, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == 1);
        DeskflowNotificationListenerService.bound = false;
        receiver.onReceive(context, event(3, true, true, "connected"));
        DeskflowNotificationListenerService.bound = true;
        receiver.onReceive(context, event(3, true, true, "connected")); // rebind cannot retry
        check(DeskflowNotificationListenerService.played == 1);
        SystemClock.now += 15000;
        check("unknown".equals(ConnectionStatusReceiver.current(context)));
        receiver.onReceive(context, event(3, false, true, "connected")); // expired lease renewal
        Settings.Global.boot++;
        receiver.onReceive(context, event(3, false, true, "connected")); // reboot renewal
        check(DeskflowNotificationListenerService.played == 1);
        receiver.onReceive(context, event(4, true, true, "disconnecting"));
        receiver.onReceive(context, event(4, true, true, "connected").put("version", 1));
        receiver.onReceive(context, event(4, true, true, "connected").put("version", 9));
        receiver.onReceive(context, event(4, true, true, "connected").put("connection_id", "bad"));
        check(DeskflowNotificationListenerService.played == 1);
        prefs.writable = false;
        receiver.onReceive(context, event(4, true, true, "connected")); // persistence failure
        check(DeskflowNotificationListenerService.played == 1);
        prefs.writable = true;
        receiver.onReceive(context, event(4, false, true, "connected")); // no retry on heartbeat
        for (int n = 5; n <= 80; ++n) receiver.onReceive(context, event(n, true, true, "connected"));
        String[] history = prefs.getString("consumed_ids", "").split(",");
        check(history.length == 64 && history[0].equals(id(17)) && history[63].equals(id(80)));
        int played = DeskflowNotificationListenerService.played;
        receiver.onReceive(context, event(80, true, true, "connected"));
        check(DeskflowNotificationListenerService.played == played);
        System.out.println("PASS: real receiver versioning, opt-in, durable edge consumption and bounded history");
        testNotifications();
        testDisconnectionNotifications();
        testUsbRemoval();
    }
}
""",
}


def main():
    javac, java = shutil.which("javac"), shutil.which("java")
    if not javac or not java:
        print("SKIP: JDK required for Android receiver host regression")
        return 77
    root = Path(__file__).resolve().parent.parent
    app = root / "android/deskflow-bridge/app/src/main"
    manifest = ET.parse(app / "AndroidManifest.xml")
    android = "{http://schemas.android.com/apk/res/android}"
    assert manifest.find(f"uses-permission[@{android}name='android.permission.POST_NOTIFICATIONS']") is not None
    receiver = manifest.find("application/receiver")
    assert receiver.get(android + "permission") == "android.permission.DUMP"
    assert receiver.get(android + "exported") == "true"
    with tempfile.TemporaryDirectory(prefix="otg-android-receiver-") as temporary:
        directory = Path(temporary)
        sources = []
        for name, content in STUBS.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            sources.append(str(path))
        package = app / "java/com/github/gh6497/deskflowotg"
        sources += [str(package / name) for name in (
            "ConnectionStatusReceiver.java", "ConnectionStatus.java", "ConnectionNotifications.java",
            "AutoplayPolicy.java", "AutoplaySettings.java", "UsbDisconnectReceiver.java", "UsbDisconnectMonitor.java")]
        subprocess.run([javac, "-encoding", "UTF-8", "-Xlint:all", "-Werror", "-d", temporary, *sources],
                       check=True, timeout=30)
        subprocess.run([java, "-cp", temporary, "com.github.gh6497.deskflowotg.ReceiverHostTest"],
                       check=True, timeout=10)
    return 0


if __name__ == "__main__":
    sys.exit(main())
