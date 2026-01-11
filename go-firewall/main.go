package main

import (
	"encoding/json"
	"log"
	"net"
	"os"
	"bytes"
)

type Request struct {
	IP     string `json:"ip"`
	Host   string `json:"host"`
	Method string `json:"method"`
	URI    string `json:"uri"`
}

func handleConn(conn net.Conn) {
	defer conn.Close()

	buf := make([]byte, 8192)

	n, err := conn.Read(buf)
	if err != nil || n == 0 {
		return
	}

	// Trim NUL bytes and whitespace
	data := bytes.Trim(buf[:n], "\x00 \t\r\n")

	// Find first '{'
	idx := bytes.IndexByte(data, '{')
	if idx == -1 {
		log.Println("invalid payload, no JSON start")
		return
	}

	data = data[idx:]

	var req Request
	if err := json.Unmarshal(data, &req); err != nil {
		log.Println("unmarshal error:", err)
		log.Printf("RAW: %q\n", data)
		return
	}
	log.Printf("Raw: %q\n", data)
	log.Printf("[Engine] ip=%s host=%s method=%s uri=%s",
		req.IP, req.Host, req.Method, req.URI)

	action := "allow"
	if req.URI == "/blockme" {
		action = "block"
	}

	_, _ = conn.Write([]byte(action + "\n"))
}


func main() {
	socketPath := "/tmp/lazyfirewall.sock"
	_ = os.Remove(socketPath)

	l, err := net.Listen("unix", socketPath)
	if err != nil {
		log.Fatalf("listen error: %v", err)
	}
	defer l.Close()

	_ = os.Chmod(socketPath, 0777)

	log.Println("[Engine] Listening on", socketPath)

	for {
		conn, err := l.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn)
	}
}
