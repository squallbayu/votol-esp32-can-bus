package main

import (
	"bufio"
	"fmt"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
)

var (
	serialPort   serial.Port
	serialMutex  sync.Mutex
	serialPaused bool
)

func startSerial() {
	for {
		serialMutex.Lock()
		paused := serialPaused
		serialMutex.Unlock()

		if paused {
			time.Sleep(1 * time.Second)
			continue
		}

		portName := findESP32Port()
		if portName != "" {
			fmt.Printf("[Serial] Found ESP32 on %s\n", portName)
			err := readSerialLoop(portName)
			if err != nil {
				// Don't log error if we intentionally closed it (paused via API)
				serialMutex.Lock()
				isPaused := serialPaused
				serialMutex.Unlock()
				if !isPaused {
					fmt.Printf("[Serial] Error or disconnected: %v\n", err)
				}
			}
		} else {
			// fmt.Println("[Serial] ESP32 not found, retrying...")
		}
		time.Sleep(3 * time.Second)
	}
}

// Global control functions

func PauseSerial() {
	serialMutex.Lock()
	defer serialMutex.Unlock()
	serialPaused = true
	if serialPort != nil {
		fmt.Println("[Serial] Pausing... closing port.")
		serialPort.Close()
		serialPort = nil
	}
}

func ResumeSerial() {
	serialMutex.Lock()
	defer serialMutex.Unlock()
	fmt.Println("[Serial] Resuming...")
	serialPaused = false
}

func findESP32Port() string {
	ports, err := serial.GetPortsList()
	if err != nil {
		fmt.Println("[Serial] Error listing ports:", err)
		return ""
	}
	
	// Linux typically /dev/ttyUSBx or /dev/ttyACM.
	for _, port := range ports {
		// Simple filter for Linux
		if strings.Contains(port, "USB") || strings.Contains(port, "ACM") || strings.Contains(port, "COM") {
			return port
		}
	}
	return ""
}

func readSerialLoop(portName string) error {
	mode := &serial.Mode{
		BaudRate: 115200,
	}
	port, err := serial.Open(portName, mode)
	if err != nil {
		return err
	}
	
	serialMutex.Lock()
	serialPort = port
	serialMutex.Unlock()

	defer func() {
		serialMutex.Lock()
		if serialPort == port {
			serialPort = nil
		}
		serialMutex.Unlock()
		port.Close()
	}()

	fmt.Printf("[Serial] Connected to %s\n", portName)

	scanner := bufio.NewScanner(port)
	for scanner.Scan() {
		line := scanner.Text()
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}

		// CAN Sniffer
		if strings.Contains(line, "[CAN]") {
			broadcastCanSniffer(line)
		} else if strings.HasPrefix(line, "[JSON]") {
			jsonStr := strings.TrimPrefix(line, "[JSON]")
			processJSON(jsonStr, "USB")
		}
	}

	return scanner.Err()
}
