#!/bin/sh
# build xcmd for every platform (Go 1.22+)
set -e
go mod tidy
GOOS=windows GOARCH=386 go build -trimpath -ldflags "-s -w" -o ../xcmd.exe .
GOOS=linux GOARCH=amd64 CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o ../xcmd .
GOOS=linux GOARCH=386 CGO_ENABLED=0 go build -trimpath -ldflags "-s -w" -o ../xcmd-i386 .
