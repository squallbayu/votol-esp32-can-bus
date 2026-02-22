package main

// DashboardData represents the JSON structure sent to the UI (Full Keys)
type DashboardData struct {
	ConnType      string        `json:"connType"`
	Speed         float64       `json:"speed"`
	RPM           int           `json:"rpm"`
	Volts         float64       `json:"volts"`
	Amps          float64       `json:"amps"`
	Power         int           `json:"power"`
	SOC           int           `json:"soc"`
	Mode          string        `json:"mode"`
	Temps         Temps         `json:"temps"`
	Health        BatteryHealth `json:"health,omitempty"`
	Cells         []int         `json:"cells,omitempty"`
	CellTemps     []int         `json:"cellTemps,omitempty"`
	BmsInfo       BMSInfo       `json:"bmsInfo,omitempty"`
	Debug         DebugRaw      `json:"debug,omitempty"`
	CellVoltStats CellVoltStats `json:"cellVoltStats,omitempty"`
	RawHex        RawHex        `json:"rawHex,omitempty"`
	Balance       BalanceInfo   `json:"balance,omitempty"`
	TempStats     TempStats     `json:"tempStats,omitempty"`
	Charger       ChargerInfo   `json:"charger,omitempty"`
	Odometer      float64       `json:"odometer,omitempty"`

	// Statistics (Debug)
	DebugHz    float64 `json:"debug_hz,omitempty"`
	DebugKbps  float64 `json:"debug_kbps,omitempty"`
	DebugBurst int     `json:"debug_burst,omitempty"`
	CanRate    int     `json:"canRate,omitempty"`
	Heartbeat  int     `json:"heartbeat,omitempty"`
}

// ESP32Incoming represents the short-key JSON from the Firmware
type ESP32Incoming struct {
	Type  string  `json:"type,omitempty"`
	RPM   int     `json:"r"`
	Speed int     `json:"s"`
	Mode  string  `json:"m"`
	Volts float64 `json:"v"`
	Amps  float64 `json:"a"`
	Power float64 `json:"p"`
	SOC   int     `json:"sc"`
	Temps struct {
		Ctrl  int `json:"c"`
		Motor int `json:"m"`
		Batt  int `json:"b"`
	} `json:"t"`
	CanRate   int `json:"cr"`
	Heartbeat int `json:"hb"`

	// Full fields
	Cells     []int `json:"cells,omitempty"`
	CellDelta int   `json:"cd,omitempty"`

	Health struct {
		SOH     int     `json:"soh"`
		Cycles  int     `json:"cyc"`
		RemCap  float64 `json:"rc"`
		FullCap float64 `json:"fc"`
	} `json:"h,omitempty"`

	Stats struct {
		High     int `json:"hi"`
		HighCell int `json:"hiC"`
		Low      int `json:"lo"`
		LowCell  int `json:"loC"`
		Avg      int `json:"av"`
	} `json:"cvs,omitempty"`

	TempStats struct {
		Max     int `json:"max"`
		MaxCell int `json:"maxC"`
		Min     int `json:"min"`
		MinCell int `json:"minC"`
	} `json:"ts,omitempty"`

	Balance struct {
		Mode   int   `json:"md"`
		Status int   `json:"st"`
		Cells  []int `json:"cells"` // Firmware sends array of 0/1 ints (or bools?) snprintf says %d
	} `json:"b,omitempty"`

	Charger struct {
		On       int     `json:"on"`
		Volts    float64 `json:"v"`
		Amps     float64 `json:"a"`
		Original int     `json:"ori"`
	} `json:"chr,omitempty"`

	BMS struct {
		HW string `json:"hw"`
		FW string `json:"fw"`
	} `json:"bms,omitempty"`
}

type Temps struct {
	Ctrl  int `json:"ctrl"`
	Motor int `json:"motor"`
	Batt  int `json:"batt"`
}

type BatteryHealth struct {
	SOH     int     `json:"soh"`
	Cycles  int     `json:"cycles"`
	RemCap  float64 `json:"remCap"`
	FullCap float64 `json:"fullCap"`
}

type BMSInfo struct {
	Serial string `json:"serial"`
	HwVer  string `json:"hwVer"`
	FwVer  string `json:"fwVer"`
}

type DebugRaw struct {
	CurrentHex string `json:"currentHex"`
	VoltageHex string `json:"voltageHex"`
	SocHex     string `json:"socHex"`
	BalanceHex string `json:"balanceHex"`
}

type RawHex struct {
	Current string `json:"current"`
	Voltage string `json:"voltage"`
	SOC     string `json:"soc"`
}

type CellVoltStats struct {
	Delta       int `json:"delta"`
	Highest     int `json:"highest"`
	HighestCell int `json:"highestCell"`
	Lowest      int `json:"lowest"`
	LowestCell  int `json:"lowestCell"`
	Average     int `json:"average"`
}

type BalanceInfo struct {
	Mode   int    `json:"mode"`
	Status int    `json:"status"`
	Cells  []bool `json:"cells"`
}

type TempStats struct {
	Max     int `json:"max"`
	MaxCell int `json:"maxCell"`
	Min     int `json:"min"`
	MinCell int `json:"minCell"`
}

type ChargerInfo struct {
	On       bool    `json:"on"`
	Volts    float64 `json:"volts"`
	Amps     float64 `json:"amps"`
	Original bool    `json:"original"`
}

// WSMessage represents a message wrapper for WebSocket communication
type WSMessage struct {
	Type string      `json:"type"`
	Data interface{} `json:"data"`
}
