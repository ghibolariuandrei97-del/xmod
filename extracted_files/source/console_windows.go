//go:build windows

package main

import "golang.org/x/sys/windows"

// Windows 10 console: UTF-8 output + ANSI colours for the plain (non-TUI) commands too
func init() {
	windows.SetConsoleOutputCP(65001)
	windows.SetConsoleCP(65001)
	for _, h := range []windows.Handle{windows.Stdout, windows.Stderr} {
		var mode uint32
		if windows.GetConsoleMode(h, &mode) == nil {
			windows.SetConsoleMode(h, mode|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING|windows.ENABLE_PROCESSED_OUTPUT)
		}
	}
}
