#!/usr/bin/env python3
"""
VOTOL BLE Dashboard - Python Web Server
=========================================
Receives data from ESP32 via BLE and serves web dashboard
Also reads USB Serial for CAN Sniffer output

Author: Zekri R (ZEKRI.ID)
Website: https://zekri.id

Based on: https://github.com/yudhaime/displaypolytronfoxrs

=========================================
DISCLAIMER / PERINGATAN
=========================================
This project is provided "AS IS" without any warranty.
Use at your own risk. The author is NOT responsible
for any damage, malfunction, or injury to your vehicle,
controller, battery, or any other components.

Proyek ini disediakan "APA ADANYA" tanpa jaminan apapun.
Gunakan dengan risiko Anda sendiri. Penulis TIDAK
bertanggung jawab atas kerusakan pada kendaraan,
controller, baterai, atau komponen lainnya.
=========================================
"""

import asyncio
import json
import threading
import time
import copy
import serial
import serial.tools.list_ports
from flask import Flask, render_template
from flask_socketio import SocketIO, emit
from bleak import BleakScanner, BleakClient

app = Flask(__name__)
app.config['SECRET_KEY'] = 'votol_secret_ble'
socketio = SocketIO(app, cors_allowed_origins="*")

# BLE Configuration
DEVICE_NAME = "Votol_BLE"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"

# UDP Configuration (WiFi Sniffer)
UDP_PORT = 4000

# CAN ID Dictionary / Map
CAN_ID_MAP = {
    0x0A010810: "DRIVE / TEMPS",
    0x0A6D0D09: "VOLTAGE / AMPS",
    0x0E6C0D09: "BATT TEMP",
    0x0A6E0D09: "SOC / HEALTH",
    0x0A6F0D09: "CELL STATS",
    0x0A700D09: "TEMP STATS",
    0x0A730D09: "BALANCE INFO",
    0x1810D0F3: "CHARGER 1",
    0x1811D0F3: "CHARGER 2"
    # 0x0E6XXXXX are Cell Voltages (dynamic)
}

# USB Serial Configuration
USB_SERIAL_PORT = None  # Auto-detect
USB_SERIAL_BAUD = 115200

# Thread safety
data_lock = threading.Lock()

# Global data (minimal - ESP32 sends complete data)
latest_data = {"connType": "DISCONNECTED"}
last_data_time = 0  # Heartbeat timeout detection

# Debug Stats
ble_stats = {
    "packets": 0,
    "bytes": 0,
    "start_time": 0,
    "last_packet_time": 0,
    "burst_count": 0,
    "hz": 0.0,
    "kbps": 0.0
}

ble_connected = False
ble_client = None

# BLE buffer with overflow protection
ble_buffer = ""
MAX_BUFFER_SIZE = 8192  # Prevent memory leak

def notification_handler(sender, data):
    """Handle BLE notifications with reassembly, stats, and thread safety"""
    global latest_data, ble_buffer, last_data_time, ble_stats
    
    # --- STATS CALCULATION ---
    current_time = time.time()
    if ble_stats["start_time"] == 0:
        ble_stats["start_time"] = current_time
        
    # Burst detection (< 10ms gap)
    if (current_time - ble_stats["last_packet_time"]) < 0.010:
        ble_stats["burst_count"] += 1
    else:
        ble_stats["burst_count"] = 0
        
    ble_stats["last_packet_time"] = current_time
    ble_stats["packets"] += 1
    ble_stats["bytes"] += len(data)
    
    # Calculate rates every 1 sec to avoid jitter
    elapsed = current_time - ble_stats["start_time"]
    if elapsed >= 1.0:
        ble_stats["hz"] = ble_stats["packets"] / elapsed
        ble_stats["kbps"] = (ble_stats["bytes"] / 1024) / elapsed
        # Reset counters
        ble_stats["packets"] = 0
        ble_stats["bytes"] = 0
        ble_stats["start_time"] = current_time
    # -------------------------

    try:
        content = data.decode('utf-8')
        ble_buffer += content
        
        # Buffer overflow protection
        if len(ble_buffer) > MAX_BUFFER_SIZE:
            print("[BLE] Buffer overflow, resetting...")
            ble_buffer = ""
            return
        
        # Process complete messages (delimited by newline)
        if '\n' in ble_buffer:
            lines = ble_buffer.split('\n')
            for line in lines[:-1]:
                if line.strip():
                    try:
                        data_obj = json.loads(line.strip())
                        data_obj['connType'] = 'BLE'
                        
                        # Inject Debug Stats
                        data_obj['debug_hz'] = round(ble_stats["hz"], 1)
                        data_obj['debug_kbps'] = round(ble_stats["kbps"], 1)
                        data_obj['debug_burst'] = ble_stats["burst_count"]
                        
                        # --- CAN SNIFFER EMULATION ---
                        # Verify we have debug data
                        if 'debug' in data_obj:
                            debug = data_obj['debug']
                            # Emulate Voltage ID 0x0A6D0D09 (contains volts/amps in real CAN)
                            if 'voltageHex' in debug and 'currentHex' in debug:
                                socketio.emit('can_sniffer', {'data': f"[BLE-SIM] ID: 0x0A6D0D09 Data: {debug['voltageHex']} {debug['currentHex']} ..."})
                            
                            # Emulate SOC ID 0x0A6E0D09
                            if 'socHex' in debug:
                                socketio.emit('can_sniffer', {'data': f"[BLE-SIM] ID: 0x0A6E0D09 Data: {debug['socHex']} ..."})
                                
                            # Emulate Balance ID 0x0A730D09
                            if 'balanceHex' in debug:
                                socketio.emit('can_sniffer', {'data': f"[BLE-SIM] ID: 0x0A730D09 Data: {debug['balanceHex']}"})
                        # -----------------------------
                        
                        # Thread-safe update
                        with data_lock:
                            latest_data = copy.deepcopy(data_obj)
                            last_data_time = time.time()
                        
                        socketio.emit('dashboard_data', data_obj)
                    except json.JSONDecodeError:
                        pass  # Skip malformed JSON
            ble_buffer = lines[-1]
    except Exception as e:
        print(f"[BLE] Notification error: {e}")

async def ble_connect_loop():
    """BLE connection loop with auto-reconnect"""
    global ble_client, ble_connected
    
    while True:
        try:
            if ble_client is None or not ble_client.is_connected:
                print(f"[BLE] Scanning for {DEVICE_NAME}...")
                
                devices = await BleakScanner.discover(timeout=5.0)
                target_device = None
                
                for d in devices:
                    if d.name == DEVICE_NAME:
                        target_device = d
                        break
                
                if target_device:
                    print(f"[BLE] Found {DEVICE_NAME}, connecting...")
                    ble_client = BleakClient(target_device.address)
                    await ble_client.connect()
                    
                    if ble_client.is_connected:
                        print(f"[BLE] Connected!")
                        ble_connected = True
                        socketio.emit('connection_status', {'connected': True})
                        
                        # Start notifications
                        await ble_client.start_notify(CHAR_UUID, notification_handler)
                        
                        # Keep connection alive
                        while ble_client.is_connected:
                            await asyncio.sleep(1)
                        
                        print("[BLE] Disconnected")
                        ble_connected = False
                        socketio.emit('connection_status', {'connected': False})
                else:
                    print(f"[BLE] {DEVICE_NAME} not found, retrying...")
                    await asyncio.sleep(3)
            
            await asyncio.sleep(1)
            
        except Exception as e:
            print(f"[BLE] Error: {e}")
            ble_connected = False
            socketio.emit('connection_status', {'connected': False})
            if ble_client:
                try:
                    await ble_client.disconnect()
                except:
                    pass
                ble_client = None
            await asyncio.sleep(3)

def run_ble_loop():
    """Run BLE loop in separate thread"""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(ble_connect_loop())

def usb_serial_reader():
    """Read USB Serial for both CAN Sniffer output AND JSON data"""
    global USB_SERIAL_PORT
    
    while True:
        try:
            # Auto-detect ESP32 USB port
            if USB_SERIAL_PORT is None:
                ports = serial.tools.list_ports.comports()
                for port in ports:
                    if 'USB' in port.description or 'CH340' in port.description or 'CP210' in port.description:
                        USB_SERIAL_PORT = port.device
                        print(f"[USB] Found ESP32 on {USB_SERIAL_PORT}")
                        break
            
            if USB_SERIAL_PORT:
                ser = serial.Serial(USB_SERIAL_PORT, USB_SERIAL_BAUD, timeout=1)
                print(f"[USB] Connected to {USB_SERIAL_PORT}")
                print(f"[USB] USB Serial has PRIORITY over BLE for data")
                
                while True:
                    try:
                        line = ser.readline().decode('utf-8', errors='ignore').strip()
                        if not line:
                            continue
                            
                        # CAN Sniffer output (for display)
                        if '[CAN]' in line:
                            socketio.emit('can_sniffer', {'data': line})
                            # Don't print to avoid spam
                            
                        # JSON data (for dashboard) - PRIORITY!
                        elif line.startswith('[JSON]'):
                            json_str = line[6:]  # Remove [JSON] marker
                            try:
                                data_obj = json.loads(json_str)
                                data_obj['connType'] = 'USB'
                                
                                # Thread-safe update
                                with data_lock:
                                    latest_data.update(data_obj)
                                    last_data_time = time.time()
                                
                                socketio.emit('dashboard_data', data_obj)
                                # Print confirmation every 100 messages
                                if data_obj.get('heartbeat', 0) % 100 == 0:
                                    print(f"[USB] Data: RPM={data_obj.get('rpm')}, V={data_obj.get('volts')}, A={data_obj.get('amps')}")
                            except json.JSONDecodeError as e:
                                print(f"[USB] JSON parse error: {e}")
                                
                    except Exception as e:
                        print(f"[USB] Read error: {e}")
                        break
                        
                ser.close()
                USB_SERIAL_PORT = None
                
        except Exception as e:
            print(f"[USB] Connection error: {e}")
            USB_SERIAL_PORT = None
            
        import time
        time.sleep(3)  # Retry after 3 seconds

def udp_sniffer_listener():
    """Receive UDP packets from ESP32 WiFi Sniffer"""
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', UDP_PORT))
    print(f"[UDP] Listening for WiFi Sniffer on port {UDP_PORT}...")
    
    while True:
        try:
            data, addr = sock.recvfrom(1024) # Buffer size is 1024 bytes
            text = data.decode('utf-8').strip()
            
            # Format: TIMESTAMP,ID,LEN,D0,D1...
            parts = text.split(',')
            if len(parts) >= 3:
                timestamp = parts[0]
                can_id_hex = parts[1]
                can_id = int(can_id_hex, 16)
                
                # Check for Known ID
                name = CAN_ID_MAP.get(can_id, "UNKNOWN")
                
                # Formatting (reconstruct for display)
                data_bytes = " ".join(parts[3:])
                
                # Emit to Sniffer Panel with timestamp
                socketio.emit('can_sniffer', {'data': f"[{timestamp}ms] [{name}] 0x{can_id_hex} | {data_bytes}"})
                
        except Exception as e:
            print(f"[UDP] Error: {e}")
            time.sleep(1)

@app.route('/')
def index():
    return render_template('dashboard.html')

@socketio.on('connect')
def handle_connect():
    print('Client connected to dashboard')
    emit('connection_status', {'connected': ble_connected})
    emit('dashboard_data', latest_data)

@socketio.on('disconnect')
def handle_disconnect():
    print('Client disconnected from dashboard')

if __name__ == '__main__':
    print("\n" + "="*50)
    print("   VOTOL BLE + USB DASHBOARD")
    print("="*50)
    print("Starting BLE connection...")
    print("Starting USB Serial reader...")
    print("Dashboard: http://localhost:5000")
    print("="*50 + "\n")
    
    # Start BLE thread
    ble_thread = threading.Thread(target=run_ble_loop, daemon=True)
    ble_thread.start()
    
    # Start USB Serial thread
    usb_thread = threading.Thread(target=usb_serial_reader, daemon=True)
    usb_thread.start()

    # Start UDP Sniffer thread
    udp_thread = threading.Thread(target=udp_sniffer_listener, daemon=True)
    udp_thread.start()
    
    # Run Flask
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, allow_unsafe_werkzeug=True)
