package main

import (
	"bufio"
	"encoding/json"
	"log"
	"net"
	"os"
)

type Request struct {
	IP     string `json:"ip"`
	Method string `json:"method"`
	URI    string `json:"uri"`
}

type Response struct {
	Action string `json:"action"`
}

func handleConn(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	line, err := reader.ReadBytes('\n')
	if err != nil{
		return
	}

	var req Request
	if err := json.Unmarshal(line, &req); err != nil {
		return
	}
	log.Printf("[Engine] ip=%s method=%s uri=%s",&req.IP, &req.Method,&req.URI)
	
	action := "ALLOW" // default action

	if req.URI == "/blockme" {
		action = "BLOCK"
	}

	resp := Response{Action: action}
	b, _ := json.Marshal(resp)
	b = append(b, '\n')
	conn.Write(b)
}

func main() {
	socketPath := "/tmp/lazyfirewall.sock"
	os.Remove(socketPath)
	l, err := net.Listen("unix",socketPath)
	if err != nil {
		log.Fatalf("listen error: %v", err)
	}
	defer l.Close()

	log.Println("[Engine] Listening on", socketPath)

	for {
		conn, err := l.Accept()
		if err != nil{
			continue
		}
		go handleConn(conn)
	}
}