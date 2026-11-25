/* SPDX-License-Identifier: Apache-2.0 */
package com.github.gh6497.deskflowotg;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.provider.Settings;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

public final class BluetoothDiagnosticActivity extends Activity {
    private enum Action { SCAN, TEST, CONNECT }

    private static final int REQUEST_BLUETOOTH = 1;
    private static final int MAX_RESULTS = 24;
    private static final long SCAN_MS = 12_000;
    private static final long LINK_MS = 15_000;
    private static final long BOND_MS = 45_000;
    private static final long HID_SETTLE_MS = 4_000;
    private static final ParcelUuid HID_SERVICE =
            ParcelUuid.fromString("00001812-0000-1000-8000-00805f9b34fb");

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Map<String, BluetoothDevice> devices = new HashMap<>();
    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private ScanCallback scanCallback;
    private BluetoothGatt link;
    private boolean waitingForBond;
    private boolean waitingForHid;
    private int broadcastCount;
    private Action pendingPermission;

    private EditText address;
    private Button scanButton, testButton, connectButton;
    private TextView scanStatus, linkStatus, localStatus;
    private LinearLayout results;
    private BluetoothLinkStatus bluetooth;

    private final Runnable scanDeadline = () -> finishScan(
            broadcastCount == 0 ? R.string.ble_scan_no_broadcasts
            : devices.isEmpty() ? R.string.ble_scan_no_matches : R.string.ble_scan_done);
    private final Runnable linkDeadline = () -> finishLink(
            waitingForBond ? R.string.ble_bond_timeout : R.string.ble_link_timeout);
    private final Runnable hidDeadline = () -> finishLink(R.string.ble_hid_setup);
    private final Runnable refreshLocal = new Runnable() {
        @Override public void run() {
            String local = bluetooth.display();
            localStatus.setText(local.isEmpty() ? getString(R.string.ble_local_unselected) : local);
            handler.postDelayed(this, 1_000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_bluetooth_diagnostic);
        BluetoothManager manager = getSystemService(BluetoothManager.class);
        adapter = manager == null ? null : manager.getAdapter();
        bluetooth = BluetoothLinkStatus.get(this);
        address = findViewById(R.id.ble_address);
        address.setText(bluetooth.address());
        scanButton = findViewById(R.id.ble_scan);
        scanStatus = findViewById(R.id.ble_scan_status);
        results = findViewById(R.id.ble_scan_results);
        testButton = findViewById(R.id.ble_test_link);
        connectButton = findViewById(R.id.ble_connect);
        localStatus = findViewById(R.id.ble_local_status);
        linkStatus = findViewById(R.id.ble_link_status);
        scanButton.setOnClickListener(view -> {
            if (scanCallback == null) start(Action.SCAN);
            else finishScan(R.string.ble_scan_stopped);
        });
        testButton.setOnClickListener(view -> start(Action.TEST));
        connectButton.setOnClickListener(view -> start(Action.CONNECT));
        findViewById(R.id.ble_open_settings).setOnClickListener(view ->
                startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)));
    }

    private boolean granted(String permission) {
        return checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    @SuppressLint("MissingPermission")
    private void start(Action action) {
        if (link != null || pendingPermission != null) return;
        TextView status = action == Action.SCAN ? scanStatus : linkStatus;
        if (adapter == null || !getPackageManager().hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE)) {
            status.setText(R.string.ble_unavailable);
            return;
        }
        String target = address.getText().toString().trim().toUpperCase(Locale.ROOT);
        if (action != Action.SCAN && !BluetoothAdapter.checkBluetoothAddress(target)) {
            status.setText(R.string.ble_bad_address);
            return;
        }

        ArrayList<String> missing = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (!granted(Manifest.permission.BLUETOOTH_CONNECT))
                missing.add(Manifest.permission.BLUETOOTH_CONNECT);
            if (action == Action.SCAN && !granted(Manifest.permission.BLUETOOTH_SCAN))
                missing.add(Manifest.permission.BLUETOOTH_SCAN);
        } else if (action == Action.SCAN && !granted(Manifest.permission.ACCESS_FINE_LOCATION)) {
            missing.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (!missing.isEmpty()) {
            pendingPermission = action;
            requestPermissions(missing.toArray(new String[0]), REQUEST_BLUETOOTH);
            return;
        }

        try {
            if (!adapter.isEnabled()) {
                status.setText(R.string.ble_disabled);
            } else if (action == Action.SCAN) {
                LocationManager location = getSystemService(LocationManager.class);
                if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S && location != null && !location.isLocationEnabled())
                    status.setText(R.string.ble_location_disabled);
                else startScan(BluetoothAdapter.checkBluetoothAddress(target) ? target : "");
            } else {
                startLink(action, target);
            }
        } catch (SecurityException | IllegalStateException exception) {
            status.setText(R.string.ble_access_failed);
        }
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code != REQUEST_BLUETOOTH || pendingPermission == null) return;
        Action action = pendingPermission;
        pendingPermission = null;
        boolean allowed = results.length == permissions.length && results.length > 0;
        for (int result : results) allowed &= result == PackageManager.PERMISSION_GRANTED;
        if (allowed) start(action);
        else (action == Action.SCAN ? scanStatus : linkStatus).setText(R.string.ble_permission_denied);
    }

    @SuppressLint("MissingPermission")
    private void startScan(String target) {
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            scanStatus.setText(R.string.ble_unavailable);
            return;
        }
        devices.clear();
        results.removeAllViews();
        broadcastCount = 0;
        scanCallback = new ScanCallback() {
            @Override public void onScanResult(int type, ScanResult result) {
                if (scanCallback == this) showScanResult(result, target);
            }

            @Override public void onScanFailed(int errorCode) {
                if (scanCallback != this) return;
                stopScan();
                scanStatus.setText(getString(R.string.ble_scan_error, errorCode));
            }
        };
        try {
            scanner.startScan(null, new ScanSettings.Builder()
                    .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCallback);
        } catch (SecurityException | IllegalStateException exception) {
            stopScan();
            throw exception;
        }
        scanButton.setText(R.string.ble_scan_stop);
        scanStatus.setText(R.string.ble_scanning);
        handler.postDelayed(scanDeadline, SCAN_MS);
    }

    private void showScanResult(ScanResult result, String requestedAddress) {
        ++broadcastCount;
        if (devices.size() >= MAX_RESULTS) return;
        BluetoothDevice device = result.getDevice();
        String target = device.getAddress();
        ScanRecord record = result.getScanRecord();
        String name = record == null ? null : record.getDeviceName();
        boolean hid = record != null && record.getServiceUuids() != null && record.getServiceUuids().contains(HID_SERVICE);
        if (!hid && !"Deskflow Bridge".equalsIgnoreCase(name) && !target.equals(requestedAddress)) return;
        if (devices.putIfAbsent(target, device) != null) return;
        Button row = (Button) getLayoutInflater().inflate(R.layout.item_ble_diagnostic, results, false);
        row.setText(getString(R.string.ble_scan_row,
                name == null || name.trim().isEmpty() ? getString(R.string.ble_unnamed_device) : name,
                target, result.getRssi(), getString(result.isConnectable()
                        ? R.string.ble_advert_connectable : R.string.ble_advert_nonconnectable)));
        row.setOnClickListener(view -> address.setText(target));
        results.addView(row);
    }

    @SuppressLint("MissingPermission")
    private void stopScan() {
        if (scanCallback == null) return;
        ScanCallback callback = scanCallback;
        scanCallback = null;
        handler.removeCallbacks(scanDeadline);
        try {
            scanner.stopScan(callback);
        } catch (SecurityException | IllegalStateException ignored) {
            // Bluetooth or its permission may have changed during the scan.
        }
        scanner = null;
        scanButton.setText(R.string.ble_scan);
    }

    private void finishScan(int message) {
        stopScan();
        if (message == R.string.ble_scan_no_broadcasts) {
            scanStatus.setText(message);
        } else if (message == R.string.ble_scan_no_matches) {
            scanStatus.setText(getString(message, broadcastCount));
        } else {
            scanStatus.setText(getString(message, devices.size(), broadcastCount));
        }
    }

    @SuppressLint("MissingPermission")
    private void startLink(Action action, String target) {
        if (scanCallback != null) finishScan(R.string.ble_scan_stopped);
        BluetoothDevice device = devices.get(target);
        if (device == null) {
            device = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                    ? adapter.getRemoteLeDevice(target, BluetoothDevice.ADDRESS_TYPE_PUBLIC)
                    : adapter.getRemoteDevice(target);
        }
        if (action == Action.CONNECT) bluetooth.select(target);
        link = device.connectGatt(this, false, new BluetoothGattCallback() {
            @Override public void onConnectionStateChange(BluetoothGatt gatt, int status, int state) {
                handler.post(() -> onLinkState(gatt, status, state, action));
            }
        }, BluetoothDevice.TRANSPORT_LE);
        if (link == null) {
            linkStatus.setText(R.string.ble_link_rejected);
            return;
        }
        linkStatus.setText(getString(R.string.ble_link_connecting, target));
        updateControls();
        handler.postDelayed(linkDeadline, LINK_MS);
    }

    @SuppressLint("MissingPermission")
    private void onLinkState(BluetoothGatt gatt, int status, int state, Action action) {
        if (gatt != link) return;
        if (status != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
            int message = waitingForHid ? R.string.ble_hid_link_lost
                    : waitingForBond ? R.string.ble_pair_link_lost : R.string.ble_link_failed;
            finishLink(getString(message, status));
            return;
        }
        if (state != BluetoothProfile.STATE_CONNECTED || waitingForBond || waitingForHid) return;
        if (action == Action.TEST) {
            finishLink(R.string.ble_link_ok);
            return;
        }
        try {
            BluetoothDevice device = gatt.getDevice();
            int bond = device.getBondState();
            if (bond == BluetoothDevice.BOND_BONDED) {
                allowHidSetup();
                return;
            }
            // Keep the explicit LE link open while the system picks the bond transport.
            if (bond != BluetoothDevice.BOND_BONDING && !device.createBond()) {
                finishLink(R.string.ble_bond_rejected);
                return;
            }
            waitingForBond = true;
            linkStatus.setText(R.string.ble_bond_waiting);
            handler.removeCallbacks(linkDeadline);
            handler.postDelayed(linkDeadline, BOND_MS);
            pollBond(gatt, bond == BluetoothDevice.BOND_BONDING);
        } catch (SecurityException exception) {
            finishLink(R.string.ble_access_failed);
        }
    }

    @SuppressLint("MissingPermission")
    private void pollBond(BluetoothGatt gatt, boolean sawBonding) {
        if (gatt != link) return;
        try {
            int bond = gatt.getDevice().getBondState();
            if (bond == BluetoothDevice.BOND_BONDED) {
                allowHidSetup();
            } else if (bond == BluetoothDevice.BOND_NONE && sawBonding) {
                finishLink(R.string.ble_bond_failed);
            } else {
                handler.postDelayed(() -> pollBond(gatt, sawBonding || bond == BluetoothDevice.BOND_BONDING), 250);
            }
        } catch (SecurityException exception) {
            finishLink(R.string.ble_access_failed);
        }
    }

    private void allowHidSetup() {
        waitingForBond = false;
        waitingForHid = true;
        handler.removeCallbacks(linkDeadline);
        linkStatus.setText(R.string.ble_hid_waiting);
        // Give Android's HID host time to open its channel on the LE link.
        handler.postDelayed(hidDeadline, HID_SETTLE_MS);
    }

    private void updateControls() {
        boolean idle = link == null;
        scanButton.setEnabled(idle);
        testButton.setEnabled(idle);
        connectButton.setEnabled(idle);
        address.setEnabled(idle);
    }

    private void finishLink(int message) {
        finishLink(getString(message));
    }

    private void finishLink(String message) {
        closeLink();
        linkStatus.setText(message);
        updateControls();
    }

    @SuppressLint("MissingPermission")
    private void closeLink() {
        BluetoothGatt current = link;
        link = null;
        waitingForBond = false;
        waitingForHid = false;
        handler.removeCallbacks(linkDeadline);
        handler.removeCallbacks(hidDeadline);
        if (current == null) return;
        try {
            current.close();
        } catch (SecurityException ignored) {
            // Losing permission may also invalidate the GATT client.
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        refreshLocal.run();
    }

    @Override
    protected void onStop() {
        handler.removeCallbacks(refreshLocal);
        if (scanCallback != null) finishScan(R.string.ble_scan_stopped);
        if (link != null && !waitingForBond && !waitingForHid) finishLink(R.string.ble_link_stopped);
        // Android may take the screen while asking the user to approve pairing.
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        stopScan();
        closeLink();
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
