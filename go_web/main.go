package main

import (
	"bytes"
	_ "embed"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"runtime"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

//go:embed dashboard.html
var dashboardHTML []byte

var (
	// State
	latestData   DashboardData
	dataLock     sync.RWMutex
	lastDataTime time.Time

	// WebSockets
	upgrader = websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool { return true },
	}
	clients   = make(map[*websocket.Conn]bool)
	clientsMu sync.Mutex
	broadcast = make(chan WSMessage)
)

func main() {
	// Initialize default data
	latestData.ConnType = "DISCONNECTED"

	// Start background routines
	go startBLE()
	go startSerial()
	go startUDP()
	go startWiFi()
	go handleMessages()

	// Web Server
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		w.Write(dashboardHTML)
	})

	http.HandleFunc("/ws", handleConnections)

	// Serial Control API
	http.HandleFunc("/api/serial/control", func(w http.ResponseWriter, r *http.Request) {
		// Add CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")

		// Handle preflight OPTIONS request
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}

		if r.Method != http.MethodPost {
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
			return
		}

		var req struct {
			Action string `json:"action"` // "pause" or "resume"
		}

		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		if req.Action == "pause" {
			PauseSerial()
		} else if req.Action == "resume" {
			ResumeSerial()
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"status": "ok", "action": req.Action})
	})

	// BLE Control API
	http.HandleFunc("/api/ble/control", func(w http.ResponseWriter, r *http.Request) {
		// Add CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")

		// Handle preflight OPTIONS request
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}

		if r.Method != http.MethodPost {
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
			return
		}

		var req struct {
			Action string `json:"action"` // "pause" or "resume"
		}

		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		if req.Action == "pause" {
			PauseBLE()
		} else if req.Action == "resume" {
			ResumeBLE()
		}

		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"status": "ok", "action": req.Action})
	})

	// WiFi OTA Proxy API
	http.HandleFunc("/api/wifi/ota", func(w http.ResponseWriter, r *http.Request) {
		// Add CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "POST, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")

		// Handle preflight OPTIONS request
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}

		if r.Method != http.MethodPost {
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
			return
		}

		// limit to 2MB
		r.ParseMultipartForm(2 << 20)

		file, _, err := r.FormFile("firmware")
		if err != nil {
			http.Error(w, "Error retrieving file", http.StatusBadRequest)
			return
		}
		defer file.Close()

		// Read file content
		fileBytes, err := io.ReadAll(file)
		if err != nil {
			http.Error(w, "Error reading file", http.StatusInternalServerError)
			return
		}

		// Proxy to ESP32
		espURL := "http://192.168.4.1:81/update"
		fmt.Printf("[WiFi OTA] Proxying %d bytes to %s\n", len(fileBytes), espURL)

		req, err := http.NewRequest("POST", espURL, bytes.NewBuffer(fileBytes))
		if err != nil {
			http.Error(w, "Failed to create request", http.StatusInternalServerError)
			return
		}
		req.Header.Set("Content-Type", "application/octet-stream")

		client := &http.Client{Timeout: 3 * time.Minute}
		resp, err := client.Do(req)
		if err != nil {
			http.Error(w, "Failed to connect to ESP32: "+err.Error(), http.StatusBadGateway)
			return
		}
		defer resp.Body.Close()

		body, _ := io.ReadAll(resp.Body)
		fmt.Printf("[WiFi OTA] Response: %v %s\n", resp.Status, string(body))

		if resp.StatusCode != 200 {
			http.Error(w, "ESP32 Error: "+string(body), http.StatusBadGateway)
			return
		}

		w.Write([]byte("OK"))
	})

	// WiFi OTA Status Proxy API
	http.HandleFunc("/api/wifi/ota_status", func(w http.ResponseWriter, r *http.Request) {
		// Add CORS headers
		w.Header().Set("Access-Control-Allow-Origin", "*")
		w.Header().Set("Access-Control-Allow-Methods", "GET, OPTIONS")
		w.Header().Set("Access-Control-Allow-Headers", "Content-Type")

		// Handle preflight OPTIONS request
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusOK)
			return
		}

		if r.Method != http.MethodGet {
			http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
			return
		}

		espURL := "http://192.168.4.1:81/ota_status"
		client := &http.Client{Timeout: 2 * time.Second}
		resp, err := client.Get(espURL)
		if err != nil {
			http.Error(w, "Failed to connect to ESP32: "+err.Error(), http.StatusBadGateway)
			return
		}
		defer resp.Body.Close()

		body, _ := io.ReadAll(resp.Body)
		if resp.StatusCode != 200 {
			http.Error(w, "ESP32 Error: "+string(body), http.StatusBadGateway)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Write(body)
	})

	port := "8080"
	fmt.Printf("Starting server on http://localhost:%s\n", port)

	// Open browser automatically
	openBrowser("http://localhost:" + port)

	err := http.ListenAndServe(":"+port, nil)
	if err != nil {
		log.Fatal("ListenAndServe: ", err)
	}
}

func handleConnections(w http.ResponseWriter, r *http.Request) {
	ws, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Printf("error upgrading: %v", err)
		return
	}
	defer ws.Close()

	clientsMu.Lock()
	clients[ws] = true
	clientsMu.Unlock()

	// Send initial state
	dataLock.RLock()
	ws.WriteJSON(WSMessage{Type: "dashboard_data", Data: latestData})
	dataLock.RUnlock()

	// Send status
	// We don't track connection status strictly here yet, assume disconnected unless data flows
	// But let's send what we have in cache

	// Listen for incoming (not used much, maybe for commands later)
	for {
		_, _, err := ws.ReadMessage()
		if err != nil {
			clientsMu.Lock()
			delete(clients, ws)
			clientsMu.Unlock()
			break
		}
	}
}

func handleMessages() {
	for {
		msg := <-broadcast
		clientsMu.Lock()
		for client := range clients {
			err := client.WriteJSON(msg)
			if err != nil {
				log.Printf("error: %v", err)
				client.Close()
				delete(clients, client)
			}
		}
		clientsMu.Unlock()
	}
}

// Helpers called from other files

func processJSON(jsonStr string, sourceType string) {
	// 1. Unmarshal into the raw ESP32 struct (Short Keys)
	var raw ESP32Incoming
	err := json.Unmarshal([]byte(jsonStr), &raw)
	if err != nil {
		// fmt.Printf("JSON Parse Error: %v\n", err)
		return
	}

	// 2. Start from last known state to avoid fast packets wiping full data
	dataLock.RLock()
	newData := latestData
	dataLock.RUnlock()

	// 3. Always update fast/basic fields
	newData.ConnType = sourceType
	newData.Speed = float64(raw.Speed)
	newData.RPM = raw.RPM
	newData.Volts = raw.Volts
	newData.Amps = raw.Amps
	newData.Power = int(raw.Power)
	newData.SOC = raw.SOC
	// Only update mode if not empty (prevent fast packets from clearing it)
	if raw.Mode != "" {
		newData.Mode = raw.Mode
	}
	newData.Temps = Temps{
		Ctrl:  raw.Temps.Ctrl,
		Motor: raw.Temps.Motor,
		Batt:  raw.Temps.Batt,
	}
	newData.CanRate = raw.CanRate
	newData.Heartbeat = raw.Heartbeat

	isFull := raw.Type == "" || raw.Type == "full"

	// 4. Only update full/extended fields on full packets
	if isFull {
		if len(raw.Cells) > 0 {
			newData.Cells = raw.Cells
		}

		newData.CellVoltStats = CellVoltStats{
			Delta:       raw.CellDelta, // Firmware sends top-level cd, map to CellVoltStats.Delta
			Highest:     raw.Stats.High,
			HighestCell: raw.Stats.HighCell,
			Lowest:      raw.Stats.Low,
			LowestCell:  raw.Stats.LowCell,
			Average:     raw.Stats.Avg,
		}

		// Explicitly overwrite if stats struct exists
		if raw.CellDelta > 0 {
			newData.CellVoltStats.Delta = raw.CellDelta
		}

		newData.Health = BatteryHealth{
			SOH:     raw.Health.SOH,
			Cycles:  raw.Health.Cycles,
			RemCap:  raw.Health.RemCap,
			FullCap: raw.Health.FullCap,
		}

		newData.BmsInfo = BMSInfo{
			HwVer: raw.BMS.HW,
			FwVer: raw.BMS.FW,
		}

		// Map Balance info
		newData.Balance.Mode = raw.Balance.Mode
		newData.Balance.Status = raw.Balance.Status
		if len(raw.Balance.Cells) > 0 {
			newData.Balance.Cells = make([]bool, len(raw.Balance.Cells))
			for i, val := range raw.Balance.Cells {
				newData.Balance.Cells[i] = (val == 1)
			}
		}

		// Map Temperature Stats
		newData.TempStats = TempStats{
			Max:     raw.TempStats.Max,
			MaxCell: raw.TempStats.MaxCell,
			Min:     raw.TempStats.Min,
			MinCell: raw.TempStats.MinCell,
		}

		// Map Charger Info
		newData.Charger = ChargerInfo{
			On:       raw.Charger.On == 1,
			Volts:    raw.Charger.Volts,
			Amps:     raw.Charger.Amps,
			Original: raw.Charger.Original == 1,
		}
	}

	// Thread-safe update
	dataLock.Lock()
	latestData = newData
	lastDataTime = time.Now()
	dataLock.Unlock()

	// Broadcast to UI
	broadcast <- WSMessage{Type: "dashboard_data", Data: newData}
}

func broadcastCanSniffer(data string) {
	broadcast <- WSMessage{Type: "can_sniffer", Data: data}
}

func broadcastConnectionStatus(connected bool) {
	broadcast <- WSMessage{Type: "connection_status", Data: map[string]bool{"connected": connected}}
}

func openBrowser(url string) {
	var err error
	switch runtime.GOOS {
	case "linux":
		err = execCommand("xdg-open", url)
	case "windows":
		err = execCommand("rundll32", "url.dll,FileProtocolHandler", url)
	case "darwin":
		err = execCommand("open", url)
	}
	if err != nil {
		// specific logic or ignore
	}
}

func execCommand(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	cmd.Stderr = os.Stderr
	return cmd.Start()
}
