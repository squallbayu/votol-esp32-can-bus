package main

import (
	"fmt"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	espWifiAddr = "192.168.4.1:81"
	espWifiPath = "/ws"
)

var (
	espWifiConn   *websocket.Conn
	espWifiConnMu sync.Mutex
)

// SendToESP32WiFi sends a text command to ESP32 via WiFi WebSocket
func SendToESP32WiFi(cmd string) error {
	espWifiConnMu.Lock()
	defer espWifiConnMu.Unlock()
	if espWifiConn == nil {
		return fmt.Errorf("not connected to ESP32 WiFi")
	}
	return espWifiConn.WriteMessage(websocket.TextMessage, []byte(cmd))
}

func startWiFi() {
	u := url.URL{Scheme: "ws", Host: espWifiAddr, Path: espWifiPath}

	for {
		// Attempt to connect
		// fmt.Printf("[WiFi] Connecting to %s...\n", u.String())

		c, _, err := websocket.DefaultDialer.Dial(u.String(), nil)
		if err != nil {
			// Quietly fail/retry (it's expected if not in WiFi mode)
			// fmt.Printf("[WiFi] Connect error: %v\n", err)
			time.Sleep(5 * time.Second)
			continue
		}

		fmt.Println("[WiFi] Connected to ESP32!")
		espWifiConnMu.Lock()
		espWifiConn = c
		espWifiConnMu.Unlock()
		broadcastConnectionStatus(true)

		// Read loop
		for {
			_, message, err := c.ReadMessage()
			if err != nil {
				fmt.Printf("[WiFi] Read error: %v\n", err)
				break
			}

			// Process message
			msgStr := string(message)
			if strings.TrimSpace(msgStr) != "" {
				processJSON(msgStr, "WIFI")
			}
		}

		c.Close()
		espWifiConnMu.Lock()
		espWifiConn = nil
		espWifiConnMu.Unlock()
		fmt.Println("[WiFi] Disconnected. Retrying in 5s...")
		time.Sleep(5 * time.Second)
	}
}
