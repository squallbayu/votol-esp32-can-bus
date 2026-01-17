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

# USB Serial Configuration
USB_SERIAL_PORT = None  # Auto-detect
USB_SERIAL_BAUD = 115200

# Global data
latest_data = {
    "rpm": 0, "speed": 0, "mode": "PARK", "volts": 0.0, "amps": 0.0,
    "soc": 0, "temps": {"ctrl": 0, "motor": 0, "batt": 0},
    "cells": [0] * 23, "cellDelta": 0, "canRate": 0,
    "connType": "DISCONNECTED"
}

ble_connected = False
ble_client = None

ble_buffer = ""

def notification_handler(sender, data):
    """Handle BLE notifications with reassembly"""
    global latest_data, ble_buffer
    try:
        content = data.decode('utf-8')
        ble_buffer += content
        
        # Process complete messages (delimited by newline)
        if '\n' in ble_buffer:
            lines = ble_buffer.split('\n')
            for line in lines[:-1]:
                if line.strip():
                    try:
                        data_obj = json.loads(line.strip())
                        data_obj['connType'] = 'BLE'
                        latest_data = data_obj
                        socketio.emit('dashboard_data', data_obj)
                    except json.JSONDecodeError:
                        pass  # Skip malformed JSON
            ble_buffer = lines[-1]
    except Exception:
        pass

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
                                latest_data.update(data_obj)
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
    
    # Run Flask
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, allow_unsafe_werkzeug=True)
