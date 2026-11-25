/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.Manifest;
import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;

/** Observed Android link status for the computer chosen in the companion. */
final class BluetoothLinkStatus {
    @SuppressLint("StaticFieldLeak") // Holds only the application context.
    private static BluetoothLinkStatus instance;
    private final Context app;
    private final SharedPreferences preferences;
    private final BluetoothManager manager;
    private String address;
    private Boolean le, classic, unspecified;

    static BluetoothLinkStatus get(Context context) {
        if (instance == null) instance = new BluetoothLinkStatus(context.getApplicationContext());
        return instance;
    }

    private BluetoothLinkStatus(Context app) {
        this.app = app;
        manager = app.getSystemService(BluetoothManager.class);
        preferences = app.getSharedPreferences("bluetooth_connection", Context.MODE_PRIVATE);
        String saved = preferences.getString("computer_address", "");
        address = BluetoothAdapter.checkBluetoothAddress(saved) ? saved : "";
        IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_ACL_CONNECTED);
        filter.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED);
        filter.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                observe(intent);
            }
        };
        // ACL and adapter changes are protected broadcasts sent by the Bluetooth process.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            app.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        else app.registerReceiver(receiver, filter);
    }

    String address() {
        return address;
    }

    void select(String address) {
        if (!BluetoothAdapter.checkBluetoothAddress(address) || address.equals(this.address)) return;
        this.address = address;
        le = classic = unspecified = null;
        preferences.edit().putString("computer_address", address).apply();
    }

    @SuppressWarnings("deprecation")
    private void observe(Intent intent) {
        String action = intent.getAction();
        if (BluetoothAdapter.ACTION_STATE_CHANGED.equals(action)) {
            le = classic = unspecified = null;
            return;
        }
        if (!BluetoothDevice.ACTION_ACL_CONNECTED.equals(action) &&
                !BluetoothDevice.ACTION_ACL_DISCONNECTED.equals(action)) return;
        BluetoothDevice device = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                ? intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class)
                : intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
        if (device == null || !address.equals(device.getAddress())) return;
        boolean connected = BluetoothDevice.ACTION_ACL_CONNECTED.equals(action);
        int transport = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                ? intent.getIntExtra(BluetoothDevice.EXTRA_TRANSPORT, BluetoothDevice.TRANSPORT_AUTO)
                : BluetoothDevice.TRANSPORT_AUTO;
        unspecified = null;
        if (transport == BluetoothDevice.TRANSPORT_LE) le = connected;
        else if (transport == BluetoothDevice.TRANSPORT_BREDR) classic = connected;
        else {
            le = classic = null;
            unspecified = connected;
        }
    }

    @SuppressLint("MissingPermission")
    String display() {
        if (address.isEmpty()) return "";
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                app.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED))
            return app.getString(R.string.ble_local_unknown, address);
        try {
            if (!adapter.isEnabled()) return app.getString(R.string.ble_local_disabled, address);
            boolean gatt = false;
            for (BluetoothDevice device : manager.getConnectedDevices(BluetoothProfile.GATT)) {
                if (address.equals(device.getAddress())) {
                    gatt = true;
                    break;
                }
            }
            if (gatt || Boolean.TRUE.equals(le)) {
                return app.getString(Boolean.TRUE.equals(classic) ? R.string.ble_local_dual
                        : R.string.ble_local_le, address);
            }
            if (Boolean.TRUE.equals(classic)) return app.getString(R.string.ble_local_classic, address);
            if (Boolean.TRUE.equals(unspecified)) return app.getString(R.string.ble_local_connected, address);
            if (Boolean.FALSE.equals(unspecified) || (Boolean.FALSE.equals(le) && Boolean.FALSE.equals(classic)))
                return app.getString(R.string.ble_local_disconnected, address);
            if (Boolean.FALSE.equals(le)) return app.getString(R.string.ble_local_le_disconnected, address);
        } catch (SecurityException | IllegalStateException ignored) {
            // Permission or adapter state can change while the screen is visible.
        }
        return app.getString(R.string.ble_local_unknown, address);
    }
}
