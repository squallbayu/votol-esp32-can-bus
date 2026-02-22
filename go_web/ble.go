//go:build cgo

package main

import (
	"fmt"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"
)

var (
	serviceUUID = bluetooth.NewUUID([16]byte{0x4f, 0xaf, 0xc2, 0x01, 0x1f, 0xb5, 0x45, 0x9e, 0x8f, 0xcc, 0xc5, 0xc9, 0xc3, 0x31, 0x91, 0x4b}) // 4fafc201-1fb5-459e-8fcc-c5c9c331914b
	charUUID    = bluetooth.NewUUID([16]byte{0xbe, 0xb5, 0x48, 0x3e, 0x36, 0xe1, 0x46, 0x88, 0xb7, 0xf5, 0xea, 0x07, 0x36, 0x1b, 0x26, 0xa8}) // beb5483e-36e1-4688-b7f5-ea07361b26a8
)

var (
	adapter    = bluetooth.DefaultAdapter
	bleBuffer  string
	blePaused  bool
	bleMutex   sync.Mutex
	currentDev *bluetooth.Device
)

func PauseBLE() {
	bleMutex.Lock()
	defer bleMutex.Unlock()
	blePaused = true
	fmt.Println("[BLE] Pausing... Disconnecting if active.")
	if currentDev != nil {
		currentDev.Disconnect()
		currentDev = nil
	}
}

func ResumeBLE() {
	bleMutex.Lock()
	defer bleMutex.Unlock()
	blePaused = false
	fmt.Println("[BLE] Resuming...")
}

func startBLE() {
	// Enable BLE interface
	err := adapter.Enable()
	if err != nil {
		fmt.Printf("[BLE] Error enabling adapter: %v\n", err)
		return
	}

	fmt.Println("[BLE] Adapter enabled. Starting scan logic...")

	for {
		bleMutex.Lock()
		if blePaused {
			bleMutex.Unlock()
			time.Sleep(1 * time.Second)
			continue
		}
		bleMutex.Unlock()

		// Scan for device
		var foundDevice *bluetooth.ScanResult
		fmt.Println("[BLE] Scanning for Votol_BLE...")

		err = adapter.Scan(func(adapter *bluetooth.Adapter, device bluetooth.ScanResult) {
			if device.LocalName() == "Votol_BLE" {
				fmt.Println("[BLE] Found Votol_BLE:", device.Address.String())
				foundDevice = &device
				adapter.StopScan()
			}
		})

		if err != nil {
			fmt.Printf("[BLE] Scan error: %v\n", err)
			time.Sleep(5 * time.Second)
			continue
		}

		if foundDevice != nil {
			connectToDevice(*foundDevice)
		} else {
			// Scan timeout or not found, retry
			time.Sleep(2 * time.Second)
		}
	}
}

func connectToDevice(deviceResult bluetooth.ScanResult) {
	fmt.Printf("[BLE] Connecting to %s...\n", deviceResult.Address.String())

	device, err := adapter.Connect(deviceResult.Address, bluetooth.ConnectionParams{})
	if err != nil {
		fmt.Printf("[BLE] Failed to connect: %v\n", err)
		return
	}

	bleMutex.Lock()
	currentDev = &device
	bleMutex.Unlock()

	defer func() {
		bleMutex.Lock()
		currentDev = nil
		bleMutex.Unlock()
		device.Disconnect()
	}()

	fmt.Println("[BLE] Connected!")
	broadcastConnectionStatus(true)

	// Discover services
	services, err := device.DiscoverServices([]bluetooth.UUID{serviceUUID})
	if err != nil || len(services) == 0 {
		fmt.Printf("[BLE] Failed to discover services: %v\n", err)
		return
	}

	service := services[0]

	// Discover characteristics
	chars, err := service.DiscoverCharacteristics([]bluetooth.UUID{charUUID})
	if err != nil || len(chars) == 0 {
		fmt.Printf("[BLE] Failed to discover characteristics: %v\n", err)
		return
	}

	char := chars[0]

	// Subscribe to notifications
	err = char.EnableNotifications(func(buf []byte) {
		handleBLENotification(buf)
	})
	if err != nil {
		fmt.Printf("[BLE] Failed to enable notifications: %v\n", err)
		return
	}

	fmt.Println("[BLE] Listening for data...")

	// Keep connection alive until disconnected
	select {}
}

func handleBLENotification(data []byte) {
	content := string(data)
	bleBuffer += content

	if len(bleBuffer) > 8192 {
		bleBuffer = "" // Overflow protect
	}

	if strings.Contains(bleBuffer, "\n") {
		lines := strings.Split(bleBuffer, "\n")
		// Process all complete lines
		for _, line := range lines[:len(lines)-1] {
			line = strings.TrimSpace(line)
			if len(line) > 0 {
				processJSON(line, "BLE")
			}
		}
		// Keep the last partial line
		bleBuffer = lines[len(lines)-1]
	}
}
