package main

import (
	"fmt"
	"net"
	"strconv"
	"strings"
)

const udpPort = 4000

var canIDMap = map[int]string{
	0x0A010810: "DRIVE / TEMPS",
	0x0A6D0D09: "VOLTAGE / AMPS",
	0x0E6C0D09: "BATT TEMP",
	0x0A6E0D09: "SOC / HEALTH",
	0x0A6F0D09: "CELL STATS",
	0x0A700D09: "TEMP STATS",
	0x0A730D09: "BALANCE INFO",
	0x1810D0F3: "CHARGER 1",
	0x1811D0F3: "CHARGER 2",
}

func startUDP() {
	addr := net.UDPAddr{
		Port: udpPort,
		IP:   net.ParseIP("0.0.0.0"),
	}
	conn, err := net.ListenUDP("udp", &addr)
	if err != nil {
		fmt.Printf("[UDP] Error listening: %v\n", err)
		return
	}
	defer conn.Close()

	fmt.Printf("[UDP] Listening for WiFi Sniffer on port %d...\n", udpPort)

	buf := make([]byte, 1024)
	for {
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			fmt.Printf("[UDP] Read error: %v\n", err)
			continue
		}

		text := string(buf[:n])
		text = strings.TrimSpace(text)
		
		// Format: TIMESTAMP,ID,LEN,D0,D1...
		parts := strings.Split(text, ",")
		if len(parts) >= 3 {
			timestamp := parts[0]
			canIDHex := parts[1]
			
			// Parse ID to look up name
			canID, err := strconv.ParseInt(canIDHex, 16, 64)
			name := "UNKNOWN"
			if err == nil {
				if val, ok := canIDMap[int(canID)]; ok {
					name = val
				}
			}

			// Reconstruct data bytes
			dataBytes := strings.Join(parts[3:], " ")
			
			logMsg := fmt.Sprintf("[%sms] [%s] 0x%s | %s", timestamp, name, canIDHex, dataBytes)
			broadcastCanSniffer(logMsg)
		}
	}
}
