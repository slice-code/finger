package com.pjtki.finger

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.View
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.textfield.TextInputEditText
import com.pjtki.finger.ble.FingerBleManager

class MainActivity : AppCompatActivity(), FingerBleManager.Callback {

    private lateinit var ble: FingerBleManager
    private lateinit var logView: TextView
    private lateinit var statusText: TextView
    private lateinit var scanBtn: MaterialButton
    private lateinit var autoScanBtn: MaterialButton
    private lateinit var enrollBtn: MaterialButton
    private lateinit var deleteBtn: MaterialButton
    private lateinit var wifiSaveBtn: MaterialButton
    private lateinit var empIdInput: TextInputEditText
    private lateinit var empNameInput: TextInputEditText
    private lateinit var delIdInput: TextInputEditText
    private lateinit var macInput: TextInputEditText
    private lateinit var directBtn: MaterialButton
    private lateinit var wifiSsidInput: TextInputEditText
    private lateinit var wifiPassInput: TextInputEditText
    private lateinit var deviceSpinner: Spinner
    private lateinit var connectBtn: MaterialButton

    private val devices = mutableListOf<Pair<String, String>>()
    private val REQ_PERM = 100

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ble = FingerBleManager(this)
        ble.callback = this

        logView = findViewById(R.id.log)
        statusText = findViewById(R.id.status)
        scanBtn = findViewById(R.id.btn_scan)
        autoScanBtn = findViewById(R.id.btn_autoscan)
        enrollBtn = findViewById(R.id.btn_enroll)
        deleteBtn = findViewById(R.id.btn_delete)
        wifiSaveBtn = findViewById(R.id.btn_wifi_save)
        empIdInput = findViewById(R.id.emp_id)
        empNameInput = findViewById(R.id.emp_name)
        delIdInput = findViewById(R.id.del_id)
        wifiSsidInput = findViewById(R.id.wifi_ssid)
        wifiPassInput = findViewById(R.id.wifi_pass)
        macInput = findViewById(R.id.mac_addr)
        directBtn = findViewById(R.id.btn_direct)
        deviceSpinner = findViewById(R.id.device_spinner)
        connectBtn = findViewById(R.id.btn_connect)

        scanBtn.setOnClickListener { checkPerms { ble.startScan() } }
        directBtn.setOnClickListener {
            val mac = macInput.text.toString().trim()
            if (mac.isNotEmpty()) { appendLog("Direct connect: $mac"); ble.connect(mac) }
        }
        connectBtn.setOnClickListener {
            if (ble.isConnected) {
                ble.disconnect()
                onDisconnected()
            } else {
                val pos = deviceSpinner.selectedItemPosition
                if (pos >= 0 && pos < devices.size) {
                    appendLog("Connecting...")
                    ble.connect(devices[pos].second)
                }
            }
        }
        autoScanBtn.setOnClickListener {
            if (ble.isConnected) {
                val cmd = if (autoScanBtn.text == "START SCAN") "AUTOSCAN ON" else "AUTOSCAN OFF"
                ble.sendCommand(cmd)
            }
        }
        wifiSaveBtn.setOnClickListener {
            val ssid = wifiSsidInput.text.toString().trim()
            val pass = wifiPassInput.text.toString().trim()
            if (ssid.isNotEmpty() && ble.isConnected) {
                appendLog("Saving WiFi: $ssid")
                ble.saveWiFi(ssid, pass)
            }
        }
        enrollBtn.setOnClickListener {
            val id = empIdInput.text.toString().trim()
            val nm = empNameInput.text.toString().trim()
            if (id.isNotEmpty() && nm.isNotEmpty() && ble.isConnected) {
                appendLog("Enrolling: $id ($nm)")
                ble.enrollFinger(id, nm)
            }
        }
        deleteBtn.setOnClickListener {
            val sid = delIdInput.text.toString().trim()
            val id = sid.toIntOrNull() ?: return@setOnClickListener
            if (id in 1..100 && ble.isConnected) ble.deleteFinger(id)
        }

        checkPerms { }
    }

    private fun checkPerms(then: () -> Unit) {
        val needed = listOf(
            Manifest.permission.BLUETOOTH_SCAN,
            Manifest.permission.BLUETOOTH_CONNECT,
            Manifest.permission.ACCESS_FINE_LOCATION
        ).filter { ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }
        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), REQ_PERM)
        } else {
            then()
        }
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, perms, results)
        if (code == REQ_PERM && results.all { it == PackageManager.PERMISSION_GRANTED }) {
            ble.startScan()
        }
    }

    // ── BLE Callbacks ──

    override fun onScanFound(name: String, address: String) {
        if (devices.none { it.second == address }) {
            devices.add(name to address)
            val adapter = ArrayAdapter(this,
                android.R.layout.simple_spinner_item,
                devices.map { "${it.first}\n${it.second}" })
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            deviceSpinner.adapter = adapter
            appendLog("Found: $name")
        }
    }

    override fun onConnected() {
        appendLog("✓ Connected")
        ble.readStatus()
        connectBtn.text = "Disconnect"
    }

    override fun onDisconnected() {
        appendLog("✗ Disconnected")
        connectBtn.text = "Connect"
        statusText.text = "Offline"
    }

    override fun onStatus(status: FingerBleManager.DeviceStatus) {
        statusText.text = buildString {
            append("Sensor: ${if (status.sensorReady) "✓" else "✗"}  ")
            append("FP: ${status.count}  ")
            append("WiFi: ${status.wifiMode}  ")
            append("${status.temp}°C  ")
            append(if (status.autoActive) "▶ SCAN" else "⏸ STOP")
        }
        autoScanBtn.text = if (status.autoActive) "STOP SCAN" else "START SCAN"
    }

    override fun onEvent(json: String) {
        appendLog("Event: $json")
    }

    override fun onLog(msg: String) {
        appendLog(msg)
    }

    private fun appendLog(msg: String) {
        runOnUiThread {
            logView.append("$msg\n")
            logView.parent?.let {
                (it as? ScrollView)?.fullScroll(View.FOCUS_DOWN)
            }
        }
    }
}
