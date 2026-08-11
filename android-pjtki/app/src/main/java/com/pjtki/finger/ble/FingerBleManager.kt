package com.pjtki.finger.ble

import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import com.google.gson.Gson
import java.util.UUID

class FingerBleManager(private val ctx: Context) {

    companion object {
        val SERVICE_UUID = UUID.fromString("4fafc201-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_STATUS   = UUID.fromString("4fafc202-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_COMMAND  = UUID.fromString("4fafc203-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_ENROLL   = UUID.fromString("4fafc204-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_DELETE   = UUID.fromString("4fafc205-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_SETTINGS = UUID.fromString("4fafc206-1fb5-459e-8fcc-c5c9c331914b")
        val CHAR_EVENTS   = UUID.fromString("4fafc207-1fb5-459e-8fcc-c5c9c331914b")
    }

    data class DeviceStatus(
        val ready: Boolean = false,
        val autoActive: Boolean = false,
        val count: Int = 0,
        val wifiMode: String = "AP",
        val temp: Int = 0,
        val sensorReady: Boolean = false,
        val irEnabled: Boolean = true
    )

    data class FingerSettings(
        val apiBaseUrl: String = "",
        val kodeCabang: String = "",
        val deviceId: String = "",
        val apiKey: String = "",
        val irEnabled: Boolean = true,
        val scanSchedule: Boolean = false,
        val scanStartHour: Int = 6,
        val scanEndHour: Int = 22
    )

    interface Callback {
        fun onScanFound(name: String, address: String)
        fun onConnected()
        fun onDisconnected()
        fun onStatus(status: DeviceStatus)
        fun onEvent(json: String)
        fun onLog(msg: String)
    }

    private val btManager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val btAdapter: BluetoothAdapter? = btManager.adapter
    private val handler = Handler(Looper.getMainLooper())
    private val gson = Gson()

    var callback: Callback? = null
    var isConnected = false
        private set

    private var bleGatt: BluetoothGatt? = null
    private var statusChar: BluetoothGattCharacteristic? = null

    fun startScan() {
        if (btAdapter?.isEnabled != true) {
            callback?.onLog("Bluetooth tidak aktif")
            return
        }
        val scanner = btAdapter?.bluetoothLeScanner ?: return
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner.startScan(null, settings, scanCallback)
        callback?.onLog("Scanning...")
    }

    fun stopScan() {
        btAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    fun disconnect() {
        bleGatt?.disconnect()
        bleGatt?.close()
        bleGatt = null
        isConnected = false
    }

    fun connect(address: String) {
        stopScan()
        val device = btAdapter?.getRemoteDevice(address) ?: return
        bleGatt = device.connectGatt(ctx, false, gattCallback)
    }

    // ── BLE Operations ──

    fun sendCommand(cmd: String) {
        writeChar(CHAR_COMMAND, cmd.toByteArray())
    }

    fun enrollFinger(employeeId: String, name: String) {
        val json = """{"employeeId":"$employeeId","name":"$name"}"""
        writeChar(CHAR_ENROLL, json.toByteArray())
    }

    fun deleteFinger(id: Int) {
        writeChar(CHAR_DELETE, id.toString().toByteArray())
    }

    fun saveSettings(settings: FingerSettings) {
        writeChar(CHAR_SETTINGS, gson.toJson(settings).toByteArray())
    }

    fun saveWiFi(ssid: String, pass: String) {
        val json = """{"wifiSsid":"$ssid","wifiPass":"$pass"}"""
        writeChar(CHAR_SETTINGS, json.toByteArray())
    }

    fun readStatus() {
        readChar(CHAR_STATUS)
    }

    fun readSettings() {
        readChar(CHAR_SETTINGS)
    }

    // ── Internal ──

    private fun writeChar(uuid: UUID, data: ByteArray) {
        val gatt = bleGatt ?: return
        val service = gatt.getService(SERVICE_UUID) ?: return
        val char = service.getCharacteristic(uuid) ?: return
        char.value = data
        gatt.writeCharacteristic(char)
    }

    private fun readChar(uuid: UUID) {
        val gatt = bleGatt ?: return
        val service = gatt.getService(SERVICE_UUID) ?: return
        val char = service.getCharacteristic(uuid) ?: return
        gatt.readCharacteristic(char)
    }

    // ── Scan Callback ──

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: result.scanRecord?.deviceName ?: ""
            val addr = result.device.address
            val uuids = result.scanRecord?.serviceUuids?.map { it.uuid.toString() } ?: emptyList()
            // Cari device kita: by nama atau service UUID
            if (name.contains("PJTKI", true) || addr == "8C:94:DF:48:27:EE" ||
                uuids.any { it.startsWith("4fafc201") }) {
                callback?.onScanFound(name.ifEmpty { "PJTKI-Finger" }, addr)
            }
        }
    }

    // ── GATT Callback ──

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                isConnected = true
                gatt?.discoverServices()
            } else {
                isConnected = false
                callback?.onDisconnected()
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) return
            callback?.onConnected()

            // Enable notifications for Status
            val service = gatt?.getService(SERVICE_UUID) ?: return
            statusChar = service.getCharacteristic(CHAR_STATUS)
            statusChar?.let {
                gatt.setCharacteristicNotification(it, true)
                val desc = it.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))
                desc?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(desc)
            }

            // Enable notifications for Events
            val eventChar = service.getCharacteristic(CHAR_EVENTS)
            eventChar?.let {
                gatt.setCharacteristicNotification(it, true)
                val desc = it.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"))
                desc?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(desc)
            }
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt?,
            char: BluetoothGattCharacteristic?,
            status: Int
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) return
            val uuid = char?.uuid ?: return
            when (uuid) {
                CHAR_STATUS -> {
                    val json = String(char.value)
                    val st = gson.fromJson(json, DeviceStatus::class.java)
                    callback?.onStatus(st)
                }
                CHAR_SETTINGS -> {
                    val json = String(char.value)
                    callback?.onLog("Settings: $json")
                }
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt?,
            char: BluetoothGattCharacteristic?
        ) {
            val uuid = char?.uuid ?: return
            val value = String(char.value)
            when (uuid) {
                CHAR_STATUS -> {
                    val st = gson.fromJson(value, DeviceStatus::class.java)
                    callback?.onStatus(st)
                }
                CHAR_EVENTS -> callback?.onEvent(value)
            }
        }
    }
}
