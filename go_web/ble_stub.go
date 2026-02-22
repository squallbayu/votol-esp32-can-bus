//go:build !cgo

package main

import "fmt"

// BLE is not available in this build (CGO disabled)

func PauseBLE()  {}
func ResumeBLE() {}

func startBLE() {
	fmt.Println("[BLE] Disabled in this build (CGO not available)")
}
