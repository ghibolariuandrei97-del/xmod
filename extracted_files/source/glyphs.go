package main

import (
	"os"
	"runtime"
	"strings"
)

// Glyphs: full Unicode on modern terminals; a set that the classic Windows console fonts (Consolas, Lucida) can draw
// when xcmd runs in the old conhost window (no Windows Terminal). XCMD_GLYPHS=unicode|safe forces one.
var safeGlyphs = func() bool {
	switch strings.ToLower(os.Getenv("XCMD_GLYPHS")) {
	case "safe", "ascii":
		return true
	case "unicode", "fancy":
		return false
	}
	return runtime.GOOS == "windows" && os.Getenv("WT_SESSION") == "" && os.Getenv("TERM_PROGRAM") == ""
}()

func G(fancy, safe string) string {
	if safeGlyphs {
		return safe
	}
	return fancy
}

var (
	gOK    = G("✔", "√")
	gErr   = G("✖", "×")
	gDead  = G("☠", "×")
	gHP    = G("✚", "♥")
	gAP    = G("◈", "♦")
	gTime  = G("⏱ ", "")
	gHS    = G("⌖ ", "")
	gOff   = G("◌", "o")
	gDeadM = G("✕", "x")
)

func spinnerFrames() []string {
	if safeGlyphs {
		return []string{"|", "/", "-", "\\"}
	}
	return []string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}
}
